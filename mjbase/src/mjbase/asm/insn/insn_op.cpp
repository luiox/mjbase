/// @file        insn_op.cpp
/// @brief       opcode 分类谓词与栈效应表的实现。
///
/// 字节区间判断依赖 jbc::core::Op 的连续性（NOP=0x00..JSR_W=0xC9 无空洞，
/// 见 opcode.hpp）：区间分支与全枚举测试互为防线。
#include "insn_op.hpp"

#include "jbc/analysis/frame.hpp"

namespace mjbase::asm_insn {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using jbc::core::Op;

namespace {

/// v 是否落在 [lo, hi] 闭区间。
inline bool between(ca::u8 v, ca::u8 lo, ca::u8 hi) noexcept
{
    return v >= lo && v <= hi;
}

/// load/store 族的 cat2 判定（long/double 占 2 槽）。
inline bool loadStoreCat2(ca::u8 v) noexcept
{
    // 显式形态：lload 0x16 / dload 0x18 / lstore 0x37 / dstore 0x39；
    // 紧凑形态：lload_n 0x1e..0x21 / dload_n 0x26..0x29 /
    //           lstore_n 0x3f..0x42 / dstore_n 0x47..0x4a。
    return v == 0x16 || v == 0x18 || v == 0x37 || v == 0x39 ||
           between(v, 0x1e, 0x21) || between(v, 0x26, 0x29) || between(v, 0x3f, 0x42) ||
           between(v, 0x47, 0x4a);
}

}   // namespace

OpKind classify(Op op) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(op);
    if (v == 0x00) {
        return OpKind::Nop;
    }
    if (between(v, 0x01, 0x14)) {
        return OpKind::Const;   // *const_* / bipush / sipush / ldc 族
    }
    if (between(v, 0x15, 0x2d)) {
        return OpKind::LocalLoad;   // iload..aload（含紧凑）
    }
    if (between(v, 0x2e, 0x35)) {
        return OpKind::ArrayLoad;
    }
    if (between(v, 0x36, 0x4e)) {
        return OpKind::LocalStore;   // istore..astore（含紧凑）
    }
    if (between(v, 0x4f, 0x56)) {
        return OpKind::ArrayStore;
    }
    if (between(v, 0x57, 0x5f)) {
        return OpKind::Stack;
    }
    if (between(v, 0x60, 0x83)) {
        return OpKind::Arithmetic;   // iadd..lxor（iinc 在下一条）
    }
    if (v == 0x84) {
        return OpKind::LocalInc;
    }
    if (between(v, 0x85, 0x93)) {
        return OpKind::Conversion;
    }
    if (between(v, 0x94, 0x98)) {
        return OpKind::Compare;
    }
    if (between(v, 0x99, 0xa8)) {
        return OpKind::Branch;   // if* / goto / jsr
    }
    if (v == 0xa9) {
        return OpKind::Ret;
    }
    if (between(v, 0xaa, 0xab)) {
        return OpKind::Switch;
    }
    if (between(v, 0xac, 0xb1)) {
        return OpKind::Return;
    }
    if (between(v, 0xb2, 0xb5)) {
        return OpKind::FieldAccess;
    }
    if (between(v, 0xb6, 0xba)) {
        return OpKind::Invoke;
    }
    if (v == 0xbb) {
        return OpKind::New;
    }
    if (between(v, 0xbc, 0xbd)) {
        return OpKind::NewArray;   // newarray / anewarray
    }
    if (v == 0xbe) {
        return OpKind::ArrayLength;
    }
    if (v == 0xbf) {
        return OpKind::Throw;
    }
    if (v == 0xc0) {
        return OpKind::CheckCast;
    }
    if (v == 0xc1) {
        return OpKind::InstanceOf;
    }
    if (between(v, 0xc2, 0xc3)) {
        return OpKind::Monitor;
    }
    if (v == 0xc4) {
        return OpKind::Wide;
    }
    if (v == 0xc5) {
        return OpKind::NewArray;   // multianewarray
    }
    if (between(v, 0xc6, 0xc9)) {
        return OpKind::Branch;   // ifnull / ifnonnull / goto_w / jsr_w
    }
    return OpKind::Count;   // 枚举外字节（0xCA/0xFE/0xFF 等保留码）
}

