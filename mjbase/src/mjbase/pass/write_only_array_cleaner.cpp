#include "mjbase/pass/write_only_array_cleaner.hpp"

#include <unordered_map>
#include <unordered_set>
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
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// 弹值定义是否纯常量生产者（与 DeadLocalStoreRemover 帧面同族口径）。
bool isConstProducer(const jbc::ir::ClassSession& session, const Insn& in)
{
    ca::i32         iv = 0;
    ca::i64         lv = 0;
    float           fv = 0.0F;
    double          dv = 0.0;
    std::string_view sv;
    return in.op == Op::ACONST_NULL
           || method_rewrite_util::intValueOf(session, in, iv)
           || method_rewrite_util::longValueOf(session, in, lv)
           || method_rewrite_util::floatValueOf(session, in, fv)
           || method_rewrite_util::doubleValueOf(session, in, dv)
           || method_rewrite_util::stringValueOf(session, in, sv);
}

/// 局部槽号（泛形取操作数 a，快捷形按 opcode 基准）；非局部读写 op 返回
/// false。load/store 各类型族 + IINC + RET 全覆盖（S2 逃逸面全扫依据）。
bool localIndexOf(const Insn& in, ca::u32& slot)
{
    const ca::u32 op = static_cast<ca::u32>(in.op);
    if (op >= static_cast<ca::u32>(Op::ILOAD) && op <= static_cast<ca::u32>(Op::ALOAD)) {
        slot = static_cast<ca::u32>(in.a);   // ILOAD/LLOAD/FLOAD/DLOAD/ALOAD 泛形
        return true;
    }
    if (op >= static_cast<ca::u32>(Op::ILOAD_0) && op <= static_cast<ca::u32>(Op::ALOAD_3)) {
        slot = (op - static_cast<ca::u32>(Op::ILOAD_0)) % 4;   // 五族快捷形连续段
        return true;
    }
    if (op >= static_cast<ca::u32>(Op::ISTORE) && op <= static_cast<ca::u32>(Op::ASTORE)) {
        slot = static_cast<ca::u32>(in.a);
        return true;
    }
    if (op >= static_cast<ca::u32>(Op::ISTORE_0) && op <= static_cast<ca::u32>(Op::ASTORE_3)) {
        slot = (op - static_cast<ca::u32>(Op::ISTORE_0)) % 4;
        return true;
    }
    if (in.op == Op::IINC || in.op == Op::RET) {
        slot = static_cast<ca::u32>(in.a);
        return true;
    }
    return false;
}

/// 数组存储族（XASTORE 连续段）。
bool isArrayStore(Op op)
{
    return op >= Op::IASTORE && op <= Op::SASTORE;
}

struct Window
{
    std::vector<InsnId> erase;
    ca::usize           end = 0;   // 快照序窗尾（末元素下标 + 1）
};

/// S2 暂存形的局部 n 全方法读写位点是否全部落窗内。
bool localFullyInside(const CodeUnit& cu, const std::vector<InsnId>& ids, ca::u32 slot,
                      const std::unordered_set<ca::u32>& windowSlots)
{
    for (const InsnId id : ids) {
        const Insn* in = cu.dom.peek(id);
        if (in == nullptr) return false;
        ca::u32 s = 0;
        if (!localIndexOf(*in, s) || s != slot) continue;
        if (windowSlots.count(static_cast<ca::u32>(id.slot)) == 0) return false;
    }
    return true;
}

