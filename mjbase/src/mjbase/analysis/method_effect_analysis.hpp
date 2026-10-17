/// @file        method_effect_analysis.hpp
/// @brief       方法副作用模型：局部字节码效应分类 + 直接调用边不动点传播
///              + 纯函数常量返回摘要。
///
/// 旧栈对应物：mbase/analysis/method_effect_analysis（MethodEffectModel）。
/// 语义照旧、实现重写：类字节经 libjbc 会话升格后按 CodeDom 程序序做局部
/// 分类——常量/局部读写/算术/分支/转换/返回为 Pure；字段读与 arraylength
/// 升 ReadOnly；堆写/数组存储/分配/显式抛出/监视器/invokedynamic 为 Impure；
/// native/abstract/构造器/类初始化器/解析缺失为 Unknown。局部分类后沿直接
/// 调用图做不动点；容器外调用回退到 JDK 小白名单（java/lang/Math、
/// java/lang/StrictMath 与少量只读 java/lang/String 查询），其余外部调用
/// 保持 Unknown。
///
/// 本模型只回答堆副作用问题：不证明可以在不改变异常、空检查或类初始化时
/// 机的情况下删除调用。破坏性通行证必须自行叠加指令形状与异常/类初始化
/// 策略（旧栈模块头消费者规则照搬）。
///
/// 与旧实现的边界差（见迁移报告）：方法键为 MemberKey（KeyPool 驻留），
/// 查询接口收 string_view；分析管理器缓存（GlobalAnalysisManager 注册）
/// 属旧栈 pass 基建，mjbase pass 域批次未到，不迁。
#pragma once

#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"

namespace mjbase::analysis {

/// @brief 保守副作用级别（格：Pure ⊑ ReadOnly ⊑ Unknown ⊑ Impure 的合并序）。
enum class MethodEffectKind
{
    Pure,      ///< 仅局部栈/局部变量运算、常量、分支与已证 Pure 调用
    ReadOnly,  ///< 可读字段或调用 ReadOnly 方法，不写堆
    Impure,    ///< 确定写堆、分配、同步、抛出或其他效应字节码
    Unknown    ///< 方法体或被调不可得、未解析、native、abstract 或超出保守模型
};

/// @brief 常量返回类别（旧栈 ConstantReturnKind 语义照旧）。
enum class ConstantReturnKind
{
    Unknown,
    Int,
    Long,
    Float,
    Double,
    String,
    Null
};

/// @brief 纯函数的常量返回摘要。
struct ConstantReturnValue
{
    ConstantReturnKind kind          = ConstantReturnKind::Unknown;
    ca::i64            intValue      = 0;
    double             floatingValue = 0.0;
    std::string        stringValue;   ///< 出口物化（诊断/测试用），池驻留视图见 summary

    bool isKnown() const { return kind != ConstantReturnKind::Unknown; }
    bool operator==(const ConstantReturnValue& other) const
    {
        if (kind != other.kind) return false;
        switch (kind) {
        case ConstantReturnKind::Int:
        case ConstantReturnKind::Long: return intValue == other.intValue;
        case ConstantReturnKind::Float:
        case ConstantReturnKind::Double:
            return floatingValue == other.floatingValue ||
                   (std::isnan(floatingValue) && std::isnan(other.floatingValue));
        case ConstantReturnKind::String: return stringValue == other.stringValue;
        case ConstantReturnKind::Unknown:
        case ConstantReturnKind::Null: return true;
        }
        return false;
    }
};

/// @brief 一个声明方法的稳定效应摘要。
struct MethodEffectSummary
{
    MemberKey           method;
    MethodEffectKind    kind     = MethodEffectKind::Unknown;
    bool                declared = false;
    bool                hasBody  = false;
    ConstantReturnValue constantReturn;
};

/// @brief 效应级别 → 稳定显示名（诊断与测试）。
const char* methodEffectKindName(MethodEffectKind kind);

/// @brief 全程序方法副作用模型。
class MethodEffectModel
{
public:
    /// @brief 从资源容器重建摘要（丢弃既有结果）。
    void build(const mjbase::model::ResourceContainer& container);

    /// @brief 清空全部摘要与查找表。
    void clear();

    /// @brief 按精确 owner/name/desc 查摘要；未声明返回 nullptr。
    const MethodEffectSummary* effectOf(std::string_view owner, std::string_view name,
                                        std::string_view desc) const;

    /// @brief 效应级别；未声明返回 Unknown。
    MethodEffectKind effectKindOf(std::string_view owner, std::string_view name,
                                  std::string_view desc) const;

    /// @brief 是否已知 Pure。
    bool isPure(std::string_view owner, std::string_view name, std::string_view desc) const;

    /// @brief 是否已知 Pure 或 ReadOnly。
    bool isReadOnlyOrPure(std::string_view owner, std::string_view name,
                          std::string_view desc) const;

    /// @brief 纯直接/包装方法的已证常量返回。
    ConstantReturnValue constantReturnOf(std::string_view owner, std::string_view name,
                                         std::string_view desc) const;

    /// @brief 全部摘要（确定性扫描序）。
    const std::vector<MethodEffectSummary>& summaries() const { return summaries_; }

    /// @brief 当前摘要数量。
    ca::usize size() const { return summaries_.size(); }

    /// @brief 查询用驻留池。
    const KeyPool& pool() const noexcept { return pool_; }

private:
    /// summaryOf 按名查询时把查询键驻留进来（只增不改）；mutable 服务 const 查询。
    mutable KeyPool pool_;

    std::vector<MethodEffectSummary>                        summaries_;
    std::unordered_map<MemberKey, ca::usize, MemberKeyHash> byKey_;
};

}   // namespace mjbase::analysis
