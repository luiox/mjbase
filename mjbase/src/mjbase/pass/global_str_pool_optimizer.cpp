#include "mjbase/pass/global_str_pool_optimizer.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;

// 中性 helper 名：不携带任何工具身份（旧栈类名携带来源仓身份，公开面禁现）。
constexpr std::string_view kPoolClass = "str/Pool";
constexpr std::string_view kPoolUri   = "str/Pool.class";
constexpr std::string_view kPoolField = "strPool";
constexpr std::string_view kArrayDesc = "[Ljava/lang/String;";

/// 一个已解析类资源：URI + 会话（普查与改写共用同一会话）。
struct LoadedClass
{
    std::string                            uri;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 全量普查：逐类资源建会话并收集 String ldc（LDC/LDC_W）去重编号。map
/// 承载使 <clinit> 写出序为字典序；索引按遭遇序编号（对齐旧栈
/// stringIndex.size() 语义，数组槽位 = 索引值）。任一类不可解析或任一
/// 方法 promote 失败 → nullopt（整件放弃，fail-closed）；非 class 资源
/// 忽略；池类自身跳过。
std::optional<std::map<std::string, ca::u32>> collect(
    mjbase::model::ResourceContainer& container,
    std::vector<LoadedClass>&         loaded)
{
    std::map<std::string, ca::u32> index;
    for (auto& [uri, res] : container.getResources()) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(res.get());
        if (cls == nullptr) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) return std::nullopt;
        auto session = std::move(sessionResult).unwrap();
        if (session->backing().thisName() == kPoolClass) continue;
        jbc::ir::ClassUnit& unit = session->unit();
        for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
            if (!jbc::ir::hasCode(*session, idx)) continue;
            auto promoted = jbc::ir::promoteMethod(*session, idx);
            if (promoted.is_err()) return std::nullopt;
            const jbc::ir::CodeUnit& code = *promoted.unwrap().get();
            for (const jbc::ir::InsnId id : code.dom.order()) {
                const jbc::ir::Insn* in = code.dom.peek(id);
                if (in == nullptr) continue;
                std::string_view value;
                if (!method_rewrite_util::stringValueOf(*session, *in, value)) continue;
                index.emplace(std::string{value}, static_cast<ca::u32>(index.size()));
            }
        }
        loaded.push_back({std::string{uri}, std::move(session)});
    }
    return index;
}

/// 改写单方法：命中的 String ldc 原位改写为索引装载（materializeIntConst
/// 波段），紧随插入 INVOKESTATIC str/Pool.getStr(I)——对是净栈中性替换，
/// 分支目标帧不漂移，无需帧再生；方法整体 maxStack 保守 +1（每处改写点位
/// 中间深度 +1，多点改写不叠乘——任一点位不超旧 max+1）。调用点插入失败
/// 回滚半截改写（索引装载落回原 ldc）。
bool rewriteMethod(jbc::ir::ClassSession& session, jbc::ir::CodeUnit& code,
                   const std::map<std::string, ca::u32>& index)
{
    const ca::u32 mref =
        session.internMethodRef(kPoolClass, "getStr", "(I)Ljava/lang/String;");
    jbc::ir::CodeEditor                editor(code);
    const std::vector<jbc::ir::InsnId> ids = code.dom.order();
    bool                               modified = false;
    for (const jbc::ir::InsnId id : ids) {
        const jbc::ir::Insn* in = code.dom.peek(id);
        if (in == nullptr) continue;
        std::string_view value;
        if (!method_rewrite_util::stringValueOf(session, *in, value)) continue;
        const auto it = index.find(std::string{value});
        if (it == index.end()) continue;
        auto got = code.dom.get(id);
        if (got.is_err()) continue;
        jbc::ir::Insn*          slot     = std::move(got).unwrap();
        const jbc::ir::Insn     original = *slot;
        const jbc::ir::Insn intInsn =
            method_rewrite_util::materializeIntConst(session,
                                                     static_cast<ca::i32>(it->second));
        const ca::u32 oldSrcPc = slot->srcPc;
        *slot                  = intInsn;
        slot->srcPc            = oldSrcPc;
        if (editor
                .insertAfter(id,
                             jbc::ir::CodeEditor::cpInsn(Op::INVOKESTATIC, mref))
                .is_err()) {
            *slot = original;   // 回滚半截改写：索引装载落回原 ldc
            continue;
        }
        modified = true;
    }
    if (modified) {
        code.maxStack = static_cast<ca::u16>(code.maxStack + 1);
    }
    return modified;
}

