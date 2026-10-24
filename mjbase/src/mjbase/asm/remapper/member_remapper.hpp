/// @file        member_remapper.hpp
/// @brief       把 MemberRemapPlan 应用到一个 libjbc 类会话的改写器。
///
/// 改写载体为 libjbc backing
/// DOM（@ref jbc::core::ClassFile），方法体经 promote/CodeDom/lower 往返，
/// 指令 CP 操作数经会话语义层（setMember/setClass/setString + intern）
/// 重指向——不改写任何常量池 Utf8 条目内容，只做条目级重指向/追加，
/// 规避 Utf8 跨面共享误伤。
///
/// 改写面（RemapPolicy 门控）：
/// - bytecode：类头（this/super/interfaces）、字段/方法名与描述符、方法体
///   指令（字段/方法/类型/multianewarray/ldc 常量/invokedynamic）、异常表
///   捕获类型、Exceptions 属性、结构元数据（EnclosingMethod/NestHost/
///   NestMembers/PermittedSubclasses/InnerClasses/Record/Module 族）、
///   StackMapTable Object 校验类型、局部变量表描述符。
/// - reflectionStrings：常量反射成员查找的名称字符串改写（线性状态机 +
///   alias 数据流快照）。
/// - annotations：Runtime{Visible,Invisible}Annotations 与 AnnotationDefault。
/// - signatures：类/字段/方法 Signature 属性与 LVTT 签名。
///
/// 资源面（jar 内 URI 换名、META-INF/services、MANIFEST、映射文本）在
/// resource_remapper。
///
/// 改写以 libjbc 的 lazy/保真写回为前提：未触碰属性走 copy-through，触碰
/// 的属性由槽状态驱动重编码；嵌套属性直改后必须显式置脏（见实现内注记）。
#pragma once

#include <string_view>
#include <vector>

#include "libca/core/result.hpp"

#include "jbc/ir/units.hpp"

#include "mjbase/asm/remapper/member_remap_plan.hpp"

namespace mjbase::analysis {
class AliasAnalysis;
}   // namespace mjbase::analysis

namespace mjbase::asm_remapper {

using ca::core::Result;
using ca::core::Ok;
using ca::core::Err;

/// @brief 改写面开关。
///
/// 默认只改写字节码结构面；元数据、反射字符串与资源需显式开启。
struct RemapPolicy
{
    bool bytecode          = true;
    bool reflectionStrings = false;
    bool annotations       = false;
    bool signatures        = false;
    bool resources         = false;
    /// resources 开启时的细分门（obf 改写面显式化）。
    /// 默认 true 保持既有 resources=true 的完整改写面不变；false 仅关掉对应
    /// 资源面，字节码/反射常量/注解/签名/授权文本资源不受影响。
    bool manifest      = true;
    bool serviceLoader = true;
    /// 允许按映射文本口径改写的未知资源 URI（默认空：任意业务文本不猜）。
    std::vector<std::string> authorizedTextResourceUris;

    static RemapPolicy bytecodeOnly();
    static RemapPolicy bytecodeAndReflectionStrings();
    static RemapPolicy bytecodeAndMetadata();
    static RemapPolicy all();
};

/// @brief 改写失败码（libjbc 写回面的真失败；类字节解析失败不走此通道）。
enum class RemapError : ca::u8
{
    PromoteFailed,   ///< 方法体升格失败（AttrNotDecoded / BadInstructions）
    LowerFailed,     ///< 方法体写回失败（分支溢出 / relocation 未建模等）
};

/// @brief 错误码 → 短名（诊断输出用）。
const char* to_string(RemapError error) noexcept;

/// @brief 反射类名形态判定（含 '/' 或 '.'、无空白/括号；改写候选与 xref
///        元数据事实共用同一语法，两处口径不得漂移）。
bool looksLikeClassName(std::string_view value) noexcept;

/// @brief 反射类名 → 内部名：剥 L..; 外壳、点分换斜杠。
std::string toInternalReflectionName(std::string_view value);

/// @brief 把重映射计划应用到一个类会话。
///
/// @param session 已按类字节构建的会话（backing + IR 同生共死）；改写在
///                backing 上就地生效，写回字节由调用方经
///                jbc::core::writeClass(session.backing()) 取得。
/// @param plan    重映射计划；empty 计划直接返回 false。
/// @param policy  改写面开关。
/// @param aliases 反射接收者别名分析（policy.reflectionStrings 时消费；
///                可空——为空只走线性状态机轨道）。
/// @return 任一改写面实际发生变化时为 true；libjbc 写回面真失败返回
///         Err（改写半途而止的会话不再交付，调用方应放弃该类）。
Result<bool, RemapError> applyMemberRemapPlan(jbc::ir::ClassSession&      session,
                                              const MemberRemapPlan&      plan,
                                              const RemapPolicy&          policy,
                                              const analysis::AliasAnalysis* aliases);

/// @brief 同上，默认 bytecodeOnly 面。
Result<bool, RemapError> applyMemberRemapPlan(jbc::ir::ClassSession& session,
                                              const MemberRemapPlan& plan);

/// @brief 同上，显式面、无别名。
Result<bool, RemapError> applyMemberRemapPlan(jbc::ir::ClassSession& session,
                                              const MemberRemapPlan& plan,
                                              const RemapPolicy&     policy);

}   // namespace mjbase::asm_remapper
