#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 局部表达式形状折叠（legacy ExpressionSimplifier 的窗口规则移植）
///
/// 与 `FixPatternSimplifier` 的分工：本 pass 处理**可复用的代数表达式**，
/// 固定反混淆字节码模式仍由 FixPattern 负责。所有规则都是纯 peephole 窗口，
/// 匹配失败或标签落在窗口内部时保持原样。
///
/// ## 规则集（与旧 mbase pass/opt/expression_simplifier 对齐）
///
/// 恒等元：
/// - `loadInt; iconst_0; iadd|ior|ixor` -> `loadInt`
/// - `loadInt; iconst_1; imul` -> `loadInt`
/// - `loadInt; iconst_m1; iand` -> `loadInt`
/// - `loadInt; iconst_0; iand` -> `iconst_0`
/// - `iconst_m1; iand` / `iconst_0; ixor` -> 删除
///
/// 交换/结合：
/// - `swap; iadd|ixor|iand|ior` -> 对应二元指令
/// - `constInt; op; constInt; op` -> `fold(c1 op c2); op`（add/mul/or/xor/and）
///
/// 特殊表达式：
/// - `loadInt; dup; ixor` -> `iconst_0`
/// - `loadInt; iconst_1; ishl` -> `loadInt; iconst_2; imul`
/// - `ineg; ineg` / `lneg; lneg` -> 删除
/// - `iconst_m1; imul` -> `ineg`
/// - `iconst_m1; ixor; iconst_m1; isub` -> `ineg`
/// - `iconst_m1; ixor; iconst_1; iadd` -> `ineg`
/// - `iconst_m1; iadd; iconst_m1; ixor` -> `ineg`
/// - `ineg; isub` -> `iadd`
/// - `iconst_m1; ixor; isub; iconst_1; isub` -> `iadd`
/// - `ineg; iadd` -> `isub`
/// - 以及旧规则集中的 swap/dup2 位运算恒等式与
///   `(x | ~y) - ~y => x & y`、`x + (y & ~x) => x | y`
///
/// 旧规则 `iconst_m1; isub; iconst_m1; ixor -> ineg` 代数上不成立
/// （`~(x + 1) == -x - 2`），移植时丢弃；其余规则经栈语义穷举校验。
///
/// ## 写入边界
///
/// - 复用 `window_rule::replaceWindow`：窗口头标签迁移、内部标签拒配、
///   替换头继承首成员 srcPc、失败回滚。
/// - 只折叠常量或局部变量上的无副作用表达式，不改变异常路径。
/// - 每轮应用至不动点；changed -> `PreservedAnalyses::none()`。
///
/// @par 标签
/// - `scope:method`
/// - `action:simplify`
/// - `stability:opt-in`
class ExpressionSimplifier : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