/// 合成池 helper 类字节：str/Pool —— public static String[] strPool 字段 +
/// <init> + <clinit>（new String[n] + 逐项 aastore）+ private static
/// getStr(I)。全合成模型 makeSession(NewClassSeed) → addMethod →
/// promote/append/lower。maxStack 口径：<init> 1（aload+invokespecial）、
/// <clinit> 3（getstatic+idx+ldc 中间深度）、getStr 2（getstatic+iload）。
std::optional<std::vector<ca::u8>> buildPoolClass(const std::map<std::string, ca::u32>& index)
{
    jbc::ir::NewClassSeed seed;
    seed.thisName  = kPoolClass;
    seed.superName = "java/lang/Object";
    auto sessionResult = jbc::ir::makeSession(seed);
    if (sessionResult.is_err()) return std::nullopt;
    auto session = std::move(sessionResult).unwrap();

    {
        const jbc::ir::FieldSeed f{kPoolField, kArrayDesc, 0x0009};   // public static
        if (session->addField(f).is_err()) return std::nullopt;
    }

    const auto appendBody = [&](jbc::ir::MethodId mid,
                                const std::vector<jbc::ir::Insn>& body) -> bool {
        auto promoted = jbc::ir::promoteMethod(*session, mid);
        if (promoted.is_err()) return false;
        jbc::ir::CodeEditor editor(*promoted.unwrap().get());
        for (const jbc::ir::Insn& in : body) {
            if (editor.append(in).is_err()) return false;
        }
        return jbc::ir::lowerMethod(*session, mid).is_ok();
    };

    {
        jbc::ir::MethodSeed m;
        m.name      = "<init>";
        m.desc      = "()V";
        m.access    = 0x0001;   // public
        m.withCode  = true;
        m.maxStack  = 1;
        m.maxLocals = 1;
        auto added  = session->addMethod(m);
        if (added.is_err()) return std::nullopt;
        const jbc::ir::MethodId mid = added.unwrap();
        const ca::u32 objInit =
            session->internMethodRef("java/lang/Object", "<init>", "()V");
        if (!appendBody(mid,
                        {jbc::ir::CodeEditor::local(Op::ALOAD, 0),
                         jbc::ir::CodeEditor::cpInsn(Op::INVOKESPECIAL, objInit),
                         jbc::ir::CodeEditor::simple(Op::RETURN)})) {
            return std::nullopt;
        }
    }
    {
        jbc::ir::MethodSeed m;
        m.name      = "<clinit>";
        m.desc      = "()V";
        m.access    = 0x0008;   // static
        m.withCode  = true;
        m.maxStack  = 3;
        m.maxLocals = 0;
        auto added  = session->addMethod(m);
        if (added.is_err()) return std::nullopt;
        const jbc::ir::MethodId mid = added.unwrap();
        const ca::u32 strCls = session->internClass("java/lang/String");
        const ca::u32 fld    = session->internFieldRef(kPoolClass, kPoolField, kArrayDesc);
        std::vector<jbc::ir::Insn> body;
        body.push_back(method_rewrite_util::materializeIntConst(
            *session, static_cast<ca::i32>(index.size())));
        body.push_back(jbc::ir::CodeEditor::cpInsn(Op::ANEWARRAY, strCls));
        body.push_back(jbc::ir::CodeEditor::cpInsn(Op::PUTSTATIC, fld));
        for (const auto& [value, idx] : index) {
            body.push_back(jbc::ir::CodeEditor::cpInsn(Op::GETSTATIC, fld));
            body.push_back(method_rewrite_util::materializeIntConst(
                *session, static_cast<ca::i32>(idx)));
            body.push_back(
                jbc::ir::CodeEditor::loadConst(session->internString(value)));
            body.push_back(jbc::ir::CodeEditor::simple(Op::AASTORE));
        }
        body.push_back(jbc::ir::CodeEditor::simple(Op::RETURN));
        if (!appendBody(mid, body)) return std::nullopt;
    }
    {
        jbc::ir::MethodSeed m;
        m.name      = "getStr";
        m.desc      = "(I)Ljava/lang/String;";
        m.access    = 0x000a;   // private static
        m.withCode  = true;
        m.maxStack  = 2;
        m.maxLocals = 1;
        auto added  = session->addMethod(m);
        if (added.is_err()) return std::nullopt;
        const jbc::ir::MethodId mid = added.unwrap();
        const ca::u32 fld = session->internFieldRef(kPoolClass, kPoolField, kArrayDesc);
        if (!appendBody(mid,
                        {jbc::ir::CodeEditor::cpInsn(Op::GETSTATIC, fld),
                         jbc::ir::CodeEditor::local(Op::ILOAD, 0),
                         jbc::ir::CodeEditor::simple(Op::AALOAD),
                         jbc::ir::CodeEditor::simple(Op::ARETURN)})) {
            return std::nullopt;
        }
    }

    auto written = jbc::core::writeClass(session->backing());
    if (written.is_err()) return std::nullopt;
    return std::move(written).unwrap();
}

}   // namespace

