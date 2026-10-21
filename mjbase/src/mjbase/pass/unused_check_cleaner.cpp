#include "mjbase/pass/unused_check_cleaner.hpp"

#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::ir::ClassSession;
using jbc::ir::Insn;

/// INVOKESTATIC java/util/Objects.requireNonNull (Ljava/lang/Object;)Ljava/lang/Object;
bool isRequireNonNullProbe(const ClassSession& session, const Insn& in)
{
    if (in.op != jbc::core::Op::INVOKESTATIC) return false;
    ClassSession::MemberSem sem;
    if (!session.memberOf(in, sem)) return false;
    if (sem.kind != ClassSession::MemberSem::Kind::Method) return false;
    return sem.owner == "java/util/Objects" && sem.name == "requireNonNull"
           && sem.desc == "(Ljava/lang/Object;)Ljava/lang/Object;";
}

/// 成员非末条（fallthrough 删除需要后继承接引用）。
bool hasSuccessor(const jbc::ir::CodeDom& dom, jbc::ir::InsnId id)
{
    const std::vector<jbc::ir::InsnId>& order = dom.order();
    return !order.empty() && order.back() != id;
}

}   // namespace

mjbase::spi::PreservedAnalyses UnusedCheckCleaner::run(mjbase::spi::MethodScope&       scope,
                                                       mjbase::spi::MethodAnalysisManager& mam)
{
    // 静态方法无 this 隐式非空前提，整方法不参与（对齐）。
    if ((scope.access & 0x0008) != 0) {   // ACC_STATIC
        return mjbase::spi::PreservedAnalyses::all();
    }

    const jbc::ir::ClassSession& session = scope.session;
    jbc::ir::CodeDom&            dom     = scope.code.dom;
    jbc::ir::CodeEditor          editor(scope.code);

    const std::vector<jbc::ir::InsnId> ids = dom.order();
    bool                               modified = false;
    for (ca::usize i = 0; i + 2 < ids.size();) {
        const jbc::ir::Insn* aload = dom.peek(ids[i]);
        const jbc::ir::Insn* probe = dom.peek(ids[i + 1]);
        const jbc::ir::Insn* pop   = dom.peek(ids[i + 2]);
        if (aload == nullptr || probe == nullptr || pop == nullptr) {
            ++i;
            continue;
        }
        // this 装载的两种编码都认：专用短码 ALOAD_0 与通用 ALOAD a=0
        // （libjbc 保留 _0 短码原样，不归一化成 ALOAD）。
        const bool isThisLoad = aload->op == jbc::core::Op::ALOAD_0
                                || (aload->op == jbc::core::Op::ALOAD && aload->a == 0);
        if (!isThisLoad || !isRequireNonNullProbe(session, *probe)
            || pop->op != jbc::core::Op::POP)
        {
            ++i;
            continue;
        }
        // 边界守卫：第 2/3 成员锚定标签 → 跳过。
        if (method_rewrite_util::labelsAnchorAt(dom, ids[i + 1])
            || method_rewrite_util::labelsAnchorAt(dom, ids[i + 2]))
        {
            ++i;
            continue;
        }
        // 成员非末条前置检查（保证 fallthrough 删除链每步都有后继）。
        if (!hasSuccessor(dom, ids[i + 2])) {
            ++i;
            continue;
        }
        // 顺序删除：引用沿 aload → probe → pop → 组后继逐级重定向。匹配面
        // 已保证三步都可删除；意外失败即停止本方法（已删除部分保留，失败
        // 语义归管线容错档位）。
        bool aborted = false;
        for (const ca::usize k : {ca::usize{0}, ca::usize{1}, ca::usize{2}}) {
            if (editor.erase(ids[i + k],
                             jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                    .is_err())
            {
                aborted = true;
                break;
            }
            modified = true;
        }
        if (aborted) break;
        i += 3;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
