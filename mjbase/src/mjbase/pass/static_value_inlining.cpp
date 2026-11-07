#include "mjbase/pass/static_value_inlining.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/pass/static_value_collection.hpp"
#include "mjbase/spi/class_resource_pending_commit.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace mjbase::pass {
namespace {

using jbc::core::Op;

constexpr ca::u16 kAccVolatile = 0x0040;
/// CP 容量红线（与 ConstantFolder 同款 0xFFFF 口径；物化至多追加双槽条目
/// 一条，预检按最坏 +2 预留）。
constexpr ca::usize kCpCapacityLimit = 0xFFFF;

/// 装载完成的类：会话 + 写回目标 + 名字索引。
struct LoadedClass
{
    std::string                            name;
    mjbase::model::ClassResource*          resource = nullptr;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 容器内全部 class 资源可解析性预检 + 装载：任一空 bytes / 不可解析 /
/// 无类名返回 false（视图消费的一致性前提：改写是全容器一次性提交，
/// 不做部分容器）。
bool loadAllClasses(mjbase::model::ResourceContainer& container,
                    std::vector<LoadedClass>&         out)
{
    for (auto& [uri, resource] : container.getResources()) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr) continue;
        if (cls->get().empty()) return false;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) return false;
        LoadedClass entry;
        entry.session = std::move(sessionResult).unwrap();
        entry.name.assign(entry.session->backing().thisName());
        if (entry.name.empty()) return false;
        entry.resource =
            const_cast<mjbase::model::ClassResource*>(cls);
        out.push_back(std::move(entry));
    }
    return true;
}

/// 视图值 → 替换装载指令（物化进读点所属会话的 CP；容量预检在调用方）。
/// 类别匹配已在调用方校验，此处按值类别直接选物化面。
jbc::ir::Insn materialize(jbc::ir::ClassSession& session, const StaticValue& v)
{
    switch (v.kind) {
    case StaticValue::Kind::Int:
        return method_rewrite_util::materializeIntConst(
            session, static_cast<ca::i32>(v.num));
    case StaticValue::Kind::Long:
        return method_rewrite_util::materializeLongConst(session, v.num);
    case StaticValue::Kind::Float: {
        ca::f32 f = 0.0f;
        static_assert(sizeof(f) == 4, "float 位图宽度");
        const ca::u32 bits = static_cast<ca::u32>(v.bits);
        std::memcpy(&f, &bits, sizeof(f));
        return method_rewrite_util::materializeFloatConst(session, f);
    }
    case StaticValue::Kind::Double: {
        ca::f64 d = 0.0;
        static_assert(sizeof(d) == 8, "double 位图宽度");
        std::memcpy(&d, &v.bits, sizeof(d));
        return method_rewrite_util::materializeDoubleConst(session, d);
    }
    case StaticValue::Kind::Str:
        return jbc::ir::CodeEditor::loadConst(session.internString(v.str));
    }
    return jbc::ir::CodeEditor::simple(Op::NOP);   // 不可达（枚举全覆盖）
}

/// owner 类的 volatile 字段二次复核（物化件侧双保险桶）：owner 不在容器
/// 或字段找不到（复核证据不可得）一律按 volatile 处理——保留读点。
bool fieldIsVolatile(std::string_view owner, std::string_view name, std::string_view desc,
                     const std::unordered_map<std::string, LoadedClass*>& byName)
{
    const auto it = byName.find(std::string(owner));
    if (it == byName.end()) return true;   // owner 不在容器：复核不可得，保守保留
    jbc::ir::ClassUnit& unit = it->second->session->unit();
    const ca::usize     idx  = unit.findField(name, desc);
    if (idx == jbc::ir::kNoField) return true;
    return (unit.fields[idx].access & kAccVolatile) != 0;
}

}   // namespace

