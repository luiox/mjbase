/// @file        evaluator.cpp
/// @brief       具体求值栈机：CodeDom 只读遍历、resolveLabel 控制流、步数熔断。
#include "evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mjbase::asm_evaluator {
namespace {

// ---------------------------------------------------------------------------
// Java 数值语义核心。整数运算一律经无符号位运算回读，避免有符号溢出 UB；
// 浮点转换按 JLS：NaN→0、越界饱和、向零截断。
// ---------------------------------------------------------------------------

template <typename To, typename From>
To bitCast(const From& from) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bitCast size mismatch");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

ca::i32 addI(ca::i32 a, ca::i32 b) noexcept
{
    return bitCast<ca::i32>(bitCast<ca::u32>(a) + bitCast<ca::u32>(b));
}

ca::i32 negI(ca::i32 v) noexcept
{
    return bitCast<ca::i32>(0u - bitCast<ca::u32>(v));
}

ca::i32 shlI(ca::i32 v, ca::i32 d) noexcept
{
    return bitCast<ca::i32>(bitCast<ca::u32>(v) << (static_cast<ca::u32>(d) & 0x1fu));
}

ca::i32 shrI(ca::i32 v, ca::i32 d) noexcept
{
    const ca::u32 s = static_cast<ca::u32>(d) & 0x1fu;
    const ca::u32 bits = bitCast<ca::u32>(v);
    if (s == 0 || v >= 0) return bitCast<ca::i32>(bits >> s);
    const ca::u32 sign = ~ca::u32{0} << (32u - s);
    return bitCast<ca::i32>((bits >> s) | sign);
}

ca::i32 ushrI(ca::i32 v, ca::i32 d) noexcept
{
    return bitCast<ca::i32>(bitCast<ca::u32>(v) >> (static_cast<ca::u32>(d) & 0x1fu));
}

/// 除法（除数非零已由调用方检查）：MIN / -1 回落 MIN（Java 溢出语义）。
ca::i32 divI(ca::i32 a, ca::i32 b) noexcept
{
    if (a == (std::numeric_limits<ca::i32>::min)() && b == -1) return a;
    return a / b;
}

ca::i32 remI(ca::i32 a, ca::i32 b) noexcept
{
    if (a == (std::numeric_limits<ca::i32>::min)() && b == -1) return 0;
    return a % b;
}

ca::i64 addL(ca::i64 a, ca::i64 b) noexcept
{
    return bitCast<ca::i64>(bitCast<ca::u64>(a) + bitCast<ca::u64>(b));
}

ca::i64 negL(ca::i64 v) noexcept
{
    return bitCast<ca::i64>(0ull - bitCast<ca::u64>(v));
}

ca::i64 shlL(ca::i64 v, ca::i32 d) noexcept
{
    return bitCast<ca::i64>(bitCast<ca::u64>(v) << (static_cast<ca::u32>(d) & 0x3fu));
}

ca::i64 shrL(ca::i64 v, ca::i32 d) noexcept
{
    const ca::u32 s = static_cast<ca::u32>(d) & 0x3fu;
    const ca::u64 bits = bitCast<ca::u64>(v);
    if (s == 0 || v >= 0) return bitCast<ca::i64>(bits >> s);
    const ca::u64 sign = ~ca::u64{0} << (64u - s);
    return bitCast<ca::i64>((bits >> s) | sign);
}

ca::i64 ushrL(ca::i64 v, ca::i32 d) noexcept
{
    return bitCast<ca::i64>(bitCast<ca::u64>(v) >> (static_cast<ca::u32>(d) & 0x3fu));
}

ca::i64 divL(ca::i64 a, ca::i64 b) noexcept
{
    if (a == (std::numeric_limits<ca::i64>::min)() && b == -1) return a;
    return a / b;
}

ca::i64 remL(ca::i64 a, ca::i64 b) noexcept
{
    if (a == (std::numeric_limits<ca::i64>::min)() && b == -1) return 0;
    return a % b;
}

/// l2i：低 32 位截断（JVMS 语义），经位运算避免有符号窄化实现定义。
ca::i32 longToI32(ca::i64 v) noexcept
{
    return bitCast<ca::i32>(static_cast<ca::u32>(bitCast<ca::u64>(v)));
}

/// f2i/d2i：NaN→0，越界饱和，其余向零截断。
ca::i32 doubleToI32(ca::f64 v) noexcept
{
    if (std::isnan(v)) return 0;
    if (v >= 2147483647.0) return (std::numeric_limits<ca::i32>::max)();
    if (v <= -2147483648.0) return (std::numeric_limits<ca::i32>::min)();
    return static_cast<ca::i32>(v);
}

/// f2l/d2l：同上；比较阈取 2^63（可精确表示的浮点界）。
ca::i64 doubleToI64(ca::f64 v) noexcept
{
    if (std::isnan(v)) return 0;
    if (v >= 9223372036854775808.0) return (std::numeric_limits<ca::i64>::max)();
    if (v <= -9223372036854775808.0) return (std::numeric_limits<ca::i64>::min)();
    return static_cast<ca::i64>(v);
}

ca::i32 toByte(ca::i32 v) noexcept
{
    const ca::i32 b = v & 0xff;
    return b >= 0x80 ? b - 0x100 : b;
}

ca::i32 toChar(ca::i32 v) noexcept
{
    return v & 0xffff;
}

ca::i32 toShort(ca::i32 v) noexcept
{
    const ca::i32 s = v & 0xffff;
    return s >= 0x8000 ? s - 0x10000 : s;
}

/// fcmpl/fcmpg/dcmpl/dcmpg 的比较核心：NaN 时 *L 得 -1、*G 得 +1，
/// NaN 判定必须先于大小比较。
ca::i32 cmpF(bool nanLow, ca::f32 a, ca::f32 b) noexcept
{
    if (std::isnan(a) || std::isnan(b)) return nanLow ? -1 : 1;
    if (a > b) return 1;
    if (a < b) return -1;
    return 0;
}

ca::i32 cmpD(bool nanLow, ca::f64 a, ca::f64 b) noexcept
{
    if (std::isnan(a) || std::isnan(b)) return nanLow ? -1 : 1;
    if (a > b) return 1;
    if (a < b) return -1;
    return 0;
}

/// @brief 数值二元运算（算术/移位/位运算/比较）。类别不符返回 BadState；
/// 整数除零返回 BadState（JVM 抛 ArithmeticException，求值器不猜结果）。
Result<Value, EvalError> applyBinary(jbc::core::Op op, const Value& l, const Value& r,
                                     jbc::ir::InsnId at)
{
    const EvalError bad{EvalError::Kind::BadState, at};
    switch (op) {
    // ---- i32 x i32 -> i32 ----
    case jbc::core::Op::IADD:
    case jbc::core::Op::ISUB:
    case jbc::core::Op::IMUL:
    case jbc::core::Op::IDIV:
    case jbc::core::Op::IREM:
    case jbc::core::Op::ISHL:
    case jbc::core::Op::ISHR:
    case jbc::core::Op::IUSHR:
    case jbc::core::Op::IAND:
    case jbc::core::Op::IOR:
    case jbc::core::Op::IXOR:
    {
        if (l.kind != Value::Kind::I32 || r.kind != Value::Kind::I32) return Err(bad);
        ca::i32 v = 0;
        switch (op) {
        case jbc::core::Op::IADD: v = addI(l.i32, r.i32); break;
        case jbc::core::Op::ISUB: v = addI(l.i32, negI(r.i32)); break;
        case jbc::core::Op::IMUL:
            v = bitCast<ca::i32>(bitCast<ca::u32>(l.i32) * bitCast<ca::u32>(r.i32));
            break;
        case jbc::core::Op::IDIV:
            if (r.i32 == 0) return Err(bad);
            v = divI(l.i32, r.i32);
            break;
        case jbc::core::Op::IREM:
            if (r.i32 == 0) return Err(bad);
            v = remI(l.i32, r.i32);
            break;
        case jbc::core::Op::ISHL: v = shlI(l.i32, r.i32); break;
        case jbc::core::Op::ISHR: v = shrI(l.i32, r.i32); break;
        case jbc::core::Op::IUSHR: v = ushrI(l.i32, r.i32); break;
        case jbc::core::Op::IAND: v = l.i32 & r.i32; break;
        case jbc::core::Op::IOR: v = l.i32 | r.i32; break;
        default: v = l.i32 ^ r.i32; break;   // IXOR
        }
        return Ok(Value::ofI32(v));
    }
    // ---- i64 x i64 -> i64 ----
    case jbc::core::Op::LADD:
    case jbc::core::Op::LSUB:
    case jbc::core::Op::LMUL:
    case jbc::core::Op::LDIV:
    case jbc::core::Op::LREM:
    case jbc::core::Op::LAND:
    case jbc::core::Op::LOR:
    case jbc::core::Op::LXOR:
    {
        if (l.kind != Value::Kind::I64 || r.kind != Value::Kind::I64) return Err(bad);
        ca::i64 v = 0;
        switch (op) {
        case jbc::core::Op::LADD: v = addL(l.i64, r.i64); break;
        case jbc::core::Op::LSUB: v = addL(l.i64, negL(r.i64)); break;
        case jbc::core::Op::LMUL:
            v = bitCast<ca::i64>(bitCast<ca::u64>(l.i64) * bitCast<ca::u64>(r.i64));
            break;
        case jbc::core::Op::LDIV:
            if (r.i64 == 0) return Err(bad);
            v = divL(l.i64, r.i64);
            break;
        case jbc::core::Op::LREM:
            if (r.i64 == 0) return Err(bad);
            v = remL(l.i64, r.i64);
            break;
        case jbc::core::Op::LAND: v = l.i64 & r.i64; break;
        case jbc::core::Op::LOR: v = l.i64 | r.i64; break;
        default: v = l.i64 ^ r.i64; break;   // LXOR
        }
        return Ok(Value::ofI64(v));
    }
    // ---- i64 x i32(移位数) -> i64 ----
    case jbc::core::Op::LSHL:
    case jbc::core::Op::LSHR:
    case jbc::core::Op::LUSHR:
    {
        if (l.kind != Value::Kind::I64 || r.kind != Value::Kind::I32) return Err(bad);
        switch (op) {
        case jbc::core::Op::LSHL: return Ok(Value::ofI64(shlL(l.i64, r.i32)));
        case jbc::core::Op::LSHR: return Ok(Value::ofI64(shrL(l.i64, r.i32)));
        default: return Ok(Value::ofI64(ushrL(l.i64, r.i32)));
        }
    }
    // ---- f32 x f32 -> f32 ----
    case jbc::core::Op::FADD:
    case jbc::core::Op::FSUB:
    case jbc::core::Op::FMUL:
    case jbc::core::Op::FDIV:
    {
        if (l.kind != Value::Kind::F32 || r.kind != Value::Kind::F32) return Err(bad);
        ca::f32 v = 0.0f;
        switch (op) {
        case jbc::core::Op::FADD: v = l.f32 + r.f32; break;
        case jbc::core::Op::FSUB: v = l.f32 - r.f32; break;
        case jbc::core::Op::FMUL: v = l.f32 * r.f32; break;
        default: v = l.f32 / r.f32; break;   // FDIV
        }
        return Ok(Value::ofF32(v));
    }
    // frem 与 IEEE 754 remainder 一致（std::fmod；除数为 0 得 NaN，不报错）。
    case jbc::core::Op::FREM:
    {
        if (l.kind != Value::Kind::F32 || r.kind != Value::Kind::F32) return Err(bad);
        return Ok(Value::ofF32(std::fmod(l.f32, r.f32)));
    }
    // ---- f64 x f64 -> f64 ----
    case jbc::core::Op::DADD:
    case jbc::core::Op::DSUB:
    case jbc::core::Op::DMUL:
    case jbc::core::Op::DDIV:
    {
        if (l.kind != Value::Kind::F64 || r.kind != Value::Kind::F64) return Err(bad);
        ca::f64 v = 0.0;
        switch (op) {
        case jbc::core::Op::DADD: v = l.f64 + r.f64; break;
        case jbc::core::Op::DSUB: v = l.f64 - r.f64; break;
        case jbc::core::Op::DMUL: v = l.f64 * r.f64; break;
        default: v = l.f64 / r.f64; break;   // DDIV
        }
        return Ok(Value::ofF64(v));
    }
    case jbc::core::Op::DREM:
    {
        if (l.kind != Value::Kind::F64 || r.kind != Value::Kind::F64) return Err(bad);
        return Ok(Value::ofF64(std::fmod(l.f64, r.f64)));
    }
    // ---- 比较产出 i32 ----
    case jbc::core::Op::LCMP:
    {
        if (l.kind != Value::Kind::I64 || r.kind != Value::Kind::I64) return Err(bad);
        return Ok(Value::ofI32(static_cast<ca::i32>((l.i64 > r.i64) - (l.i64 < r.i64))));
    }
    case jbc::core::Op::FCMPL:
    case jbc::core::Op::FCMPG:
    {
        if (l.kind != Value::Kind::F32 || r.kind != Value::Kind::F32) return Err(bad);
        return Ok(Value::ofI32(cmpF(op == jbc::core::Op::FCMPL, l.f32, r.f32)));
    }
    case jbc::core::Op::DCMPL:
    case jbc::core::Op::DCMPG:
    {
        if (l.kind != Value::Kind::F64 || r.kind != Value::Kind::F64) return Err(bad);
        return Ok(Value::ofI32(cmpD(op == jbc::core::Op::DCMPL, l.f64, r.f64)));
    }
    default: return Err(EvalError{EvalError::Kind::Unsupported, at});
    }
}

/// @brief 一元运算（取负 + 全部数值转换）。类别不符返回 BadState。
Result<Value, EvalError> applyUnary(jbc::core::Op op, const Value& v, jbc::ir::InsnId at)
{
    const EvalError bad{EvalError::Kind::BadState, at};
    const bool isI32 = v.kind == Value::Kind::I32;
    const bool isI64 = v.kind == Value::Kind::I64;
    const bool isF32 = v.kind == Value::Kind::F32;
    const bool isF64 = v.kind == Value::Kind::F64;
    Value out;
    switch (op) {
    case jbc::core::Op::INEG:
        if (!isI32) return Err(bad);
        out = Value::ofI32(negI(v.i32));
        break;
    case jbc::core::Op::LNEG:
        if (!isI64) return Err(bad);
        out = Value::ofI64(negL(v.i64));
        break;
    case jbc::core::Op::FNEG:
        if (!isF32) return Err(bad);
        out = Value::ofF32(-v.f32);
        break;
    case jbc::core::Op::DNEG:
        if (!isF64) return Err(bad);
        out = Value::ofF64(-v.f64);
        break;
    case jbc::core::Op::I2L:
        if (!isI32) return Err(bad);
        out = Value::ofI64(v.i32);
        break;
    case jbc::core::Op::I2F:
        if (!isI32) return Err(bad);
        out = Value::ofF32(static_cast<ca::f32>(v.i32));
        break;
    case jbc::core::Op::I2D:
        if (!isI32) return Err(bad);
        out = Value::ofF64(static_cast<ca::f64>(v.i32));
        break;
    case jbc::core::Op::I2B:
        if (!isI32) return Err(bad);
        out = Value::ofI32(toByte(v.i32));
        break;
    case jbc::core::Op::I2C:
        if (!isI32) return Err(bad);
        out = Value::ofI32(toChar(v.i32));
        break;
    case jbc::core::Op::I2S:
        if (!isI32) return Err(bad);
        out = Value::ofI32(toShort(v.i32));
        break;
    case jbc::core::Op::L2I:
        if (!isI64) return Err(bad);
        out = Value::ofI32(longToI32(v.i64));
        break;
    case jbc::core::Op::L2F:
        if (!isI64) return Err(bad);
        out = Value::ofF32(static_cast<ca::f32>(v.i64));
        break;
    case jbc::core::Op::L2D:
        if (!isI64) return Err(bad);
        out = Value::ofF64(static_cast<ca::f64>(v.i64));
        break;
    case jbc::core::Op::F2I:
        if (!isF32) return Err(bad);
        out = Value::ofI32(doubleToI32(static_cast<ca::f64>(v.f32)));
        break;
    case jbc::core::Op::F2L:
        if (!isF32) return Err(bad);
        out = Value::ofI64(doubleToI64(static_cast<ca::f64>(v.f32)));
        break;
    case jbc::core::Op::F2D:
        if (!isF32) return Err(bad);
        out = Value::ofF64(static_cast<ca::f64>(v.f32));
        break;
    case jbc::core::Op::D2I:
        if (!isF64) return Err(bad);
        out = Value::ofI32(doubleToI32(v.f64));
        break;
    case jbc::core::Op::D2L:
        if (!isF64) return Err(bad);
        out = Value::ofI64(doubleToI64(v.f64));
        break;
    case jbc::core::Op::D2F:
        if (!isF64) return Err(bad);
        out = Value::ofF32(static_cast<ca::f32>(v.f64));
        break;
    default: return Err(EvalError{EvalError::Kind::Unsupported, at});
    }
    return Ok(out);
}

/// 一元 int 分支谓词。
bool testBranchUnary(jbc::core::Op op, ca::i32 v) noexcept
{
    switch (op) {
    case jbc::core::Op::IFEQ: return v == 0;
    case jbc::core::Op::IFNE: return v != 0;
    case jbc::core::Op::IFLT: return v < 0;
    case jbc::core::Op::IFGE: return v >= 0;
    case jbc::core::Op::IFGT: return v > 0;
    default: return v <= 0;   // IFLE
    }
}

/// 二元 int 分支谓词。
bool testBranchBinary(jbc::core::Op op, ca::i32 a, ca::i32 b) noexcept
{
    switch (op) {
    case jbc::core::Op::IF_ICMPEQ: return a == b;
    case jbc::core::Op::IF_ICMPNE: return a != b;
    case jbc::core::Op::IF_ICMPLT: return a < b;
    case jbc::core::Op::IF_ICMPGE: return a >= b;
    case jbc::core::Op::IF_ICMPGT: return a > b;
    default: return a <= b;   // IF_ICMPLE
    }
}

/// @brief 局部变量访问的槽号。
///
/// _n 短形式按 JVMS opcode 布局取模：load 短形式区间 [0x1a,0x2d]、
/// store 短形式区间 [0x3b,0x4e] 内（offset & 3）即槽号；显式索引形式
/// （含 wide，解码器已把 u16 展开进 a）读 a。
ca::u32 localIndexOf(jbc::core::Op op, const jbc::ir::Insn& in) noexcept
{
    const ca::u32 raw = static_cast<ca::u32>(op);
    if (raw >= 0x1au && raw <= 0x2du) return (raw - 0x1au) & 0x3u;
    if (raw >= 0x3bu && raw <= 0x4eu) return (raw - 0x3bu) & 0x3u;
    return static_cast<ca::u32>(in.a);
}

/// 单步执行结果：顺序前进 / 跳转 / 类型化返回 / void 返回。
struct StepResult
{
    enum class Kind : ca::u8 {
        Next = 0,
        Jump,
        RetValue,
        RetVoid,
    };

