/// @file        insn_op.hpp
/// @brief       opcode 分类谓词全集与栈效应静态表。
///
/// 语义来源：mbase asm/insn 的 InsnUtil 分类与 Stack 分析（isReturn/isIf/
/// invertIf/getStack*Count 等），在 jbc::core::Op 值域上重写。两处有意的
/// 语义修订：
///   - 常量族补齐 LDC_W/LDC2_W（旧 isConstant 只认 LDC，宽形态被注释掉）；
///   - isGoto 纳入 GOTO_W、isTerminator 纳入 RET——与 FrameEngine 的控制流
///     终结判定（domain.hpp 的 ends 名单）对齐。
/// 栈效应与 jbc::analysis::FrameEngine 的槽位迁移语义一致：cat2 值占 2 槽，
/// dup 族按「净新增槽位」计数（DUP_X2 为 {pop 0, push 1} 而非触碰 3 槽）。
#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/opcode.hpp"

namespace mjbase::asm_insn {

/// @brief 主分类：互斥且覆盖 jbc::core::Op 全枚举。
///
/// 单一定义源（single source of truth）：下面的便利谓词全部由本分类派生，
/// 不另立判断逻辑；测试对全枚举断言「每 op 恰归一类、每类非空」。
/// Wide 是 wide 前缀的占位分类——解码产物以 RawInsn::wide 标志携带前缀，
/// op 本身不会是 WIDE。
enum class OpKind : ca::u8 {
    Nop = 0,       ///< nop
    Const,         ///< aconst_null..dconst_1 / bipush / sipush / ldc 族
    LocalLoad,     ///< iload..aload（含紧凑 _0.._3 形态）
    ArrayLoad,     ///< iaload..saload
    LocalStore,    ///< istore..astore（含紧凑 _0.._3 形态）
    ArrayStore,    ///< iastore..sastore
    Stack,         ///< pop/pop2/dup 族/swap
    Arithmetic,    ///< iadd..lxor（含移位与位运算；不含 iinc/lcmp）
    LocalInc,      ///< iinc
    Conversion,    ///< i2l..i2s（数值转换；旧名 isTypeCastInsn）
    Compare,       ///< lcmp/fcmpl/fcmpg/dcmpl/dcmpg
    Branch,        ///< if* / goto / jsr（含 ifnull 族与 *_w 宽形态）
    Switch,        ///< tableswitch / lookupswitch
    Ret,           ///< ret（jsr/ret 遗留指令）
    Return,        ///< ireturn..return（不含 ret）
    FieldAccess,   ///< getstatic/putstatic/getfield/putfield
    Invoke,        ///< invokevirtual..invokedynamic
    New,           ///< new
    NewArray,      ///< newarray / anewarray / multianewarray
    ArrayLength,   ///< arraylength
    Throw,         ///< athrow
    CheckCast,     ///< checkcast
    InstanceOf,    ///< instanceof
    Monitor,       ///< monitorenter / monitorexit
    Wide,          ///< wide 前缀占位（解码产物不携带）
    Count,         ///< 哨兵：分类数；classify 对枚举外的值返回它
};

/// @brief opcode 的唯一主分类；枚举外的字节值返回 OpKind::Count。
OpKind classify(jbc::core::Op op) noexcept;

/// @brief 分类名（诊断输出用）。
const char* to_string(OpKind kind) noexcept;

// ---- 便利谓词（全部由 classify 派生；不同谓词间可交叠） ----

bool isConst(jbc::core::Op op) noexcept;                ///< 常量装载（含 ldc/ldc_w/ldc2_w）
bool isLoad(jbc::core::Op op) noexcept;                 ///< 局部变量读（含紧凑形态）
bool isStore(jbc::core::Op op) noexcept;                ///< 局部变量写（含紧凑形态）
bool isLocalAccess(jbc::core::Op op) noexcept;          ///< 局部变量读写 / iinc / ret
bool isArrayLoad(jbc::core::Op op) noexcept;            ///< 数组元素读
bool isArrayStore(jbc::core::Op op) noexcept;           ///< 数组元素写
bool isArray(jbc::core::Op op) noexcept;                ///< 数组载荷族（读/写/arraylength）
bool isStackOp(jbc::core::Op op) noexcept;              ///< pop/dup 族/swap
bool isArithmetic(jbc::core::Op op) noexcept;           ///< 算术/移位/位运算（iadd..lxor）
bool isConversion(jbc::core::Op op) noexcept;           ///< 数值转换（i2l..i2s）
bool isCompare(jbc::core::Op op) noexcept;              ///< lcmp/f*cmp*/d*cmp*
bool isBranch(jbc::core::Op op) noexcept;               ///< 一切跳转（if* / goto / jsr，含 _w）
bool isConditionalBranch(jbc::core::Op op) noexcept;    ///< 条件跳转（if* 家族，旧名 isIf）
bool isGoto(jbc::core::Op op) noexcept;                 ///< 无条件跳转（goto/goto_w）
bool isIfNull(jbc::core::Op op) noexcept;               ///< ifnull / ifnonnull
bool isSwitch(jbc::core::Op op) noexcept;               ///< tableswitch / lookupswitch
bool isReturn(jbc::core::Op op) noexcept;               ///< 值返回与 void 返回（不含 ret）
bool isInvoke(jbc::core::Op op) noexcept;               ///< invokevirtual..invokedynamic
bool isFieldAccess(jbc::core::Op op) noexcept;          ///< get/putstatic + get/putfield
bool isThrow(jbc::core::Op op) noexcept;                ///< athrow
bool isMonitor(jbc::core::Op op) noexcept;              ///< monitorenter / monitorexit
/// 基本块终结指令：return/goto/switch/throw/if*/ret（ret 为新纳入，见文件头）。
bool isTerminator(jbc::core::Op op) noexcept;
/// 纯计算指令（无控制流/字段访问/调用/抛出）：常量/算术/转换/栈操作。
/// 沿用旧 isPureComputation 语义：不含 compare（lcmp 等亦无副作用，
/// 未纳入，保持行为兼容）。
bool isPureComputation(jbc::core::Op op) noexcept;

/// @brief 条件跳转取反（IFEQ↔IFNE / IF_ICMPLT↔IF_ICMPGE / IFNULL↔IFNONNULL 等）。
///        非条件跳转 opcode 原样返回（旧契约不变）。
jbc::core::Op invertIf(jbc::core::Op op) noexcept;

// ---- 栈效应静态表 ----

/// @brief 栈效应静态不可知标记（值依赖 CP 描述符或操作数）。
inline constexpr ca::i32 kStackDependent = -1;

/// @brief 一条指令对操作数栈的净效应（槽位计）。
///
/// pop/push 是净移除/净新增的**槽位数**（非「触碰」数）：DUP 为 {0,1}，
/// SWAP 为 {0,0}。cat2（long/double）按 2 槽计。
struct StackEffect
{
    ca::i32 pop = 0;    ///< 净弹出的槽位数；kStackDependent = 操作数依赖
    ca::i32 push = 0;   ///< 净压入的槽位数；kStackDependent = 操作数依赖

