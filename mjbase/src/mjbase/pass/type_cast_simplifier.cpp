#include "mjbase/pass/type_cast_simplifier.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/pass/window_rule.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::Insn;

using window_rule::BuildFn;
using window_rule::StepFn;
using window_rule::WindowRule;

StepFn opStep(Op expected)
{
    return [expected](const ClassSession&, const Insn& in, const std::vector<const Insn*>&) {
        return in.op == expected;
    };
}

StepFn constIntStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        ca::i32 value = 0;
        return method_rewrite_util::intValueOf(session, in, value);
    };
}

StepFn constLongStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        ca::i64 value = 0;
        return method_rewrite_util::longValueOf(session, in, value);
    };
}

StepFn constFloatStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        float value = 0.0f;
        return method_rewrite_util::floatValueOf(session, in, value);
    };
}

StepFn constDoubleStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        double value = 0.0;
        return method_rewrite_util::doubleValueOf(session, in, value);
    };
}

StepFn loadStringStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        std::string_view value;
        return method_rewrite_util::stringValueOf(session, in, value);
    };
}

StepFn invokestaticStep(std::string_view owner, std::string_view name, std::string_view desc)
{
    return [owner = std::string(owner), name = std::string(name), desc = std::string(desc)](
               const ClassSession& session, const Insn& in,
               const std::vector<const Insn*>&) {
        if (in.op != Op::INVOKESTATIC) return false;
        ClassSession::MemberSem member;
        if (!session.memberOf(in, member)) return false;
        if (member.kind != ClassSession::MemberSem::Kind::Method
            && member.kind != ClassSession::MemberSem::Kind::InterfaceMethod) {
            return false;
        }
        return member.owner == owner && member.name == name && member.desc == desc;
    };
}

template<typename Integer, typename Floating>
Integer javaFloatingToInteger(Floating value)
{
    if (std::isnan(value)) return 0;
    const auto minimum = static_cast<Floating>(std::numeric_limits<Integer>::lowest());
    const auto maximum = static_cast<Floating>(std::numeric_limits<Integer>::max());
    if (value <= minimum) return std::numeric_limits<Integer>::lowest();
    if (value >= maximum) return std::numeric_limits<Integer>::max();
    return static_cast<Integer>(value);
}

ca::i32 javaIntToByte(ca::i32 value)
{
    value &= 0xFF;
    return value >= 0x80 ? value - 0x100 : value;
}

ca::i32 javaIntToChar(ca::i32 value)
{
    return value & 0xFFFF;
}

ca::i32 javaIntToShort(ca::i32 value)
{
    value &= 0xFFFF;
    return value >= 0x8000 ? value - 0x10000 : value;
}

ca::i32 wrapToInt(ca::i64 value)
{
    const ca::u32 low = static_cast<ca::u32>(static_cast<ca::u64>(value));
    ca::i32       out = 0;
    std::memcpy(&out, &low, sizeof(out));
    return out;
}

bool parseJavaIntStrict(std::string_view text, ca::i32& value)
{
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last  = first + text.size();
    if (*first == '+') {
        ++first;
        if (first == last || *first == '+' || *first == '-') return false;
    }
    const auto result = std::from_chars(first, last, value, 10);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parseJavaUnsignedLongStrict(std::string_view text, ca::i32 radix, ca::i64& value)
{
    if (text.empty() || radix < 2 || radix > 36) return false;
    const char* first = text.data();
    const char* last  = first + text.size();
    if (*first == '-') return false;
    if (*first == '+') {
        ++first;
        if (first == last || *first == '+' || *first == '-') return false;
    }
    ca::u64    parsed = 0;
    const auto result = std::from_chars(first, last, parsed, radix);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    std::memcpy(&value, &parsed, sizeof(value));
    return true;
}

StepFn parseableIntStringStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        std::string_view text;
        ca::i32          value = 0;
        return method_rewrite_util::stringValueOf(session, in, text)
               && parseJavaIntStrict(text, value);
    };
}

StepFn parseableUnsignedLongRadixStep()
{
    return [](const ClassSession& session, const Insn& in,
              const std::vector<const Insn*>& matched) {
        if (matched.empty()) return false;
        std::string_view text;
        ca::i32          radix = 0;
        ca::i64          value = 0;
        return method_rewrite_util::stringValueOf(session, *matched[0], text)
               && method_rewrite_util::intValueOf(session, in, radix)
               && parseJavaUnsignedLongStrict(text, radix, value);
    };
}

