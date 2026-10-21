#include "mjbase/pass/enum_cast_check_remover.hpp"

#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::ir::ClassSession;
using jbc::ir::CodeDom;
using jbc::ir::CodeEditor;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// access_flags 的 ACC_ENUM 位（libjbc 不定义命名常量，按 u16 位面直用）。
constexpr ca::u16 kAccEnum = 0x4000;

/// 调用描述符 == 枚举数组类型的旧栈字面口径：`"[L" + 本类名 + ";"`。
std::string enumArrayDesc(const jbc::ir::ClassSession& session)
{
    std::string desc = "[L";
    desc += session.backing().thisName();
    desc += ';';
    return desc;
}

}   // namespace

mjbase::spi::PreservedAnalyses EnumCastCheckRemover::run(mjbase::spi::MethodScope&            scope,
                                                         mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    const jbc::ir::ClassSession& session = scope.session;
    if ((session.backing().access & kAccEnum) == 0)
        return mjbase::spi::PreservedAnalyses::all();

    // 旧栈只进「首个方法」（javac 枚举的 values()）；驱动面按会话位序核对。
    const jbc::ir::ClassUnit& unit = session.unit();
    if (unit.methods.empty() || !(unit.methods.front().id == scope.method))
        return mjbase::spi::PreservedAnalyses::all();

    CodeDom&                  dom = scope.code.dom;
    const std::vector<InsnId> ids = dom.order();
    if (ids.size() < 2)
        return mjbase::spi::PreservedAnalyses::all();

    const std::string   desc     = enumArrayDesc(session);
    bool                modified = false;
    std::vector<InsnId> toRemove;

    // 与旧栈一致的扫描面：物理相邻（dom 顺序位直邻 == ）成对收桶。
    for (ca::usize i = 0; i + 1 < ids.size(); ++i) {
        const Insn* call = dom.peek(ids[i]);
        if (call == nullptr || call->op != jbc::core::Op::INVOKEVIRTUAL) continue;
        jbc::ir::ClassSession::MemberSem sem;
        if (!session.memberOf(*call, sem)
            || sem.kind != jbc::ir::ClassSession::MemberSem::Kind::Method || sem.desc != desc)
            continue;
        const Insn* cast = dom.peek(ids[i + 1]);
        if (cast == nullptr || cast->op != jbc::core::Op::CHECKCAST) continue;
        // cast 站点被锚定即旧栈「标签隔断物理相邻」面：不匹配（成对收不进）。
        if (method_rewrite_util::labelsAnchorAt(dom, ids[i + 1])) continue;
        toRemove.push_back(ids[i]);
        toRemove.push_back(ids[i + 1]);
        modified = true;
        ++i;   // 成对的 cast 位不再二次候选，与旧栈 getNext 消费序一致
    }
    if (!modified)
        return mjbase::spi::PreservedAnalyses::all();

    CodeEditor editor(scope.code);
    if (!editor.eraseBatch(toRemove, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed).is_ok())
        return mjbase::spi::PreservedAnalyses::all();   // 锚定 use 拒绝即整 pass 零动作
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
