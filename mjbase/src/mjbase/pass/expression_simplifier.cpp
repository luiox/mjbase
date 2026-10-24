#include "mjbase/pass/expression_simplifier.hpp"

#include <cstring>
#include <initializer_list>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/pass/window_rule.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeEditor;
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

StepFn loadIntStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        if (in.op == Op::ILOAD || (in.op >= Op::ILOAD_0 && in.op <= Op::ILOAD_3)) {
            return true;
        }
        ca::i32 value = 0;
        return method_rewrite_util::intValueOf(session, in, value);
    };
}

StepFn intConstStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        ca::i32 value = 0;
        return method_rewrite_util::intValueOf(session, in, value);
    };
}

StepFn sameIntValueAs(std::size_t previousIndex)
{
    return [previousIndex](const ClassSession& session, const Insn& in,
                           const std::vector<const Insn*>& matched) {
        if (previousIndex >= matched.size()) return false;
        ca::i32 previous = 0;
        ca::i32 current  = 0;
        return method_rewrite_util::intValueOf(session, *matched[previousIndex], previous)
               && method_rewrite_util::intValueOf(session, in, current) && previous == current;
    };
}

Insn synthetic(Op op)
{
    Insn in = CodeEditor::simple(op);
    in.srcPc = jbc::ir::kNoPc;
    return in;
}

