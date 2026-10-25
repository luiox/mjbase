#include "mjbase/pass/method_rewrite_util.hpp"

#include <cmath>
#include <cstring>

#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"

namespace mjbase::pass {

namespace method_rewrite_util {

namespace {

ca::u32 internLong(jbc::ir::ClassSession& session, ca::i64 v)
{
    auto& cp = session.backing().cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (cp.valid(i) && cp.tagOf(i) == jbc::core::CpTag::Long && cp.at(i).lval == v) {
            return i;
        }
    }
    return cp.addLong(v);
}

ca::u32 internFloatBits(jbc::ir::ClassSession& session, ca::u32 bits)
{
    auto& cp = session.backing().cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (cp.valid(i) && cp.tagOf(i) == jbc::core::CpTag::Float
            && static_cast<ca::u32>(cp.at(i).ival) == bits) {
            return i;
        }
    }
    return cp.addFloatBits(bits);
}

ca::u32 internDoubleBits(jbc::ir::ClassSession& session, ca::u64 bits)
{
    auto& cp = session.backing().cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (cp.valid(i) && cp.tagOf(i) == jbc::core::CpTag::Double
            && static_cast<ca::u64>(cp.at(i).lval) == bits) {
            return i;
        }
    }
    return cp.addDoubleBits(bits);
}

}   // namespace