const char* to_string(OpKind kind) noexcept
{
    switch (kind) {
    case OpKind::Nop: return "nop";
    case OpKind::Const: return "const";
    case OpKind::LocalLoad: return "local-load";
    case OpKind::ArrayLoad: return "array-load";
    case OpKind::LocalStore: return "local-store";
    case OpKind::ArrayStore: return "array-store";
    case OpKind::Stack: return "stack";
    case OpKind::Arithmetic: return "arithmetic";
    case OpKind::LocalInc: return "local-inc";
    case OpKind::Conversion: return "conversion";
    case OpKind::Compare: return "compare";
    case OpKind::Branch: return "branch";
    case OpKind::Switch: return "switch";
    case OpKind::Ret: return "ret";
    case OpKind::Return: return "return";
    case OpKind::FieldAccess: return "field-access";
    case OpKind::Invoke: return "invoke";
    case OpKind::New: return "new";
    case OpKind::NewArray: return "new-array";
    case OpKind::ArrayLength: return "array-length";
    case OpKind::Throw: return "throw";
    case OpKind::CheckCast: return "checkcast";
    case OpKind::InstanceOf: return "instanceof";
    case OpKind::Monitor: return "monitor";
    case OpKind::Wide: return "wide";
    case OpKind::Count: break;
    }
    return "?";
}

bool isConst(Op op) noexcept { return classify(op) == OpKind::Const; }

bool isLoad(Op op) noexcept { return classify(op) == OpKind::LocalLoad; }

bool isStore(Op op) noexcept { return classify(op) == OpKind::LocalStore; }

bool isLocalAccess(Op op) noexcept
{
    switch (classify(op)) {
    case OpKind::LocalLoad:
    case OpKind::LocalStore:
    case OpKind::LocalInc:
    case OpKind::Ret: return true;
    default: return false;
    }
}

bool isArrayLoad(Op op) noexcept { return classify(op) == OpKind::ArrayLoad; }

bool isArrayStore(Op op) noexcept { return classify(op) == OpKind::ArrayStore; }

bool isArray(Op op) noexcept
{
    switch (classify(op)) {
    case OpKind::ArrayLoad:
    case OpKind::ArrayStore:
    case OpKind::ArrayLength: return true;
    default: return false;
    }
}

bool isStackOp(Op op) noexcept { return classify(op) == OpKind::Stack; }

bool isArithmetic(Op op) noexcept { return classify(op) == OpKind::Arithmetic; }

bool isConversion(Op op) noexcept { return classify(op) == OpKind::Conversion; }

bool isCompare(Op op) noexcept { return classify(op) == OpKind::Compare; }

bool isBranch(Op op) noexcept { return classify(op) == OpKind::Branch; }

bool isConditionalBranch(Op op) noexcept
{
    if (!isBranch(op)) {
        return false;
    }
    // goto/goto_w/jsr/jsr_w 不是条件跳转。
    const ca::u8 v = static_cast<ca::u8>(op);
    return v != 0xa7 && v != 0xa8 && v != 0xc8 && v != 0xc9;
}

bool isGoto(Op op) noexcept
{
    return op == Op::GOTO || op == Op::GOTO_W;
}

bool isIfNull(Op op) noexcept { return op == Op::IFNULL || op == Op::IFNONNULL; }

bool isSwitch(Op op) noexcept { return classify(op) == OpKind::Switch; }

bool isReturn(Op op) noexcept { return classify(op) == OpKind::Return; }

bool isInvoke(Op op) noexcept { return classify(op) == OpKind::Invoke; }

bool isFieldAccess(Op op) noexcept { return classify(op) == OpKind::FieldAccess; }

bool isThrow(Op op) noexcept { return classify(op) == OpKind::Throw; }

bool isMonitor(Op op) noexcept { return classify(op) == OpKind::Monitor; }

bool isTerminator(Op op) noexcept
{
    switch (classify(op)) {
    case OpKind::Return:
    case OpKind::Branch:
    case OpKind::Switch:
    case OpKind::Throw:
    case OpKind::Ret: return true;
    default: return false;
    }
}

bool isPureComputation(Op op) noexcept
{
    switch (classify(op)) {
    case OpKind::Const:
    case OpKind::Arithmetic:
    case OpKind::Conversion:
    case OpKind::Stack: return true;
    default: return false;
    }
}

Op invertIf(Op op) noexcept
{
    switch (op) {
    case Op::IFEQ: return Op::IFNE;
    case Op::IFNE: return Op::IFEQ;
    case Op::IFLT: return Op::IFGE;
    case Op::IFGE: return Op::IFLT;
    case Op::IFGT: return Op::IFLE;
    case Op::IFLE: return Op::IFGT;
    case Op::IF_ICMPEQ: return Op::IF_ICMPNE;
    case Op::IF_ICMPNE: return Op::IF_ICMPEQ;
    case Op::IF_ICMPLT: return Op::IF_ICMPGE;
    case Op::IF_ICMPGE: return Op::IF_ICMPLT;
    case Op::IF_ICMPGT: return Op::IF_ICMPLE;
    case Op::IF_ICMPLE: return Op::IF_ICMPGT;
    case Op::IF_ACMPEQ: return Op::IF_ACMPNE;
    case Op::IF_ACMPNE: return Op::IF_ACMPEQ;
    case Op::IFNULL: return Op::IFNONNULL;
    case Op::IFNONNULL: return Op::IFNULL;
    default: return op;   // 非条件跳转原样返回（旧契约）
    }
}