mjbase::spi::PreservedAnalyses StaticValueInlining::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    // 视图消费面：链上无收集件（或未发布任何内容）→ 严格 no-op（独立启停）。
    const StaticValueView* view = am.getPinned<StaticValueViewAnalysis>();
    if (view == nullptr || view->empty()) return mjbase::spi::PreservedAnalyses::all();

    std::vector<LoadedClass> loaded;
    if (!loadAllClasses(container, loaded)) return mjbase::spi::PreservedAnalyses::all();
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    std::unordered_map<std::string, LoadedClass*> byName;
    for (LoadedClass& lc : loaded) byName.emplace(lc.name, &lc);

    // 帧再生底盘：层次图自容器摘要建（分支体 Strict 重算的公共父类求解），
    // 与 MethodCallRemover 同款。
    mjbase::KeyPool                   pool;
    mjbase::hierarchy::ClassHierarchy hierarchy{pool};
    for (const LoadedClass& lc : loaded) {
        if (hierarchy.findClass(lc.name).valid()) continue;
        auto added = hierarchy.addClass(mjbase::hierarchy::fillSummary(*lc.session, pool));
        if (added.is_err()) return mjbase::spi::PreservedAnalyses::all();
    }
    mjbase::spi::CommonSuperClassResolver  superResolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{superResolver};

    mjbase::spi::ClassResourcePendingCommit pending;
    for (LoadedClass& lc : loaded) {
        jbc::ir::ClassSession& session = *lc.session;
        jbc::ir::ClassUnit&    unit    = session.unit();
        bool                   classModified = false;

        for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
            const jbc::ir::MethodUnit& mu = unit.methods[idx];
            // 初始化语义面：clinit/<init> 内读点跳过（发布类 clinit 无
            // invoke 形态，同类闭包坍缩为 clinit 自身，见头注）。
            const std::string_view mname = unit.symbols().str(mu.name);
            if (mname == "<clinit>" || mname == "<init>") continue;
            if (!jbc::ir::hasCode(session, idx)) continue;
            auto promoted = jbc::ir::promoteMethod(session, idx);
            if (promoted.is_err()) continue;   // 升格失败的方法保守不动
            auto               cuRef = std::move(promoted).unwrap();
            jbc::ir::CodeUnit& cu    = *cuRef.get();

            bool methodModified = false;
            for (const jbc::ir::InsnId id : cu.dom.order()) {
                const jbc::ir::Insn* in = cu.dom.peek(id);
                if (in == nullptr || in->op != Op::GETSTATIC) continue;
                jbc::ir::ClassSession::MemberSem ref;
                if (!session.memberOf(*in, ref) ||
                    ref.kind != jbc::ir::ClassSession::MemberSem::Kind::Field) {
                    continue;
                }
                const StaticValue* value = view->find(ref.owner, ref.name, ref.desc);
                if (value == nullptr) continue;   // 视图未命中：读点保留
                // 类别匹配（cat1/cat2 防御桶）。
                if (!staticValueFamilyMatches(ref.desc, value->kind)) continue;
                // volatile 复核（内存语义桶双保险）。
                if (fieldIsVolatile(ref.owner, ref.name, ref.desc, byName)) continue;
                // CP 容量红线（物化至多 +2 槽）。
                if (session.backing().cp.size() + 2 > kCpCapacityLimit) continue;

                auto got = cu.dom.get(id);
                if (got.is_err()) continue;
                *std::move(got).unwrap() = materialize(session, *value);
                methodModified           = true;
            }
            if (!methodModified) continue;

            // ── 写回：嵌套属性布局适配 → lower → 分支体 Strict 帧再生 ──
            mjbase::spi::frame_rewire::prepareNestedAttrsForLayout(session, cu);
            auto lowered = jbc::ir::lowerMethod(session, mu.id);
            if (lowered.is_err()) {
                throw std::runtime_error("StaticValueInlining: lower failed for '" + lc.name +
                                         "." + std::string(mname) + "': " +
                                         jbc::ir::to_string(lowered.unwrap_err()));
            }
            bool needsFrames = !cu.exc.empty();
            if (!needsFrames) {
                const ca::u32 branchBit = 1u << static_cast<ca::u32>(jbc::ir::InsnFamily::Branch);
                for (const jbc::ir::InsnId id : cu.dom.order()) {
                    const jbc::ir::Insn* in = cu.dom.peek(id);
                    if (in != nullptr && (jbc::ir::insnFamilyBit(in->op) & branchBit) != 0) {
                        needsFrames = true;
                        break;
                    }
                }
            }
            if (needsFrames) {
                const mjbase::ClassKey ownerKey = mjbase::classKeyOf(pool, lc.name);
                const mjbase::MemberKey key     = mjbase::memberKeyOf(
                    pool, ownerKey, mname, unit.symbols().str(mu.desc));
                auto scope = std::make_unique<mjbase::spi::MethodScope>(mjbase::spi::MethodScope{
                    mjbase::spi::MethodTarget{key, mu.id, mname,
                                              unit.symbols().str(mu.desc), mu.access, session},
                    cu});
                mjbase::spi::frame_rewire::rewireFramesAfterLower(session, *scope, typeResolver);
            }
            classModified = true;
        }

        if (!classModified) continue;
        auto written = jbc::core::writeClass(session.backing());
        if (written.is_err()) {
            throw std::runtime_error("StaticValueInlining: write failed for '" + lc.name + "'");
        }
        pending.stage(lc.resource, std::move(written).unwrap());
    }

    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();
    pending.commit();
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
