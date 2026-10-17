#include "mjbase/pass/strength_reducer.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeEditor;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// 匹配到的运算宽度（决定指令形态与常量读取面）。
enum class Width { None, Int, Long };

/// 正整数是否恰为 2 的幂且指数在 [1, width)：是则回写 k。
///
/// k==0（值 1）显式排除——乘/除 1 留给 nop 清理家族；k>=width 为防御性
/// 守卫（正常量实际最大 2^30 / 2^62，不可达，见头文件保守停止清单）。
bool powerOfTwoExponent(ca::i64 v, ca::i32 width, ca::i32& kOut)
{
    if (v <= 0) return false;
    ca::u64 u = static_cast<ca::u64>(v);
    ca::i32 k = 0;
    while ((u & 1u) == 0u) {
        u >>= 1;
        ++k;
    }
    if (u != 1u) return false;              // 除 2 的幂外还有别的因子
    if (k < 1 || k >= width) return false;   // k==0 不动；越界保守不动
    kOut = k;
    return true;
}

/// int 常量装载物化：-1..5 ICONST；-128..127 BIPUSH；-32768..32767
/// SIPUSH；其余 LDC/LDC_W（CodeEditor::loadConst 按索引宽度选型，
/// internInteger 先去重再追加）。
Insn materializeIntConst(jbc::ir::ClassSession& session, ca::i32 v)
{
    Insn in;
    if (v >= -1 && v <= 5) {
        in.op = static_cast<Op>(static_cast<ca::u8>(Op::ICONST_M1) + (v + 1));
        return in;
    }
    if (v >= -128 && v <= 127) {
        in.op = Op::BIPUSH;
        in.a  = v;
        return in;
    }
    if (v >= -32768 && v <= 32767) {
        in.op = Op::SIPUSH;
        in.a  = v;
        return in;
    }
    return CodeEditor::loadConst(session.internInteger(v));
}

/// long 常量装载物化：0/1 → LCONST_0/1；其余 LDC2_W（cp.addLong 只追加
/// 不去重——ClassSession 无 long intern 面，与 ConstantFolder 物化策略
/// 同款；调用方先做 CP 容量预检）。
Insn materializeLongConst(jbc::ir::ClassSession& session, ca::i64 v)
{
    Insn in;
    if (v == 0 || v == 1) {
        in.op = (v == 0) ? Op::LCONST_0 : Op::LCONST_1;
        return in;
    }
    in.op = Op::LDC2_W;
    in.cp = session.backing().cp.addLong(v);
    return in;
}

/// CP 追加容量预检：cp.size() 即 constant_pool_count（含 0 号占位），
/// JVMS 上限 0xFFFF；wide 条目（Long/Double）占 2 槽。
bool cpHasRoomFor(const jbc::ir::ClassSession& session, ca::u32 slots)
{
    return session.backing().cp.size() + slots <= 0xFFFFu;
}

/// BIPUSH 立即数（移位量 31/63 与指数 k 均在 signed byte 域内）。
Insn bipush(ca::i32 v)
{
    Insn in;
    in.op = Op::BIPUSH;
    in.a  = v;
    return in;
}

}   // namespace

