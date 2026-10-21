#include "mjbase/pass/method_rewrite_util.hpp"

#include <cstring>

#include "jbc/core/opcode.hpp"

namespace mjbase::pass {

namespace method_rewrite_util {

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
    // 覆盖：LCONST_0/1 + ldc 族（LDC/LDC_W/LDC2_W）装载 Long。
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

bool doubleValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, double& out)
{
    using jbc::core::Op;
    // DCONST_0/1 + tree LDC 节点持 double（classfile 面
    // = ldc 族装载 Double）。
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

bool labelsAnchorAt(const jbc::ir::CodeDom& dom, jbc::ir::InsnId anchor)
{
    return !dom.labelsAt(anchor).empty();
}

}   // namespace method_rewrite_util

}   // namespace mjbase::pass