StackEffect stackEffectOf(Op op) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(op);
    const auto dep = kStackDependent;

    if (v == 0x00) {
        return {0, 0};   // nop
    }
    if (v <= 0x11) {
        // 常量族 0x01..0x0f（cat2：lconst/dconst）+ bipush 0x10 / sipush 0x11。
        const bool cat2 = between(v, 0x09, 0x0a) || between(v, 0x0e, 0x0f);
        return {0, cat2 ? 2 : 1};
    }
    if (v == 0x12 || v == 0x13) {
        return {0, 1};   // ldc / ldc_w（单槽常量；cat2 必须走 ldc2_w）
    }
    if (v == 0x14) {
        return {0, 2};   // ldc2_w
    }
    if (v <= 0x2d) {
        return {0, loadStoreCat2(v) ? 2 : 1};   // load 族（含紧凑）
    }
    if (v <= 0x35) {
        // 数组读：ref + index 弹 2；laload/daload 产 cat2。
        return {2, (v == 0x2f || v == 0x31) ? 2 : 1};
    }
    if (v <= 0x4e) {
        return {loadStoreCat2(v) ? 2 : 1, 0};   // store 族（含紧凑）
    }
    if (v <= 0x56) {
        // 数组写：value + index + ref = 3；cat2 值为 4。
        return {(v == 0x50 || v == 0x52) ? 4 : 3, 0};
    }
    switch (op) {
    // 栈操作：dup 族按净新增计数，pop 按净移除计数。
    case Op::POP: return {1, 0};
    case Op::POP2: return {2, 0};
    case Op::DUP:
    case Op::DUP_X1:
    case Op::DUP_X2: return {0, 1};
    case Op::DUP2:
    case Op::DUP2_X1:
    case Op::DUP2_X2: return {0, 2};
    case Op::SWAP: return {0, 0};
    default: break;
    }
    if (between(v, 0x60, 0x73)) {
        // add/sub/mul/div/rem：类型按 v&3（i/l/f/d 同组步进 4）。
        switch (v & 3) {
        case 0: return {2, 1};   // int
        case 1: return {4, 2};   // long
        case 2: return {2, 1};   // float
        default: return {4, 2};   // double
        }
    }
    if (between(v, 0x74, 0x77)) {
        return (v & 1) != 0 ? StackEffect{2, 2} : StackEffect{1, 1};   // *neg
    }
    if (between(v, 0x78, 0x7d)) {
        // 移位：long 被移位数是 int（弹 2+1），结果 long。
        return (v & 1) != 0 ? StackEffect{3, 2} : StackEffect{2, 1};
    }
    if (between(v, 0x7e, 0x83)) {
        return (v & 1) != 0 ? StackEffect{4, 2} : StackEffect{2, 1};   // and/or/xor
    }
    if (v == 0x84) {
        return {0, 0};   // iinc
    }
    if (between(v, 0x85, 0x93)) {
        // 数值转换 {pop, push}：i2l..i2s 共 15 项。
        static constexpr ca::i8 kConv[15][2] = {
            {1, 2},   // i2l
            {1, 1},   // i2f
            {1, 2},   // i2d
            {2, 1},   // l2i
            {2, 1},   // l2f
            {2, 2},   // l2d
            {1, 1},   // f2i
            {1, 2},   // f2l
            {1, 2},   // f2d
            {2, 1},   // d2i
            {2, 2},   // d2l
            {2, 1},   // d2f
            {1, 1},   // i2b
            {1, 1},   // i2c
            {1, 1},   // i2s
        };
        const auto& e = kConv[v - 0x85];
        return {e[0], e[1]};
    }
    if (between(v, 0x94, 0x98)) {
        // 比较：操作数弹 cat2 对（lcmp/dcmp* 4 槽，fcmp* 2 槽），结果 int。
        return {(v == 0x94 || v >= 0x97) ? 4 : 2, 1};
    }
    if (between(v, 0x99, 0x9e) || v == 0xc6 || v == 0xc7) {
        return {1, 0};   // ifeq..ifle / ifnull / ifnonnull
    }
    if (between(v, 0x9f, 0xa6)) {
        return {2, 0};   // if_icmp* / if_acmp*
    }
    if (v == 0xa7 || v == 0xc8) {
        return {0, 0};   // goto / goto_w
    }
    if (v == 0xa8 || v == 0xc9) {
        return {0, 1};   // jsr / jsr_w（目标侧压 returnAddress）
    }
    if (v == 0xa9) {
        return {0, 0};   // ret
    }
    if (v == 0xaa || v == 0xab) {
        return {1, 0};   // tableswitch / lookupswitch（弹选择子）
    }
    if (between(v, 0xac, 0xb1)) {
        if (v == 0xb1) {
            return {0, 0};   // return
        }
        return (v == 0xad || v == 0xaf) ? StackEffect{2, 0} : StackEffect{1, 0};
    }
    if (v == 0xb2) {
        return {0, dep};   // getstatic
    }
    if (v == 0xb3) {
        return {dep, 0};   // putstatic
    }
    if (v == 0xb4) {
        return {1, dep};   // getfield（含对象引用）
    }
    if (v == 0xb5) {
        return {dep, 0};   // putfield（值 + 对象引用）
    }
    if (between(v, 0xb6, 0xba)) {
        return {dep, dep};   // invoke 族
    }
    if (v == 0xbb) {
        return {0, 1};   // new
    }
    if (v == 0xbc || v == 0xbd) {
        return {1, 1};   // newarray / anewarray（弹长度，压引用）
    }
    if (v == 0xbe) {
        return {1, 1};   // arraylength
    }
    if (v == 0xbf) {
        return {1, 0};   // athrow
    }
    if (v == 0xc0) {
        return {1, 1};   // checkcast
    }
    if (v == 0xc1) {
        return {1, 1};   // instanceof
    }
    if (v == 0xc2 || v == 0xc3) {
        return {1, 0};   // monitorenter / monitorexit
    }
    if (v == 0xc4) {
        return {0, 0};   // wide（前缀，解码产物不携带）
    }
    if (v == 0xc5) {
        return {dep, 1};   // multianewarray（弹出数 = 操作数 dims）
    }
    return {0, 0};   // 枚举外（保留码）：无意义值，调用方不应触达
}

