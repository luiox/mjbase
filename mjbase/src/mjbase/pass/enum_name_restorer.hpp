#pragma once

#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 枚举名恢复映射分析（Recaf EnumNameRestorationTransformer 第三吸收项）：
///        扫容器内枚举类，从 `<clinit>` 泄漏的常量真名恢复混淆枚举成员名，
///        产出家族映射（`IntermediateMapping`），只产映射不改字节。
///
/// 泄漏原理：javac 生成的 `<clinit>` 必须以字符串 ldc 传入枚举常量真名
/// （`Enum.valueOf`/`Enum.name` 运行期语义依赖它），混淆器可以改名字段与
/// 合成方法，改不掉这些 ldc——它们是可验证的"真名"来源。
///
/// 判据（fail-closed，逐类独立，全部满足才产出）：
/// 1. 枚举形态锚：类 access 带 ACC_ENUM（0x4000），且 `values` 方法签名
///    `()[L<本类>;`、`valueOf` 方法签名 `(Ljava/lang/String;)L<本类>;` 同时
///    在；缺一即跳过。
/// 2. 构造点形状（形状 A）：clinit 内 `NEW <本类>; DUP; ldc "<真名>";
///    <常量 ordinal>; INVOKESPECIAL <本类>.<init>((Ljava/lang/String;I…)V`
///    四窗口，随后直线段（仅允许 astore/aload 同槽中转对与 nop）到达
///    `PUTSTATIC <本类>.<字段名> L<本类>;`，且目标字段 access 带 ACC_ENUM。
///    窗口断裂的点丢弃（带参枚举参数在 ordinal 之前的形状窗口不成立）。
/// 3. `$values` 数组形状（形状 B）：clinit 内恰好一处
///    `INVOKESTATIC <本类>.<数组方法> ()[L<本类>;`（名字为 `values` 视为
///    病态拒），随后直线段到达 `PUTSTATIC <本类>.<数组字段> [L<本类>;`；
///    数组方法体内 `GETSTATIC <本类>.<字段> L<本类>;` 的出现序即数组序。
/// 4. 双形状互证：形状 A 按 ordinal 升序排列，ordinal 必须恰为 0..n-1
///    无重无缺；排列后的字段序与形状 B 的 getstatic 字段序逐元素相等，
///    序长不等或任一错位即整类拒绝（宁可漏恢复不可错恢复）。
/// 5. 名字口径（映射面最小化）：泄漏真名必须是合法 Java 标识符
///    （`IdentifierUtil::isValidJavaIdentifier`）；被混淆名必须是非法标识符
///    才产映射——合法名（含已与真名同名的字段、`a`/`b`/`c` 这类合法短名）
///    一律不动。数组字段恢复目标为 javac 规范名 `$VALUES`，数组方法恢复
///    目标为 `$values`，同样只在被混淆名为非法标识符且不等于规范名时产出。
///
/// 产出通道：作为全局分析挂在容器 AM 上（`getResult` 惰性计算并缓存），
/// 消费方经 `GlobalAnalysisManager::getResult<EnumNameMappingAnalysis>`
/// 取映射，再交改名执行面（如 `asm_remapper::MappingRemapper` /
/// `MappingFormatRegistry` 导出）消费；改名执行不归本件。
///
/// 边界（v1 申报）：仅认 javac 9+ 的 `$values` 合成方法形状（javap 亲验，
/// `--release 8` 产物同形）；Java 8 时代 clinit 内联数组的形状不在本批；
/// 带参枚举参数先于 ordinal 的窗口形状整类拒绝（互证不齐 fail-closed）。
/// 畸形类资源跳过（renamer 面 class_scan fail-open 同口径）。
///
/// @par 标签
/// - `scope:global`
/// - `action:recover`
/// - `stability:opt-in`
class EnumNameMappingAnalysis
{
public:
    using Result = mjbase::mapping::IntermediateMapping;

    Result run(mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am);
};

/// @brief 枚举名恢复 pass：映射生成件（`EnumNameMappingAnalysis`）的管线
///        挂载壳。注册分析并触发计算（惰性求值进缓存），字节码零改动，
///        恒返回 `PreservedAnalyses::all()`（只读 pass 口径）。
class EnumNameRestorer : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
