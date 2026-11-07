#pragma once

#include <memory>
#include <vector>

#include "mjbase/spi/indy_resolver.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief indy 站点内联：消费 @ref mjbase::spi::IndyResolver "IndyResolver"
///        产出的直接目标，把混淆器家族的 invokedynamic 站点改写为直接成员
///        调用（opt-in，不进默认链；无 resolver 构造 = 严格 no-op）。
///
/// 机制借鉴 Recaf 的 InvokeDynamicInliningTransformer（只读参考），接口按
/// mjbase 惯例重塑。SPI 契约与改写判据矩阵见 doc:indy-inline-spec；微观
/// 契约（判据逐条、栈/帧不变量）在本头注，RST 不复制。
///
/// ## 站点准入与排除
///
/// 只考虑 `invokedynamic` 站点；家族判定消费 `asm_classpath` 的 BSM 解析面：
/// - `LambdaMetafactory` / `StringConcatFactory`：生成面语义，一律不动
///   （resolver 不被询问，站点字节原样保留）；
/// - 其余家族（含 Unknown）：候选站点，逐站点喂 resolver。
///
/// ## 改写判据矩阵（resolver 目标 → 直接调用；逐门全过才落笔）
///
/// 设站点描述符参数段为 Ps、返回段 Rs，目标描述符参数段 Pt、返回段 Rt，
/// `trailingMetadataArgs = k`（Ps 尾部 k 个实参为 BSM 元数据，改写时丢弃）：
///
/// 1. `k <= |Ps|`；
/// 2. represented = Ps 去尾 k 个；按形态分派：
///    - Static：`|represented| == |Pt|` 且逐参数描述符等形，且 `Rs == Rt`；
///    - Virtual/Interface：`|represented| == |Pt| + 1`，首参数为非数组类形
///      且内部名 == 目标 owner（接收者），余参数与 Pt 逐等形，且 `Rs == Rt`；
/// 3. 落笔形态：Static/Virtual → `INVOKESTATIC`/`INVOKEVIRTUAL` +
///    Methodref；Interface → `INVOKEINTERFACE` + InterfaceMethodref（count
///    按目标描述符重算，intern 面 libjbc 自理）。
///
/// 描述符等形 = 结构逐位同（kind/primitive/类内部名/数组维数），不做任何
/// 装箱/拆箱/加宽/checkcast 适配——形态不合即拒，适配器是后续组合件。
/// `invokespecial`（super 调用/构造器）与字段句柄族不在 v1 面：resolver
/// 契约表达不出（Form 三态），结构性 fail-closed。
///
/// ## 改写落笔
///
/// - `k == 0`：原位改写（opcode + CP 引用），无插入；站点指令保留原位，
///   标签锚定与帧边界不受影响，Code 长度差由 lower 重定位统一消化。
/// - `k > 0`：实参暂存洗牌——全部站点实参按逆参序存入 fresh 局部（槽号
///   自 `maxLocals` 起、槽宽按描述符），再按 represented 正参序装载回栈，
///   然后原位改写调用指令；`maxLocals` 相应抬升，maxStack 不抬（洗牌峰值
///   不超过原站点全参数在栈的峰值）。插入改变站点前置边界形态：站点指令
///   上锚定有标签（分支目标/异常表/调试边界）时拒绝该站点（保守保留）。
/// - 栈/帧不变量：改写前后站点弹推形状一致（弹 represented、压 Rs），洗
///   形只发生在站点前缘之内，StackMapTable 逐点不动；fresh 局部不进
///   LocalVariableTable（调试面可缺，验证面不查）。
///
/// ## 放弃清单（保守保留，字节原样）
///
/// resolver 无目标/失败、任一判据门不过、站点指令事实上不是 indy（分析-
/// 字节码漂移）、`k > 0` 且站点锚定标签、描述符解析失败、插入/改写/intern
/// 拒绝、装载/升格/lower/序列化失败（整类放弃）。整类无改写不写回：容器
/// 资源逐字节保持。幂等：改写后站点已是直接调用，二次运行无候选站点，
/// 严格 no-op（无 resolver 或无命中返回 all()；有改写返回 none()）。
///
/// @par 标签
/// - `scope:global`
/// - `action:rewrite`
/// - `stability:opt-in`
class IndySiteInliner : public mjbase::spi::GlobalPass
{
public:
    /// @brief 以给定 resolver 集构造（按 SPI 管理器纪律排序）。缺省无
    ///        resolver：run 严格 no-op。
    explicit IndySiteInliner(
        std::vector<std::unique_ptr<mjbase::spi::IndyResolver>> resolvers = {})
        : manager_()
    {
        for (auto& resolver : resolvers) {
            (void)manager_.registerResolver(std::move(resolver));
        }
    }

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&  container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    mjbase::spi::IndyResolverManager manager_;
};

}   // namespace mjbase::pass
