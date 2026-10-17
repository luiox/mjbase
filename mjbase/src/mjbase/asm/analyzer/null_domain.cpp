/// @file        null_domain.cpp
/// @brief       nullness 域实现：join 代数（照旧 NullabilityUtil::merge）+ 产点事实。
#include "null_domain.hpp"

namespace mjbase::asm_analyzer {

namespace {

using jbc::analysis::VKind;
using jbc::core::CpTag;
using jbc::core::Op;

/// 槽宽 join：同宽保持，异宽取窄（与 ConstDomain/OriginDomain 同例）。
ca::u8 joinSlots(ca::u8 a, ca::u8 b) noexcept { return a == b ? a : (a < b ? a : b); }

/// nullness join：NullState 的代数层级 Unknown ⊐ Nullable ⊐ {NotNull, Null}。
/// 与旧 NullabilityUtil::merge 的唯一差别是 Unknown 的地位：旧格按优先级把
/// UNKNOWN 当 bottom（单位元），本格按引擎约定当 top（吸收元）。
NullState joinState(NullState a, NullState b) noexcept
{
    if (a == b) {
        return a;
    }
    if (a == NullState::Unknown || b == NullState::Unknown) {
        return NullState::Unknown;
    }
    return NullState::Nullable;   // NotNull+Null / Nullable+事实
}

}   // namespace

NullValue NullDomain::merge(const NullValue& a, const NullValue& b)
{
    return NullValue::of(joinState(a.s, b.s), joinSlots(a.slots, b.slots));
}

NullValue NullDomain::newValue(const jbc::ir::Insn& producer, jbc::ir::InsnId,
                               VKind shape)
{
    switch (producer.op) {
    case Op::ACONST_NULL: return NullValue::of(NullState::Null);
    case Op::LDC:
    case Op::LDC_W:
        if (cp_->valid(producer.cp) && cp_->tagOf(producer.cp) == CpTag::String) {
            return NullValue::of(NullState::NotNull);
        }
        return NullValue::unknownOfShape(shape);
    default: return NullValue::unknownOfShape(shape);
    }
}

NullValue NullDomain::newParameter(ca::u32, VKind shape)
{
    return NullValue::unknownOfShape(shape);
}

NullValue NullDomain::newException(const jbc::ir::CodeUnit::Exc&)
{
    return NullValue::of(NullState::Unknown);
}

bool NullDomain::equal(const NullValue& a, const NullValue& b)
{
    return a.s == b.s && a.slots == b.slots;
}

NullResult analyzeNullness(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u)
{
    NullDomain dom(cp);
    jbc::analysis::DomainEngine<NullValue> eng(cp, dom);
    return NullResult(FrameQuery<NullValue>(u, eng.analyze(u)));
}

NullResult analyzeNullness(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                           const jbc::analysis::MethodSig& sig)
{
    NullDomain dom(cp);
    jbc::analysis::DomainEngine<NullValue> eng(cp, dom);
    return NullResult(FrameQuery<NullValue>(u, eng.analyze(u, sig)));
}

}   // namespace mjbase::asm_analyzer
