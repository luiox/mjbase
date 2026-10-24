#include "mjbase/pass/access_modifier_tightener.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_resource_pending_commit.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

namespace mjbase::pass {

namespace {

using jbc::ir::ClassSession;
using jbc::ir::CodeUnitRef;

// JVM 访问标志位（libjbc 未提供旗标常量，JVMS 2 / 4.1/4.5/4.6 表）。
constexpr ca::u16 kAccPublic  = 0x0001;
constexpr ca::u16 kAccPrivate = 0x0002;
constexpr ca::u16 kAccStatic  = 0x0008;

/// 普查记账：条目存在 = 该成员被引用过；usedOuter = 存在异类引用源。
struct StaticMemberUse
{
    bool usedOuter = false;
};

/// 装载完成的类：会话 + 全部已升格方法体（普查证据集）+ 写回目标。
struct LoadedClass
{
    std::string_view              name      = {};     ///< 类内部名（会话符号池视图）
    mjbase::model::ClassResource* resource  = nullptr;
    std::shared_ptr<ClassSession> session;
    std::vector<CodeUnitRef>      methods;            ///< hasCode 方法的 promote 产物（保活）
};

/// 普查键：owner+name+desc 以 '\n' 连接后驻留 arena（去重），视图长期有效。
/// memberUseKey 的连接符一致；一次性拼接经 arena intern，自有存储归池。
std::string_view memberUseKey(ca::str::Utf8StringArena& arena, std::string_view owner,
                              std::string_view name, std::string_view desc)
{
    std::string joined;
    joined.reserve(owner.size() + name.size() + desc.size() + 2);
    joined.append(owner);
    joined.push_back('\n');
    joined.append(name);
    joined.push_back('\n');
    joined.append(desc);
    return arena.intern(std::string_view(joined));
}

/// @brief 装载并验证一个 class 资源；准入失败返回 false（资源保持原样）。
///
/// 准入条件：可解析、有类名；并加一条新底座
/// 必要的安全阀——全部有码方法必须能升格。普查证据不全时做收紧是不安全
/// 方向（漏记异类引用会误收紧），故任一方法升格失败即整类退出。
bool loadClass(mjbase::model::ClassResource& resource, LoadedClass& out)
{
    const auto& bytes = resource.get();
    if (bytes.empty()) return false;
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (sessionResult.is_err()) return false;
    out.session  = std::move(sessionResult).unwrap();
    out.resource = &resource;

    jbc::ir::ClassUnit& unit = out.session->unit();
    out.name                 = unit.symbols().str(unit.thisClass);
    if (out.name.empty()) return false;

    out.methods.reserve(unit.methods.size());
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (!jbc::ir::hasCode(*out.session, i)) continue;
        auto promoted = jbc::ir::promoteMethod(*out.session, i);
        if (promoted.is_err()) return false;
        out.methods.push_back(std::move(promoted).unwrap());
    }
    return true;
}

/// @brief 阶段一普查：扫描 kind 对应的引用操作码，异类来源记 usedOuter。
///
/// 普查面：方法表只记 INVOKESTATIC，字段表只记
/// GETSTATIC/PUTSTATIC；其余操作码（invokevirtual/special/interface、
/// getfield/putfield）不参与记账。
void censusClass(ca::str::Utf8StringArena&                              arena,
                 std::unordered_map<std::string_view, StaticMemberUse>& uses,
                 const LoadedClass&                                     loaded,
                 bool                                                   methodCensus)
{
    const std::string_view thisName = loaded.name;
    for (const CodeUnitRef& code : loaded.methods) {
        const std::vector<jbc::ir::InsnId> ids = code->dom.order();   // 快照防遍历失稳
        for (const jbc::ir::InsnId id : ids) {
            const jbc::ir::Insn* in = code->dom.peek(id);
            if (in == nullptr) continue;
            const bool isMethodRef = methodCensus && in->op == jbc::core::Op::INVOKESTATIC;
            const bool isFieldRef  = !methodCensus && (in->op == jbc::core::Op::GETSTATIC ||
                                                      in->op == jbc::core::Op::PUTSTATIC);
            if (!isMethodRef && !isFieldRef) continue;
            jbc::ir::ClassSession::MemberSem sem;
            if (!loaded.session->memberOf(*in, sem)) continue;   // 引用结构不符：不记账
            auto& use = uses[memberUseKey(arena, sem.owner, sem.name, sem.desc)];
            if (sem.owner != thisName) use.usedOuter = true;
        }
    }
}

/// @brief 阶段二收紧：普查命中且仅本类引用的 public static 成员降为 private。
///
/// 收紧条件：普查表有条目（至少被引用一次，含仅自类引用）
/// 且 !usedOuter 且当前 static 且当前 public。从未被引用的成员不动。
/// 改动同时落在会话骨架（unit().methods/fields，后续读者的一致视图）与
/// backing（MemberInfo.access，writeClass 的序列化源）。
bool tightenClass(ca::str::Utf8StringArena&                                    arena,
                  const std::unordered_map<std::string_view, StaticMemberUse>& uses,
                  LoadedClass&                                                 loaded)
{
    ClassSession&         session  = *loaded.session;
    jbc::ir::ClassUnit&   unit     = session.unit();
    jbc::core::ClassFile& cf       = session.backing();
    const std::string_view thisName = loaded.name;
    bool                  modified = false;

    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        jbc::ir::MethodUnit& mu    = unit.methods[i];
        const auto           it    = uses.find(memberUseKey(arena, thisName,
                                                           unit.symbols().str(mu.name),
                                                           unit.symbols().str(mu.desc)));
        if (it == uses.end() || it->second.usedOuter) continue;
        if ((mu.access & kAccStatic) != 0 && (mu.access & kAccPublic) != 0) {
            const ca::u16 tightened =
                static_cast<ca::u16>((mu.access & ~kAccPublic) | kAccPrivate);
            mu.access                              = tightened;
            cf.methods[mu.backingIndex].access     = tightened;
            modified                               = true;
        }
    }
    for (ca::usize i = 0; i < unit.fields.size(); ++i) {
        jbc::ir::FieldUnit& fu    = unit.fields[i];
        const auto          it    = uses.find(memberUseKey(arena, thisName,
                                                          unit.symbols().str(fu.name),
                                                          unit.symbols().str(fu.desc)));
        if (it == uses.end() || it->second.usedOuter) continue;
        if ((fu.access & kAccStatic) != 0 && (fu.access & kAccPublic) != 0) {
            const ca::u16 tightened =
                static_cast<ca::u16>((fu.access & ~kAccPublic) | kAccPrivate);
            fu.access                          = tightened;
            cf.fields[i].access                = tightened;
            modified                           = true;
        }
    }
    return modified;
}

}   // namespace