ca::i32 wrapInt(ca::u32 bits)
{
    ca::i32 value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool evalIntBinary(Op op, ca::i32 left, ca::i32 right, ca::i32& out)
{
    const ca::u32 lhs = static_cast<ca::u32>(left);
    const ca::u32 rhs = static_cast<ca::u32>(right);
    ca::u32       bits = 0;
    switch (op) {
    case Op::IADD: bits = lhs + rhs; break;
    case Op::ISUB: bits = lhs - rhs; break;
    case Op::IMUL: bits = lhs * rhs; break;
    case Op::IAND: bits = lhs & rhs; break;
    case Op::IOR: bits = lhs | rhs; break;
    case Op::IXOR: bits = lhs ^ rhs; break;
    default: return false;
    }
    out = wrapInt(bits);
    return true;
}

bool buildNothing(ClassSession&, const std::vector<const Insn*>&, std::vector<Insn>& out)
{
    out.clear();
    return true;
}

BuildFn buildClone(std::size_t index)
{
    return [index](ClassSession&, const std::vector<const Insn*>& matched, std::vector<Insn>& out) {
        if (index >= matched.size()) return false;
        out.clear();
        out.push_back(*matched[index]);
        return true;
    };
}

BuildFn buildSequence(std::initializer_list<Op> ops)
{
    const std::vector<Op> sequence(ops);
    return [sequence](ClassSession&, const std::vector<const Insn*>&, std::vector<Insn>& out) {
        out.clear();
        out.reserve(sequence.size());
        for (const Op op : sequence) out.push_back(synthetic(op));
        return true;
    };
}

BuildFn buildAssoc(Op op)
{
    return [op](ClassSession& session, const std::vector<const Insn*>& matched,
                std::vector<Insn>& out) {
        ca::i32 left  = 0;
        ca::i32 right = 0;
        if (matched.size() < 4
            || !method_rewrite_util::intValueOf(session, *matched[0], left)
            || !method_rewrite_util::intValueOf(session, *matched[2], right)) {
            return false;
        }
        ca::i32 folded = 0;
        if (!evalIntBinary(op, left, right, folded)) return false;
        out.clear();
        out.push_back(method_rewrite_util::materializeIntConst(session, folded));
        out.push_back(synthetic(op));
        return true;
    };
}

BuildFn buildShiftLeftOne()
{
    return [](ClassSession&, const std::vector<const Insn*>& matched, std::vector<Insn>& out) {
        if (matched.empty()) return false;
        out.clear();
        out.push_back(*matched[0]);
        out.push_back(synthetic(Op::ICONST_2));
        out.push_back(synthetic(Op::IMUL));
        return true;
    };
}

BuildFn buildTwoLoadsAnd(Op result)
{
    return [result](ClassSession&, const std::vector<const Insn*>& matched,
                    std::vector<Insn>& out) {
        if (matched.size() < 2) return false;
        out.clear();
        out.push_back(*matched[0]);
        out.push_back(*matched[1]);
        out.push_back(synthetic(result));
        return true;
    };
}

std::vector<WindowRule> makeRules()
{
    const StepFn load  = loadIntStep();
    const StepFn cint  = intConstStep();

    return {
        // --- 恒等元 ---
        WindowRule{{load, opStep(Op::ICONST_0), opStep(Op::IADD)}, buildClone(0)},
        WindowRule{{load, opStep(Op::ICONST_0), opStep(Op::IOR)}, buildClone(0)},
        WindowRule{{load, opStep(Op::ICONST_0), opStep(Op::IXOR)}, buildClone(0)},
        WindowRule{{load, opStep(Op::ICONST_1), opStep(Op::IMUL)}, buildClone(0)},
        WindowRule{{load, opStep(Op::ICONST_M1), opStep(Op::IAND)}, buildClone(0)},
        WindowRule{{load, opStep(Op::ICONST_0), opStep(Op::IAND)},
                   buildSequence({Op::ICONST_0})},
        WindowRule{{opStep(Op::ICONST_M1), opStep(Op::IAND)}, buildNothing},
        WindowRule{{opStep(Op::ICONST_0), opStep(Op::IXOR)}, buildNothing},

        // --- 交换/结合 ---
        WindowRule{{opStep(Op::SWAP), opStep(Op::IADD)}, buildSequence({Op::IADD})},
        WindowRule{{opStep(Op::SWAP), opStep(Op::IXOR)}, buildSequence({Op::IXOR})},
        WindowRule{{opStep(Op::SWAP), opStep(Op::IAND)}, buildSequence({Op::IAND})},
        WindowRule{{opStep(Op::SWAP), opStep(Op::IOR)}, buildSequence({Op::IOR})},
        WindowRule{{cint, opStep(Op::IADD), cint, opStep(Op::IADD)}, buildAssoc(Op::IADD)},
        WindowRule{{cint, opStep(Op::IMUL), cint, opStep(Op::IMUL)}, buildAssoc(Op::IMUL)},
        WindowRule{{cint, opStep(Op::IOR), cint, opStep(Op::IOR)}, buildAssoc(Op::IOR)},
        WindowRule{{cint, opStep(Op::IXOR), cint, opStep(Op::IXOR)}, buildAssoc(Op::IXOR)},
        WindowRule{{cint, opStep(Op::IAND), cint, opStep(Op::IAND)}, buildAssoc(Op::IAND)},

        // --- 特殊表达式 ---
        WindowRule{{load, opStep(Op::DUP), opStep(Op::IXOR)}, buildSequence({Op::ICONST_0})},
        WindowRule{{load, opStep(Op::ICONST_1), opStep(Op::ISHL)}, buildShiftLeftOne()},
        WindowRule{{opStep(Op::INEG), opStep(Op::INEG)}, buildNothing},
        WindowRule{{opStep(Op::LNEG), opStep(Op::LNEG)}, buildNothing},

        // iconst_m1 取负族
        WindowRule{{opStep(Op::ICONST_M1), opStep(Op::IMUL)}, buildSequence({Op::INEG})},
        WindowRule{{opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::ICONST_M1),
                    opStep(Op::ISUB)},
                   buildSequence({Op::INEG})},
        WindowRule{{opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::ICONST_1),
                    opStep(Op::IADD)},
                   buildSequence({Op::INEG})},
        WindowRule{{opStep(Op::ICONST_M1), opStep(Op::IADD), opStep(Op::ICONST_M1),
                    opStep(Op::IXOR)},
                   buildSequence({Op::INEG})},

        // iadd 族
        WindowRule{{opStep(Op::INEG), opStep(Op::ISUB)}, buildSequence({Op::IADD})},
        WindowRule{{opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::ISUB),
                    opStep(Op::ICONST_1), opStep(Op::ISUB)},
                   buildSequence({Op::IADD})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::IOR), opStep(Op::DUP_X2), opStep(Op::POP),
                    opStep(Op::IAND), opStep(Op::IADD)},
                   buildSequence({Op::IADD})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::IXOR), opStep(Op::DUP_X2), opStep(Op::POP),
                    opStep(Op::IAND), opStep(Op::ICONST_2), opStep(Op::IMUL),
                    opStep(Op::IADD)},
                   buildSequence({Op::IADD})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::IOR), opStep(Op::ICONST_2), opStep(Op::IMUL),
                    opStep(Op::DUP_X2), opStep(Op::POP), opStep(Op::IXOR), opStep(Op::ISUB)},
                   buildSequence({Op::IADD})},

        // isub 族
        WindowRule{{opStep(Op::INEG), opStep(Op::IADD)}, buildSequence({Op::ISUB})},
        WindowRule{{opStep(Op::SWAP), opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::IADD),
                    opStep(Op::ICONST_M1), opStep(Op::IXOR)},
                   buildSequence({Op::ISUB})},
        WindowRule{{opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::IADD),
                    opStep(Op::ICONST_1), opStep(Op::IADD)},
                   buildSequence({Op::ISUB})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::IXOR), opStep(Op::DUP_X2), opStep(Op::POP),
                    opStep(Op::SWAP), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::IAND), opStep(Op::ICONST_2), opStep(Op::IMUL),
                    opStep(Op::ISUB)},
                   buildSequence({Op::ISUB})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::IAND), opStep(Op::DUP_X2), opStep(Op::POP), opStep(Op::SWAP),
                    opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::IAND),
                    opStep(Op::ISUB)},
                   buildSequence({Op::ISUB})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::IAND), opStep(Op::ICONST_2), opStep(Op::IMUL),
                    opStep(Op::DUP_X2), opStep(Op::POP), opStep(Op::IXOR), opStep(Op::ISUB)},
                   buildSequence({Op::ISUB})},

        // iand 族
        WindowRule{{opStep(Op::SWAP), opStep(Op::DUP_X1), opStep(Op::ICONST_M1),
                    opStep(Op::IXOR), opStep(Op::IOR), opStep(Op::SWAP),
                    opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::ISUB)},
                   buildSequence({Op::IAND})},

        // ior 族
        WindowRule{{opStep(Op::DUP_X1), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::IAND), opStep(Op::IADD)},
                   buildSequence({Op::IOR})},

        // ixor 族
        WindowRule{{opStep(Op::DUP2), opStep(Op::IOR), opStep(Op::DUP_X2), opStep(Op::POP),
                    opStep(Op::IAND), opStep(Op::ISUB)},
                   buildSequence({Op::IXOR})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::IAND), opStep(Op::DUP_X2), opStep(Op::POP), opStep(Op::SWAP),
                    opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::IAND),
                    opStep(Op::IOR)},
                   buildSequence({Op::IXOR})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::IOR), opStep(Op::DUP_X2), opStep(Op::POP),
                    opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::SWAP),
                    opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::IOR),
                    opStep(Op::IAND)},
                   buildSequence({Op::IXOR})},
        WindowRule{{opStep(Op::DUP2), opStep(Op::IOR), opStep(Op::DUP_X2), opStep(Op::POP),
                    opStep(Op::IAND), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::IAND)},
                   buildSequence({Op::IXOR})},

        // 复合代数式
        WindowRule{{load, load, opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::IOR),
                    sameIntValueAs(1), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::ISUB)},
                   buildTwoLoadsAnd(Op::IAND)},
        WindowRule{{load, load, sameIntValueAs(0), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::IAND), opStep(Op::IADD)},
                   buildTwoLoadsAnd(Op::IOR)},
    };
}

}   // namespace

mjbase::spi::PreservedAnalyses
    ExpressionSimplifier::run(mjbase::spi::MethodScope&       scope,
                              mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    static const std::vector<WindowRule> kRules = makeRules();
    return window_rule::applyWindowRules(scope, kRules)
               ? mjbase::spi::PreservedAnalyses::none()
               : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
