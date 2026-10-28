#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief GOTO 重排器：把 GOTO 目标尾段整体搬移，使 GOTO 退化为可消除的
///        fallthrough 冗余跳转（对齐旧栈 mbase pass/opt/jump_label_layout 的
///        GotoRearranger 可观测面；标签布局族第二件，消费 libjbc 0.4.19
///        moveRange 区间搬移原语）。
///
/// 首件窗口（宁窄勿错）：方法内 GOTO/GOTO_W 前向跳过中间段 M、目标标签 L
/// 锚定的尾段 S = [L, 方法末)。用 moveRange（kHeadFollows）把 S 整体搬到
/// GOTO 之后，L 随段迁移锚定 GOTO 的新顺序后继，GOTO 的目标与紧邻后继
/// 重合，按家族同款 fallthrough 等价整删（RedundantJumpRemover 的边界
/// 形态）。可观测价值：无条件跳转次数 -1，方法尾布局改写（尾段提前、
/// M 段沉底）。
///
/// 保守门（每条可证伪）：
///   - 仅前向非邻接目标：邻接形态（目标 = 紧邻后继）是 RedundantJumpRemover
///     的既有窗口不重收；后向目标（回边）搬尾段不可达——消除回边需要逆转
///     语义，moveRange 只做重排，搬了跳转依旧。
///   - 含 JSR/RET 的方法整体不动：RET 的后继是栈上动态地址，前驱集无法在
///     指令枚举面上保守闭包（单前驱门会漏计子程序边）；Strict 帧重算对
///     jsr/ret 同样显式不可算。
///   - 异常表门：任一 entry 的 start/end/handler 锚定段内 [L, 末) 或锚定
///     GOTO 自身前置边界 → 拒动。跨段边界的保护区搬移后覆盖集改变（字节
///     语义可证改变）；erase 的 kRetargetToFallthrough 对异常边界引用一律
///     StillUsed——先验证后动手，把失败排除在搬移之前。
///   - 单前驱门：L 的前驱恰好只有该 GOTO。M 尾的 fallthrough（M 尾为非
///     终结指令）在 M 沉底后变成方法末悬空 fallthrough，verifier 拒载；
///     显式分支前驱虽由句柄锚定保边，首件把多前驱目标块的布局改写整体
///     排除（宁窄勿错）。
///   - 环形门：段内任一指令的控制目标（分支/switch 全部 target）锚定段外
///     → 拒动。段→前方的回边构成跨段循环，循环体跨段重排需逐边证明帧链
///     与布局效果，首件不证。
///
/// 策略：kHeadFollows——L 的语义就是「段入口」，必须随段迁移才能使 GOTO
/// 目标与新顺序后继重合；kHeadStays 会把入口标签留在原段位承接 M 的
/// fallthrough，与搬移意图相反，且引入 StillUsed 拒绝面。
///
/// 两步序列原子性：「搬块 + 删 GOTO」的全部失败条件（moveRange 的句柄/
/// 段几何，erase 的无顺序后继/异常边界引用）都在搬移前由门与形态排除；
/// moveRange 本身校验先于变更、原子落盘。防御层：erase 意外 Err 时不
/// 回滚——搬移后的布局仍是语义等价的合法形态，记改写重开快照即可，该
/// GOTO 的目标已变邻接、被本件窗口排除，无死循环。
///
/// 帧面：不做显式帧操作（与 RedundantJumpRemover 同口径）——管线 lower
/// 后 rewireFramesAfterLower 以 Strict 全量重算 + SMT 再生；本变换不引入
/// 新状态边（删 GOTO 换来的 fallthrough 与 GOTO 同为恒等传函），各分支
/// 目标的入边集与入边传函不变，帧语义不变。
///
/// 每轮快照全扫 + 外层不动点；每个接受候选净删 1 条指令，循环终止。
///
/// 发生改写返回 none()，否则 all()。maxStack 不升。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class GotoRearranger : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&         scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