mjbase::spi::PreservedAnalyses GlobalStrPoolOptimizer::run(
    mjbase::model::ResourceContainer&   container,
    mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    // 池类已存在 → 幂等跳过。
    if (container.getResource(kPoolUri) != nullptr) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    // 普查 + 会话装载（fail-closed：任一类不可解析整件放弃）。
    std::vector<LoadedClass>                      loaded;
    std::optional<std::map<std::string, ca::u32>> index = collect(container, loaded);
    if (!index.has_value() || index->empty()) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    // helper 字节先于任何容器变更合成（两阶段提交）。
    std::optional<std::vector<ca::u8>> poolBytes = buildPoolClass(*index);
    if (!poolBytes.has_value()) return mjbase::spi::PreservedAnalyses::all();

    // 改写批次：全部序列化成功才动容器。
    struct PendingWrite
    {
        std::string uri;
        std::vector<ca::u8> bytes;
    };
    std::vector<PendingWrite> pending;
    for (LoadedClass& entry : loaded) {
        jbc::ir::ClassUnit& unit = entry.session->unit();
        bool                touched = false;
        for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
            if (!jbc::ir::hasCode(*entry.session, idx)) continue;
            auto promoted = jbc::ir::promoteMethod(*entry.session, idx);
            if (promoted.is_err()) {
                return mjbase::spi::PreservedAnalyses::all();
            }
            jbc::ir::CodeUnit& code = *promoted.unwrap().get();
            if (!rewriteMethod(*entry.session, code, *index)) continue;
            if (jbc::ir::lowerMethod(*entry.session, idx).is_err()) {
                return mjbase::spi::PreservedAnalyses::all();
            }
            touched = true;
        }
        if (!touched) continue;
        auto written = jbc::core::writeClass(entry.session->backing());
        if (written.is_err()) return mjbase::spi::PreservedAnalyses::all();
        pending.push_back({entry.uri, std::move(written).unwrap()});
    }
    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();

    for (PendingWrite& item : pending) {
        container.replaceResource(
            item.uri,
            std::make_unique<mjbase::model::ClassResource>(item.uri,
                                                           std::move(item.bytes)));
    }
    container.replaceResource(kPoolUri,
                              std::make_unique<mjbase::model::ClassResource>(
                                  std::string{kPoolUri}, std::move(*poolBytes)));
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
