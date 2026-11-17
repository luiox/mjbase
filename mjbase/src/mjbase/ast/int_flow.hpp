/// @file int_flow.hpp
/// @brief 共享方法体 AST 的跨块整数常量事实。
#pragma once

#include "method_body.hpp"

namespace mjbase::ast {

struct MethodIntFlow
{
    /// 按块和语句编号索引，仅记录具有整数语义的已知值。
    std::vector<std::vector<std::optional<ca::i32>>> values;
    /// 按块编号索引，整数条件可证明时记录其结果。
    std::vector<std::optional<bool>> branches;
};

/// 从入口分析普通控制流，合流时保留所有到达路径均同意的事实。
/// 分支结果在收敛后计算。不可达块保留对应的空事实。
/// 异常边、无效控制流或超出分析工作上限时返回空结果。
std::optional<MethodIntFlow> analyzeIntFlow(const MethodBody& body);

}   // namespace mjbase::ast
