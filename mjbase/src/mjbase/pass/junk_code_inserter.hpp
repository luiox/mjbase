#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 确定性垃圾代码插入器：在方法体的安全边界插入栈中性的垃圾分支块
///        （控制流混淆的垃圾代码插入族；本批 B 组，A1 帧链的首批真实消费者）。
///
/// 语义参考旧栈混淆器线的垃圾代码类（方法体垃圾面 /
/// CaesiumControlFlowObf 的栈中性分块），按 mjbase 底座重设计：**确定性**
/// 插入（无 RNG——插入点与垃圾形态都由位置决定），使产物可复现、可入
/// golden 基线、可做字节级回归。
///
/// 垃圾块（5 指令，栈效应净零，不可抛）：
///
/// ```
/// ICONST_0
/// IFEQ L_end      // 恒跳转（运行时路径）；not-taken 臂供 verifier 类型检查
/// ICONST_1
/// POP
/// L_end:          // 新分支目标 → 需要 SMT 帧（A1 再生的验证面）
/// ```
///
/// 插入点选择（确定性）：原指令快照中每第 k 个位置（k=8，首位置除外）且
///   - 前驱指令是顺序 fallthrough（前驱不是 GOTO/ATHROW/return 族/RET——
///     无条件转移之后插入会产出死代码，verifier 跳过、帧再生无从验证）；
///   - 目标指令不是异常 handler 入口（handler 入口插桩会改写 handler 帧
///     语义的验证面，保守跳过）。
///
/// maxStack 防御性抬升到 ≥1（垃圾块占栈 1；旧栈同类 pass 同样就地抬
/// maxStack）。注入 resolver 的管线上，垃圾块的新分支目标由 A1 链再生
/// SMT 覆盖（Strict 帧 → buildStackMapTable → applyComputedMaxes）；未接
/// resolver 时新目标无帧，产出类会被 V50+ verifier 拒收——本 pass 为
/// `opt-in`，须与管线 typeResolver 联用。
///
/// 发生插入返回 none()，否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:obfuscate`
/// - `stability:opt-in`
class JunkCodeInserter : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