mjbase::spi::PreservedAnalyses AccessModifierTightener::run(mjbase::model::ResourceContainer&     container,
                                               mjbase::spi::GlobalAnalysisManager& am)
{
    // 分析中间串（普查键）驻留作业级 arena——各级 AM 共享同一 arena 的契约面。
    ca::str::Utf8StringArena& arena = am.arena();

    // 阶段零：装载与准入（不可解析/空/无类名/升格失败的资源跳过，原样保留）。
    std::vector<LoadedClass> loaded;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!resource ||
            !ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) {
            continue;
        }
        auto*       cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource.get());
        LoadedClass entry;
        if (loadClass(*cls, entry)) loaded.push_back(std::move(entry));
    }
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 装载后按类内部名字节序排序（确定性）。
    std::stable_sort(loaded.begin(), loaded.end(),
                     [](const LoadedClass& lhs, const LoadedClass& rhs) {
                         return lhs.name < rhs.name;
                     });

    // 阶段一：全容器普查（方法/字段各一张表，聚合序无关）。
    std::unordered_map<std::string_view, StaticMemberUse> methodUses;
    std::unordered_map<std::string_view, StaticMemberUse> fieldUses;
    for (const LoadedClass& entry : loaded) {
        censusClass(arena, methodUses, entry, /*methodCensus*/ true);
        censusClass(arena, fieldUses, entry, /*methodCensus*/ false);
    }

    // 阶段二：收紧 + 写回（两阶段提交：序列化推迟到循环结束统一落盘）。
    bool                             modifiedAny = false;
    mjbase::spi::ClassResourcePendingCommit pending;
    for (LoadedClass& entry : loaded) {
        // 方法表与字段表各收一轮（两结果都必须求出，不能短路）。
        const bool modifiedMethods = tightenClass(arena, methodUses, entry);
        const bool modifiedFields  = tightenClass(arena, fieldUses, entry);
        if (!modifiedMethods && !modifiedFields) continue;
        auto written = jbc::core::writeClass(entry.session->backing());
        if (written.is_err()) {
            throw std::runtime_error(std::string("AccessModifierTightener: write failed for '") +
                                     std::string(entry.name) + "': " +
                                     jbc::core::to_string(written.unwrap_err()));
        }
        pending.stage(entry.resource, std::move(written).unwrap());
        modifiedAny = true;
    }

    pending.commit();
    return modifiedAny ? mjbase::spi::PreservedAnalyses::none() : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
