/// @file        field_access_graph.hpp
/// @brief       成员级字段访问图：直接读写事实 + 沿调用边传播的方法摘要。
///
/// 输入是现成的两个全程序事实面：ReferenceIndex（字段访问指令的直接编码
/// 事实）与 CallGraph（直接调用边）。本模块把字段/方法身份重新驻留进自有
/// KeyPool，按「方法摘要 = 直接字段集 ∪ 被调方法摘要」做并集不动点，再把
/// 方法摘要倒排成字段的读写方集合。unused_field 类消费者用 readersOf /
/// writersOf / unreadDeclaredFields 直接判定「写了没人读」。
///
/// 保守边界：
/// - 虚/接口调用、invokedynamic、未声明/abstract/native 被调方：调用方标记
///   hasUnknownCallees（目标是未解析的，可能读写任意字段）。该标记沿调用边
///   传递；消费者决定是否接受。
/// - 反射查询（FieldReferenceKind::Reflection）只是查找，不算读写；
///   ReflectionAccess 与 MethodHandle 可能执行 get/set，读写两边都记。
/// - 字段集只回答问题「已知谁读写」，不证明「没人读写」；负向结论还必须
///   叠加上面的 unknown 标记与扫描范围（外部类、反射）。
#pragma once

#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/reference/reference_index.hpp"

namespace mjbase::reference {

/// @brief 方法级字段摘要（含经调用边传递的集合）。
struct MethodFieldSummary
{
    MemberKey method;
    std::vector<MemberKey> fieldsRead;
    std::vector<MemberKey> fieldsWritten;
    /// 存在未解析被调方（虚分派/indy/外部/abstract/native）：可能访问任意字段。
    bool hasUnknownCallees = false;
};

/// @brief 字段级访问摘要：谁读、谁写（含经调用边传递的读写方）。
struct FieldAccessSummary
{
    MemberKey field;
    std::vector<MemberKey> readers;
    std::vector<MemberKey> writers;
    bool declared = false;

    bool isRead() const noexcept { return !readers.empty(); }
    bool isWritten() const noexcept { return !writers.empty(); }
    /// 有写无读；仍可能被 unknown 调用方读过，见 MethodFieldSummary。
    bool isOnlyWritten() const noexcept { return readers.empty() && !writers.empty(); }
};

/// @brief 全程序字段访问图。
///
/// 身份键随图自有池存活；查询结果借用池内文本，图销毁后失效。
class FieldAccessGraph
{
public:
    /// @brief 重建图（丢弃既有结果）。
    ///
    /// @param index 直接字段/方法声明事实。
    /// @param callGraph 直接调用边；查询时会向图的池驻留调用方文本
    ///        （只增不改，调用图语义不变）。
    void build(const ReferenceIndex& index, mjbase::asm_classpath::CallGraph& callGraph);

    /// @brief 清空全部摘要与查找表。
    void clear();

    const KeyPool& pool() const noexcept { return pool_; }

    /// @brief 全部方法摘要（声明方法按扫描序，随后是调用边引入的外部方法）。
    const std::vector<MethodFieldSummary>& methodSummaries() const noexcept
    {
        return methodSummaries_;
    }
    /// @brief 全部字段摘要（声明字段按扫描序，随后是引用引入的外部字段）。
    const std::vector<FieldAccessSummary>& fieldSummaries() const noexcept
    {
        return fieldSummaries_;
    }

    const MethodFieldSummary* methodSummary(MemberKey method) const noexcept;
    const MethodFieldSummary* methodSummary(std::string_view owner, std::string_view name,
                                            std::string_view desc) const;
    const FieldAccessSummary* fieldSummary(MemberKey field) const noexcept;
    const FieldAccessSummary* fieldSummary(std::string_view owner, std::string_view name,
                                           std::string_view desc) const;

    /// @brief 字段的已知读写方（图中键）。
    std::vector<MemberKey> readersOf(MemberKey field) const;
    std::vector<MemberKey> readersOf(std::string_view owner, std::string_view name,
                                     std::string_view desc) const;
    std::vector<MemberKey> writersOf(MemberKey field) const;
    std::vector<MemberKey> writersOf(std::string_view owner, std::string_view name,
                                     std::string_view desc) const;

    /// @brief 声明字段中没有已知读者的（写后未读候选）。
    std::vector<MemberKey> unreadDeclaredFields() const;
    /// @brief 声明字段中既无读者也无写者的。
    std::vector<MemberKey> untouchedDeclaredFields() const;

    /// @brief 任一方法带未解析被调方（全局保守度提示）。
    bool hasUnknownEffects() const noexcept { return hasUnknownEffects_; }

    /// @brief 有字段引用缺 owner/name/desc（CP 畸形或反射查找）。
    bool hasUnresolvedFieldRefs() const noexcept { return unresolved_; }

private:
    MemberKey internMethod(std::string_view owner, std::string_view name, std::string_view desc);
    MemberKey internField(std::string_view owner, std::string_view name, std::string_view desc);
    ca::u32 methodIndex(MemberKey method);
    ca::u32 fieldIndex(MemberKey field);

    /// 驻留池在 const 查询中也要接文本键（内容寻址、幂等）。
    mutable KeyPool pool_;

    std::vector<MemberKey> methodKeys_;
    std::vector<MemberKey> fieldKeys_;
    std::vector<bool>      fieldDeclared_;
    std::unordered_map<MemberKey, ca::u32, MemberKeyHash> methodIndexOf_;
    std::unordered_map<MemberKey, ca::u32, MemberKeyHash> fieldIndexOf_;

    std::vector<std::vector<ca::u32>> read_;
    std::vector<std::vector<ca::u32>> write_;
    std::vector<bool>                 unknown_;

    std::vector<MethodFieldSummary> methodSummaries_;
    std::vector<FieldAccessSummary> fieldSummaries_;
    bool                            hasUnknownEffects_ = false;
    bool                            unresolved_ = false;
};

}   // namespace mjbase::reference
