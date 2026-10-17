#include "mjbase/pass/constant_folder.hpp"

#include <cstring>
#include <limits>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/insn/insn_factory.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {
namespace {

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;

// ---------------------------------------------------------------------------
// Java 数值语义核心。与 mjbase/asm/evaluator/evaluator.cpp 的 in-house 实现
// 同构（该文件是 CodeDom 域 JVM 算术的既有先例）：
//   - 整数加减乘与移位一律经无符号域计算后按位读回——C++ 有符号溢出是 UB，
//     JVMS 语义是回绕（evaluator.cpp:27-54 同款）；
//   - ishr 对负值用显式符号扩展公式——右移负数是实现定义行为；
//   - div/rem 的守卫（除数非 0、非 MIN/-1）由 applyInt/applyLong 内完成，
//     失败返回 false = 不折叠；守卫之后 C++ 的 / 与 % 即 JLS §15.17.2/§15.17.3
//     语义（向零截断、余数符号跟随被除数），无 UB；
//   - float/double 用 C++ 基本运算：x64 SSE 的 IEEE-754 单/双精度结果与
//     JVM 逐位一致（实证前提：编译目标 x64、未开 fast-math；项目
//     set_languages("cxx17") 无快速数学开关）。
// ---------------------------------------------------------------------------

template <typename To, typename From>
To bitCast(const From& from) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bitCast size mismatch");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

/// 链支持的运算族（与 binOpOf 的 op 表一一对应）。
enum class Arith { Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr, Ushr };

bool isShiftArith(Arith ar) noexcept
{
    return ar == Arith::Shl || ar == Arith::Shr || ar == Arith::Ushr;
}

/// int 运算（JVMS §6.5 对应指令）。div/rem 的除数 0 与 MIN/-1 返回 false。
bool applyInt(Arith ar, ca::i32 a, ca::i32 b, ca::i32& out) noexcept
{
    switch (ar) {
    case Arith::Add:
        out = bitCast<ca::i32>(bitCast<ca::u32>(a) + bitCast<ca::u32>(b));
        return true;
    case Arith::Sub:
        out = bitCast<ca::i32>(bitCast<ca::u32>(a) - bitCast<ca::u32>(b));
        return true;
    case Arith::Mul:
        out = bitCast<ca::i32>(bitCast<ca::u32>(a) * bitCast<ca::u32>(b));
        return true;
    case Arith::Div:
        if (b == 0) return false;
        if (a == (std::numeric_limits<ca::i32>::min)() && b == -1) return false;
        out = a / b;
        return true;
    case Arith::Rem:
        if (b == 0) return false;
        if (a == (std::numeric_limits<ca::i32>::min)() && b == -1) return false;
        out = a % b;
        return true;
    case Arith::And:
        out = a & b;
        return true;
    case Arith::Or:
        out = a | b;
        return true;
    case Arith::Xor:
        out = a ^ b;
        return true;
    case Arith::Shl:
        out = bitCast<ca::i32>(bitCast<ca::u32>(a) << (static_cast<ca::u32>(b) & 0x1fu));
        return true;
    case Arith::Shr:
    {
        const ca::u32 s    = static_cast<ca::u32>(b) & 0x1fu;
        const ca::u32 bits = bitCast<ca::u32>(a);
        if (s == 0 || a >= 0) {
            out = bitCast<ca::i32>(bits >> s);
            return true;
        }
        out = bitCast<ca::i32>((bits >> s) | (~ca::u32{0} << (32u - s)));
        return true;
    }
    case Arith::Ushr:
        out = bitCast<ca::i32>(bitCast<ca::u32>(a) >> (static_cast<ca::u32>(b) & 0x1fu));
        return true;
    }
    return false;
}

/// long 运算。移位族（(long,int) 操作数形状）不在此面——由 applyLongShift
/// 承担，general 路径也拒绝 long 移位（见 run 的链步进）。
bool applyLong(Arith ar, ca::i64 a, ca::i64 b, ca::i64& out) noexcept
{
    switch (ar) {
    case Arith::Add:
        out = bitCast<ca::i64>(bitCast<ca::u64>(a) + bitCast<ca::u64>(b));
        return true;
    case Arith::Sub:
        out = bitCast<ca::i64>(bitCast<ca::u64>(a) - bitCast<ca::u64>(b));
        return true;
    case Arith::Mul:
        out = bitCast<ca::i64>(bitCast<ca::u64>(a) * bitCast<ca::u64>(b));
        return true;
    case Arith::Div:
        if (b == 0) return false;
        if (a == (std::numeric_limits<ca::i64>::min)() && b == -1) return false;
        out = a / b;
        return true;
    case Arith::Rem:
        if (b == 0) return false;
        if (a == (std::numeric_limits<ca::i64>::min)() && b == -1) return false;
        out = a % b;
        return true;
    case Arith::And:
        out = a & b;
        return true;
    case Arith::Or:
        out = a | b;
        return true;
    case Arith::Xor:
        out = a ^ b;
        return true;
    default:
        return false;   // 移位族不走此路径
    }
}

/// long 移位（JVMS §6.5 lshl/lshr/lushr：操作数为 (long, int)，移位量掩 6 位）。
bool applyLongShift(Arith ar, ca::i64 v, ca::i32 d, ca::i64& out) noexcept
{
    const ca::u32 s = static_cast<ca::u32>(d) & 0x3fu;
    switch (ar) {
    case Arith::Shl:
        out = bitCast<ca::i64>(bitCast<ca::u64>(v) << s);
        return true;
    case Arith::Shr:
    {
        const ca::u64 bits = bitCast<ca::u64>(v);
        if (s == 0 || v >= 0) {
            out = bitCast<ca::i64>(bits >> s);
            return true;
        }
        out = bitCast<ca::i64>((bits >> s) | (~ca::u64{0} << (64u - s)));
        return true;
    }
    case Arith::Ushr:
        out = bitCast<ca::i64>(bitCast<ca::u64>(v) >> s);
        return true;
    default:
        return false;
    }
}

/// float 运算（fadd/fsub/fmul/fdiv）。除 0 不抛异常（IEEE 产 ±Inf/NaN），
/// 照常折叠——与 idiv/irem 的除零红线不同（JVMS §6.5 fdiv 无异常条款）。
bool applyFloat(Arith ar, ca::f32 a, ca::f32 b, ca::f32& out) noexcept
{
    switch (ar) {
    case Arith::Add: out = a + b; return true;
    case Arith::Sub: out = a - b; return true;
    case Arith::Mul: out = a * b; return true;
    case Arith::Div: out = a / b; return true;
    default: return false;
    }
}

/// double 运算（dadd/dsub/dmul/ddiv）。同 float：除 0 折叠。
bool applyDouble(Arith ar, ca::f64 a, ca::f64 b, ca::f64& out) noexcept
{
    switch (ar) {
    case Arith::Add: out = a + b; return true;
    case Arith::Sub: out = a - b; return true;
    case Arith::Mul: out = a * b; return true;
    case Arith::Div: out = a / b; return true;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// 常量装载读取（输入边界）。int/long/double 复用方法级改写族共享探测面；
// float 本地补（util 无 float 臂），与 util 的 double 臂同构。
// ---------------------------------------------------------------------------

enum class Kind { Int, Long, Float, Double };

struct ConstVal
{
    Kind    kind = Kind::Int;
    ca::i32 i    = 0;
    ca::i64 l    = 0;
    ca::f32 f    = 0.0f;
    ca::f64 d    = 0.0;
};

bool readConstPush(const jbc::ir::ClassSession& session, const Insn& in, ConstVal& out)
{
    if (method_rewrite_util::intValueOf(session, in, out.i)) {
        out.kind = Kind::Int;
        return true;
    }
    if (method_rewrite_util::longValueOf(session, in, out.l)) {
        out.kind = Kind::Long;
        return true;
    }
    if (method_rewrite_util::doubleValueOf(session, in, out.d)) {
        out.kind = Kind::Double;
        return true;
    }
    if (in.op == Op::FCONST_0) {
        out.f   = 0.0f;
        out.kind = Kind::Float;
        return true;
    }
    if (in.op == Op::FCONST_1) {
        out.f   = 1.0f;
        out.kind = Kind::Float;
        return true;
    }
    if (in.op == Op::FCONST_2) {
        out.f   = 2.0f;
        out.kind = Kind::Float;
        return true;
    }
    if (in.op == Op::LDC || in.op == Op::LDC_W || in.op == Op::LDC2_W) {
        jbc::ir::ClassSession::ConstSem c;
        if (session.constOf(in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::Float) {
            // ConstSem::bits 为 Float/Double 共用的 u64：Float 图案零扩展于低 32 位
            //（operand.cpp: out.bits = static_cast<ca::u32>(e.ival)），先收窄再 memcpy
            // 位级还原，避免 double 版"宽度相等"断言在 float 上误判。
            static_assert(sizeof(ca::u32) <= sizeof(c.bits), "float 位图案须容纳于 ConstSem::bits");
            const ca::u32 fbits = static_cast<ca::u32>(c.bits);
            std::memcpy(&out.f, &fbits, sizeof(out.f));   // 原始位图案还原 float 值
            out.kind = Kind::Float;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 二元 op 表：任务书列的 i/l/f/d 对应变体全集。
// ---------------------------------------------------------------------------

struct BinOp
{
    Kind  kind  = Kind::Int;
    Arith arith = Arith::Add;
};

bool binOpOf(Op op, BinOp& out)
{
    switch (op) {
    case Op::IADD: out = {Kind::Int, Arith::Add}; return true;
    case Op::ISUB: out = {Kind::Int, Arith::Sub}; return true;
    case Op::IMUL: out = {Kind::Int, Arith::Mul}; return true;
    case Op::IDIV: out = {Kind::Int, Arith::Div}; return true;
    case Op::IREM: out = {Kind::Int, Arith::Rem}; return true;
    case Op::IAND: out = {Kind::Int, Arith::And}; return true;
    case Op::IOR:  out = {Kind::Int, Arith::Or};  return true;
    case Op::IXOR: out = {Kind::Int, Arith::Xor}; return true;
    case Op::ISHL: out = {Kind::Int, Arith::Shl}; return true;
    case Op::ISHR: out = {Kind::Int, Arith::Shr}; return true;
    case Op::IUSHR: out = {Kind::Int, Arith::Ushr}; return true;
    case Op::LADD: out = {Kind::Long, Arith::Add}; return true;
    case Op::LSUB: out = {Kind::Long, Arith::Sub}; return true;
    case Op::LMUL: out = {Kind::Long, Arith::Mul}; return true;
    case Op::LDIV: out = {Kind::Long, Arith::Div}; return true;
    case Op::LREM: out = {Kind::Long, Arith::Rem}; return true;
    case Op::LAND: out = {Kind::Long, Arith::And}; return true;
    case Op::LOR:  out = {Kind::Long, Arith::Or};  return true;
    case Op::LXOR: out = {Kind::Long, Arith::Xor}; return true;
    case Op::LSHL: out = {Kind::Long, Arith::Shl}; return true;
    case Op::LSHR: out = {Kind::Long, Arith::Shr}; return true;
    case Op::LUSHR: out = {Kind::Long, Arith::Ushr}; return true;
    case Op::FADD: out = {Kind::Float, Arith::Add}; return true;
    case Op::FSUB: out = {Kind::Float, Arith::Sub}; return true;
    case Op::FMUL: out = {Kind::Float, Arith::Mul}; return true;
    case Op::FDIV: out = {Kind::Float, Arith::Div}; return true;
    case Op::DADD: out = {Kind::Double, Arith::Add}; return true;
    case Op::DSUB: out = {Kind::Double, Arith::Sub}; return true;
    case Op::DMUL: out = {Kind::Double, Arith::Mul}; return true;
    case Op::DDIV: out = {Kind::Double, Arith::Div}; return true;
    default: return false;
    }
}

bool applyBinOp(const BinOp& op, const ConstVal& a, const ConstVal& b, ConstVal& out)
{
    switch (op.kind) {
    case Kind::Int:   return applyInt(op.arith, a.i, b.i, out.i);
    case Kind::Long:  return applyLong(op.arith, a.l, b.l, out.l);
    case Kind::Float: return applyFloat(op.arith, a.f, b.f, out.f);
    case Kind::Double: return applyDouble(op.arith, a.d, b.d, out.d);
    }
    return false;
}

// ---------------------------------------------------------------------------
// 物化：折叠结果 → 替换头指令。短形态策略走 mjbase::asm_insn 的常量工厂
// （insn_factory.hpp:119-133 头注即本组合模式的依据：短形态工厂 + 调用方
// intern 后走 CP 路径）；CP 追加一律先过容量预检（对齐 units.cpp
// commitClassMetadata 的 cp.size() + 追加量 > 0xFFFF 拒绝式），预检在
// 任何 addXxx 之前。
// ---------------------------------------------------------------------------

bool buildHeadInsn(jbc::ir::ClassSession& session, const ConstVal& v, Insn& out)
{
    jbc::core::ClassFile& cf = session.backing();
    out                      = Insn{};
    switch (v.kind) {
    case Kind::Int:
        if (const auto r = mjbase::asm_insn::intConstInsn(v.i); r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        if (cf.cp.size() + 1 > 0xFFFFu) return false;
        out = jbc::ir::CodeEditor::loadConst(session.internInteger(v.i));
        return true;
    case Kind::Long:
        if (const auto r = mjbase::asm_insn::longConstInsn(v.l); r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        if (cf.cp.size() + 2 > 0xFFFFu) return false;
        out.op = Op::LDC2_W;
        out.cp = cf.cp.addLong(v.l);
        return true;
    case Kind::Float:
        if (const auto r = mjbase::asm_insn::floatConstInsn(v.f); r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        if (cf.cp.size() + 1 > 0xFFFFu) return false;
        {
            ca::u32 bits = 0;
            std::memcpy(&bits, &v.f, sizeof(bits));
            out = jbc::ir::CodeEditor::loadConst(cf.cp.addFloatBits(bits));
        }
        return true;
    case Kind::Double:
        if (const auto r = mjbase::asm_insn::doubleConstInsn(v.d); r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        if (cf.cp.size() + 2 > 0xFFFFu) return false;
        {
            ca::u64 bits = 0;
            std::memcpy(&bits, &v.d, sizeof(bits));
            out.op = Op::LDC2_W;
            out.cp = cf.cp.addDoubleBits(bits);
        }
        return true;
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses ConstantFolder::run(mjbase::spi::MethodScope&       scope,
                                                   mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::ClassSession& session = scope.session;
    CodeDom&                dom     = scope.code.dom;
    jbc::ir::CodeEditor     editor(scope.code);

    // 快照扫描：本轮内的插入/删除只发生在游标之前的窗口，快照句柄全程有效
    // （与 redundant_load_val_eliminator / char_arr_to_str_simplifier 同款）。
    const std::vector<InsnId> ids = dom.order();
    const ca::usize           n   = ids.size();

    bool modified = false;
    for (ca::usize i = 0; i < n;) {
        const Insn* headInsn = dom.peek(ids[i]);
        if (headInsn == nullptr) {
            ++i;
            continue;
        }
        ConstVal first{};
        if (!readConstPush(session, *headInsn, first)) {
            ++i;
            continue;
        }

        // ---- 链步进：虚拟操作数栈跟踪本链自身压入的值 ----
        const Kind                kind = first.kind;
        std::vector<ConstVal>     stack;
        stack.push_back(first);
        ca::usize                 foldEnd  = i;   // 最后「栈深 1 且已消费运算」的位置
        bool                      consumed = false;

        ca::usize j = i + 1;
        while (j < n) {
            const Insn* in = dom.peek(ids[j]);
            if (in == nullptr) break;
            ConstVal  v{};
            const bool isPush = readConstPush(session, *in, v);
            if (isPush && v.kind == kind) {
                stack.push_back(v);
                ++j;
                continue;
            }
            // long 链特例：int 常量装载紧随 LSHL/LSHR/LUSHR → 按对消费
            // （弹 long、取 int 移位量、压回 long，栈深不变）。
            if (kind == Kind::Long && isPush && v.kind == Kind::Int && j + 1 < n) {
                const Insn* nxt = dom.peek(ids[j + 1]);
                BinOp       nb{};
                if (nxt != nullptr && binOpOf(nxt->op, nb) && nb.kind == Kind::Long
                    && isShiftArith(nb.arith)) {
                    ca::i64 shifted = 0;
                    if (!applyLongShift(nb.arith, stack.back().l, v.i, shifted)) break;
                    stack.back().l = shifted;
                    j += 2;
                    if (stack.size() == 1) {
                        foldEnd  = j - 1;
                        consumed = true;
                    }
                    continue;
                }
                break;   // int 装载不属于本链
            }
            BinOp b{};
            if (!binOpOf(in->op, b) || b.kind != kind) break;
            // long 移位只认上面的成对形态：general 路径的虚拟栈同质（全 long），
            // 与 (long, int) 操作数形状不符 → 停。
            if (b.kind == Kind::Long && isShiftArith(b.arith)) break;
            if (stack.size() < 2) break;   // 该运算要消费链外值 → 保守停
            ConstVal r{};
            r.kind = kind;
            if (!applyBinOp(b, stack[stack.size() - 2], stack[stack.size() - 1], r)) break;
            stack.pop_back();
            stack.back() = r;
            ++j;
            if (stack.size() == 1) {
                foldEnd  = j - 1;
                consumed = true;
            }
        }

        if (!consumed) {
            ++i;   // 链未消费任何运算（纯装载序列）→ 不折
            continue;
        }

        // ---- 守卫：链中段成员前置边界有标签锚定（分支目标/异常表边界）→ 整链不动 ----
        bool guarded = false;
        for (ca::usize k = i + 1; k <= foldEnd; ++k) {
            if (method_rewrite_util::labelsAnchorAt(dom, ids[k])) {
                guarded = true;
                break;
            }
        }
        if (guarded) {
            ++i;
            continue;
        }

        // ---- 物化 + 编辑序列 ----
        Insn head{};
        if (!buildHeadInsn(session, stack.front(), head)) {
            ++i;   // CP 容量预检失败：整链不动（池无半截追加）
            continue;
        }
        auto headId = editor.insertBefore(ids[i], head);
        if (headId.is_err()) {
            ++i;
            continue;
        }
        // 首成员锚定的标签迁移到替换头（分支/异常边界居链首的落点语义；
        // char_arr_to_str_simplifier.cpp:146-148 同款）。
        bool labelFailed = false;
        for (const auto label : dom.labelsAt(ids[i])) {
            if (editor.bindLabel(label, headId.unwrap()).is_err()) {
                labelFailed = true;
                break;
            }
        }

        // 删除整链成员。守卫 + 迁移之后成员均无锚定标签、无引用，批删不
        // 应失败；labelFailed 或批删失败都走恢复（回迁标签、删替换头），
        // 本链跳过，方法体回到折叠前状态。
        const std::vector<InsnId> members(ids.begin() + static_cast<std::ptrdiff_t>(i),
                                          ids.begin() + static_cast<std::ptrdiff_t>(foldEnd) + 1);
        const bool erased =
            !labelFailed
            && editor.eraseBatch(members, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed).is_ok();
        if (!erased) {
            for (const auto label : dom.labelsAt(headId.unwrap())) {
                (void)editor.bindLabel(label, ids[i]);
            }
            (void)editor.erase(headId.unwrap(), jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed);
            ++i;
            continue;
        }
        modified = true;
        i        = foldEnd + 1;   // 快照推进：跳过整条被消费的链
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