    static StepResult next() noexcept
    {
        StepResult s;
        s.kind = Kind::Next;
        return s;
    }

    static StepResult jump(ca::usize to) noexcept
    {
        StepResult s;
        s.kind = Kind::Jump;
        s.jumpTo = to;
        return s;
    }

    static StepResult ret(Value value) noexcept
    {
        StepResult s;
        s.kind = Kind::RetValue;
        s.value = value;
        return s;
    }

    static StepResult retVoid() noexcept
    {
        StepResult s;
        s.kind = Kind::RetVoid;
        return s;
    }

    Kind kind = Kind::Next;
    ca::usize jumpTo = 0;
    Value value;
};

constexpr ca::usize kNoOrderIdx = ~ca::usize{0};

/// @brief 具体栈机：持有操作数栈与局部变量，遍历 CodeDom 执行。
class Machine
{
public:
    Machine(const jbc::ir::CodeDom& dom, const MethodSig& sig, const Options& options) noexcept
        : dom_(dom), sig_(sig), opts_(options)
    {}

    Result<EvalOutcome, EvalError> run(const std::vector<Value>& args)
    {
        buildSlotIndex();
        const auto inject = injectArgs(args);
        if (inject.is_err()) return Err(inject.unwrap_err());

        const std::vector<jbc::ir::InsnId>& order = dom_.order();
        ca::usize pc = 0;
        ca::u32 steps = 0;
        while (true) {
            if (pc >= order.size()) {
                // 控制流落到方法末尾边界：没有 return 的完成是坏状态。
                return Err(EvalError{EvalError::Kind::BadState, jbc::ir::kNoInsn});
            }
            if (steps >= opts_.maxSteps) {
                cur_ = order[pc];
                return Err(EvalError{EvalError::Kind::StepLimit, cur_});
            }
            ++steps;
            cur_ = order[pc];
            const auto s = step();
            if (s.is_err()) return Err(s.unwrap_err());
            const StepResult sr = s.unwrap();
            switch (sr.kind) {
            case StepResult::Kind::Next: ++pc; break;
            case StepResult::Kind::Jump: pc = sr.jumpTo; break;
            case StepResult::Kind::RetValue: {
                EvalOutcome out;
                out.kind = EvalOutcome::Kind::Value;
                out.value = sr.value;
                return Ok(out);
            }
            case StepResult::Kind::RetVoid: return Ok(EvalOutcome{});
            }
        }
    }

private:
    void buildSlotIndex()
    {
        ca::u32 maxSlot = 0;
        for (const jbc::ir::InsnId& id : dom_.order()) {
            maxSlot = (std::max)(maxSlot, id.slot);
        }
        idxBySlot_.assign(static_cast<ca::usize>(maxSlot) + 1, kNoOrderIdx);
        const std::vector<jbc::ir::InsnId>& order = dom_.order();
        for (ca::usize i = 0; i < order.size(); ++i) {
            idxBySlot_[order[i].slot] = i;
        }
    }