mjbase::spi::PreservedAnalyses StrengthReducer::run(mjbase::spi::MethodScope&       scope,
                                                    mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::ClassSession& session = scope.session;
    CodeDom&                dom    = scope.code.dom;
    jbc::ir::CodeEditor     editor(scope.code);

    const std::vector<InsnId> ids = dom.order();
    bool                      modified = false;
    bool                      divDone  = false;   // 有 div 改写（maxStack 按宽度抬升）
    bool                      divLong  = false;   // 有 long div 改写（峰值 +2；int 侧 +1）

    for (ca::usize i = 1; i < ids.size(); ++i) {
        const Insn* op = dom.peek(ids[i]);
        if (op == nullptr) continue;
        Width width = Width::None;
        switch (op->op) {
        case Op::IMUL:
        case Op::IDIV:
            width = Width::Int;
            break;
        case Op::LMUL:
        case Op::LDIV:
            width = Width::Long;
            break;
        default:
            continue;
        }

        // 前驱必须是同宽度编译期常量装载（严格相邻对）。
        const Insn* cst = dom.peek(ids[i - 1]);
        if (cst == nullptr) continue;
        ca::i64 cv = 0;
        if (width == Width::Int) {
            ca::i32 iv = 0;
            if (!method_rewrite_util::intValueOf(session, *cst, iv)) continue;
            cv = iv;
        }
        else {
            if (!method_rewrite_util::longValueOf(session, *cst, cv)) continue;
        }

        const ca::i32 bits = (width == Width::Int) ? 32 : 64;
        ca::i32       k    = 0;
        if (!powerOfTwoExponent(cv, bits, k)) continue;

        // 边界守卫：两成员前置边界有标签锚定（分支目标/异常表边界）→ 不动。
        if (method_rewrite_util::labelsAnchorAt(dom, ids[i - 1])
            || method_rewrite_util::labelsAnchorAt(dom, ids[i])) {
            continue;
        }

        const bool isMul = (op->op == Op::IMUL || op->op == Op::LMUL);

        // 预检（无 IR 变更）：div 序列峰值按宽度抬升（int +1 / long +2）需要
        // maxStack 不溢 u16；mask 走 CP 时需要槽位（int mask：k>=16 超 SIPUSH
        // 域；long mask：k>=2 超 LCONST 域）。
        if (!isMul) {
            if (scope.code.maxStack >= 0xFFFDu) continue;
            const bool intLdcMask  = (width == Width::Int) && k >= 16;
            const bool longLdcMask = (width == Width::Long) && k >= 2;
            if (intLdcMask && !cpHasRoomFor(session, 1)) continue;
            if (longLdcMask && !cpHasRoomFor(session, 2)) continue;
        }

        modified = true;
        if (isMul) {
            // [x][2^k] IMUL → [x] BIPUSH k; ISHL：栈形不变，maxStack 不动。
            // 先插移位量（落在除数常量原位），再删常量（无引用，直接成功），
            // 最后就地改写运算码；中间失败回删已插指令，原模式原样保留。
            const auto inserted = editor.insertBefore(ids[i], bipush(k));
            if (inserted.is_err()) break;
            if (editor.erase(ids[i - 1], CodeEditor::ErasePolicy::kRetargetToFallthrough)
                    .is_err()) {
                (void)editor.erase(inserted.unwrap(),
                                   CodeEditor::ErasePolicy::kRetargetToFallthrough);
                break;
            }
            auto got = dom.get(ids[i]);
            if (got.is_err()) break;
            got.unwrap()->op = (width == Width::Int) ? Op::ISHL : Op::LSHL;
            continue;
        }

        // div：原运算就地改写为序列首指令（DUP/DUP2），其余成员顺序后插，
        // 除数常量最后删除。任一步失败整体回滚——回删已插指令、恢复原运算
        // 码——不留半截序列（半截是验证器必拒形态）；mask 物化的 CP 追加是
        // 只追加契约内的遗留（未被引用条目无害）。divDone 只在整序列成功后
        // 记账，回滚不抬 maxStack。
        const auto first = dom.get(ids[i]);
        if (first.is_err()) break;
        const Op            origOp  = first.unwrap()->op;
        const InsnId        divInsn = ids[i];
        std::vector<InsnId> inserted;
        auto                rollback = [&editor, &dom, &inserted, divInsn, origOp]() {
            for (auto it = inserted.rbegin(); it != inserted.rend(); ++it) {
                (void)editor.erase(*it, CodeEditor::ErasePolicy::kRetargetToFallthrough);
            }
            if (auto back = dom.get(divInsn); back.is_ok()) {
                back.unwrap()->op = origOp;
            }
        };
        first.unwrap()->op = (width == Width::Int) ? Op::DUP : Op::DUP2;

        const ca::i64 mask    = (static_cast<ca::i64>(1) << k) - 1;
        const Insn   maskPush = (width == Width::Int)
                                    ? materializeIntConst(session, static_cast<ca::i32>(mask))
                                    : materializeLongConst(session, mask);

        InsnId cursor = divInsn;
        auto   step   = [&editor, &cursor, &inserted](const Insn& in) {
            const auto r = editor.insertAfter(cursor, in);
            if (r.is_err()) return false;
            cursor = r.unwrap();
            inserted.push_back(cursor);
            return true;
        };
        const bool seqOk =
            step(bipush((width == Width::Int) ? 31 : 63))
            && step(CodeEditor::simple((width == Width::Int) ? Op::ISHR : Op::LSHR))
            && step(maskPush)
            && step(CodeEditor::simple((width == Width::Int) ? Op::IAND : Op::LAND))
            && step(CodeEditor::simple((width == Width::Int) ? Op::IADD : Op::LADD))
            && step(bipush(k))
            && step(CodeEditor::simple((width == Width::Int) ? Op::ISHR : Op::LSHR));
        if (!seqOk) {
            rollback();
            break;
        }
        if (editor.erase(ids[i - 1], CodeEditor::ErasePolicy::kRetargetToFallthrough).is_err()) {
            rollback();
            break;
        }
        if (width == Width::Long) {
            divLong = true;
        }
        divDone = true;
    }

    if (divDone) {
        // 序列峰值栈深按宽度抬升：int 侧原 [x][c] 峰 d+2 → 序列峰 d+3
        // （+1）；long 侧原 [x long][c long] 峰 d+4 → DUP2/BIPUSH/mask-long
        // 峰 d+6（+2）。原类可验证 ⇒ maxStack>=原峰，抬升后恒覆盖新峰；
        // u16 溢出已被预检（maxStack >= 0xFFFD 不改写）排除。
        const ca::u16 bump = divLong ? 2u : 1u;
        scope.code.maxStack = static_cast<ca::u16>(scope.code.maxStack + bump);
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
