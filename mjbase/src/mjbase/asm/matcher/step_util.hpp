/// @file        step_util.hpp
/// @brief       常用语义谓词步骤与常量值读写助手（旧 StepUtil 的可移植面）。
///
/// 旧栈两块面不可移植，有意不迁：按 owner/name/desc 匹配 invoke 家族、
/// 按 CP 载荷判别 ldc 内容——都需要常量池语义，纯 CodeDom 层只有 cp 索引。
/// 需要精确判别时用 MatchStep::opIf 携带调用方自己的谓词。
#pragma once

#include "jbc/ir/code_dom.hpp"

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "match_error.hpp"
#include "match_step.hpp"

namespace mjbase::asm_matcher {

/// 语义步骤工厂 + 常量值读写助手。
struct Steps
{
    /// @brief 整数装载族：iload/_n、iconst_m1..5、bipush、sipush、ldc 族。
    /// @note ldc/ldc_w 载荷类型在纯 DOM 层不可判别，按宽语义计入。
    static MatchStep loadInt();
    /// @brief 浮点装载族：fload/_n、fconst_0..2、ldc 族（宽语义同上）。
    static MatchStep loadFloat();
    /// @brief double 装载族：dload/_n、dconst_0..1。
    static MatchStep loadDouble();
    /// @brief long 装载族：lload/_n、lconst_0..1。
    static MatchStep loadLong();
    /// @brief 引用常量装载：ldc 族（载荷不可判别，宽语义）。
    static MatchStep loadStr();

    /// @brief DSL 类型标签 'I'|'J'|'F'|'D' 的常量装载判定。
    ///
    /// 语义照旧：I 含 bipush/sipush/ldc/iconst 族；F 含 fconst 族/ldc；
    /// J/D 只认 lconst/dconst；
    /// 'L' 归一为 'J'；其它标签恒 false。
    static bool isConstLoadOfType(char typeTag, const jbc::ir::Insn& insn) noexcept;

    /// isConstLoadOfType 的通配步骤。
    static MatchStep constLoad(char typeTag);

    // ---- 常量值读取（只读无 CP 形态；ldc 族载荷不可读，返回 false） ----

    /// iconst 族 / bipush / sipush 的立即值。
    static bool getIntValue(const jbc::ir::Insn& in, ca::i32& out) noexcept;
    /// lconst_0 / lconst_1。
    static bool getLongValue(const jbc::ir::Insn& in, ca::i64& out) noexcept;
    /// fconst_0..2（以 double 交付）。
    static bool getFloatValue(const jbc::ir::Insn& in, double& out) noexcept;
    /// dconst_0 / dconst_1。
    static bool getDoubleValue(const jbc::ir::Insn& in, double& out) noexcept;

    // ---- 常量装载发射（无 CP 可用形态；超范围 Err(EmitUnsupported)） ----

    /// -1..5 → iconst 族；i8 → bipush；i16 → sipush；更宽需要 ldc（有 CP 才可发）。
    static ca::core::Result<jbc::ir::Insn, MatcherError> makeIntInsn(ca::i32 v) noexcept;
    /// 0/1 → lconst；其它需要 ldc2_w。
    static ca::core::Result<jbc::ir::Insn, MatcherError> makeLongInsn(ca::i64 v) noexcept;
    /// 取 float 精度后 0/1/2 → fconst；其它需要 ldc。
    static ca::core::Result<jbc::ir::Insn, MatcherError> makeFloatInsn(double v) noexcept;
    /// 0/1 → dconst；其它需要 ldc2_w。
    static ca::core::Result<jbc::ir::Insn, MatcherError> makeDoubleInsn(double v) noexcept;
};

}   // namespace mjbase::asm_matcher
