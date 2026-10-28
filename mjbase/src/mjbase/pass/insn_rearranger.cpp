#include "mjbase/pass/insn_rearranger.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;

// ── i/l 族形状判定（快捷形与泛形同判；旧栈 tree 模型规范化快捷形） ──

bool isIntLoad(Op op)
{
    const ca::u8 v = static_cast<ca::u8>(op);
    return v == static_cast<ca::u8>(Op::ILOAD)
        || (v >= static_cast<ca::u8>(Op::ILOAD_0) && v <= static_cast<ca::u8>(Op::ILOAD_3));
}

bool isLongLoad(Op op)
{
    const ca::u8 v = static_cast<ca::u8>(op);
    return v == static_cast<ca::u8>(Op::LLOAD)
        || (v >= static_cast<ca::u8>(Op::LLOAD_0) && v <= static_cast<ca::u8>(Op::LLOAD_3));
}

bool isIntStore(Op op)
{
    const ca::u8 v = static_cast<ca::u8>(op);
    return v == static_cast<ca::u8>(Op::ISTORE)
        || (v >= static_cast<ca::u8>(Op::ISTORE_0) && v <= static_cast<ca::u8>(Op::ISTORE_3));
}

bool isLongStore(Op op)
{
    const ca::u8 v = static_cast<ca::u8>(op);
    return v == static_cast<ca::u8>(Op::LSTORE)
        || (v >= static_cast<ca::u8>(Op::LSTORE_0) && v <= static_cast<ca::u8>(Op::LSTORE_3));
}

/// load 族槽位：泛形读 a 域，ILOAD_0..LLOAD_3 连续段（0x1a..0x21）按
/// (op - ILOAD_0) % 4 还原。
bool loadSlotOf(const jbc::ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ILOAD || in.op == Op::LLOAD) {
        slot = static_cast<ca::u32>(in.a);
        return true;
    }
    const ca::u8 v = static_cast<ca::u8>(in.op);
    if (v >= static_cast<ca::u8>(Op::ILOAD_0) && v <= static_cast<ca::u8>(Op::LLOAD_3)) {
        slot = static_cast<ca::u32>((v - static_cast<ca::u8>(Op::ILOAD_0)) % 4);
        return true;
    }
    return false;
}

/// store 族槽位：泛形读 a 域，ISTORE_0..LSTORE_3 连续段（0x3b..0x42）按
/// (op - ISTORE_0) % 4 还原。
bool storeSlotOf(const jbc::ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ISTORE || in.op == Op::LSTORE) {
        slot = static_cast<ca::u32>(in.a);
        return true;
    }
    const ca::u8 v = static_cast<ca::u8>(in.op);
    if (v >= static_cast<ca::u8>(Op::ISTORE_0) && v <= static_cast<ca::u8>(Op::LSTORE_3)) {
        slot = static_cast<ca::u32>((v - static_cast<ca::u8>(Op::ISTORE_0)) % 4);
        return true;
    }
    return false;
}

bool isIntCommutative(Op op)
{
    return op == Op::IADD || op == Op::IMUL || op == Op::IAND || op == Op::IOR
        || op == Op::IXOR;
}

bool isLongCommutative(Op op)
{
    return op == Op::LADD || op == Op::LMUL || op == Op::LAND || op == Op::LOR
        || op == Op::LXOR;
}

/// 常量装载判定（旧栈 InsnUtil::isConstant 对齐 + LDC_W/LDC2_W 读取器
/// 折叠等价面）：ACONST_NULL..DCONST_1 + BIPUSH/SIPUSH + ldc 族。
bool isConstPush(const jbc::ir::Insn& in)
{
    const ca::u8 v = static_cast<ca::u8>(in.op);
    return (v >= static_cast<ca::u8>(Op::ACONST_NULL)
            && v <= static_cast<ca::u8>(Op::DCONST_1))
        || in.op == Op::BIPUSH || in.op == Op::SIPUSH || in.op == Op::LDC
        || in.op == Op::LDC_W || in.op == Op::LDC2_W;
}