bool intValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i32& out)
{
    using jbc::core::Op;
    if (in.op >= Op::ICONST_M1 && in.op <= Op::ICONST_5) {
        static constexpr ca::i32 kValues[] = {-1, 0, 1, 2, 3, 4, 5};
        out = kValues[static_cast<ca::u8>(in.op) - static_cast<ca::u8>(Op::ICONST_M1)];
        return true;
    }
    if (in.op == Op::BIPUSH || in.op == Op::SIPUSH) {
        out = in.a;
        return true;
    }
    if (in.op == Op::LDC || in.op == Op::LDC_W) {
        jbc::ir::ClassSession::ConstSem c;
        if (session.constOf(in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::Int) {
            out = static_cast<ca::i32>(c.num);
            return true;
        }
    }
    return false;
}

bool longValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i64& out)
{
    using jbc::core::Op;
    // 可观测行为：LCONST_0/1 + ldc 族装载 Long（LDC2_W 在 classfile 面
    // 同样参与）。
    if (in.op == Op::LCONST_0 || in.op == Op::LCONST_1) {
        out = in.op == Op::LCONST_1 ? 1 : 0;
        return true;
    }
    if (in.op == Op::LDC || in.op == Op::LDC_W || in.op == Op::LDC2_W) {
        jbc::ir::ClassSession::ConstSem c;
        if (session.constOf(in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::Long) {
            out = c.num;
            return true;
        }
    }
    return false;
}

bool floatValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, float& out)
{
    using jbc::core::Op;
    if (in.op == Op::FCONST_0) {
        out = 0.0f;
        return true;
    }
    if (in.op == Op::FCONST_1) {
        out = 1.0f;
        return true;
    }
    if (in.op == Op::FCONST_2) {
        out = 2.0f;
        return true;
    }
    if (in.op == Op::LDC || in.op == Op::LDC_W) {
        jbc::ir::ClassSession::ConstSem c;
        if (session.constOf(in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::Float) {
            const ca::u32 bits = static_cast<ca::u32>(c.bits);
            static_assert(sizeof(float) == sizeof(bits), "float ����λͼ��ͬ��");
            std::memcpy(&out, &bits, sizeof(out));
            return true;
        }
    }
    return false;
}

bool doubleValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, double& out)
{
    using jbc::core::Op;
    // DCONST_0/1 + ldc 族装载 Double。
    if (in.op == Op::DCONST_0) {
        out = 0.0;
        return true;
    }
    if (in.op == Op::DCONST_1) {
        out = 1.0;
        return true;
    }
    if (in.op == Op::LDC || in.op == Op::LDC_W || in.op == Op::LDC2_W) {
        jbc::ir::ClassSession::ConstSem c;
        if (session.constOf(in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::Double) {
            static_assert(sizeof(double) == sizeof(c.bits), "double 与其位图案同宽");
            std::memcpy(&out, &c.bits, sizeof(out));   // 原始位图案还原 double 值
            return true;
        }
    }
    return false;
}

jbc::ir::Insn materializeIntConst(jbc::ir::ClassSession& session, ca::i32 v)
{
    using jbc::core::Op;
    using jbc::ir::CodeEditor;
    using jbc::ir::Insn;

    Insn in;
    if (v >= -1 && v <= 5) {
        in.op    = static_cast<Op>(static_cast<ca::u8>(Op::ICONST_M1) + (v + 1));
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    if (v >= -128 && v <= 127) {
        in       = CodeEditor::simple(Op::BIPUSH);
        in.a     = v;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    if (v >= -32768 && v <= 32767) {
        in       = CodeEditor::simple(Op::SIPUSH);
        in.a     = v;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    in       = CodeEditor::loadConst(session.internInteger(v));
    in.srcPc = jbc::ir::kNoPc;
    return in;
}

jbc::ir::Insn materializeLongConst(jbc::ir::ClassSession& session, ca::i64 v)
{
    using jbc::core::Op;
    using jbc::ir::CodeEditor;
    using jbc::ir::Insn;

    Insn in;
    if (v == 0 || v == 1) {
        in.op    = v == 0 ? Op::LCONST_0 : Op::LCONST_1;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    in       = CodeEditor::cpInsn(Op::LDC2_W, internLong(session, v));
    in.srcPc = jbc::ir::kNoPc;
    return in;
}

jbc::ir::Insn materializeFloatConst(jbc::ir::ClassSession& session, float v)
{
    using jbc::core::Op;
    using jbc::ir::CodeEditor;
    using jbc::ir::Insn;

    Insn in;
    if (v == 0.0f && !std::signbit(v)) {
        in.op    = Op::FCONST_0;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    if (v == 1.0f) {
        in.op    = Op::FCONST_1;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    if (v == 2.0f) {
        in.op    = Op::FCONST_2;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    ca::u32 bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float ����λͼ��ͬ��");
    std::memcpy(&bits, &v, sizeof(bits));
    in       = CodeEditor::loadConst(internFloatBits(session, bits));
    in.srcPc = jbc::ir::kNoPc;
    return in;
}

jbc::ir::Insn materializeDoubleConst(jbc::ir::ClassSession& session, double v)
{
    using jbc::core::Op;
    using jbc::ir::CodeEditor;
    using jbc::ir::Insn;

    Insn in;
    if (v == 0.0 && !std::signbit(v)) {
        in.op    = Op::DCONST_0;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    if (v == 1.0) {
        in.op    = Op::DCONST_1;
        in.srcPc = jbc::ir::kNoPc;
        return in;
    }
    ca::u64 bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "double ����λͼ��ͬ��");
    std::memcpy(&bits, &v, sizeof(bits));
    in       = CodeEditor::cpInsn(Op::LDC2_W, internDoubleBits(session, bits));
    in.srcPc = jbc::ir::kNoPc;
    return in;
}

bool stringValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                   std::string_view& out)
{
    using jbc::core::Op;
    if (in.op != Op::LDC && in.op != Op::LDC_W) {
        return false;
    }
    jbc::ir::ClassSession::ConstSem c;
    if (session.constOf(in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::String) {
        out = c.str;
        return true;
    }
    return false;
}

bool isPopFoldableConstant(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in)
{
    ca::i32        iv  = 0;
    ca::i64        lv  = 0;
    std::string_view sv;
    return intValueOf(session, in, iv) || longValueOf(session, in, lv)
           || stringValueOf(session, in, sv);
}

bool isPop2FoldableConstant(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in)
{
    ca::i64  lv = 0;
    double   dv = 0;
    return longValueOf(session, in, lv) || doubleValueOf(session, in, dv);
}

bool isPopFoldableLocalLoad(const jbc::ir::Insn& in)
{
    using jbc::core::Op;
    switch (in.op) {
    case Op::ILOAD:
    case Op::FLOAD:
    case Op::ALOAD: return true;
    default:        break;
    }
    const auto inRange = [](Op op, Op first, Op last) { return op >= first && op <= last; };
    return inRange(in.op, Op::ILOAD_0, Op::ILOAD_3) || inRange(in.op, Op::FLOAD_0, Op::FLOAD_3)
           || inRange(in.op, Op::ALOAD_0, Op::ALOAD_3);
}

bool isPop2FoldableLocalLoad(const jbc::ir::Insn& in)
{
    using jbc::core::Op;
    switch (in.op) {
    case Op::LLOAD:
    case Op::DLOAD: return true;
    default:        break;
    }
    const auto inRange = [](Op op, Op first, Op last) { return op >= first && op <= last; };
    return inRange(in.op, Op::LLOAD_0, Op::LLOAD_3) || inRange(in.op, Op::DLOAD_0, Op::DLOAD_3);
}

jbc::ir::LabelId switchCaseLabelOf(const jbc::ir::Insn& sw, ca::i32 key)
{
    using jbc::core::Op;
    if (sw.op == Op::LOOKUPSWITCH) {
        for (ca::usize i = 0; i < sw.keys.size() && i < sw.targets.size(); ++i) {
            if (sw.keys[i] == key) return sw.targets[i];
        }
        return sw.target;
    }
    // TABLESWITCH：targets[i] 对应 key = a + i（a 为 low）。
    const ca::i64 idx = static_cast<ca::i64>(key) - static_cast<ca::i64>(sw.a);
    if (idx >= 0 && idx < static_cast<ca::i64>(sw.targets.size())) {
        return sw.targets[static_cast<ca::usize>(idx)];
    }
    return sw.target;
}

bool labelsAnchorAt(const jbc::ir::CodeDom& dom, jbc::ir::InsnId anchor)
{
    return !dom.labelsAt(anchor).empty();
}

}   // namespace method_rewrite_util

}   // namespace mjbase::pass
