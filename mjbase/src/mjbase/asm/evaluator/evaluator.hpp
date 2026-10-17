/// @file        evaluator.hpp
/// @brief       方法级具体求值栈机：CodeDom 只读遍历 + 步数熔断。
///
/// 迁移自 mbase 的 asm/evaluator（Recaf 式迷你 VM）的数值子集：语义照旧
/// （Java 溢出/移位/除法/NaN 语义），实现按 libjbc CodeDom 重写——分支经
/// resolveLabel 走目标，不碰字节偏移。与旧实现的边界差异：
///   - 只做具体求值：不存在 unknown 值与优雅降级，遇不可求值形状返回
///     Err(Unsupported)，栈/局部变量状态不符返回 Err(BadState)；
///   - 不做递归求值、字段缓存、JDK shim（属旧栈 workspace 级能力）；
///   - LDC* 依赖常量池，CodeDom 单方法视图无法解析，归入 Unsupported；
///   - 整数除零在旧栈降级为 unknown，此处是 BadState（JVM 会抛
///     ArithmeticException，求值器不猜结果）。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "method_sig.hpp"
#include "value.hpp"

namespace mjbase::asm_evaluator {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief 求值配置。
struct Options
{
    /// 步数熔断上限：每条执行的指令计 1 步；达到上限仍未返回则
    /// Err(StepLimit)。防御不可达 return 的循环/交叉跳转。
    ca::u32 maxSteps = 4096;
};

/// @brief 求值失败的结构化错误。
struct EvalError
{
    enum class Kind : ca::u8 {
        Unsupported = 0,  ///< 指令不在求值子集内（不猜、不静默跳过）
        StepLimit,        ///< 步数熔断：预算耗尽仍未产出返回
        BadState,         ///< 栈/局部变量状态与指令不符（下溢、类别错、读 Top、
                          ///< 除零、入参与描述符不匹配、落到方法末尾无 return 等）
    };

    Kind kind = Kind::Unsupported;

    /// 出错相关的指令句柄；kNoInsn 表示不指向具体指令（入参校验失败、
    /// 控制流落到方法末尾边界等）。StepLimit 时指向熔断处将执行的指令。
    jbc::ir::InsnId insn = jbc::ir::kNoInsn;
};

/// @brief 求值产物：类型化返回值，或 void 方法的正常完成。
struct EvalOutcome
{
    enum class Kind : ca::u8 {
        Void = 0,  ///< void return 完成
        Value,     ///< 类型化返回值
    };

    Kind kind = Kind::Void;
    /// kind == Value 时的返回值；类别与描述符返回类别一致。
    Value value;
};

/// @brief opcode 是否在求值子集内（const/load/store/算术/比较/转换/
///        分支/switch/return/栈整形 + aconst_null/arraylength）。
bool isSupportedOp(jbc::core::Op op) noexcept;

/// @brief 错误类别名（诊断输出）。
const char* to_string(EvalError::Kind kind) noexcept;

/// @brief 求值一个方法体。
///
/// @param dom  方法体指令 DOM；只读遍历（order/peek/resolveLabel），
///             求值不修改 DOM。
/// @param sig  方法签名：驱动初始局部变量注入（实例方法 local0 = this，
///             参数按描述符序入槽，cat2 参数占双槽）。
/// @param args 入参值：实例方法首元素为 this，随后按描述符参数序。
///             引用入参经 RefFactory 构造以携带身份。数量或类别与
///             sig 不符返回 Err(BadState)。
/// @param options 步数熔断等配置。
///
/// @return Ok(EvalOutcome)：Value（ireturn/lreturn/freturn/dreturn/areturn，
///         类别已与描述符返回类别核对）或 Void（return）。
///         Err：Unsupported / StepLimit / BadState，见 EvalError。
Result<EvalOutcome, EvalError> evaluate(const jbc::ir::CodeDom& dom, const MethodSig& sig,
                                        const std::vector<Value>& args,
                                        const Options& options = Options{});

}   // namespace mjbase::asm_evaluator
