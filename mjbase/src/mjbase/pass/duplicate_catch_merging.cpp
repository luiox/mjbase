#include "mjbase/pass/duplicate_catch_merging.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::CpTag;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

/// 条目位置视图（TryCatchCleanup 同口径复制件——该面未跨件导出）：
/// start/handler 悬空 = 不可锚定；end 悬空 = 方法末语义，取指令数。
struct EntryView
{
    ca::usize start   = 0;
    ca::usize end     = 0;
    ca::usize handler = 0;
    bool      validLabels = false;   ///< start/handler 均可锚定
    bool      emptyRange  = false;   ///< end <= start（无覆盖指令）
};

/// label → order 下标映射（dangling 标签不入表）。
void buildLabelPos(const CodeDom& dom, const std::vector<InsnId>& ids,
                   std::vector<std::pair<LabelId, ca::usize>>& out)
{
    out.clear();
    for (ca::usize i = 0; i < ids.size(); ++i) {
        for (const LabelId l : dom.labelsAt(ids[i])) {
            out.emplace_back(l, i);
        }
    }
}

const ca::usize* findLabelPos(const std::vector<std::pair<LabelId, ca::usize>>& map,
                              LabelId l)
{
    for (const auto& [label, pos] : map) {
        if (label == l) return &pos;
    }
    return nullptr;
}

/// 条目位置解析。
EntryView viewEntry(const CodeUnit& cu, const CodeUnit::Exc& e,
                    const std::vector<std::pair<LabelId, ca::usize>>& labelPos,
                    ca::usize insnCount)
{
    EntryView v;
    const ca::usize* s = findLabelPos(labelPos, e.start);
    const ca::usize* h = findLabelPos(labelPos, e.handler);
    v.validLabels      = s != nullptr && h != nullptr;
    const ca::usize* en = findLabelPos(labelPos, e.end);
    v.start            = s ? *s : 0;
    v.end              = en ? *en : insnCount;   // end 悬空 = code_length 语义
    v.handler          = h ? *h : 0;
    v.emptyRange       = v.end <= v.start;
    return v;
}

/// catch 类型面：0（any）给空串；Class 条目给内名；越界/非 Class 给
/// nullopt（畸形面，永不参并）。
std::optional<std::string> catchTypeFace(const jbc::core::ConstantPool& cp, ca::u32 catchCp)
{
    if (catchCp == 0) return std::string{};
    if (!cp.valid(catchCp) || cp.tagOf(catchCp) != CpTag::Class) return std::nullopt;
    return std::string{cp.utf8Of(cp.at(catchCp).a)};
}

/// 相邻可并判定（头注判据 2-5；判据 1 由调用点的 i, i+1 扫描承担）。
bool canMerge(const CodeUnit& cu, const jbc::core::ConstantPool& cp,
              const std::vector<std::pair<LabelId, ca::usize>>& labelPos, ca::usize insnCount,
              ca::usize i, ca::usize j)
{
    const EntryView a = viewEntry(cu, cu.exc[i], labelPos, insnCount);
    const EntryView b = viewEntry(cu, cu.exc[j], labelPos, insnCount);
    if (!a.validLabels || !b.validLabels) return false;
    if (a.emptyRange || b.emptyRange) return false;
    if (a.handler != b.handler) return false;

    const auto ta = catchTypeFace(cp, cu.exc[i].catchCp);
    const auto tb = catchTypeFace(cp, cu.exc[j].catchCp);
    if (!ta.has_value() || !tb.has_value() || *ta != *tb) return false;

    const ca::usize loEnd   = std::min(a.end, b.end);
    const ca::usize hiStart = std::max(a.start, b.start);
    return loEnd >= hiStart;   ///< 无间隙：相切/相交/包含均并
}

}   // namespace

mjbase::spi::PreservedAnalyses DuplicateCatchMerging::run(mjbase::spi::MethodScope&          scope,
                                                          mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeUnit& cu = scope.code;
    if (cu.exc.size() < 2) return mjbase::spi::PreservedAnalyses::all();

    const CodeDom&                    dom = cu.dom;
    const std::vector<InsnId>         ids = dom.order();
    const jbc::core::ConstantPool&    cp  = scope.session.backing().cp;
    std::vector<std::pair<LabelId, ca::usize>> labelPos;
    buildLabelPos(dom, ids, labelPos);

    // 逐对扫描收口：可并即扩保留项区间并删被并项；不前移 i，继续把下一项
    // 并进同一保留项（游程收口）。每次合并表长 -1，必终止。
    bool changed = false;
    for (ca::usize i = 0; i + 1 < cu.exc.size();) {
        if (!canMerge(cu, cp, labelPos, ids.size(), i, i + 1)) {
            ++i;
            continue;
        }
        // start/end 标签各取位置更小/更大一方的标签（同位取保留项原标签），
        // 保留项表序位、handler 与 catchCp 不动。
        const EntryView a = viewEntry(cu, cu.exc[i], labelPos, ids.size());
        const EntryView b = viewEntry(cu, cu.exc[i + 1], labelPos, ids.size());
        if (b.start < a.start) cu.exc[i].start = cu.exc[i + 1].start;
        if (b.end > a.end) cu.exc[i].end = cu.exc[i + 1].end;
        cu.exc.erase(cu.exc.begin() + static_cast<ca::usize>(i + 1));
        changed = true;
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
