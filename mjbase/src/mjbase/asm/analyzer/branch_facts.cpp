/// @file        branch_facts.cpp
/// @brief       分支预测事实实现：条件比较符 + switch 键匹配（读到达帧常量）。
#include "branch_facts.hpp"

namespace mjbase::asm_analyzer {

namespace {

using jbc::core::Op;

/// 一元 int 条件（与）。
bool conditionI(ca::i32 v, Op op) noexcept
{
    switch (op) {
    case Op::IFEQ: return v == 0;
    case Op::IFNE: return v != 0;
    case Op::IFLT: return v < 0;
    case Op::IFGE: return v >= 0;
    case Op::IFGT: return v > 0;
    case Op::IFLE: return v <= 0;
    default: return false;
    }
}

/// 二元 int 条件（与）。
bool conditionII(ca::i32 a, ca::i32 b, Op op) noexcept
{
    switch (op) {
    case Op::IF_ICMPEQ: return a == b;
    case Op::IF_ICMPNE: return a != b;
    case Op::IF_ICMPLT: return a < b;
    case Op::IF_ICMPGE: return a >= b;
    case Op::IF_ICMPGT: return a > b;
    case Op::IF_ICMPLE: return a <= b;
    default: return false;
    }
}

BranchPrediction decide(bool taken) noexcept
{
    BranchPrediction p;
    p.fact = taken ? BranchFact::Taken : BranchFact::FallThrough;
    return p;
}

/// 已知引用常量（本域只有 Null 与 String 两类事实）。
bool isKnownRef(const ConstLattice& v) noexcept { return v.isNull() || v.isStr(); }

}   // namespace

BranchPrediction predictBranch(const jbc::ir::CodeUnit& u, const ConstResult& frames,
                               jbc::ir::InsnId branch)
{
    BranchPrediction unknown;
    const jbc::ir::Insn* in = u.dom.peek(branch);
    if (in == nullptr) {
        return unknown;
    }
    const Op op = in->op;
    const auto operand = [&](ca::u32 fromTop) -> const ConstLattice* {
        return frames.stackFromTop(branch, fromTop);
    };

    switch (op) {
    // 一元 int 条件：栈顶为 Int 常量即可判定（旧 predictIf 的 int 路径）。
    case Op::IFEQ:
    case Op::IFNE:
    case Op::IFLT:
    case Op::IFGE:
    case Op::IFGT:
    case Op::IFLE: {
        const ConstLattice* v = operand(0);
        if (v == nullptr || !v->isInt()) {
            return unknown;
        }
        return decide(conditionI(static_cast<ca::i32>(v->asI64()), op));
    }

    // null 系：Null 常量与 String 常量（非空引用）可判定（旧 ReAnalyzer
    // 预测器 prunesKnownNullBranch 的语义；旧 predictIf 对本族不判定）。
    case Op::IFNULL:
    case Op::IFNONNULL: {
        const ConstLattice* v = operand(0);
        if (v == nullptr) {
            return unknown;
        }
        if (v->isNull()) {
            return decide(op == Op::IFNULL);
        }
        if (v->isStr()) {
            return decide(op == Op::IFNONNULL);
        }
        return unknown;
    }

    // 二元 int 条件：栈顶两行（fromTop 0 为右操作数）均为 Int 常量。
    case Op::IF_ICMPEQ:
    case Op::IF_ICMPNE:
    case Op::IF_ICMPLT:
    case Op::IF_ICMPGE:
    case Op::IF_ICMPGT:
    case Op::IF_ICMPLE: {
        const ConstLattice* b = operand(0);
        const ConstLattice* a = operand(1);
        if (a == nullptr || b == nullptr || !a->isInt() || !b->isInt()) {
            return unknown;
        }
        return decide(conditionII(static_cast<ca::i32>(a->asI64()),
                                  static_cast<ca::i32>(b->asI64()), op));
    }

    // 引用相等：两侧均为已知引用常量即可比（旧 ReAnalyzer 预测器语义；
    // Null 与 String 按 intern 语义判等——Null/Null 相等，Null/非空不等，
    // String/String 按内容）。
    case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE: {
        const ConstLattice* b = operand(0);
        const ConstLattice* a = operand(1);
        if (a == nullptr || b == nullptr || !isKnownRef(*a) || !isKnownRef(*b)) {
            return unknown;
        }
        bool equal = false;
        if (a->isNull() && b->isNull()) {
            equal = true;
        }
        else if (a->isStr() && b->isStr()) {
            equal = frames.strOf(*a) == frames.strOf(*b);
        }
        return decide(op == Op::IF_ACMPEQ ? equal : !equal);
    }

    // TABLESWITCH：CodeDom 契约 targets[i] 对应 key = a + i；越界键走
    // default（与旧 predictTableSwitch 的 labels.size() 防越界分支同语义）。
    case Op::TABLESWITCH: {
        const ConstLattice* k = operand(0);
        if (k == nullptr || !k->isInt()) {
            return unknown;
        }
        const ca::i64 key = k->asI64();
        const ca::i64 low = in->a;
        BranchPrediction p;
        p.fact = BranchFact::Case;
        p.caseIndex =
            (key < low || key - low >= static_cast<ca::i64>(in->targets.size()))
                ? -1
                : static_cast<ca::i32>(key - low);
        return p;
    }

    // LOOKUPSWITCH：扫描 keys 命中 case，否则 default。
    case Op::LOOKUPSWITCH: {
        const ConstLattice* k = operand(0);
        if (k == nullptr || !k->isInt()) {
            return unknown;
        }
        const ca::i64 key = k->asI64();
        BranchPrediction p;
        p.fact = BranchFact::Case;
        p.caseIndex = -1;
        for (ca::usize i = 0; i < in->keys.size(); ++i) {
            if (static_cast<ca::i64>(in->keys[i]) == key) {
                p.caseIndex = static_cast<ca::i32>(i);
                break;
            }
        }
        return p;
    }

    default:
        // GOTO/return 等无条件控制流与 JSR/RET 不是预测问题（引擎职责）。
        return unknown;
    }
}

}   // namespace mjbase::asm_analyzer