/// 窗口原位内容置换：新序第 k 位取原序 perm[k] 的内容（srcPc 随内容搬
/// 移）。窗口内部无标签锚定（调用方守卫），置换不触碰任何锚点；下游
/// pc 由 lowerMethod 重定位。
bool permuteWindow(jbc::ir::CodeUnit& code, const std::vector<jbc::ir::InsnId>& ids,
                   ca::usize begin, const std::vector<ca::usize>& perm)
{
    std::vector<jbc::ir::Insn> contents;
    contents.reserve(perm.size());
    for (const ca::usize k : perm) {
        const jbc::ir::Insn* in = code.dom.peek(ids[begin + k]);
        if (in == nullptr) return false;
        contents.push_back(*in);
    }
    for (ca::usize k = 0; k < perm.size(); ++k) {
        auto got = code.dom.get(ids[begin + k]);
        if (got.is_err()) return false;
        *std::move(got).unwrap() = contents[k];
    }
    return true;
}

bool isIdentityPerm(const std::vector<ca::usize>& perm)
{
    for (ca::usize k = 0; k < perm.size(); ++k) {
        if (perm[k] != k) return false;
    }
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses ArithmeticInsnRearranger::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit&                cu  = scope.code;
    const std::vector<jbc::ir::InsnId> ids = cu.dom.order();
    if (ids.size() < 8) return mjbase::spi::PreservedAnalyses::all();

    const auto viewAt = [&](ca::usize k) -> const jbc::ir::Insn* {
        return cu.dom.peek(ids[k]);
    };

    // 单块匹配：[load s][const][同族可交换算术][store s]；块内边界锚标
    // 签即不成块（旧栈标签节点入列即断模式）。
    const auto matchChunk = [&](ca::usize k, ca::u32& slot) -> bool {
        if (k + 4 > ids.size()) return false;
        for (ca::usize b = k + 1; b <= k + 3; ++b) {
            if (method_rewrite_util::labelsAnchorAt(cu.dom, ids[b])) return false;
        }
        const jbc::ir::Insn* load  = viewAt(k);
        const jbc::ir::Insn* cst   = viewAt(k + 1);
        const jbc::ir::Insn* arith = viewAt(k + 2);
        const jbc::ir::Insn* store = viewAt(k + 3);
        if (load == nullptr || cst == nullptr || arith == nullptr || store == nullptr) {
            return false;
        }
        const bool intPair  = isIntLoad(load->op) && isIntStore(store->op)
                             && isIntCommutative(arith->op);
        const bool longPair = isLongLoad(load->op) && isLongStore(store->op)
                              && isLongCommutative(arith->op);
        if (!intPair && !longPair) return false;
        if (!isConstPush(*cst)) return false;
        ca::u32 ls = 0;
        ca::u32 ss = 0;
        if (!loadSlotOf(*load, ls) || !storeSlotOf(*store, ss)) return false;
        if (ls != ss) return false;
        slot = ls;
        return true;
    };

    ca::usize i        = 0;
    bool      modified = false;
    while (i < ids.size()) {
        // 窗口起点锚标签 → 整窗拒收（锚点随重排漂移改变入边落点）。
        if (method_rewrite_util::labelsAnchorAt(cu.dom, ids[i])) {
            ++i;
            continue;
        }
        ca::u32 slot = 0;
        if (!matchChunk(i, slot)) {
            ++i;
            continue;
        }
        // 连续块收集：后续块起点锚标签即断窗（尾边界允许——窗后锚点随
        // 窗外指令不动）。
        std::vector<ca::u32> chunkSlots;
        chunkSlots.push_back(slot);
        ca::usize end = i + 4;
        while (end + 4 <= ids.size()) {
            if (method_rewrite_util::labelsAnchorAt(cu.dom, ids[end])) break;
            ca::u32 nextSlot = 0;
            if (!matchChunk(end, nextSlot)) break;
            chunkSlots.push_back(nextSlot);
            end += 4;
        }
        if (chunkSlots.size() < 2) {
            i += 4;   // 单块重排无意义，原样跳过
            continue;
        }
        // 槽首现序分组（同槽保持原相对序），拼接为置换表：新窗口第 k 位
        // = 原窗口第 perm[k] 条。
        std::vector<ca::u32>                 order;
        std::vector<std::vector<ca::usize>> bySlot;
        for (ca::usize c = 0; c < chunkSlots.size(); ++c) {
            ca::usize g = 0;
            while (g < order.size() && order[g] != chunkSlots[c]) ++g;
            if (g == order.size()) {
                order.push_back(chunkSlots[c]);
                bySlot.emplace_back();
            }
            bySlot[g].push_back(c);
        }
        std::vector<ca::usize> perm;
        perm.reserve(chunkSlots.size() * 4);
        for (const auto& group : bySlot) {
            for (const ca::usize c : group) {
                for (ca::usize j = 0; j < 4; ++j) perm.push_back(c * 4 + j);
            }
        }
        if (!isIdentityPerm(perm) && permuteWindow(cu, ids, i, perm)) {
            modified = true;
        }
        i = end;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

mjbase::spi::PreservedAnalyses CommutativeOperatorReorder::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit&                cu  = scope.code;
    const std::vector<jbc::ir::InsnId> ids = cu.dom.order();
    if (ids.size() < 3) return mjbase::spi::PreservedAnalyses::all();

    const auto viewAt = [&](ca::usize k) -> const jbc::ir::Insn* {
        return cu.dom.peek(ids[k]);
    };

    ca::i32  dummyInt  = 0;
    ca::i64  dummyLong = 0;
    ca::usize i        = 0;
    bool      modified = false;
    while (i < ids.size()) {
        // 链起点锚标签 → 不成链（声明偏离：旧栈列表语义隐式重锚，新栈
        // 保守拒收）。
        if (method_rewrite_util::labelsAnchorAt(cu.dom, ids[i])) {
            ++i;
            continue;
        }
        // 预扫：首个可交换算术；途经锚标签断链（旧栈标签节点入列即断）。
        ca::usize firstOp   = 0;
        bool      found     = false;
        bool      intKind   = false;
        for (ca::usize k = i; k < ids.size(); ++k) {
            if (k > i && method_rewrite_util::labelsAnchorAt(cu.dom, ids[k])) break;
            const jbc::ir::Insn* in = viewAt(k);
            if (in == nullptr) break;
            if (isIntCommutative(in->op) || isLongCommutative(in->op)) {
                firstOp = k;
                found   = true;
                intKind = isIntCommutative(in->op);
                break;
            }
        }
        if (!found || firstOp == i) {
            ++i;
            continue;
        }
        // 操作数校验：[i, firstOp) 全部为同宽常量装载或同宽局部装载。
        bool operandsOk = true;
        for (ca::usize k = i; k < firstOp; ++k) {
            const jbc::ir::Insn* in = viewAt(k);
            if (in == nullptr) {
                operandsOk = false;
                break;
            }
            const bool ok =
                intKind ? (isIntLoad(in->op)
                              || method_rewrite_util::intValueOf(scope.session, *in,
                                                                 dummyInt))
                        : (isLongLoad(in->op)
                              || method_rewrite_util::longValueOf(scope.session, *in,
                                                                  dummyLong));
            if (!ok) {
                operandsOk = false;
                break;
            }
        }
        // 算术运行：同一操作码连续段；锚标签截断（旧栈 ops 运行遇标签
        // 节点收尾，同形）。
        const Op opOpcode = viewAt(firstOp)->op;
        ca::usize end     = firstOp;
        while (end < ids.size()) {
            if (end > firstOp && method_rewrite_util::labelsAnchorAt(cu.dom, ids[end])) {
                break;
            }
            const jbc::ir::Insn* in = viewAt(end);
            if (in == nullptr || in->op != opOpcode) break;
            ++end;
        }
        const ca::usize opCount = end - firstOp;
        if (!operandsOk || opCount == 0 || (firstOp - i) != opCount + 1) {
            ++i;
            continue;
        }
        // 操作数稳定划分：非常量在前、常量在后（常量相邻供折叠命中）。
        const auto isConstOperand = [&](ca::usize k) {
            const jbc::ir::Insn* in = viewAt(k);
            return intKind ? method_rewrite_util::intValueOf(scope.session, *in, dummyInt)
                           : method_rewrite_util::longValueOf(scope.session, *in,
                                                              dummyLong);
        };
        std::vector<ca::usize> perm;
        perm.reserve(firstOp - i);
        for (ca::usize k = i; k < firstOp; ++k) {
            if (!isConstOperand(k)) perm.push_back(k - i);
        }
        for (ca::usize k = i; k < firstOp; ++k) {
            if (isConstOperand(k)) perm.push_back(k - i);
        }
        if (!isIdentityPerm(perm) && permuteWindow(cu, ids, i, perm)) {
            modified = true;
        }
        i = end;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
