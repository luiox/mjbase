#include "mjbase/pass/fix_pattern_simplifier.hpp"

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

bool isTwoSlotIf(Op op)
{
    switch (op) {
    case Op::IF_ICMPEQ:
    case Op::IF_ICMPNE:
    case Op::IF_ICMPLT:
    case Op::IF_ICMPGE:
    case Op::IF_ICMPGT:
    case Op::IF_ICMPLE: return true;
    default: return false;
    }
}

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

bool isIntLoadOp(Op op)
{
    return op == Op::ILOAD || (op >= Op::ILOAD_0 && op <= Op::ILOAD_3);
}

ca::u32 slotOfIntLoad(const Insn& in)
{
    if (in.op == Op::ILOAD) return static_cast<ca::u32>(in.a);
    return static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::ILOAD_0);
}

/// 规则 12 步进：int 局部装载（仅装载，不含常量形态）。
StepFn intLocalLoadStep()
{
    return [](const ClassSession&, const Insn& in, const std::vector<const Insn*>&) {
        return isIntLoadOp(in.op);
    };
}

/// 规则 12 步进：与 matched[0] 同槽的 int 局部装载。
StepFn sameSlotIntLoadStep()
{
    return [](const ClassSession&, const Insn& in, const std::vector<const Insn*>& matched) {
        return !matched.empty() && isIntLoadOp(in.op)
            && slotOfIntLoad(in) == slotOfIntLoad(*matched[0]);
    };
}

/// 规则 12 步进：与 matched[against] 同值的 int 常量（跨 CP 条目同权）。
StepFn sameValueIntConstStep(ca::usize against)
{
    return [against](const ClassSession& session, const Insn& in,
                     const std::vector<const Insn*>& matched) {
        if (matched.size() <= against) return false;
        ca::i32 v = 0;
        ca::i32 w = 0;
        return method_rewrite_util::intValueOf(session, in, v)
            && method_rewrite_util::intValueOf(session, *matched[against], w) && v == w;
    };
}

bool isLongLoadOp(Op op)
{
    return op == Op::LLOAD || (op >= Op::LLOAD_0 && op <= Op::LLOAD_3);
}

ca::u32 slotOfLongLoad(const Insn& in)
{
    if (in.op == Op::LLOAD) return static_cast<ca::u32>(in.a);
    return static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::LLOAD_0);
}

/// 规则 15 步进：long 局部装载（仅装载，不含常量形态）。
StepFn longLocalLoadStep()
{
    return [](const ClassSession&, const Insn& in, const std::vector<const Insn*>&) {
        return isLongLoadOp(in.op);
    };
}

/// 规则 15 步进：与 matched[0] 同槽的 long 局部装载。
StepFn sameSlotLongLoadStep()
{
    return [](const ClassSession&, const Insn& in, const std::vector<const Insn*>& matched) {
        return !matched.empty() && isLongLoadOp(in.op)
            && slotOfLongLoad(in) == slotOfLongLoad(*matched[0]);
    };
}

/// 规则 15/16/17 步进：long 常量（LCONST_0/1 + ldc 族 Long，跨 CP 条目同权）。
StepFn longConstStep()
{
    return [](const ClassSession& session, const Insn& in, const std::vector<const Insn*>&) {
        ca::i64 value = 0;
        return method_rewrite_util::longValueOf(session, in, value);
    };
}

/// 规则 15 步进：与 matched[against] 同值的 long 常量。
StepFn sameValueLongConstStep(ca::usize against)
{
    return [against](const ClassSession& session, const Insn& in,
                     const std::vector<const Insn*>& matched) {
        if (matched.size() <= against) return false;
        ca::i64 v = 0;
        ca::i64 w = 0;
        return method_rewrite_util::longValueOf(session, in, v)
            && method_rewrite_util::longValueOf(session, *matched[against], w) && v == w;
    };
}

StepFn ifTwoSlotStep()
{
    return [](const ClassSession&, const Insn& in, const std::vector<const Insn*>&) {
        return isTwoSlotIf(in.op);
    };
}

