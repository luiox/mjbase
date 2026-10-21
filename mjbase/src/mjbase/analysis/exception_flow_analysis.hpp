/// @file        exception_flow_analysis.hpp
/// @brief       跨类异常流模型：throw/call 站点扫描 + 处理器覆盖 + 未捕获
///              异常类型的过程间不动点。
///
/// 行为：类字节经 libjbc 会话升格后走 CodeDom 程序序扫描——
/// NEW 记录最近分配类型，ATHROW 产出（类型, 处理器列表）抛出站点，invoke
/// 家族产出调用站点；处理器覆盖按异常表范围判定。未捕获类型沿调用边做
/// Jacobi 不动点传播（轮数上限 = 方法数 + 1）；子类型判定先走内置
/// Throwable 族表，再走项目内类层次（mjbase hierarchy，从会话骨架建图）。
///
/// 实现边界：
/// - 方法身份/异常类型一律 KeyPool 驻留（MemberKey / string_view 视图），
///   不再持有 std::string 拷贝；查询接口收 string_view。
/// - 扫描序 = 容器迭代序。
#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"

namespace mjbase::analysis {

/// @brief 一条调用边上的异常流事实。
struct ExceptionFlowEdge
{
    MemberKey         source;         ///< 调用方方法
    MemberKey         callee;         ///< 被调方法
    std::string_view  exceptionType;  ///< 传播的异常内部名（池驻留）
    std::string_view  handlerType;    ///< 覆盖它的处理器类型；未覆盖为空（池驻留）
    bool              handled = false;
};

/// @brief 一个方法的未捕获异常摘要。
struct ExceptionMethodSummary
{
    MemberKey                    method;
    std::vector<std::string_view> uncaughtTypes;   ///< 内部名（池驻留），字典序
};

class ExceptionFlowModel
{
public:
    /// @brief 从资源容器建模型（丢弃既有结果；池随模型存活）。
    void build(const mjbase::model::ResourceContainer& container);
    void clear();

    /// @brief 按精确 owner/name/desc 查摘要；未声明（含不在容器内）返回 nullptr。
    const ExceptionMethodSummary* summaryOf(std::string_view owner, std::string_view name,
                                            std::string_view desc) const;

    /// @brief 全部调用边异常流事实（扫描序）。
    const std::vector<ExceptionFlowEdge>& edges() const { return edges_; }

    /// @brief 查询用驻留池（键构造与文本展开共用）。
    const KeyPool& pool() const noexcept { return pool_; }

private:
    /// summaryOf 按名查询时把查询键驻留进来（只增不改）；mutable 服务 const 查询。
    mutable KeyPool pool_;
    std::vector<ExceptionMethodSummary>                        summaries_;
    std::unordered_map<MemberKey, ca::usize, MemberKeyHash>    byKey_;
    std::vector<ExceptionFlowEdge>                             edges_;
};

}   // namespace mjbase::analysis