    /// 描述符驱动入参注入：实例方法 local0 = this；cat2 参数占双槽（高槽 Top）。
    Result<void, EvalError> injectArgs(const std::vector<Value>& args)
    {
        const EvalError bad{EvalError::Kind::BadState, jbc::ir::kNoInsn};
        if (args.size() != sig_.argCount()) return Err(bad);
        locals_.clear();
        std::size_t i = 0;
        if (!sig_.isStatic()) {
            if (!args[i].isRef()) return Err(bad);
            locals_.push_back(args[i]);
            ++i;
        }
        for (ca::usize p = 0; p < sig_.paramCount(); ++p) {
            const Value& v = args[i++];
            const SigKind k = sig_.paramKind(p);
            bool matches = false;
            switch (k) {
            case SigKind::I32: matches = v.kind == Value::Kind::I32; break;
            case SigKind::I64: matches = v.kind == Value::Kind::I64; break;
            case SigKind::F32: matches = v.kind == Value::Kind::F32; break;
            case SigKind::F64: matches = v.kind == Value::Kind::F64; break;
            case SigKind::Ref: matches = v.isRef(); break;
            case SigKind::Void: matches = false; break;
            }
            if (!matches) return Err(bad);
            locals_.push_back(v);
            if (isCat2(k)) locals_.push_back(Value::top());
        }
        return Ok();
    }

