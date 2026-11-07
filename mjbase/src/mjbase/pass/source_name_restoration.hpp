#pragma once

#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 源文件名还原映射分析（Recaf SourceNameRestorationTransformer 的
///        吸收件）：类 SourceFile 属性值与类名不一致时产类名映射（真值源 =
///        SourceFile），只产映射不改字节。
///
/// 判据（逐类独立，任一门不过该类零产出，不影响其他类；畸形类资源跳过，
/// class_scan fail-open 同口径）：
/// 1. 内部类跳过：类自身 InnerClasses 行（innerClassInfo == 本类）带
///    outerClassInfo（具名内部类），或类带 EnclosingMethod 属性（匿名/局部
///    形态）时跳过——内部类沿用外围类的 SourceFile，若不跳过会把外围类
///    名还原出来与外围类自身映射相撞（Recaf isInnerClass 同口径，属性
///    驱动；InnerClasses 解码失败的行不判内）。
/// 2. SourceFile 形状：值须整体匹配「1..50 个 ASCII word 字符
///    （[A-Za-z0-9_]）+ .java / .kt 后缀」；无属性/空白/超长/异形零产出
///    （Recaf 同款形状门的 ASCII word 显式化）。
/// 3. 标识符合法门（Recaf 无此门，本件收紧申报）：剥离后缀的源名须为合法
///    Java 标识符（IdentifierUtil）——word 字符面允许数字开头的名字，还原
///    出非法标识符不可信，宁可漏恢复不可错恢复。
/// 4. 产出：还原名 = 包前缀（二进制名末个 '/' 及其之前，默认包无前缀）+
///    源名；还原名 == 当前二进制名（已正名）零条目，否则 addClass(当前名 →
///    还原名)。同目标冲突不消解（ENR 同口径，逐类独立盲加）。
///
/// 产出通道：作为全局分析挂在容器 AM 上（`getResult` 惰性计算并缓存），消费
/// 方经 `GlobalAnalysisManager::getResult<SourceNameRestorationAnalysis>` 取
/// 映射，再交改名执行面（`asm_remapper::MappingRemapper` /
/// `MappingFormatRegistry` 导出）消费；改名执行不归本件。
///
/// @par 标签
/// - `scope:global`
/// - `action:recover`
/// - `stability:opt-in`
class SourceNameRestorationAnalysis
{
public:
    using Result = mjbase::mapping::IntermediateMapping;

    Result run(mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am);
};

/// @brief 源文件名还原 pass：映射生成件（`SourceNameRestorationAnalysis`）
///        的管线挂载壳。注册分析并触发计算（惰性求值进缓存），字节码零
///        改动，恒返回 `PreservedAnalyses::all()`（只读 pass 口径）。
class SourceNameRestorer : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