/// 匹配从 allocPos 起的只写数组窗；不匹配返回 false。
bool matchWindow(const jbc::ir::ClassSession& session, const CodeUnit& cu,
                 const std::vector<InsnId>& ids, ca::usize allocPos, Window& out)
{
    const CodeDom& dom = cu.dom;
    const Insn*    alloc = dom.peek(ids[allocPos]);
    if (alloc == nullptr) return false;
    if (alloc->op != Op::NEWARRAY && alloc->op != Op::ANEWARRAY) return false;

    // size 生产者：紧邻前驱且为纯 int 常量（删除分配会孤儿化其输入）。
    if (allocPos == 0) return false;
    const Insn* size = dom.peek(ids[allocPos - 1]);
    ca::i32     iv   = 0;
    if (size == nullptr || !method_rewrite_util::intValueOf(session, *size, iv)) return false;

    // 元素组：(ref复制; int常量下标; 元素常量; XASTORE)。ref 来源按形态判
    // 定（S1 = DUP 复制，S2 = ALOAD n），下标/元素生产者同族口径。
    const auto constPairAt = [&](ca::usize pos) -> bool {
        if (pos + 2 > ids.size()) return false;
        const Insn* idx = dom.peek(ids[pos]);
        const Insn* val = dom.peek(ids[pos + 1]);
        if (idx == nullptr || val == nullptr) return false;
        if (!method_rewrite_util::intValueOf(session, *idx, iv)) return false;
        return isConstProducer(session, *val);
    };

    std::vector<InsnId> erase;
    erase.push_back(ids[allocPos - 1]);
    erase.push_back(ids[allocPos]);
    ca::usize pos = allocPos + 1;
    ca::u32   slot = 0;
    if (pos < ids.size()) {
        const Insn* first = dom.peek(ids[pos]);
        if (first != nullptr && first->op == Op::POP) {
            // S1 k=0：分配; pop 死分配特形。
            erase.push_back(ids[pos]);
            out.erase = std::move(erase);
            out.end   = pos + 1;
            return true;
        }
        if (first != nullptr && first->op == Op::DUP) {
            // S1 直通形：(DUP; idx; val; XASTORE)*; POP。
            for (;;) {
                if (pos + 4 > ids.size()) return false;
                const Insn* store = dom.peek(ids[pos + 3]);
                if (store == nullptr || !isArrayStore(store->op)) return false;
                if (!constPairAt(pos + 1)) return false;
                for (ca::usize k = 0; k < 4; ++k) erase.push_back(ids[pos + k]);
                pos += 4;
                if (pos >= ids.size()) return false;
                const Insn* tail = dom.peek(ids[pos]);
                if (tail != nullptr && tail->op == Op::POP) {
                    erase.push_back(ids[pos]);
                    out.erase = std::move(erase);
                    out.end   = pos + 1;
                    return true;
                }
                const Insn* next = dom.peek(ids[pos]);
                if (next == nullptr || next->op != Op::DUP) return false;
            }
        }
        // S2 暂存形：ASTORE n; (ALOAD n; idx; val; XASTORE)*。
        if (first != nullptr && localIndexOf(*first, slot)
            && (first->op == Op::ASTORE
                || (first->op >= Op::ASTORE_0 && first->op <= Op::ASTORE_3))) {
            erase.push_back(ids[pos]);
            pos += 1;
            std::unordered_set<ca::u32> windowSlots;
            for (const InsnId id : erase) windowSlots.insert(static_cast<ca::u32>(id.slot));
            for (;;) {
                if (pos + 4 > ids.size()) break;
                const Insn* load = dom.peek(ids[pos]);
                if (load == nullptr) break;
                ca::u32 loadSlot = 0;
                if (!localIndexOf(*load, loadSlot) || loadSlot != slot) break;
                if (load->op != Op::ALOAD
                    && (load->op < Op::ALOAD_0 || load->op > Op::ALOAD_3)) {
                    break;
                }
                const Insn* store = dom.peek(ids[pos + 3]);
                if (store == nullptr || !isArrayStore(store->op)) break;
                if (!constPairAt(pos + 1)) break;
                for (ca::usize k = 0; k < 4; ++k) {
                    erase.push_back(ids[pos + k]);
                    windowSlots.insert(static_cast<ca::u32>(ids[pos + k].slot));
                }
                pos += 4;
            }
            if (!localFullyInside(cu, ids, slot, windowSlots)) return false;
            out.erase = std::move(erase);
            out.end   = pos;
            return true;
        }
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses WriteOnlyArrayCleaner::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeUnit&                          cu  = scope.code;
    const std::vector<InsnId>          ids = cu.dom.order();
    std::vector<InsnId>                eraseIds;
    ca::usize                          i = 0;
    while (i < ids.size()) {
        Window window;
        if (!matchWindow(scope.session, cu, ids, i, window)) {
            ++i;
            continue;
        }
        // 窗内（含 size 生产者与窗首）任一锚标签 → 整窗放弃（回边重执行
        // 分配语义改变；删除面不触碰任何锚点）。
        bool anchored = false;
        for (const InsnId id : window.erase) {
            if (method_rewrite_util::labelsAnchorAt(cu.dom, id)) {
                anchored = true;
                break;
            }
        }
        if (!anchored) {
            // 异常范围与窗相交 → 整窗放弃（分配/存储的抛出可观测，全含
            // 亦弃）；handler 锚窗内同理。标签锚点表按 (slot, gen) 合成键。
            std::unordered_map<ca::u64, ca::usize> anchorPos;
            for (ca::usize p = 0; p < ids.size(); ++p) {
                for (const jbc::ir::LabelId label : cu.dom.labelsAt(ids[p])) {
                    anchorPos[(static_cast<ca::u64>(label.slot) << 32)
                              | static_cast<ca::u32>(label.gen)] = p;
                }
            }
            const auto anchorOf = [&](jbc::ir::LabelId label) -> ca::usize {
                const auto it = anchorPos.find((static_cast<ca::u64>(label.slot) << 32)
                                               | static_cast<ca::u32>(label.gen));
                return it != anchorPos.end() ? it->second : ids.size();
            };
            const ca::usize ws = i - 1;   // size 生产者位（allocPos >= 1 恒成立）
            const ca::usize we = window.end;
            bool            conflict = false;
            for (const CodeUnit::Exc& exc : cu.exc) {
                const ca::usize startPos = anchorOf(exc.start);
                const ca::usize endPos   = anchorOf(exc.end);
                const ca::usize handPos  = anchorOf(exc.handler);
                if (!(endPos <= ws || startPos >= we) || (handPos >= ws && handPos < we)) {
                    conflict = true;
                    break;
                }
            }
            if (!conflict) {
                eraseIds.insert(eraseIds.end(), window.erase.begin(), window.erase.end());
            }
        }
        i = window.end;
    }
    if (eraseIds.empty()) return mjbase::spi::PreservedAnalyses::all();

    jbc::ir::CodeEditor editor(cu);
    if (editor
            .eraseBatch(eraseIds, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
            .is_err()) {
        return mjbase::spi::PreservedAnalyses::all();   // 全有或全无：Boundary
                                                        // 标签锚定等 → 整件放弃
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