std::vector<WindowRule> makeRules()
{
    const StepFn cint   = constIntStep();
    const StepFn clong  = constLongStep();
    const StepFn cfloat = constFloatStep();
    const StepFn cdouble = constDoubleStep();

    return {
        // int -> long/float/double/byte/char/short
        WindowRule{{cint, opStep(Op::I2L)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i32 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::intValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeLongConst(
                           session, static_cast<ca::i64>(v)));
                       return true;
                   }},
        WindowRule{{cint, opStep(Op::I2F)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i32 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::intValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeFloatConst(
                           session, static_cast<float>(v)));
                       return true;
                   }},
        WindowRule{{cint, opStep(Op::I2D)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i32 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::intValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeDoubleConst(
                           session, static_cast<double>(v)));
                       return true;
                   }},
        WindowRule{{cint, opStep(Op::I2B)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i32 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::intValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(
                           method_rewrite_util::materializeIntConst(session, javaIntToByte(v)));
                       return true;
                   }},
        WindowRule{{cint, opStep(Op::I2C)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i32 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::intValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(
                           method_rewrite_util::materializeIntConst(session, javaIntToChar(v)));
                       return true;
                   }},
        WindowRule{{cint, opStep(Op::I2S)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i32 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::intValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(
                           method_rewrite_util::materializeIntConst(session, javaIntToShort(v)));
                       return true;
                   }},

        // long -> int/float/double
        WindowRule{{clong, opStep(Op::L2I)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i64 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::longValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(
                           method_rewrite_util::materializeIntConst(session, wrapToInt(v)));
                       return true;
                   }},
        WindowRule{{clong, opStep(Op::L2F)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i64 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::longValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeFloatConst(
                           session, static_cast<float>(v)));
                       return true;
                   }},
        WindowRule{{clong, opStep(Op::L2D)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i64 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::longValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeDoubleConst(
                           session, static_cast<double>(v)));
                       return true;
                   }},

        // float -> int/long/double
        WindowRule{{cfloat, opStep(Op::F2I)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       float v = 0.0f;
                       if (matched.empty()
                           || !method_rewrite_util::floatValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeIntConst(
                           session, javaFloatingToInteger<ca::i32>(v)));
                       return true;
                   }},
        WindowRule{{cfloat, opStep(Op::F2L)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       float v = 0.0f;
                       if (matched.empty()
                           || !method_rewrite_util::floatValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeLongConst(
                           session, javaFloatingToInteger<ca::i64>(v)));
                       return true;
                   }},
        WindowRule{{cfloat, opStep(Op::F2D)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       float v = 0.0f;
                       if (matched.empty()
                           || !method_rewrite_util::floatValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeDoubleConst(
                           session, static_cast<double>(v)));
                       return true;
                   }},

        // double -> int/long/float
        WindowRule{{cdouble, opStep(Op::D2I)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       double v = 0.0;
                       if (matched.empty()
                           || !method_rewrite_util::doubleValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeIntConst(
                           session, javaFloatingToInteger<ca::i32>(v)));
                       return true;
                   }},
        WindowRule{{cdouble, opStep(Op::D2L)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       double v = 0.0;
                       if (matched.empty()
                           || !method_rewrite_util::doubleValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeLongConst(
                           session, javaFloatingToInteger<ca::i64>(v)));
                       return true;
                   }},
        WindowRule{{cdouble, opStep(Op::D2F)},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       double v = 0.0;
                       if (matched.empty()
                           || !method_rewrite_util::doubleValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(method_rewrite_util::materializeFloatConst(
                           session, static_cast<float>(v)));
                       return true;
                   }},

        // Float.intBitsToFloat
        WindowRule{{cint,
                    invokestaticStep("java/lang/Float", "intBitsToFloat", "(I)F")},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i32 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::intValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       const ca::u32 bits = static_cast<ca::u32>(v);
                       float         value = 0.0f;
                       std::memcpy(&value, &bits, sizeof(value));
                       out.clear();
                       out.push_back(
                           method_rewrite_util::materializeFloatConst(session, value));
                       return true;
                   }},

        // Double.longBitsToDouble
        WindowRule{{clong,
                    invokestaticStep("java/lang/Double", "longBitsToDouble", "(J)D")},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       ca::i64 v = 0;
                       if (matched.empty()
                           || !method_rewrite_util::longValueOf(session, *matched[0], v)) {
                           return false;
                       }
                       const ca::u64 bits = static_cast<ca::u64>(v);
                       double        value = 0.0;
                       std::memcpy(&value, &bits, sizeof(value));
                       out.clear();
                       out.push_back(
                           method_rewrite_util::materializeDoubleConst(session, value));
                       return true;
                   }},

        // Integer.parseInt(String)
        WindowRule{{parseableIntStringStep(),
                    invokestaticStep("java/lang/Integer", "parseInt",
                                     "(Ljava/lang/String;)I")},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       std::string_view text;
                       ca::i32          value = 0;
                       if (matched.empty()
                           || !method_rewrite_util::stringValueOf(session, *matched[0], text)
                           || !parseJavaIntStrict(text, value)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(
                           method_rewrite_util::materializeIntConst(session, value));
                       return true;
                   }},

        // Long.parseUnsignedLong(String, int)
        WindowRule{{loadStringStep(), parseableUnsignedLongRadixStep(),
                    invokestaticStep("java/lang/Long", "parseUnsignedLong",
                                     "(Ljava/lang/String;I)J")},
                   [](ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out) {
                       std::string_view text;
                       ca::i32          radix = 0;
                       ca::i64          value = 0;
                       if (matched.size() < 2
                           || !method_rewrite_util::stringValueOf(session, *matched[0], text)
                           || !method_rewrite_util::intValueOf(session, *matched[1], radix)
                           || !parseJavaUnsignedLongStrict(text, radix, value)) {
                           return false;
                       }
                       out.clear();
                       out.push_back(
                           method_rewrite_util::materializeLongConst(session, value));
                       return true;
                   }},
    };
}

}   // namespace

mjbase::spi::PreservedAnalyses
    TypeCastSimplifier::run(mjbase::spi::MethodScope&       scope,
                            mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    static const std::vector<WindowRule> kRules = makeRules();
    return window_rule::applyWindowRules(scope, kRules)
               ? mjbase::spi::PreservedAnalyses::none()
               : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
