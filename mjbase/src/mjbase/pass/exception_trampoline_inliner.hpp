#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 异常跳板内联器：把异常流混淆器的「throw 跳板」还原成直落控制流。
///
/// ## 模式（异常驱动的强制跳转）
///
/// 混淆器把一条强制跳转编码成异常分派：保护区恰为 `aconst_null; athrow`
/// （恒抛 NullPointerException），catch 类型为 any，handler 紧贴保护区末尾
/// （end 与 handler 锚定同一指令），handler 首指令 `pop` 丢弃异常引用后进入
/// 真实代码。JVM 语义：athrow 清空操作数栈、压入异常引用、分派到 handler；
/// pop 还原为空栈——整体等价于「无条件跳转到 handler 体首指令，栈清空」。
/// 反编译器把每个保护区→handler 对渲染成 `try { throw null; } catch`，
/// 再与后续 goto 拼接出大量 `while(true){ … break; }` 伪循环。
//
/// ## 形态示例（JVMS 视角，合成值）
///
/// 异常流混淆批量生成的强制跳转编码（常量与标签均为合成示意值）：
///
///   exc: [start=S, end=H, handler=H, catch=any]
///   S:  aconst_null      ;; 压 null
///       athrow           ;; 恒抛 NPE → JVM 清栈、压异常引用、分派 handler
///   H:  pop              ;; 丢弃异常引用（还原空栈）——handler 头
///       ldc 111111111    ;; ↓ 真实代码：状态更新 + 转移
///       istore i1 / iload i1 / istore i2 / goto L
///
/// 消解后 S 体为空、直接落进 H 体；指向 S 的 goto 重定向到 H 体首指令。
/// 反编译伪影对比：
///   前：批量 try { throw null; } catch (Throwable) { … }，与状态机
///       goto 拼接成大量 while(true){ … break; } 伪循环（反编译器放弃
///       结构化）；
///   后：异常表清空（仅剩真 try/catch），伪影全部消失。
///
/// ## 在三层反混淆链中的位置
///
/// 该混淆通常三层叠加，本件只做第一层，后两层已立 issue：
///   1. 异常跳板层（本件）：表项消解，控制流还原为普通 goto 拓扑；
///   2. 不透明算术层（#44）：分发块恒等式族混淆算术折叠成 s + C；
///   3. 控制流扁平化层（#45）：常量状态传播 + switch 分发静态解析，
///      状态机还原直线。
/// 表项不清空，后两层的形态识别面（分发块前驱、goto 汇合点）就被
/// 异常边遮蔽，故本件是整链入口。
////// ## 改写

///
/// 命中表项整组消解：摘异常表项 + eraseBatch 删除 aconst_null/athrow/pop
/// 三条指令（kRetargetToFallthrough——指向 aconst_null 的分支重定向到
/// handler 体首指令，正是原语义的落点）。多跳板方法逐项独立成组。
///
/// ## 栈等价门
///
/// athrow 的清栈语义由直落替代后，入口处的非空栈会残留到 handler 体。
/// 帧引擎 BestEffort 形状档证明 aconst_null 的到达帧栈深为 0 才放行；
/// 不可达跳板不付改写（无到达帧）。
///
/// ## 放弃清单（逐项保守跳过，方法内其余跳板不受牵连）
///
/// - catch 类型非 any：NPE 可能被外层表项截获，首匹配语义不可拆。
/// - 保护区非恰「ACONST_NULL + ATHROW」两条（额外指令语义不明）。
/// - handler 不与 end 同锚（非紧贴形态，需物化 goto，后续切片）。
/// - handler 首指令非 POP，或 POP 为方法末条（无顺序后继可承接重定向）。
/// - 其他表项与本表项范围相交 / 共享 handler 头 / handler 锚入本保护区或
///   POP 位置：交叠保护下删表项会改变剩余表项的首匹配结果。
/// - ATHROW 或 POP 为分支目标：绕过 aconst_null 的路径到达时栈形与
///   异常分派不符（athrow 需要栈顶引用、pop 需要栈顶值），重定向后
///   残留槽位。指向 ACONST_NULL 的分支不受限（栈深门兜底）。
/// - 帧计算失败（ok=false）：形状数据流不可信，全方法放弃。
/// - 任一表项标签解析失败：交叠推理不可靠，全方法放弃。
///
/// ## 事务性与幂等
///
/// 每表项先摘表项再批量删除；eraseBatch 被拒（全有或全无）时表项原位
/// 回插，不落半改写。改写后方法无残余跳板形态，二轮无匹配返回 all()。
/// maxStack 只降不升，现值保持合法；NopRemover 不需要（无 NOP 残留）。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`（默认链不收，CLI -p 显式挂点）
class ExceptionTrampolineInliner : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
