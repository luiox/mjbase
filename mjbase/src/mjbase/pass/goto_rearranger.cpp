#include "mjbase/pass/goto_rearranger.hpp"

#include <algorithm>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::core::Shape;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

bool isGotoOpT(Op op) noexcept
{
    return op == Op::GOTO || op == Op::GOTO_W;
}

/// 指令是否向顺序后继落空（fallthrough）：分支/switch/返回族/ATHROW/RET
/// 无落空边，其余一律有。
bool hasFallthrough(Op op) noexcept
{
    switch (op) {
    case Op::GOTO:
    case Op::GOTO_W:
    case Op::JSR:
    case Op::JSR_W:
    case Op::TABLESWITCH:
    case Op::LOOKUPSWITCH:
    case Op::ATHROW:
    case Op::RET:
    case Op::IRETURN:
    case Op::LRETURN:
    case Op::FRETURN:
    case Op::DRETURN:
    case Op::ARETURN:
    case Op::RETURN:
        return false;
    default:
        return true;
    }
}

/// 指令的控制目标标签（Branch2/Branch4 唯一目标；switch default + 全部
/// case）。无控制流的指令产出空集。
void collectControlTargets(const jbc::ir::Insn& in, std::vector<LabelId>& out)
{
    const Shape sh = jbc::core::shapeOf(in.op);
    if (sh == Shape::Branch2 || sh == Shape::Branch4) {
        if (in.target != jbc::ir::kNoLabel) out.push_back(in.target);
        return;
    }
    if (sh == Shape::TableSwitch || sh == Shape::LookupSwitch) {
        if (in.target != jbc::ir::kNoLabel) out.push_back(in.target);
        for (const LabelId t : in.targets) out.push_back(t);
    }
}

/// 句柄在快照 order 中的位置；不存在返回 order.size()（哨兵，参与区间
/// 运算安全）。
ca::usize posOfInsn(const std::vector<InsnId>& order, InsnId id)
{
    return static_cast<ca::usize>(std::find(order.begin(), order.end(), id)
                                  - order.begin());
}

/// 标签解析位置：End 端点折叠为 order.size()（方法末哨兵位）；解析失败
/// 返回 false（调用方一律按「无法保守枚举」拒动）。
bool resolvePos(const jbc::ir::CodeDom& dom, const std::vector<InsnId>& order,
                LabelId label, ca::usize& pos)
{
    if (label == jbc::ir::kNoLabel) return false;
    auto resolved = dom.resolveLabel(label);
    if (resolved.is_err()) return false;
    const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
    pos = t.isEnd() ? order.size() : posOfInsn(order, t.insn);
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses GotoRearranger::run(mjbase::spi::MethodScope&         scope,
                                                   mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit&  unit  = scope.code;
    jbc::ir::CodeDom&   dom   = unit.dom;
    jbc::ir::CodeEditor editor(unit);

    // JSR/RET 门：RET 后继是栈上动态地址，前驱集不可保守闭包，方法整体
    // 不动（Strict 帧重算对子程序同样不可算）。
    for (const InsnId id : dom.order()) {
        const jbc::ir::Insn* in = dom.peek(id);
        if (in != nullptr && (in->op == Op::JSR || in->op == Op::JSR_W || in->op == Op::RET)) {
            return mjbase::spi::PreservedAnalyses::all();
        }
    }

    bool modified  = false;
    bool anyChange = true;
    while (anyChange) {
        anyChange = false;
        // 每轮快照：改写只发生在候选点，快照句柄全程有效。
        const std::vector<InsnId> ids = dom.order();
        const ca::usize           n   = ids.size();
        for (ca::usize i = 0; i < n; ++i) {
            const jbc::ir::Insn* jump = dom.peek(ids[i]);
            if (jump == nullptr || !isGotoOpT(jump->op)) continue;

            // 门 1（前向非邻接）：目标解析到指令位置 p，要求 p >= i + 2。
            ca::usize p = 0;
            if (!resolvePos(dom, ids, jump->target, p)) continue;
            if (p >= n || p < i + 2) continue;

            // 门 2（异常表）：任一 entry 的 start/end/handler 锚定段内
            // [p, n) 或锚定 GOTO 自身（i）→ 拒动。End 端点（方法末）不算
            // 段内——「从段外到方法末」的保护区搬移前后覆盖同一指令集；
            // start 解析失败/End 视为畸形拒动。
            bool excBlocked = false;
            for (const jbc::ir::CodeUnit::Exc& e : unit.exc) {
                for (const LabelId boundary : {e.start, e.end, e.handler}) {
                    ca::usize pos = 0;
                    if (!resolvePos(dom, ids, boundary, pos)) {
                        excBlocked = true;
                        break;
                    }
                    if ((pos >= p && pos < n) || pos == i) {
                        excBlocked = true;
                        break;
                    }
                }
                if (excBlocked) break;
            }
            if (excBlocked) continue;

            // 门 3（单前驱）：除本 GOTO 外，任何指令的直接后继（fallthrough
            // + 控制目标）覆盖 p → 拒动。控制目标无法保守枚举（解析失败）
            // 同样拒动。
            bool multiPred = false;
            for (ca::usize j = 0; j < n && !multiPred; ++j) {
                if (j == i) continue;
                const jbc::ir::Insn* in = dom.peek(ids[j]);
                if (in == nullptr) continue;
                if (hasFallthrough(in->op) && j + 1 < n && j + 1 == p) {
                    multiPred = true;
                    break;
                }
                std::vector<LabelId> targets;
                collectControlTargets(*in, targets);
                for (const LabelId t : targets) {
                    ca::usize pos = 0;
                    if (!resolvePos(dom, ids, t, pos) || pos == p) {
                        multiPred = true;
                        break;
                    }
                }
            }
            if (multiPred) continue;

            // 门 4（环形结构）：段内任一指令的控制目标锚定段外（位置 < p，
            // 含 End 哨兵位与方法外解析失败）→ 拒动。
            bool cyclic = false;
            for (ca::usize j = p; j < n && !cyclic; ++j) {
                const jbc::ir::Insn* in = dom.peek(ids[j]);
                if (in == nullptr) continue;
                std::vector<LabelId> targets;
                collectControlTargets(*in, targets);
                for (const LabelId t : targets) {
                    ca::usize pos = 0;
                    if (!resolvePos(dom, ids, t, pos) || pos < p) {
                        cyclic = true;
                        break;
                    }
                }
            }
            if (cyclic) continue;

            // 变换：尾段 [p, n) 整体搬到 GOTO 之后（kHeadFollows——段首
            // 入口标签 L 随段迁移），GOTO 目标与新顺序后继重合，按
            // fallthrough 等价整删。门前已排除 erase 的全部失败条件
            // （后继存在 + 异常边界不引用 GOTO 边界）；moveRange 原子，
            // Err 为理论不可达的防御路径。
            const auto moved = editor.moveRange(ids[p], jbc::ir::kNoInsn, ids[i + 1],
                                                jbc::ir::CodeEditor::MoveRangePolicy::kHeadFollows);
            if (moved.is_err()) break;
            const auto erased = editor.erase(
                ids[i], jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            if (erased.is_err()) {
                // 防御路径：搬移已落（语义等价的合法布局），不回滚；该
                // GOTO 目标已变邻接、被门 1 排除，重开快照无死循环。
                modified  = true;
                anyChange = true;
                break;
            }
            modified  = true;
            anyChange = true;
            break;   // 重开一轮：改写暴露的新形态由下一轮快照接续
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
