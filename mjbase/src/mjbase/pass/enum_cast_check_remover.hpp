#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 移除枚举类首个方法中可证明冗余的 `INVOKEVIRTUAL`/`CHECKCAST` 相邻
///        指令对（deobfuscate/recover 的 opt-in 保守件，不进默认链）。
///
/// 语义：只处理枚举类（ACC_ENUM）的**首个方法**
/// （javac 枚举的 values()）；匹配 `INVOKEVIRTUAL` 且其调用描述符恰好等于
/// 当前枚举数组类型 `"[L<本类名>;"`，调用后一条指令就是 `CHECKCAST` 时，
/// 成对删除调用与 cast。不检查调用 owner、cast 目标与本类是否一致，不推断
/// 返回值用途与栈别名（有 cast 即视为去 cast 后返回值直接被
/// 下文消费）。同一方法内多个命中对全部成对删除；调用没有 cast、cast 隔有
/// 锚定标签（标签节点隔断物理相邻）时保持原样。
///
/// 口径注记：匹配面按「调用描述符字符串 == 数组类型」的字面口径
/// 保留（golden 测试即此口径：`INVOKEVIRTUAL java/lang/Enum.clone [LT;`）。真实
/// javac 字节码的方法描述符恒带 `(…)` 包裹，该形态不会自然出现，故本 pass
/// 在真实语料上保守走 no-op 面；golden 语料经手拼字节直接携带该形态
/// （classfile 层面合法——descriptor 是 CP 内任意 Utf8），读回/写回两侧均不
/// 做描述符文法校验。
///
/// 编辑器等价化差异（保守差集，有意保留）：删除收敛在 `eraseBatch` 全有
/// 全无批删内——分支目标被批删拒绝时（编辑器对跳转偏离改写面不入拒绝桶）
/// 调用位锚定的跳转按坐标改写自然落到幸存后继；
/// 异常表边界锚定这类 StillUsed 面被拒绝时本 pass 整体零动作返回 all()，
/// 不做该守卫。锚定面在真实语料与本批 golden 语料上均无
/// 自然出现点。
///
/// 变换规则：未命中字节码不变返回 all()；命中成对删除返回 none()。
///
/// @par 标签
/// - `scope:method`（类级条件全部方法局部化：ACC_ENUM +
///   会话首方法位）
/// - `action:eliminate`
/// - `stability:opt-in`
class EnumCastCheckRemover : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&            scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
