/// @file        origin_domain.cpp
/// @brief       来源追踪域实现：单来源 join + 产值点登记。
#include "origin_domain.hpp"

namespace mjbase::asm_analyzer {

namespace {

/// 槽宽 join：同宽保持，异宽取窄（与 ConstDomain 同例）。
ca::u8 joinSlots(ca::u8 a, ca::u8 b) noexcept { return a == b ? a : (a < b ? a : b); }

}   // namespace

OriginValue OriginDomain::merge(const OriginValue& a, const OriginValue& b)
{
    if (a.kind == b.kind && a.slots == b.slots) {
        switch (a.kind) {
        case OriginValue::Kind::Unknown: return a;
        case OriginValue::Kind::Insn:
            return a.insn == b.insn ? a : OriginValue::unknownOf(a.slots);
        case OriginValue::Kind::Param:
            return a.local == b.local ? a : OriginValue::unknownOf(a.slots);
        }
    }
    return OriginValue::unknownOf(joinSlots(a.slots, b.slots));
}

OriginValue OriginDomain::newValue(const jbc::ir::Insn&, jbc::ir::InsnId at,
                                   jbc::analysis::VKind shape)
{
    return OriginValue::ofInsn(at, shape == jbc::analysis::VKind::Long ||
                                               shape == jbc::analysis::VKind::Double
                                           ? 2
                                           : 1);
}

OriginValue OriginDomain::newParameter(ca::u32 localIdx, jbc::analysis::VKind shape)
{
    return OriginValue::ofParam(
        localIdx,
        shape == jbc::analysis::VKind::Long || shape == jbc::analysis::VKind::Double ? 2 : 1);
}

OriginValue OriginDomain::newException(const jbc::ir::CodeUnit::Exc&)
{
    return OriginValue::unknownOf();
}

bool OriginDomain::equal(const OriginValue& a, const OriginValue& b)
{
    if (a.kind != b.kind || a.slots != b.slots) {
        return false;
    }
    switch (a.kind) {
    case OriginValue::Kind::Unknown: return true;
    case OriginValue::Kind::Insn: return a.insn == b.insn;
    case OriginValue::Kind::Param: return a.local == b.local;
    }
    return false;
}

OriginResult traceOrigins(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u)
{
    OriginDomain dom;
    jbc::analysis::DomainEngine<OriginValue> eng(cp, dom);
    return OriginResult(FrameQuery<OriginValue>(u, eng.analyze(u)));
}

OriginResult traceOrigins(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                          const jbc::analysis::MethodSig& sig)
{
    OriginDomain dom;
    jbc::analysis::DomainEngine<OriginValue> eng(cp, dom);
    return OriginResult(FrameQuery<OriginValue>(u, eng.analyze(u, sig)));
}

}   // namespace mjbase::asm_analyzer