StepFn sameSlotILoadAfterStoreStep()
{
    return [](const ClassSession&, const Insn& in, const std::vector<const Insn*>& matched) {
        if (in.op != Op::ILOAD || matched.size() < 2) return false;
        const Insn* store = matched[1];
        return store->op == Op::ISTORE && store->a == in.a;
    };
}

Insn synthetic(Op op)
{
    Insn in = CodeEditor::simple(op);
    in.srcPc = jbc::ir::kNoPc;
    return in;
}

Insn materializeIntConst(ClassSession& session, ca::i32 v)
{
    Insn in;
    if (v >= -1 && v <= 5) {
        in.op    = static_cast<Op>(static_cast<ca::u8>(Op::ICONST_M1) + (v + 1));
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    if (v >= -128 && v <= 127) {
        in      = CodeEditor::simple(Op::BIPUSH);
        in.a    = v;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    if (v >= -32768 && v <= 32767) {
        in      = CodeEditor::simple(Op::SIPUSH);
        in.a    = v;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    in      = CodeEditor::loadConst(session.internInteger(v));
    in.srcPc = jbc::ir::kNoPc;
    return in;
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

bool buildNothing(ClassSession&, const std::vector<const Insn*>&, std::vector<Insn>& out)
{
    out.clear();
    return true;
}

/// 规则 12：半加器窗口 → iload s; ldc K; iadd。
bool buildHalfAdderFold(ClassSession&, const std::vector<const Insn*>& matched,
                        std::vector<Insn>& out)
{
    out.clear();
    out.push_back(*matched[0]);
    out.push_back(*matched[1]);
    out.push_back(synthetic(Op::IADD));
    return true;
}

/// 规则 13：借位窗口 → ldc K; isub（净效应 x → x-K）。
bool buildBorrowSubFold(ClassSession&, const std::vector<const Insn*>& matched,
                        std::vector<Insn>& out)
{
    out.clear();
    out.push_back(*matched[0]);
    out.push_back(synthetic(Op::ISUB));
    return true;
}

/// 规则 14：加法消进位窗口 → ldc K; ixor（净效应 x → x^K）。
bool buildCarryCancelXorFold(ClassSession&, const std::vector<const Insn*>& matched,
                             std::vector<Insn>& out)
{
    out.clear();
    out.push_back(*matched[0]);
    out.push_back(synthetic(Op::IXOR));
    return true;
}

/// 规则 15：long 半加器窗口 → lload s; ldc2_w K; ladd（净效应 x → x+K）。
bool buildHalfAdderLongFold(ClassSession&, const std::vector<const Insn*>& matched,
                            std::vector<Insn>& out)
{
    out.clear();
    out.push_back(*matched[0]);
    out.push_back(*matched[1]);
    out.push_back(synthetic(Op::LADD));
    return true;
}

/// 规则 16：long 借位窗口 → lload s; ldc2_w K; lsub（净效应 x → x-K）。
bool buildBorrowLongSubFold(ClassSession&, const std::vector<const Insn*>& matched,
                            std::vector<Insn>& out)
{
    out.clear();
    out.push_back(*matched[0]);
    out.push_back(*matched[1]);
    out.push_back(synthetic(Op::LSUB));
    return true;
}

/// 规则 17：long 加法消进位窗口 → lload s; ldc2_w K; lxor（净效应 x → x^K）。
bool buildCarryCancelLongXorFold(ClassSession&, const std::vector<const Insn*>& matched,
                                 std::vector<Insn>& out)
{
    out.clear();
    out.push_back(*matched[0]);
    out.push_back(*matched[1]);
    out.push_back(synthetic(Op::LXOR));
    return true;
}

bool buildCompareFold(ClassSession& session, const std::vector<const Insn*>& matched,
                      std::vector<Insn>& out)
{
    ca::i32 left  = 0;
    ca::i32 right = 0;
    if (!method_rewrite_util::intValueOf(session, *matched[0], left)
        || !method_rewrite_util::intValueOf(session, *matched[2], right)) {
        return false;
    }

    const Op op = matched[3]->op;
    if (!isTwoSlotIf(op)) return false;
    const bool equality = op == Op::IF_ICMPEQ || op == Op::IF_ICMPNE;
    if (!equality && left != 0) return false;

    const ca::u32 bits = static_cast<ca::u32>(right) - static_cast<ca::u32>(left);
    ca::i32       diff = 0;
    static_assert(sizeof(diff) == sizeof(bits), "int 与 u32 位宽不一致");
    std::memcpy(&diff, &bits, sizeof(diff));

    // LDC 物化需要追加常量池；容量不足时放弃本次折叠（BIPUSH/SIPUSH/ICONST
    // 不占常量池）。
    if ((diff < -32768 || diff > 32767)
        && session.backing().cp.size() + 1 > 0xFFFFu) {
        return false;
    }

    out.clear();
    out.push_back(materializeIntConst(session, diff));
    out.push_back(*matched[3]);
    return true;
}

bool buildStoreLoadBounce(ClassSession&, const std::vector<const Insn*>& matched,
                          std::vector<Insn>& out)
{
    const Insn& producer = *matched[0];
    const Insn& store    = *matched[1];

    // 同 slot 的通用 ILOAD producer：替换后 reload 位 clone 会再次匹配本规则，
    // apply-until-converge 不收敛；旧栈在此显式放弃本次匹配。
    if (producer.op == Op::ILOAD && producer.a == store.a) return false;

    out.clear();
    out.reserve(5);
    out.push_back(producer);
    out.push_back(store);
    out.push_back(*matched[2]);
    Insn reload = producer;
    reload.srcPc = jbc::ir::kNoPc;   // LNT 锚只归窗口头
    out.push_back(reload);
    out.push_back(*matched[4]);
    return true;
}

std::vector<WindowRule> makeRules()
{
    return {
        // 1. ineg; iconst_m1; iadd -> iconst_m1; ixor
        WindowRule{{opStep(Op::INEG), opStep(Op::ICONST_M1), opStep(Op::IADD)},
                   buildSequence({Op::ICONST_M1, Op::IXOR})},

        // 2. swap; dup_x1; iand; isub -> iconst_m1; ixor; iand
        WindowRule{{opStep(Op::SWAP), opStep(Op::DUP_X1), opStep(Op::IAND), opStep(Op::ISUB)},
                   buildSequence({Op::ICONST_M1, Op::IXOR, Op::IAND})},

        // 3. dup_x1; ior; swap; isub -> iconst_m1; ixor; iand
        WindowRule{{opStep(Op::DUP_X1), opStep(Op::IOR), opStep(Op::SWAP), opStep(Op::ISUB)},
                   buildSequence({Op::ICONST_M1, Op::IXOR, Op::IAND})},

        // 4. dup_x1; ior; isub; ineg -> iconst_m1; ixor; iand
        WindowRule{{opStep(Op::DUP_X1), opStep(Op::IOR), opStep(Op::ISUB), opStep(Op::INEG)},
                   buildSequence({Op::ICONST_M1, Op::IXOR, Op::IAND})},

        // 5. swap; isub; iconst_1; isub -> isub; iconst_m1; ixor
        WindowRule{
            {opStep(Op::SWAP), opStep(Op::ISUB), opStep(Op::ICONST_1), opStep(Op::ISUB)},
            buildSequence({Op::ISUB, Op::ICONST_M1, Op::IXOR})},

        // 6. swap; iconst_m1; ixor; iadd -> isub; iconst_m1; ixor
        WindowRule{
            {opStep(Op::SWAP), opStep(Op::ICONST_M1), opStep(Op::IXOR), opStep(Op::IADD)},
            buildSequence({Op::ISUB, Op::ICONST_M1, Op::IXOR})},

        // 7. dup_x1; iconst_m1; ixor; iand; iadd -> ior
        WindowRule{{opStep(Op::DUP_X1), opStep(Op::ICONST_M1), opStep(Op::IXOR),
                    opStep(Op::IAND), opStep(Op::IADD)},
                   buildSequence({Op::IOR})},

        // 8. loadInt; dup; ixor -> iconst_0
        WindowRule{{loadIntStep(), opStep(Op::DUP), opStep(Op::IXOR)},
                   buildSequence({Op::ICONST_0})},

        // 9. 死代码固定序列
        WindowRule{
            {opStep(Op::ICONST_1), opStep(Op::DUP), opStep(Op::IADD), opStep(Op::I2L),
             opStep(Op::L2I), opStep(Op::POP)},
            buildNothing},
        WindowRule{{opStep(Op::ICONST_1), opStep(Op::POP)}, buildNothing},
        WindowRule{{opStep(Op::ICONST_1), opStep(Op::DUP), opStep(Op::IADD), opStep(Op::DUP),
                    opStep(Op::I2L), opStep(Op::L2I), opStep(Op::SWAP), opStep(Op::POP),
                    opStep(Op::POP)},
                   buildNothing},
        WindowRule{{opStep(Op::ICONST_M1), opStep(Op::DUP), opStep(Op::IMUL), opStep(Op::POP)},
                   buildNothing},
        WindowRule{{opStep(Op::ICONST_2), opStep(Op::DUP), opStep(Op::IMUL), opStep(Op::I2L),
                    opStep(Op::L2I), opStep(Op::DUP), opStep(Op::SWAP), opStep(Op::POP)},
                   buildNothing},
        WindowRule{{opStep(Op::LCONST_1), opStep(Op::POP2)}, buildNothing},
        WindowRule{{opStep(Op::LCONST_0), opStep(Op::POP2)}, buildNothing},

        // 10. constInt; iadd; constInt; if_icmp* -> push(c2 - c1); if_icmp*
        WindowRule{{intConstStep(), opStep(Op::IADD), intConstStep(), ifTwoSlotStep()},
                   buildCompareFold},

        // 11. loadInt; istore s; loadInt; iload s; if_icmp*
        //     -> P; istore s; C; P; if_icmp*
        WindowRule{{loadIntStep(), opStep(Op::ISTORE), loadIntStep(),
                    sameSlotILoadAfterStoreStep(), ifTwoSlotStep()},
                   buildStoreLoadBounce},

        // 12. iload s; ldc K; iload s; ldc K; ixor; dup_x2; pop; iand;
        //     iconst_1; ishl; iadd -> iload s; ldc K; iadd
        //     ((x^K)+((x&K)<<1) == x+K，半加器进位分解)
        WindowRule{{intLocalLoadStep(), intConstStep(), sameSlotIntLoadStep(),
                    sameValueIntConstStep(1), opStep(Op::IXOR), opStep(Op::DUP_X2),
                    opStep(Op::POP), opStep(Op::IAND), opStep(Op::ICONST_1),
                    opStep(Op::ISHL), opStep(Op::IADD)},
                   buildHalfAdderFold},

        // 13. ldc K; dup2; ixor; dup_x2; pop; swap; iconst_m1; ixor; iand;
        //     iconst_1; ishl; isub -> ldc K; isub
        //     ((x^K)-((K&~x)<<1) == x-K，借位形式)
        WindowRule{{intConstStep(), opStep(Op::DUP2), opStep(Op::IXOR), opStep(Op::DUP_X2),
                    opStep(Op::POP), opStep(Op::SWAP), opStep(Op::ICONST_M1),
                    opStep(Op::IXOR), opStep(Op::IAND), opStep(Op::ICONST_1),
                    opStep(Op::ISHL), opStep(Op::ISUB)},
                   buildBorrowSubFold},

        // 14. ldc K; dup2; iadd; dup_x2; pop; iand; iconst_1; ishl; isub
        //     -> ldc K; ixor    ((x+K)-((x&K)<<1) == x^K)
        WindowRule{{intConstStep(), opStep(Op::DUP2), opStep(Op::IADD), opStep(Op::DUP_X2),
                    opStep(Op::POP), opStep(Op::IAND), opStep(Op::ICONST_1),
                    opStep(Op::ISHL), opStep(Op::ISUB)},
                   buildCarryCancelXorFold},

        // ── #44 long 族（lxor/land/lshl 位级恒等式。long 与 int 族窗口形态差
        // 异：cat2 值占双槽，dup 家族按槽位语义只能把复制品下插 1/2 槽——
        // dup2_x2 在 all-cat2 三值栈 [x,K,v] 上产出 [x,v,K,v]（libjbc
        // dupSlots 与真 JVM 实测一致），且无 dup2_x4、swap 对 cat2 非法——
        // int 窗口中段的 dup_x2/pop/swap 重排在 long 上不可达。故 long 窗
        // 弃用 dup 舞蹈，改为直排形态：修正项（land + (…)<<1）与主项各算各
        // 的，lsub 按"深减顶"排操作数（ladd 可交换不敏感）；同槽装载/同值
        // 常量校验照 int 族；lshl 移位量仍是 int 槽）──

        // 15. lload s; ldc2_w K; land; iconst_1; lshl; lload s; ldc2_w K;
        //     lxor; ladd -> lload s; ldc2_w K; ladd
        //     ((x^K)+((x&K)<<1) == x+K，半加器进位分解 long 形态：先算进位项
        //     (x&K)<<1 留底，再算主项 x^K 后 ladd 收拢)
        WindowRule{{longLocalLoadStep(), longConstStep(), opStep(Op::LAND),
                    opStep(Op::ICONST_1), opStep(Op::LSHL), sameSlotLongLoadStep(),
                    sameValueLongConstStep(1), opStep(Op::LXOR), opStep(Op::LADD)},
                   buildHalfAdderLongFold},

        // 16. lload s; ldc2_w K; lxor; lload s; iconst_m1; i2l; lxor;
        //     ldc2_w K; land; iconst_1; lshl; lsub -> lload s; ldc2_w K; lsub
        //     ((x^K)-((K&~x)<<1) == x-K，借位形式 long 形态：主项先行，~x 由
        //     同槽重装载 + iconst_m1/i2l/lxor 构造，land 出借位项压顶后 lsub
        //     收拢——lsub 语义为深减顶，被减数 (x^K) 必须在深、借位项在顶)
        WindowRule{{longLocalLoadStep(), longConstStep(), opStep(Op::LXOR),
                    sameSlotLongLoadStep(), opStep(Op::ICONST_M1), opStep(Op::I2L),
                    opStep(Op::LXOR), sameValueLongConstStep(1), opStep(Op::LAND),
                    opStep(Op::ICONST_1), opStep(Op::LSHL), opStep(Op::LSUB)},
                   buildBorrowLongSubFold},

        // 17. lload s; ldc2_w K; ladd; lload s; ldc2_w K; land; iconst_1;
        //     lshl; lsub -> lload s; ldc2_w K; lxor
        //     ((x+K)-((x&K)<<1) == x^K，消进位 long 形态：主项 ladd 先行，
        //     进位项压顶（lsub 深减顶）。与 15 在第 2 位 ladd/land 分叉、
        //     与 16 在第 2 位 ladd/lxor 分叉，互不为前缀)
        WindowRule{{longLocalLoadStep(), longConstStep(), opStep(Op::LADD),
                    sameSlotLongLoadStep(), sameValueLongConstStep(1),
                    opStep(Op::LAND), opStep(Op::ICONST_1), opStep(Op::LSHL),
                    opStep(Op::LSUB)},
                   buildCarryCancelLongXorFold},

        // 18. lload s; dup2; lxor -> lconst_0    (x ^ x == 0，long 形态；仅
        //     收局部变量装载头，常量头 K^K 交给常量折叠链。15/16/17 新窗口
        //     不含 dup2，全局面匹配无吞窗口冲突)
        WindowRule{{longLocalLoadStep(), opStep(Op::DUP2), opStep(Op::LXOR)},
                   buildSequence({Op::LCONST_0})},

        // 19. lload s; dup2; lcmp -> iconst_0    (x cmp x == 0，cmp 族；同 18)
        WindowRule{{longLocalLoadStep(), opStep(Op::DUP2), opStep(Op::LCMP)},
                   buildSequence({Op::ICONST_0})},

        // 20. i2l; l2i -> 空（转换族往返恒等：int→long→int 位级还原、净效应
        //     零。逆序 l2i; i2l 是截断 + 符号扩展非恒等，不收）
        WindowRule{{opStep(Op::I2L), opStep(Op::L2I)}, buildNothing},
    };
}

}   // namespace

mjbase::spi::PreservedAnalyses FixPatternSimplifier::run(mjbase::spi::MethodScope&       scope,
                                                         mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    static const std::vector<WindowRule> kRules = makeRules();
    return window_rule::applyWindowRules(scope, kRules)
               ? mjbase::spi::PreservedAnalyses::none()
               : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
