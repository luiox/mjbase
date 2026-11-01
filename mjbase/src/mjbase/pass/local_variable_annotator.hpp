/// @file        local_variable_annotator.hpp
/// @brief       局部变量表规范化：可区分的 varN 命名 + 数据流类型提升。
///
/// 混淆产物的 LocalVariableTable 常见两种失真：只有 `this`/形参的残缺行，
/// 以及槽位复用下的类型取公共上界（实际存 String 的槽声明为 Object）。两者
/// 都让反编译器只能造名字、并保留满屏 `(String)` 强转。
///
/// 本 pass 对每个方法：
///   - 用 libjbc Strict 帧引擎（精确引用类型）计算逐指令 locals/stack；
///   - 按「槽位 + 赋入类型」切分变量生命周期（槽复用生成 `var<slot>_2` …）；
///   - 重写 LocalVariableTable：名字 `var<slot>`，描述符取推断的精确类型；
///   - 丢弃 LocalVariableTypeTable（槽复用后泛型签名不再可信）。
///
/// LVT 是纯调试元数据：JVM 校验与执行都不读它，因此本 pass 只改属性，
/// 指令、StackMapTable、maxes 一律不动。反编译器（CFR/javap）按 LVT 显示
/// 变量名与类型。
///
/// 帧计算失败的降级：Strict 失败时用 BestEffort 帧，仍失败则仅按存储指令
/// 推导基本类型（int/long/float/double），引用类型保留原声明；命名照常。
#pragma once

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 局部变量规范化选项。
struct LocalVariableAnnotatorOptions
{
    /// @brief 用推断类型覆盖声明类型（Object 槽实际存 String 时写 String）。
    bool promoteTypes = true;
    /// @brief 覆盖已有名字；false 时沿用覆盖该段起点的原 LVT 行名字。
    bool replaceNames = true;
    /// @brief 生成名前缀，最终名为 `<prefix><slot>[_<序号>]`。
    std::string prefix = "var";
};

/// @brief 局部变量表规范化 pass（容器级）。
///
/// @par 标签
/// - `scope:global`
/// - `action:annotate`
/// - `stability:opt-in`
class LocalVariableAnnotator final : public mjbase::spi::GlobalPass
{
public:
    /// @brief 运行统计（测试与工具观测面；pass 本身不因个别方法失败中断）。
    struct Stats
    {
        ca::usize classes        = 0;   ///< 尝试处理的类
        ca::usize methods        = 0;   ///< 带 Code 且成功升格的方法
        ca::usize variables      = 0;   ///< 写入 LVT 的变量行
        ca::usize promotedTypes  = 0;   ///< 描述符比原声明更精确的行
        ca::usize strictFailures = 0;   ///< Strict 帧失败、降级的次数
        ca::usize skippedMethods = 0;   ///< 升格/帧/写回失败跳过的方法
        ca::usize skippedClasses = 0;   ///< 解析或写回失败、原样保留的类
    };

    explicit LocalVariableAnnotator(LocalVariableAnnotatorOptions options = {});

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

    const Stats& stats() const noexcept { return stats_; }

private:
    LocalVariableAnnotatorOptions options_;
    Stats                         stats_;
};

}   // namespace mjbase::pass
