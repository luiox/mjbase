/// @file        resource_remapper.hpp
/// @brief       把重映射计划应用到资源容器：类资源改写 + opt-in 资源面。
///
/// 类字节改写经 libjbc 会话（makeSessionFromBytes → applyMemberRemapPlan →
/// writeClass），容器与资源模型用 mjbase::model。畸形类资源跳过。
///
/// 改写面（RemapPolicy 门控）：
/// - bytecode + resources：类资源 URI 随类改名移动（`pkg/Old.class` →
///   `pkg/New.class`；仅 URI 恰为类内部名 + ".class" 的资源）。
/// - resources 门内细分（默认开，#1032 Q6）：META-INF/services 的 URI 换名
///   与 provider 行内容改写（serviceLoader）、MANIFEST 主属性 class 形态
///   改写（manifest）、映射文本资源（.map/.srg/.tiny/... 固定清单）、
///   显式授权的未知文本资源（authorizedTextResourceUris）。
///
/// 反射接收者别名：policy.reflectionStrings 开启时按容器做 scoped 别名闭包
/// （buildReflectionClosure）供类改写消费。
#pragma once

#include "libca/core/result.hpp"

#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/model/resource_container.hpp"

namespace mjbase::asm_remapper {

/// @brief 把重映射计划应用到容器内全部类与 opt-in 资源。
///
/// @param container 资源容器（就地改写：类字节、资源 URI、文本内容）。
/// @param plan      重映射计划；empty 计划直接返回 false。
/// @param policy    改写面开关。
/// @return 任一资源（类字节/URI/文本）实际发生变化时为 true。
Result<bool, RemapError> applyResourceRemapPlan(model::ResourceContainer& container,
                                                const MemberRemapPlan&      plan,
                                                const RemapPolicy&          policy);

/// @brief 同上，默认 bytecodeOnly 面（类字节改写，资源 URI 不动）。
Result<bool, RemapError> applyResourceRemapPlan(model::ResourceContainer& container,
                                                const MemberRemapPlan&    plan);

}   // namespace mjbase::asm_remapper