    EvalError err(EvalError::Kind kind) const noexcept { return EvalError{kind, cur_}; }

    void push(const Value& v) { stack_.push_back(v); }

    Result<Value, EvalError> popAny()
    {
        if (stack_.empty()) return Err(err(EvalError::Kind::BadState));
        const Value v = stack_.back();
        stack_.pop_back();
        return Ok(v);
    }

    Result<Value, EvalError> popKind(Value::Kind kind)
    {
        auto v = popAny();
        if (v.is_err()) return v;
        if (v.unwrap().kind != kind) return Err(err(EvalError::Kind::BadState));
        return v;
    }

    /// 弹一个 cat1 值（任何非 cat2 类别）；cat2 顶是整形形式非法。
    Result<Value, EvalError> popCat1()
    {
        auto v = popAny();
        if (v.is_err()) return v;
        if (v.unwrap().isCat2()) return Err(err(EvalError::Kind::BadState));
        return v;
    }

    Result<Value, EvalError> loadLocal(ca::u32 idx, Value::Kind want)
    {
        if (idx >= locals_.size() || locals_[idx].kind != want) {
            return Err(err(EvalError::Kind::BadState));
        }
        return Ok(locals_[idx]);
    }

    Result<void, EvalError> storeLocal(ca::u32 idx, const Value& v)
    {
        const ca::usize need = static_cast<ca::usize>(idx) + (v.isCat2() ? 2 : 1);
        if (locals_.size() < need) locals_.resize(need, Value::top());
        locals_[idx] = v;
        if (v.isCat2()) locals_[static_cast<ca::usize>(idx) + 1] = Value::top();
        return Ok();
    }

    /// 分支标签 → order 下标。标签无效、指向方法末尾边界或锚定指令不在
    /// 当前顺序中均为坏状态（一致 DOM 下 append 校验已排除前两者）。
    Result<ca::usize, EvalError> jumpTarget(jbc::ir::LabelId label)
    {
        const auto t = dom_.resolveLabel(label);
        if (t.is_err()) return Err(err(EvalError::Kind::BadState));
        const jbc::ir::LabelTarget target = t.unwrap();
        if (target.isEnd()) return Err(err(EvalError::Kind::BadState));
        if (target.insn.slot >= idxBySlot_.size() ||
            idxBySlot_[target.insn.slot] == kNoOrderIdx) {
            return Err(err(EvalError::Kind::BadState));
        }
        return Ok(idxBySlot_[target.insn.slot]);
    }

    /// 条件分支：taken 时走标签目标，否则顺序前进。
    Result<StepResult, EvalError> branch(bool taken, jbc::ir::LabelId label)
    {
        if (!taken) return Ok(StepResult::next());
        const auto t = jumpTarget(label);
        if (t.is_err()) return Err(t.unwrap_err());
        return Ok(StepResult::jump(t.unwrap()));
    }

