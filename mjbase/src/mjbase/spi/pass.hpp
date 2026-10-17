#pragma once

namespace mjbase::spi {

/// @brief pass 体系的最小标记接口。
///
/// 该接口不承载运行方法，也不拥有 IR 或分析结果；它只提供统一的虚析构函数，
/// 供需要以基类指针保存不同 pass 类型的基础设施使用。具体执行契约由
/// PassModel（指令级三级作用域）与 CfPass（classfile 级）定义。
///
/// @par 所有权与异常
/// 实现对象由持有它的 std::unique_ptr 负责释放，析构不抛异常。
class IPass
{
public:
    virtual ~IPass() = default;
};

/// @brief pass 执行上下文的扩展标记接口。
///
/// 当前上下文能力通过各级 AnalysisManager、PassJobContext（arena + 身份池）
/// 和 pinned proxy 提供；本接口不隐含线程安全、生命周期或分析失效保证，
/// 不能被当作通用上下文使用。
class IPassContext
{
public:
    virtual ~IPassContext() = default;
};

}   // namespace mjbase::spi