ca::i32 minStackDepthOf(Op op) noexcept
{
    switch (op) {
    case Op::DUP: return 1;
    case Op::DUP_X1: return 2;
    case Op::DUP_X2: return 3;
    case Op::DUP2: return 2;
    case Op::DUP2_X1: return 3;
    case Op::DUP2_X2: return 4;
    case Op::SWAP: return 2;
    case Op::PUTSTATIC:
    case Op::PUTFIELD:
    case Op::INVOKEVIRTUAL:
    case Op::INVOKESPECIAL:
    case Op::INVOKESTATIC:
    case Op::INVOKEINTERFACE:
    case Op::INVOKEDYNAMIC:
    case Op::MULTIANEWARRAY: return kStackDependent;
    default: break;
    }
    // 其余族：执行前至少要有「将被净弹出的槽位」那么多（dup 族已在上面覆写）。
    return stackEffectOf(op).pop;
}

ca::i32 netStackEffectOf(Op op) noexcept
{
    const StackEffect e = stackEffectOf(op);
    if (e.pop == kStackDependent || e.push == kStackDependent) {
        return kStackDependent;
    }
    return e.push - e.pop;
}

const char* to_string(StackError e) noexcept
{
    switch (e) {
    case StackError::Ok: return "ok";
    case StackError::NotFieldOp: return "not a field-access op";
    case StackError::NotInvokeOp: return "not an invoke op";
    case StackError::BadDescriptor: return "bad descriptor";
    }
    return "?";
}

Result<StackEffect, StackError> fieldStackEffect(Op op, std::string_view fieldDesc)
{
    if (!isFieldAccess(op)) {
        return Err(StackError::NotFieldOp);
    }
    // 字段描述符不可能为 V（libjbc 的 typeSlots 对方法返回 void 返回 0，
    // 字段场景必须显式拒绝）。
    if (fieldDesc == "V") {
        return Err(StackError::BadDescriptor);
    }
    const ca::i32 n = jbc::analysis::fieldSlots(fieldDesc);
    if (n < 0) {
        return Err(StackError::BadDescriptor);
    }
    switch (op) {
    case Op::GETSTATIC: return Ok(StackEffect{0, n});
    case Op::PUTSTATIC: return Ok(StackEffect{n, 0});
    case Op::GETFIELD: return Ok(StackEffect{1, n});
    case Op::PUTFIELD: return Ok(StackEffect{n + 1, 0});
    default: return Err(StackError::NotFieldOp);
    }
}

Result<StackEffect, StackError> invokeStackEffect(Op op, std::string_view methodDesc)
{
    if (!isInvoke(op)) {
        return Err(StackError::NotInvokeOp);
    }
    const ca::i32 args = jbc::analysis::methodArgSlots(methodDesc);
    const ca::i32 ret  = jbc::analysis::methodRetSlots(methodDesc);
    if (args < 0 || ret < 0) {
        return Err(StackError::BadDescriptor);
    }
    const bool hasReceiver = op != Op::INVOKESTATIC && op != Op::INVOKEDYNAMIC;
    return Ok(StackEffect{args + (hasReceiver ? 1 : 0), ret});
}

}   // namespace mjbase::asm_insn