    Result<StepResult, EvalError> step()
    {
        const jbc::ir::Insn* in = dom_.peek(cur_);
        if (in == nullptr) return Err(err(EvalError::Kind::BadState));
        const jbc::core::Op op = in->op;

        switch (op) {
        // ---- 常量与 nop ----
        case jbc::core::Op::NOP: return Ok(StepResult::next());
        case jbc::core::Op::ACONST_NULL:
            push(Value::ofNull());
            return Ok(StepResult::next());
        case jbc::core::Op::ICONST_M1:
        case jbc::core::Op::ICONST_0:
        case jbc::core::Op::ICONST_1:
        case jbc::core::Op::ICONST_2:
        case jbc::core::Op::ICONST_3:
        case jbc::core::Op::ICONST_4:
        case jbc::core::Op::ICONST_5:
            push(Value::ofI32(static_cast<ca::i32>(op) - static_cast<ca::i32>(jbc::core::Op::ICONST_0)));
            return Ok(StepResult::next());
        case jbc::core::Op::LCONST_0:
        case jbc::core::Op::LCONST_1:
            push(Value::ofI64(static_cast<ca::i64>(static_cast<ca::i32>(op) -
                                                  static_cast<ca::i32>(jbc::core::Op::LCONST_0))));
            return Ok(StepResult::next());
        case jbc::core::Op::FCONST_0:
        case jbc::core::Op::FCONST_1:
        case jbc::core::Op::FCONST_2:
            push(Value::ofF32(static_cast<ca::f32>(static_cast<ca::i32>(op) -
                                                   static_cast<ca::i32>(jbc::core::Op::FCONST_0))));
            return Ok(StepResult::next());
        case jbc::core::Op::DCONST_0:
        case jbc::core::Op::DCONST_1:
            push(Value::ofF64(static_cast<ca::f64>(static_cast<ca::i32>(op) -
                                                   static_cast<ca::i32>(jbc::core::Op::DCONST_0))));
            return Ok(StepResult::next());
        case jbc::core::Op::BIPUSH:
        case jbc::core::Op::SIPUSH:
            push(Value::ofI32(in->a));
            return Ok(StepResult::next());

        // ---- load：类别不符 / 读 Top / 读未初始化槽都是 BadState ----
        case jbc::core::Op::ILOAD:
        case jbc::core::Op::ILOAD_0:
        case jbc::core::Op::ILOAD_1:
        case jbc::core::Op::ILOAD_2:
        case jbc::core::Op::ILOAD_3:
        {
            const auto v = loadLocal(localIndexOf(op, *in), Value::Kind::I32);
            if (v.is_err()) return Err(v.unwrap_err());
            push(v.unwrap());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::LLOAD:
        case jbc::core::Op::LLOAD_0:
        case jbc::core::Op::LLOAD_1:
        case jbc::core::Op::LLOAD_2:
        case jbc::core::Op::LLOAD_3:
        {
            const auto v = loadLocal(localIndexOf(op, *in), Value::Kind::I64);
            if (v.is_err()) return Err(v.unwrap_err());
            push(v.unwrap());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::FLOAD:
        case jbc::core::Op::FLOAD_0:
        case jbc::core::Op::FLOAD_1:
        case jbc::core::Op::FLOAD_2:
        case jbc::core::Op::FLOAD_3:
        {
            const auto v = loadLocal(localIndexOf(op, *in), Value::Kind::F32);
            if (v.is_err()) return Err(v.unwrap_err());
            push(v.unwrap());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::DLOAD:
        case jbc::core::Op::DLOAD_0:
        case jbc::core::Op::DLOAD_1:
        case jbc::core::Op::DLOAD_2:
        case jbc::core::Op::DLOAD_3:
        {
            const auto v = loadLocal(localIndexOf(op, *in), Value::Kind::F64);
            if (v.is_err()) return Err(v.unwrap_err());
            push(v.unwrap());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::ALOAD:
        case jbc::core::Op::ALOAD_0:
        case jbc::core::Op::ALOAD_1:
        case jbc::core::Op::ALOAD_2:
        case jbc::core::Op::ALOAD_3:
        {
            const auto v = loadLocal(localIndexOf(op, *in), Value::Kind::Ref);
            if (v.is_err()) return Err(v.unwrap_err());
            push(v.unwrap());
            return Ok(StepResult::next());
        }

        // ---- store：cat2 写双槽（高槽置 Top）----
        case jbc::core::Op::ISTORE:
        case jbc::core::Op::ISTORE_0:
        case jbc::core::Op::ISTORE_1:
        case jbc::core::Op::ISTORE_2:
        case jbc::core::Op::ISTORE_3:
        {
            const auto v = popKind(Value::Kind::I32);
            if (v.is_err()) return Err(v.unwrap_err());
            const auto s = storeLocal(localIndexOf(op, *in), v.unwrap());
            if (s.is_err()) return Err(s.unwrap_err());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::LSTORE:
        case jbc::core::Op::LSTORE_0:
        case jbc::core::Op::LSTORE_1:
        case jbc::core::Op::LSTORE_2:
        case jbc::core::Op::LSTORE_3:
        {
            const auto v = popKind(Value::Kind::I64);
            if (v.is_err()) return Err(v.unwrap_err());
            const auto s = storeLocal(localIndexOf(op, *in), v.unwrap());
            if (s.is_err()) return Err(s.unwrap_err());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::FSTORE:
        case jbc::core::Op::FSTORE_0:
        case jbc::core::Op::FSTORE_1:
        case jbc::core::Op::FSTORE_2:
        case jbc::core::Op::FSTORE_3:
        {
            const auto v = popKind(Value::Kind::F32);
            if (v.is_err()) return Err(v.unwrap_err());
            const auto s = storeLocal(localIndexOf(op, *in), v.unwrap());
            if (s.is_err()) return Err(s.unwrap_err());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::DSTORE:
        case jbc::core::Op::DSTORE_0:
        case jbc::core::Op::DSTORE_1:
        case jbc::core::Op::DSTORE_2:
        case jbc::core::Op::DSTORE_3:
        {
            const auto v = popKind(Value::Kind::F64);
            if (v.is_err()) return Err(v.unwrap_err());
            const auto s = storeLocal(localIndexOf(op, *in), v.unwrap());
            if (s.is_err()) return Err(s.unwrap_err());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::ASTORE:
        case jbc::core::Op::ASTORE_0:
        case jbc::core::Op::ASTORE_1:
        case jbc::core::Op::ASTORE_2:
        case jbc::core::Op::ASTORE_3:
        {
            const auto v = popKind(Value::Kind::Ref);
            if (v.is_err()) return Err(v.unwrap_err());
            const auto s = storeLocal(localIndexOf(op, *in), v.unwrap());
            if (s.is_err()) return Err(s.unwrap_err());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::IINC:
        {
            const ca::u32 idx = static_cast<ca::u32>(in->a);
            const auto v = loadLocal(idx, Value::Kind::I32);
            if (v.is_err()) return Err(v.unwrap_err());
            const auto s = storeLocal(idx, Value::ofI32(addI(v.unwrap().i32, in->b)));
            if (s.is_err()) return Err(s.unwrap_err());
            return Ok(StepResult::next());
        }

        // ---- 栈整形（按 JVMS 形式；形式不符为 BadState）----
        case jbc::core::Op::POP:
        {
            const auto v = popAny();
            if (v.is_err()) return Err(v.unwrap_err());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::POP2:
        {
            auto v = popAny();
            if (v.is_err()) return Err(v.unwrap_err());
            if (!v.unwrap().isCat2()) {
                const auto second = popCat1();
                if (second.is_err()) return Err(second.unwrap_err());
            }
            return Ok(StepResult::next());
        }
        case jbc::core::Op::DUP:
        {
            const auto v = popCat1();
            if (v.is_err()) return Err(v.unwrap_err());
            push(v.unwrap());
            push(v.unwrap());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::DUP_X1:
        {
            const auto v1 = popCat1();
            if (v1.is_err()) return Err(v1.unwrap_err());
            const auto v2 = popCat1();
            if (v2.is_err()) return Err(v2.unwrap_err());
            push(v1.unwrap());
            push(v2.unwrap());
            push(v1.unwrap());
            return Ok(StepResult::next());
        }
        case jbc::core::Op::DUP_X2:
        {
            const auto v1 = popCat1();
            if (v1.is_err()) return Err(v1.unwrap_err());
            auto v2 = popAny();
            if (v2.is_err()) return Err(v2.unwrap_err());
            if (v2.unwrap().isCat2()) {
                push(v1.unwrap());
                push(v2.unwrap());
                push(v1.unwrap());
            }
            else {
                const auto v3 = popCat1();
                if (v3.is_err()) return Err(v3.unwrap_err());
                push(v1.unwrap());
                push(v3.unwrap());
                push(v2.unwrap());
                push(v1.unwrap());
            }
            return Ok(StepResult::next());
        }
        case jbc::core::Op::DUP2:
        {
            auto v1 = popAny();
            if (v1.is_err()) return Err(v1.unwrap_err());
            if (v1.unwrap().isCat2()) {
                push(v1.unwrap());
                push(v1.unwrap());
            }
            else {
                const auto v2 = popCat1();
                if (v2.is_err()) return Err(v2.unwrap_err());
                push(v2.unwrap());
                push(v1.unwrap());
                push(v2.unwrap());
                push(v1.unwrap());
            }
            return Ok(StepResult::next());
        }
        case jbc::core::Op::DUP2_X1:
        {
            auto v1 = popAny();
            if (v1.is_err()) return Err(v1.unwrap_err());
            if (v1.unwrap().isCat2()) {
                const auto v2 = popCat1();
                if (v2.is_err()) return Err(v2.unwrap_err());
                push(v1.unwrap());
                push(v2.unwrap());
                push(v1.unwrap());
            }
            else {
                const auto v2 = popCat1();
                if (v2.is_err()) return Err(v2.unwrap_err());
                const auto v3 = popCat1();
                if (v3.is_err()) return Err(v3.unwrap_err());
                push(v2.unwrap());
                push(v1.unwrap());
                push(v3.unwrap());
                push(v2.unwrap());
                push(v1.unwrap());
            }
            return Ok(StepResult::next());
        }
        case jbc::core::Op::DUP2_X2:
        {
            auto v1 = popAny();
            if (v1.is_err()) return Err(v1.unwrap_err());
            if (v1.unwrap().isCat2()) {
                const auto v2 = popCat1();
                if (v2.is_err()) return Err(v2.unwrap_err());
                auto v3 = popAny();
                if (v3.is_err()) return Err(v3.unwrap_err());
                if (v3.unwrap().isCat2()) {
                    push(v2.unwrap());
                    push(v1.unwrap());
                    push(v3.unwrap());
                    push(v2.unwrap());
                    push(v1.unwrap());
                }
                else {
                    push(v1.unwrap());
                    push(v3.unwrap());
                    push(v2.unwrap());
                    push(v1.unwrap());
                }
            }
            else {
                const auto v2 = popCat1();
                if (v2.is_err()) return Err(v2.unwrap_err());
                const auto v3 = popCat1();
                if (v3.is_err()) return Err(v3.unwrap_err());
                const auto v4 = popCat1();
                if (v4.is_err()) return Err(v4.unwrap_err());
                push(v3.unwrap());
                push(v2.unwrap());
                push(v1.unwrap());
                push(v4.unwrap());
                push(v3.unwrap());
                push(v2.unwrap());
                push(v1.unwrap());
            }
            return Ok(StepResult::next());
        }
        case jbc::core::Op::SWAP:
        {
            const auto v1 = popCat1();
            if (v1.is_err()) return Err(v1.unwrap_err());
            const auto v2 = popCat1();
            if (v2.is_err()) return Err(v2.unwrap_err());
            push(v1.unwrap());
            push(v2.unwrap());
            return Ok(StepResult::next());
        }

        // ---- 数值算术 / 比较（二元）----
        case jbc::core::Op::IADD:
        case jbc::core::Op::LADD:
        case jbc::core::Op::FADD:
        case jbc::core::Op::DADD:
        case jbc::core::Op::ISUB:
        case jbc::core::Op::LSUB:
        case jbc::core::Op::FSUB:
        case jbc::core::Op::DSUB:
        case jbc::core::Op::IMUL:
        case jbc::core::Op::LMUL:
        case jbc::core::Op::FMUL:
        case jbc::core::Op::DMUL:
        case jbc::core::Op::IDIV:
        case jbc::core::Op::LDIV:
        case jbc::core::Op::FDIV:
        case jbc::core::Op::DDIV:
        case jbc::core::Op::IREM:
        case jbc::core::Op::LREM:
        case jbc::core::Op::FREM:
        case jbc::core::Op::DREM:
        case jbc::core::Op::ISHL:
        case jbc::core::Op::LSHL:
        case jbc::core::Op::ISHR:
        case jbc::core::Op::LSHR:
        case jbc::core::Op::IUSHR:
        case jbc::core::Op::LUSHR:
        case jbc::core::Op::IAND:
        case jbc::core::Op::LAND:
        case jbc::core::Op::IOR:
        case jbc::core::Op::LOR:
        case jbc::core::Op::IXOR:
        case jbc::core::Op::LXOR:
        case jbc::core::Op::LCMP:
        case jbc::core::Op::FCMPL:
        case jbc::core::Op::FCMPG:
        case jbc::core::Op::DCMPL:
        case jbc::core::Op::DCMPG:
        {
            const auto rhs = popAny();
            if (rhs.is_err()) return Err(rhs.unwrap_err());
            const auto lhs = popAny();
            if (lhs.is_err()) return Err(lhs.unwrap_err());
            const auto v = applyBinary(op, lhs.unwrap(), rhs.unwrap(), cur_);
            if (v.is_err()) return Err(v.unwrap_err());
            push(v.unwrap());
            return Ok(StepResult::next());
        }

        // ---- 取负与转换（一元）----
        case jbc::core::Op::INEG:
        case jbc::core::Op::LNEG:
        case jbc::core::Op::FNEG:
        case jbc::core::Op::DNEG:
        case jbc::core::Op::I2L:
        case jbc::core::Op::I2F:
        case jbc::core::Op::I2D:
        case jbc::core::Op::L2I:
        case jbc::core::Op::L2F:
        case jbc::core::Op::L2D:
        case jbc::core::Op::F2I:
        case jbc::core::Op::F2L:
        case jbc::core::Op::F2D:
        case jbc::core::Op::D2I:
        case jbc::core::Op::D2L:
        case jbc::core::Op::D2F:
        case jbc::core::Op::I2B:
        case jbc::core::Op::I2C:
        case jbc::core::Op::I2S:
        {
            const auto v = popAny();
            if (v.is_err()) return Err(v.unwrap_err());
            const auto out = applyUnary(op, v.unwrap(), cur_);
            if (out.is_err()) return Err(out.unwrap_err());
            push(out.unwrap());
            return Ok(StepResult::next());
        }

        // ---- 分支 ----
        case jbc::core::Op::IFEQ:
        case jbc::core::Op::IFNE:
        case jbc::core::Op::IFLT:
        case jbc::core::Op::IFGE:
        case jbc::core::Op::IFGT:
        case jbc::core::Op::IFLE:
        {
            const auto v = popKind(Value::Kind::I32);
            if (v.is_err()) return Err(v.unwrap_err());
            return branch(testBranchUnary(op, v.unwrap().i32), in->target);
        }
        case jbc::core::Op::IF_ICMPEQ:
        case jbc::core::Op::IF_ICMPNE:
        case jbc::core::Op::IF_ICMPLT:
        case jbc::core::Op::IF_ICMPGE:
        case jbc::core::Op::IF_ICMPGT:
        case jbc::core::Op::IF_ICMPLE:
        {
            const auto rhs = popKind(Value::Kind::I32);
            if (rhs.is_err()) return Err(rhs.unwrap_err());
            const auto lhs = popKind(Value::Kind::I32);
            if (lhs.is_err()) return Err(lhs.unwrap_err());
            return branch(testBranchBinary(op, lhs.unwrap().i32, rhs.unwrap().i32), in->target);
        }
        case jbc::core::Op::IFNULL:
        case jbc::core::Op::IFNONNULL:
        {
            const auto v = popKind(Value::Kind::Ref);
            if (v.is_err()) return Err(v.unwrap_err());
            const bool taken = op == jbc::core::Op::IFNULL ? v.unwrap().isNull()
                                                           : !v.unwrap().isNull();
            return branch(taken, in->target);
        }
        case jbc::core::Op::IF_ACMPEQ:
        case jbc::core::Op::IF_ACMPNE:
        {
            const auto rhs = popKind(Value::Kind::Ref);
            if (rhs.is_err()) return Err(rhs.unwrap_err());
            const auto lhs = popKind(Value::Kind::Ref);
            if (lhs.is_err()) return Err(lhs.unwrap_err());
            const bool equal = lhs.unwrap().sameRef(rhs.unwrap());
            return branch(op == jbc::core::Op::IF_ACMPEQ ? equal : !equal, in->target);
        }
        case jbc::core::Op::GOTO:
        case jbc::core::Op::GOTO_W: return branch(true, in->target);

        // ---- switch ----
        case jbc::core::Op::TABLESWITCH:
        {
            const auto key = popKind(Value::Kind::I32);
            if (key.is_err()) return Err(key.unwrap_err());
            const ca::i32 raw = key.unwrap().i32;
            jbc::ir::LabelId dest = in->target;   // default
            if (raw >= in->a && raw <= in->b) {
                const ca::usize i = static_cast<ca::usize>(raw - in->a);
                if (i >= in->targets.size()) return Err(err(EvalError::Kind::BadState));
                dest = in->targets[i];
            }
            return branch(true, dest);
        }
        case jbc::core::Op::LOOKUPSWITCH:
        {
            const auto key = popKind(Value::Kind::I32);
            if (key.is_err()) return Err(key.unwrap_err());
            const ca::i32 raw = key.unwrap().i32;
            jbc::ir::LabelId dest = in->target;   // default
            for (ca::usize i = 0; i < in->keys.size(); ++i) {
                if (in->keys[i] == raw) {
                    dest = in->targets[i];
                    break;
                }
            }
            return branch(true, dest);
        }

        // ---- return：与描述符返回类别核对 ----
        case jbc::core::Op::RETURN:
        {
            if (sig_.returnKind() != SigKind::Void) return Err(err(EvalError::Kind::BadState));
            return Ok(StepResult::retVoid());
        }
        case jbc::core::Op::IRETURN:
        case jbc::core::Op::LRETURN:
        case jbc::core::Op::FRETURN:
        case jbc::core::Op::DRETURN:
        case jbc::core::Op::ARETURN:
        {
            const SigKind want = sig_.returnKind();
            Value::Kind valueKind = Value::Kind::Top;
            switch (op) {
            case jbc::core::Op::IRETURN:
                if (want != SigKind::I32) return Err(err(EvalError::Kind::BadState));
                valueKind = Value::Kind::I32;
                break;
            case jbc::core::Op::LRETURN:
                if (want != SigKind::I64) return Err(err(EvalError::Kind::BadState));
                valueKind = Value::Kind::I64;
                break;
            case jbc::core::Op::FRETURN:
                if (want != SigKind::F32) return Err(err(EvalError::Kind::BadState));
                valueKind = Value::Kind::F32;
                break;
            case jbc::core::Op::DRETURN:
                if (want != SigKind::F64) return Err(err(EvalError::Kind::BadState));
                valueKind = Value::Kind::F64;
                break;
            default:
                if (want != SigKind::Ref) return Err(err(EvalError::Kind::BadState));
                valueKind = Value::Kind::Ref;
                break;
            }
            const auto v = popKind(valueKind);
            if (v.is_err()) return Err(v.unwrap_err());
            return Ok(StepResult::ret(v.unwrap()));
        }

        // ---- 只读引用形状 ----
        case jbc::core::Op::ARRAYLENGTH:
        {
            const auto v = popKind(Value::Kind::Ref);
            if (v.is_err()) return Err(v.unwrap_err());
            // null 读长度是 NPE 形状；非数组引用没有可读长度：都不猜。
            if (v.unwrap().isNull() || !v.unwrap().refIsArray) {
                return Err(err(EvalError::Kind::BadState));
            }
            push(Value::ofI32(v.unwrap().refLength));
            return Ok(StepResult::next());
        }

        default: return Err(err(EvalError::Kind::Unsupported));
        }
    }

    const jbc::ir::CodeDom& dom_;
    const MethodSig& sig_;
    const Options& opts_;
    std::vector<Value> stack_;
    std::vector<Value> locals_;
    std::vector<ca::usize> idxBySlot_;
    jbc::ir::InsnId cur_ = jbc::ir::kNoInsn;
};

}   // namespace

bool isSupportedOp(jbc::core::Op op) noexcept
{
    switch (op) {
    case jbc::core::Op::NOP:
    case jbc::core::Op::ACONST_NULL:
    case jbc::core::Op::ICONST_M1:
    case jbc::core::Op::ICONST_0:
    case jbc::core::Op::ICONST_1:
    case jbc::core::Op::ICONST_2:
    case jbc::core::Op::ICONST_3:
    case jbc::core::Op::ICONST_4:
    case jbc::core::Op::ICONST_5:
    case jbc::core::Op::LCONST_0:
    case jbc::core::Op::LCONST_1:
    case jbc::core::Op::FCONST_0:
    case jbc::core::Op::FCONST_1:
    case jbc::core::Op::FCONST_2:
    case jbc::core::Op::DCONST_0:
    case jbc::core::Op::DCONST_1:
    case jbc::core::Op::BIPUSH:
    case jbc::core::Op::SIPUSH:
    case jbc::core::Op::ILOAD:
    case jbc::core::Op::LLOAD:
    case jbc::core::Op::FLOAD:
    case jbc::core::Op::DLOAD:
    case jbc::core::Op::ALOAD:
    case jbc::core::Op::ILOAD_0:
    case jbc::core::Op::ILOAD_1:
    case jbc::core::Op::ILOAD_2:
    case jbc::core::Op::ILOAD_3:
    case jbc::core::Op::LLOAD_0:
    case jbc::core::Op::LLOAD_1:
    case jbc::core::Op::LLOAD_2:
    case jbc::core::Op::LLOAD_3:
    case jbc::core::Op::FLOAD_0:
    case jbc::core::Op::FLOAD_1:
    case jbc::core::Op::FLOAD_2:
    case jbc::core::Op::FLOAD_3:
    case jbc::core::Op::DLOAD_0:
    case jbc::core::Op::DLOAD_1:
    case jbc::core::Op::DLOAD_2:
    case jbc::core::Op::DLOAD_3:
    case jbc::core::Op::ALOAD_0:
    case jbc::core::Op::ALOAD_1:
    case jbc::core::Op::ALOAD_2:
    case jbc::core::Op::ALOAD_3:
    case jbc::core::Op::ISTORE:
    case jbc::core::Op::LSTORE:
    case jbc::core::Op::FSTORE:
    case jbc::core::Op::DSTORE:
    case jbc::core::Op::ASTORE:
    case jbc::core::Op::ISTORE_0:
    case jbc::core::Op::ISTORE_1:
    case jbc::core::Op::ISTORE_2:
    case jbc::core::Op::ISTORE_3:
    case jbc::core::Op::LSTORE_0:
    case jbc::core::Op::LSTORE_1:
    case jbc::core::Op::LSTORE_2:
    case jbc::core::Op::LSTORE_3:
    case jbc::core::Op::FSTORE_0:
    case jbc::core::Op::FSTORE_1:
    case jbc::core::Op::FSTORE_2:
    case jbc::core::Op::FSTORE_3:
    case jbc::core::Op::DSTORE_0:
    case jbc::core::Op::DSTORE_1:
    case jbc::core::Op::DSTORE_2:
    case jbc::core::Op::DSTORE_3:
    case jbc::core::Op::ASTORE_0:
    case jbc::core::Op::ASTORE_1:
    case jbc::core::Op::ASTORE_2:
    case jbc::core::Op::ASTORE_3:
    case jbc::core::Op::IINC:
    case jbc::core::Op::POP:
    case jbc::core::Op::POP2:
    case jbc::core::Op::DUP:
    case jbc::core::Op::DUP_X1:
    case jbc::core::Op::DUP_X2:
    case jbc::core::Op::DUP2:
    case jbc::core::Op::DUP2_X1:
    case jbc::core::Op::DUP2_X2:
    case jbc::core::Op::SWAP:
    case jbc::core::Op::IADD:
    case jbc::core::Op::LADD:
    case jbc::core::Op::FADD:
    case jbc::core::Op::DADD:
    case jbc::core::Op::ISUB:
    case jbc::core::Op::LSUB:
    case jbc::core::Op::FSUB:
    case jbc::core::Op::DSUB:
    case jbc::core::Op::IMUL:
    case jbc::core::Op::LMUL:
    case jbc::core::Op::FMUL:
    case jbc::core::Op::DMUL:
    case jbc::core::Op::IDIV:
    case jbc::core::Op::LDIV:
    case jbc::core::Op::FDIV:
    case jbc::core::Op::DDIV:
    case jbc::core::Op::IREM:
    case jbc::core::Op::LREM:
    case jbc::core::Op::FREM:
    case jbc::core::Op::DREM:
    case jbc::core::Op::INEG:
    case jbc::core::Op::LNEG:
    case jbc::core::Op::FNEG:
    case jbc::core::Op::DNEG:
    case jbc::core::Op::ISHL:
    case jbc::core::Op::LSHL:
    case jbc::core::Op::ISHR:
    case jbc::core::Op::LSHR:
    case jbc::core::Op::IUSHR:
    case jbc::core::Op::LUSHR:
    case jbc::core::Op::IAND:
    case jbc::core::Op::LAND:
    case jbc::core::Op::IOR:
    case jbc::core::Op::LOR:
    case jbc::core::Op::IXOR:
    case jbc::core::Op::LXOR:
    case jbc::core::Op::I2L:
    case jbc::core::Op::I2F:
    case jbc::core::Op::I2D:
    case jbc::core::Op::L2I:
    case jbc::core::Op::L2F:
    case jbc::core::Op::L2D:
    case jbc::core::Op::F2I:
    case jbc::core::Op::F2L:
    case jbc::core::Op::F2D:
    case jbc::core::Op::D2I:
    case jbc::core::Op::D2L:
    case jbc::core::Op::D2F:
    case jbc::core::Op::I2B:
    case jbc::core::Op::I2C:
    case jbc::core::Op::I2S:
    case jbc::core::Op::LCMP:
    case jbc::core::Op::FCMPL:
    case jbc::core::Op::FCMPG:
    case jbc::core::Op::DCMPL:
    case jbc::core::Op::DCMPG:
    case jbc::core::Op::IFEQ:
    case jbc::core::Op::IFNE:
    case jbc::core::Op::IFLT:
    case jbc::core::Op::IFGE:
    case jbc::core::Op::IFGT:
    case jbc::core::Op::IFLE:
    case jbc::core::Op::IF_ICMPEQ:
    case jbc::core::Op::IF_ICMPNE:
    case jbc::core::Op::IF_ICMPLT:
    case jbc::core::Op::IF_ICMPGE:
    case jbc::core::Op::IF_ICMPGT:
    case jbc::core::Op::IF_ICMPLE:
    case jbc::core::Op::IF_ACMPEQ:
    case jbc::core::Op::IF_ACMPNE:
    case jbc::core::Op::GOTO:
    case jbc::core::Op::GOTO_W:
    case jbc::core::Op::TABLESWITCH:
    case jbc::core::Op::LOOKUPSWITCH:
    case jbc::core::Op::IRETURN:
    case jbc::core::Op::LRETURN:
    case jbc::core::Op::FRETURN:
    case jbc::core::Op::DRETURN:
    case jbc::core::Op::ARETURN:
    case jbc::core::Op::RETURN:
    case jbc::core::Op::ARRAYLENGTH: return true;
    default: return false;
    }
}

const char* to_string(EvalError::Kind kind) noexcept
{
    switch (kind) {
    case EvalError::Kind::Unsupported: return "unsupported";
    case EvalError::Kind::StepLimit: return "step-limit";
    case EvalError::Kind::BadState: return "bad-state";
    }
    return "unknown";
}

Result<EvalOutcome, EvalError> evaluate(const jbc::ir::CodeDom& dom, const MethodSig& sig,
                                        const std::vector<Value>& args, const Options& options)
{
    Machine machine(dom, sig, options);
    return machine.run(args);
}

}   // namespace mjbase::asm_evaluator