    bool operator==(const StackEffect& o) const noexcept
    {
        return pop == o.pop && push == o.push;
    }
    bool operator!=(const StackEffect& o) const noexcept { return !(*this == o); }
};

/// @brief 静态可知的栈效应。
///
/// 依赖 CP 描述符的成员访问族逐字段标记：
/// getstatic {0,dep} / putstatic {dep,0} / getfield {1,dep} / putfield {dep,0} /
/// invoke* {dep,dep}；multianewarray 弹出数取自操作数 dims（{dep,1}）。
/// 这些族用 fieldStackEffect / invokeStackEffect 按描述符求值。
StackEffect stackEffectOf(jbc::core::Op op) noexcept;

/// @brief 执行前栈上至少需要的槽数（underflow 防线；旧 getConsumeStackCount 的完整版）。
///
/// 与 pop 的差异在 dup 族：DUP_X2 触碰 3 槽但净移除 0——本函数返回 3。
/// 描述符依赖族（put*/invoke*/multianewarray）返回 kStackDependent。
ca::i32 minStackDepthOf(jbc::core::Op op) noexcept;

/// @brief 净效应 push - pop；任一侧依赖描述符时返回 kStackDependent。
ca::i32 netStackEffectOf(jbc::core::Op op) noexcept;

/// @brief 描述符版栈效应的错误原因。
enum class StackError {
    Ok = 0,
    NotFieldOp,    ///< op 不属于字段访问族（get/putstatic/field 之外）
    NotInvokeOp,   ///< op 不属于 invoke 族
    BadDescriptor, ///< 描述符非法（slot 计数解析失败）
};

const char* to_string(StackError e) noexcept;

/// @brief 字段访问族按字段描述符求栈效应。
///
/// 槽位计数复用 jbc::analysis::fieldSlots（单一实现，不另立描述符扫描）。
/// desc 形如 "J" / "Ljava/lang/String;"；pop 含 getfield/putfield 的对象引用。
ca::core::Result<StackEffect, StackError>
fieldStackEffect(jbc::core::Op op, std::string_view fieldDesc);

/// @brief invoke 族按方法描述符求栈效应。
///
/// pop = 参数槽位 + 接收者 1 槽（invokestatic/invokedynamic 无接收者）；
/// push = 返回值槽位（void 描述符为 0）。
ca::core::Result<StackEffect, StackError>
invokeStackEffect(jbc::core::Op op, std::string_view methodDesc);

}   // namespace mjbase::asm_insn
