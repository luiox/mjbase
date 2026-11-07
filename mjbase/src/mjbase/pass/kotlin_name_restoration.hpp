#pragma once

#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief Kotlin 名还原映射分析（Recaf KotlinNameRestorationTransformer 的
///        组合式吸收）：消费 Kotlin Metadata 收集件
///        `KotlinMetadataCollectionAnalysis` 的模型与双向类名映射，对容器实际
///        成员做「描述符唯一匹配」，产出「混淆名 → Kotlin 真名」家族映射
///        （`IntermediateMapping`），只产映射不改字节。规格篇
///        doc:kotlin-name-restoration-spec。
///
/// 判据面（逐类独立，零产出不影响其他类；畸形类资源跳过，class_scan
/// fail-open 同口径）：
/// 1. 类名槽：类级 `Lkotlin/jvm/JvmName;` 注解声明的名字优先（声明真名，
///    覆盖 kotlin 文件门面场景——k=2/5 模型名 = 定义类二进制名，不含真名
///    信息；Recaf 原件仅无模型时读注解，模型在场即遮蔽）；注解缺席时用收集
///    模型 fqName 真名。包前缀取当前二进制名的包段（根包无前缀，Recaf 产
///    `/Name` 的边界修正）。
/// 2. 字段槽：属性 backing field——属性类型描述符（收集件
///    `ktTypeDescriptor`）经「容器真名段反向重写（reverseMapDescriptor）+
///    stdlib 替换（kotlinToJvmDescriptor）」双形态后，与本类声明字段唯一
///    命中即产映射（Recaf getCandidates 同口径：metadata 无序，只认唯一
///    描述符命中；多命中/零命中放弃）。
/// 3. 方法槽：属性访问器（getter `()T` 与 setter `(T)V`）+ 函数（含扩展
///    接收者——收集件已把 receiver 展开为 JVM 首参）同上唯一命中。getter
///    真名按 kotlinc 约定（get/is/do 前缀属性名原样，其余 get+首字母大写，
///    Recaf 同表）；setter 真名 set+首字母大写（is 前缀属性去 is，
///    `isActive` → `setActive`）。setter 槽只对 `hasSetter` 属性开
///    （收集件 setter_parameter 在场面）——val 属性上偶然同形的用户方法
///    不得误恢复（Recaf 无 setter 面，本件为任务判据面补强，以元数据
///    mutability 门保真）。
/// 4. 合成名消解：成员名含 `$`（access$ 转发 / $default 桥 / $$delegate_0
///    委托面 / lambda 编号）或 `<` 开头（init/clinit）不进候选集、不收
///    映射——防合成成员抢走唯一候选槽（Recaf 只排除 `<` 开头；Kotlin 用户
///    标识符软保留 `$`，含 `$` 即合成形态。代价：混淆名自带 `$` 的条目
///    不恢复，宁可漏恢复不可错恢复）。
/// 5. 冲突消解（同槽多真值源优先级）：类名槽注解 > metadata fqName；成员槽
///    属性 field/getter/setter > 函数，同槽以首写者胜（处理序即优先序），
///    同名幂等跳过。已正名（oldName == newName）零条目（映射面最小化）。
///
/// 产出通道：作为全局分析挂在容器 AM 上（`getResult` 惰性计算并缓存，依赖
/// 收集件经同一 AM 惰性取），消费方经
/// `GlobalAnalysisManager::getResult<KotlinNameRestorationAnalysis>` 取映射，
/// 再交改名执行面（`asm_remapper::MappingRemapper` / `MappingFormatRegistry`
/// 导出）消费；改名执行不归本件。
///
/// @par 标签
/// - `scope:global`
/// - `action:recover`
/// - `stability:opt-in`
class KotlinNameRestorationAnalysis
{
public:
    using Result = mjbase::mapping::IntermediateMapping;

    Result run(mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am);
};

/// @brief Kotlin 名还原 pass：映射生成件（`KotlinNameRestorationAnalysis`）
///        的管线挂载壳。注册收集件与本分析（幂等）并触发计算，字节码零
///        改动，恒返回 `PreservedAnalyses::all()`（只读 pass 口径）。
class KotlinNameRestorer : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
