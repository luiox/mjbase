/// @file        const_domain.cpp
/// @brief       常量传播域实现：JVM 折叠语义 + join 合并 + ldc 载荷读取。
#include "const_domain.hpp"

#include <limits>

namespace mjbase::asm_analyzer {

namespace {

using jbc::analysis::VKind;
using jbc::core::CpTag;
using jbc::core::Op;

/// 槽宽 join：同宽保持，异宽取窄。合法汇合（同为 cat2）不塌；类型错乱的
/// 畸形汇合按 best-effort 取窄（与）。
ca::u8 joinSlots(ca::u8 a, ca::u8 b) noexcept { return a == b ? a : (a < b ? a : b); }

/// int 域运算：以 u32 环绕再回符号（JVM 补码语义）。
ca::i32 wrapI(ca::u32 v) noexcept { return static_cast<ca::i32>(v); }

ca::i32 addI(ca::i32 a, ca::i32 b) noexcept
{
    return wrapI(static_cast<ca::u32>(a) + static_cast<ca::u32>(b));
}
ca::i32 subI(ca::i32 a, ca::i32 b) noexcept
{
    return wrapI(static_cast<ca::u32>(a) - static_cast<ca::u32>(b));
}
ca::i32 mulI(ca::i32 a, ca::i32 b) noexcept
{
    return wrapI(static_cast<ca::u32>(a) * static_cast<ca::u32>(b));
}
ca::i32 negI(ca::i32 a) noexcept { return wrapI(0U - static_cast<ca::u32>(a)); }

/// JVM 特例：MIN / -1 = MIN（补码环绕，无异常）；调用方保证 b != 0。
ca::i32 divI(ca::i32 a, ca::i32 b) noexcept
{
    if (a == (std::numeric_limits<ca::i32>::min)() && b == -1) {
        return a;
    }
    return a / b;
}
/// JVM 特例：MIN % -1 = 0；调用方保证 b != 0。
ca::i32 remI(ca::i32 a, ca::i32 b) noexcept
{
    if (a == (std::numeric_limits<ca::i32>::min)() && b == -1) {
        return 0;
    }
    return a % b;
}

ca::i64 wrapL(ca::u64 v) noexcept { return static_cast<ca::i64>(v); }

ca::i64 addL(ca::i64 a, ca::i64 b) noexcept
{
    return wrapL(static_cast<ca::u64>(a) + static_cast<ca::u64>(b));
}
ca::i64 subL(ca::i64 a, ca::i64 b) noexcept
{
    return wrapL(static_cast<ca::u64>(a) - static_cast<ca::u64>(b));
}
ca::i64 mulL(ca::i64 a, ca::i64 b) noexcept
{
    return wrapL(static_cast<ca::u64>(a) * static_cast<ca::u64>(b));
}
ca::i64 negL(ca::i64 a) noexcept { return wrapL(0U - static_cast<ca::u64>(a)); }

ca::i64 divL(ca::i64 a, ca::i64 b) noexcept
{
    if (a == (std::numeric_limits<ca::i64>::min)() && b == -1) {
        return a;
    }
    return a / b;
}
ca::i64 remL(ca::i64 a, ca::i64 b) noexcept
{
    if (a == (std::numeric_limits<ca::i64>::min)() && b == -1) {
        return 0;
    }
    return a % b;
}

/// 数值载荷便捷读取（调用方已保证 t 匹配）。
ca::i32 asI32(const ConstLattice& v) noexcept { return static_cast<ca::i32>(v.i); }

}   // namespace

ConstLattice ConstDomain::merge(const ConstLattice& a, const ConstLattice& b)
{
    if (a.isBottom()) {
        return b;
    }
    if (b.isBottom()) {
        return a;
    }
    if (a.isTop() || b.isTop()) {
        return ConstLattice::topOf(joinSlots(a.slots, b.slots));
    }
    // 两边都是常量：同载荷保持，不同 → Top（宽度尽量保留）。
    if (a.t != b.t) {
        return ConstLattice::topOf(joinSlots(a.slots, b.slots));
    }
    switch (a.t) {
    case ConstLattice::T::Null: return a;
    case ConstLattice::T::Int:
    case ConstLattice::T::Long: return a.i == b.i ? a : ConstLattice::topOf(joinSlots(a.slots, b.slots));
    case ConstLattice::T::Str:
        return sameStrPayload(a.cp, b.cp) ? a
                                          : ConstLattice::topOf(joinSlots(a.slots, b.slots));
    case ConstLattice::T::None: break;
    }
    return ConstLattice::topOf(joinSlots(a.slots, b.slots));
}

ConstLattice ConstDomain::newValue(const jbc::ir::Insn& producer, jbc::ir::InsnId,
                                   VKind shape)
{
    switch (producer.op) {
    case Op::ACONST_NULL: return ConstLattice::ofNull();
    case Op::ICONST_M1:
    case Op::ICONST_0:
    case Op::ICONST_1:
    case Op::ICONST_2:
    case Op::ICONST_3:
    case Op::ICONST_4:
    case Op::ICONST_5:
        return ConstLattice::ofInt(static_cast<ca::i32>(producer.op) -
                                   static_cast<ca::i32>(Op::ICONST_0));
    case Op::BIPUSH:
    case Op::SIPUSH: return ConstLattice::ofInt(producer.a);
    case Op::LCONST_0:
    case Op::LCONST_1:
        return ConstLattice::ofLong(static_cast<ca::i64>(static_cast<ca::i32>(producer.op) -
                                                         static_cast<ca::i32>(Op::LCONST_0)));
    case Op::LDC:
    case Op::LDC_W:
    case Op::LDC2_W: return ldcValue(producer, shape);
    default: return ConstLattice::unknownOf(shape);
    }
}

ConstDomain::ConstDomain(const jbc::core::ConstantPool& cp, const jbc::analysis::MethodSig& sig,
                         std::vector<ConstLattice> argSeeds)
    : cp_(&cp)
{
    // 描述符 → local 槽位映射（与 DomainEngine 的播种协议同构）：this 占
    // 0 号（实例方法），随后逐参入槽，J/D 双槽。种子个数超出参数个数时
    // 多余项忽略；描述符提前结束同样截断。
    ca::u32 slot = sig.isStatic ? 0 : 1;
    const std::string_view d = sig.desc;
    ca::usize pos = (d.size() > 1 && d[0] == '(') ? 1 : 0;
    ca::usize argIdx = 0;
    while (pos < d.size() && d[pos] != ')' && argIdx < argSeeds.size()) {
        const ca::usize start = pos;
        while (pos < d.size() && d[pos] == '[') {
            ++pos;
        }
        if (pos < d.size() && d[pos] == 'L') {
            const ca::usize e = d.find(';', pos);
            if (e == std::string_view::npos) {
                break;   // 畸形描述符：剩余种子一律不落槽
            }
            pos = e + 1;
        }
        else if (pos < d.size()) {
            ++pos;
        }
        const std::string_view t = d.substr(start, pos - start);
        if (t.empty()) {
            break;
        }
        seeds_.emplace(slot, argSeeds[argIdx]);
        ++argIdx;
        slot += (t == "J" || t == "D") ? 2 : 1;
    }
}

ConstLattice ConstDomain::newParameter(ca::u32 localIdx, VKind shape)
{
    const auto it = seeds_.find(localIdx);
    if (it != seeds_.end()) {
        return it->second;
    }
    return ConstLattice::unknownOf(shape);
}

ConstLattice ConstDomain::newException(const jbc::ir::CodeUnit::Exc&)
{
    return ConstLattice::topOf();
}

ConstLattice ConstDomain::binary(Op op, const ConstLattice& a, const ConstLattice& b,
                                 VKind result, const jbc::ir::Insn&, jbc::ir::InsnId)
{
    const ConstLattice unknown = ConstLattice::unknownOf(result);

    // ---- int 折叠 ----
    if (a.isInt() && b.isInt()) {
        const ca::i32 x = asI32(a);
        const ca::i32 y = asI32(b);
        switch (op) {
        case Op::IADD: return ConstLattice::ofInt(addI(x, y));
        case Op::ISUB: return ConstLattice::ofInt(subI(x, y));
        case Op::IMUL: return ConstLattice::ofInt(mulI(x, y));
        case Op::IAND: return ConstLattice::ofInt(x & y);
        case Op::IOR: return ConstLattice::ofInt(x | y);
        case Op::IXOR: return ConstLattice::ofInt(x ^ y);
        case Op::ISHL: return ConstLattice::ofInt(wrapI(static_cast<ca::u32>(x) << (y & 0x1F)));
        case Op::ISHR: return ConstLattice::ofInt(x >> (y & 0x1F));
        case Op::IUSHR: return ConstLattice::ofInt(wrapI(static_cast<ca::u32>(x) >> (y & 0x1F)));
        case Op::IDIV: return y == 0 ? unknown : ConstLattice::ofInt(divI(x, y));
        case Op::IREM: return y == 0 ? unknown : ConstLattice::ofInt(remI(x, y));
        default: break;
        }
    }

    // ---- long 折叠（移位量是 int 操作数）----
    if (a.isLong()) {
        const ca::i64 x = a.i;
        if (b.isLong()) {
            switch (op) {
            case Op::LADD: return ConstLattice::ofLong(addL(x, b.i));
            case Op::LSUB: return ConstLattice::ofLong(subL(x, b.i));
            case Op::LMUL: return ConstLattice::ofLong(mulL(x, b.i));
            case Op::LAND: return ConstLattice::ofLong(x & b.i);
            case Op::LOR: return ConstLattice::ofLong(x | b.i);
            case Op::LXOR: return ConstLattice::ofLong(x ^ b.i);
            case Op::LDIV: return b.i == 0 ? unknown : ConstLattice::ofLong(divL(x, b.i));
            case Op::LREM: return b.i == 0 ? unknown : ConstLattice::ofLong(remL(x, b.i));
            case Op::LCMP:
                return ConstLattice::ofInt(static_cast<ca::i32>((x > b.i) - (x < b.i)));
            default: break;
            }
        }
        if (b.isInt()) {
            const ca::i32 y = asI32(b);
            switch (op) {
            case Op::LSHL:
                return ConstLattice::ofLong(wrapL(static_cast<ca::u64>(x) << (y & 0x3F)));
            case Op::LSHR: return ConstLattice::ofLong(x >> (y & 0x3F));
            case Op::LUSHR:
                return ConstLattice::ofLong(wrapL(static_cast<ca::u64>(x) >> (y & 0x3F)));
            default: break;
            }
        }
    }

    // float/double 算术与 fcmp/dcmp：本域不折（载荷无浮点）。
    return unknown;
}

ConstLattice ConstDomain::unary(Op op, const ConstLattice& a, VKind result,
                                const jbc::ir::Insn&, jbc::ir::InsnId)
{
    const ConstLattice unknown = ConstLattice::unknownOf(result);
    if (a.isInt()) {
        const ca::i32 x = asI32(a);
        switch (op) {
        case Op::INEG: return ConstLattice::ofInt(negI(x));
        case Op::I2L: return ConstLattice::ofLong(a.i);
        case Op::I2B: return ConstLattice::ofInt(static_cast<ca::i32>(static_cast<ca::i8>(x)));
        case Op::I2C: return ConstLattice::ofInt(static_cast<ca::i32>(static_cast<ca::u16>(x)));
        case Op::I2S: return ConstLattice::ofInt(static_cast<ca::i32>(static_cast<ca::i16>(x)));
        default: break;
        }
    }
    if (a.isLong()) {
        switch (op) {
        case Op::LNEG: return ConstLattice::ofLong(negL(a.i));
        case Op::L2I: return ConstLattice::ofInt(static_cast<ca::i32>(a.i));
        default: break;
        }
    }
    // I2F/I2D/L2F/L2D/F2*/D2* 等浮点参与转换：不折。
    return unknown;
}

bool ConstDomain::equal(const ConstLattice& a, const ConstLattice& b)
{
    if (a.k != b.k || a.slots != b.slots) {
        return false;
    }
    if (a.k != ConstLattice::K::Const) {
        return true;
    }
    if (a.t != b.t) {
        return false;
    }
    switch (a.t) {
    case ConstLattice::T::Int:
    case ConstLattice::T::Long: return a.i == b.i;
    case ConstLattice::T::Str: return sameStrPayload(a.cp, b.cp);
    case ConstLattice::T::Null: return true;
    case ConstLattice::T::None: return true;
    }
    return true;
}

std::string_view ConstDomain::strOf(const ConstLattice& v) const noexcept
{
    if (!v.isStr() || !cp_->valid(v.cp) || cp_->tagOf(v.cp) != CpTag::String) {
        return {};
    }
    return cp_->utf8Of(cp_->at(v.cp).a);
}

ConstLattice ConstDomain::ldcValue(const jbc::ir::Insn& in, VKind shape) const noexcept
{
    if (!cp_->valid(in.cp)) {
        return ConstLattice::unknownOf(shape);
    }
    switch (cp_->tagOf(in.cp)) {
    case CpTag::Integer: return ConstLattice::ofInt(cp_->at(in.cp).ival);
    case CpTag::Long: return ConstLattice::ofLong(cp_->at(in.cp).lval);
    case CpTag::String: return ConstLattice::ofStr(in.cp);
    default: return ConstLattice::unknownOf(shape);
    }
}

bool ConstDomain::sameStrPayload(ca::u32 cpA, ca::u32 cpB) const noexcept
{
    if (cpA == cpB) {
        return true;
    }
    if (!cp_->valid(cpA) || !cp_->valid(cpB) || cp_->tagOf(cpA) != CpTag::String ||
        cp_->tagOf(cpB) != CpTag::String) {
        return false;
    }
    return cp_->utf8Of(cp_->at(cpA).a) == cp_->utf8Of(cp_->at(cpB).a);
}

std::string_view ConstResult::strOf(const ConstLattice& v) const noexcept
{
    if (!v.isStr() || !cp_->valid(v.cp) || cp_->tagOf(v.cp) != CpTag::String) {
        return {};
    }
    return cp_->utf8Of(cp_->at(v.cp).a);
}

ConstResult propagateConstants(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u)
{
    ConstDomain dom(cp);
    jbc::analysis::DomainEngine<ConstLattice> eng(cp, dom);
    return ConstResult(cp, FrameQuery<ConstLattice>(u, eng.analyze(u)));
}

ConstResult propagateConstants(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                               const jbc::analysis::MethodSig& sig)
{
    ConstDomain dom(cp);
    jbc::analysis::DomainEngine<ConstLattice> eng(cp, dom);
    return ConstResult(cp, FrameQuery<ConstLattice>(u, eng.analyze(u, sig)));
}

ConstResult propagateConstants(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                               const jbc::analysis::MethodSig& sig,
                               std::vector<ConstLattice> argSeeds)
{
    ConstDomain dom(cp, sig, std::move(argSeeds));
    jbc::analysis::DomainEngine<ConstLattice> eng(cp, dom);
    return ConstResult(cp, FrameQuery<ConstLattice>(u, eng.analyze(u, sig)));
}

}   // namespace mjbase::asm_analyzer
