#include "mjbase/pass/local_variable_table_rebuilder.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/analysis/lvt_display_policy.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/pass/local_op.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace mjbase::pass {

namespace {

namespace ja = jbc::analysis;

using ca::core::tag_cast::cast;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::LocalVariableTableAttr;

constexpr std::string_view kCode                   = "Code";
constexpr std::string_view kLocalVariableTable     = "LocalVariableTable";
constexpr std::string_view kLocalVariableTypeTable = "LocalVariableTypeTable";
constexpr ca::u16          kAccStatic              = 0x0008;
constexpr ca::usize        kNoSegment              = static_cast<ca::usize>(-1);
constexpr std::string_view kObjectDesc             = "Ljava/lang/Object;";

/// 属性槽按名定位（逐字节比较；CP 只追加，索引等值不可靠）。
AttrSlot* findSlot(ClassFile& cf, std::vector<AttrSlot>& attrs, std::string_view name)
{
    for (AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// 扫描式 Utf8 intern：命中返回原索引，避免重复写调试表时膨胀常量池。
ca::u32 internUtf8(ClassFile& cf, std::string_view text)
{
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (cf.cp.isUtf8(i) && cf.cp.utf8Equals(i, text)) return i;
    }
    return cf.cp.addUtf8(text);
}

/// 基本类型族 → 字段描述符；'A' 无法从操作码判定。
std::string kindDesc(char kind)
{
    switch (kind) {
    case 'I': return "I";
    case 'L': return "J";
    case 'F': return "F";
    case 'D': return "D";
    default: return {};
    }
}

/// 帧值 → 字段描述符；无精确信息返回空。
std::string vtypeDesc(const ja::VType& type)
{
    switch (type.k) {
    case ja::VKind::Int: return "I";
    case ja::VKind::Float: return "F";
    case ja::VKind::Long: return "J";
    case ja::VKind::Double: return "D";
    case ja::VKind::Ref: {
        if (type.name.empty()) return {};
        if (type.name.front() == '[') return std::string(type.name);
        return "L" + std::string(type.name) + ";";
    }
    default: return {};
    }
}

/// 解析一个字段类型（描述符内），返回消耗的字节数与槽宽。
bool nextFieldType(std::string_view desc, ca::usize& pos, std::string& out, ca::u32& width)
{
    if (pos >= desc.size()) return false;
    const ca::usize start = pos;
    switch (desc[pos]) {
    case '[': {
        ca::usize i = pos;
        while (i < desc.size() && desc[i] == '[') ++i;
        if (i >= desc.size()) return false;
        if (desc[i] == 'L') {
            const ca::usize semi = desc.find(';', i);
            if (semi == std::string_view::npos) return false;
            i = semi + 1;
        } else {
            ++i;
        }
        out   = std::string(desc.substr(start, i - start));
        width = 1;
        pos   = i;
        return true;
    }
    case 'L': {
        const ca::usize semi = desc.find(';', start);
        if (semi == std::string_view::npos) return false;
        out   = std::string(desc.substr(start, semi + 1 - start));
        width = 1;
        pos   = semi + 1;
        return true;
    }
    case 'J':
    case 'D':
        out   = std::string(1, desc[pos]);
        width = 2;
        ++pos;
        return true;
    case 'I':
    case 'F':
    case 'B':
    case 'C':
    case 'S':
    case 'Z':
        out   = std::string(1, desc[pos]);
        width = 1;
        ++pos;
        return true;
    default: return false;
    }
}

/// 方法描述符参数表 → (起始槽, 描述符)；返回参数占用的槽数。
ca::u32 seedArgSlots(std::string_view desc, ca::u32 startSlot,
                     std::vector<std::pair<ca::u32, std::string>>& out)
{
    ca::usize pos  = 1;   // '('
    ca::u32   slot = startSlot;
    while (pos < desc.size() && desc[pos] != ')') {
        std::string type;
        ca::u32     width = 0;
        if (!nextFieldType(desc, pos, type, width)) break;
        out.emplace_back(slot, std::move(type));
        slot += width;
    }
    return slot - startSlot;
}

/// 变量生命周期段：一个槽在两次异型赋值之间的一段（切分引擎与
/// LocalVariableAnnotator 同款，保证两件组合时限集一致）。
struct Segment
{
    ca::u32     slot = 0;
    ca::u32     startPc = 0;
    ca::u32     endPc = 0;
    std::string type;   ///< 推断描述符；空 = 未知
};

/// 原 LVT 行快照（名称/描述符沿用源 + 幂等比对源）。
struct DeclaredRow
{
    ca::u32     slot = 0;
    ca::u32     startPc = 0;
    ca::u32     length = 0;
    std::string name;
    std::string desc;
};

/// 快照既有调试表：LVT 行 + 是否存在 LVT/LVTT 槽。嵌套 LVT 槽 lazy 解码
/// 在此触发（需可变槽——解码就地落 body）；解码失败视为不可比空表（必走
/// 重写，fail 向「修」不向「留」）。返回是否存在 LVT/LVTT 任一槽。
bool snapshotDeclared(ClassFile& cf, CodeAttr& codeAttr, std::vector<DeclaredRow>& out,
                      bool& hadLvtt)
{
    bool hadTable = false;
    for (AttrSlot& slot : codeAttr.attrs) {
        const bool lvt  = cf.cp.utf8Equals(slot.nameCp, kLocalVariableTable);
        const bool lvtt = cf.cp.utf8Equals(slot.nameCp, kLocalVariableTypeTable);
        if (!lvt && !lvtt) continue;
        hadTable = true;
        hadLvtt  = hadLvtt || lvtt;
        if (!lvt) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) continue;
        const auto* table = cast<const LocalVariableTableAttr>(slot.peek());
        if (table == nullptr) continue;
        for (const LocalVariableTableAttr::Row& row : table->rows) {
            DeclaredRow entry;
            entry.slot    = row.slot;
            entry.startPc = row.startPc;
            entry.length  = row.length;
            entry.name    = std::string(cf.cp.utf8Of(row.nameCp));
            entry.desc    = std::string(cf.cp.utf8Of(row.descCp));
            out.push_back(std::move(entry));
        }
    }
    return hadTable;
}

/// 槽的全部可信原名（首现序去重）：lvt_display_policy 形态判定过滤。
std::vector<std::string> trustedNamesOf(ca::u32 slot, const std::vector<DeclaredRow>& declared)
{
    std::vector<std::string> names;
    for (const DeclaredRow& row : declared) {
        if (row.slot != slot) continue;
        if (!analysis::lvtNameTrustworthy(row.name)) continue;
        if (std::find(names.begin(), names.end(), row.name) == names.end()) {
            names.push_back(row.name);
        }
    }
    return names;
}

/// 槽的全部声明描述符（首现序去重）。
std::vector<std::string> declaredDescsOf(ca::u32 slot, const std::vector<DeclaredRow>& declared)
{
    std::vector<std::string> descs;
    for (const DeclaredRow& row : declared) {
        if (row.slot != slot) continue;
        if (std::find(descs.begin(), descs.end(), row.desc) == descs.end()) {
            descs.push_back(row.desc);
        }
    }
    return descs;
}

/// 描述符和解：推断与声明一致 → 声明（即推断）；原表 Object 槽存引用类型
/// → 保 Object（javac 擦除槽形态，非矛盾不做提升）；推断与声明矛盾 →
/// 声明不可验证、落推断值；无推断信息 → 唯一声明沿用，否则 Object 兜底。
std::string reconcileDesc(const std::string& inferred, const std::vector<std::string>& declared)
{
    const bool refShaped =
        !inferred.empty() && (inferred.front() == 'L' || inferred.front() == '[');
    if (!inferred.empty()) {
        if (std::find(declared.begin(), declared.end(), inferred) != declared.end()) {
            return inferred;
        }
        if (declared.size() == 1 && declared.front() == kObjectDesc && refShaped) {
            return declared.front();
        }
        return inferred;
    }
    if (declared.size() == 1) return declared.front();
    return std::string(kObjectDesc);
}

/// 产物行：重建后的 LVT 行（CP intern 前的文本形态）。
struct EmitRow
{
    ca::u32     slot = 0;
    ca::u32     startPc = 0;
    ca::u32     length = 0;
    std::string name;
    std::string desc;
};

/// 幂等比对：重算行集与既有表逐行一致（无序集合等值）。
bool sameRowSet(const std::vector<DeclaredRow>& existing, const std::vector<EmitRow>& computed)
{
    if (existing.size() != computed.size()) return false;
    std::vector<DeclaredRow> a = existing;
    std::sort(a.begin(), a.end(), [](const DeclaredRow& x, const DeclaredRow& y) {
        if (x.startPc != y.startPc) return x.startPc < y.startPc;
        if (x.slot != y.slot) return x.slot < y.slot;
        if (x.length != y.length) return x.length < y.length;
        if (x.name != y.name) return x.name < y.name;
        return x.desc < y.desc;
    });
    std::vector<EmitRow> b = computed;
    std::sort(b.begin(), b.end(), [](const EmitRow& x, const EmitRow& y) {
        if (x.startPc != y.startPc) return x.startPc < y.startPc;
        if (x.slot != y.slot) return x.slot < y.slot;
        if (x.length != y.length) return x.length < y.length;
        if (x.name != y.name) return x.name < y.name;
        return x.desc < y.desc;
    });
    for (ca::usize i = 0; i < a.size(); ++i) {
        if (a[i].startPc != b[i].startPc || a[i].slot != b[i].slot || a[i].length != b[i].length
            || a[i].name != b[i].name || a[i].desc != b[i].desc) {
            return false;
        }
    }
    return true;
}

/// 摘除 Code 嵌套的 LVT/LVTT 两槽；返回是否摘除了任何槽。
bool stripDebugTables(ClassFile& cf, CodeAttr& codeAttr)
{
    bool removed = false;
    for (std::size_t i = codeAttr.attrs.size(); i-- > 0;) {
        const AttrSlot& slot = codeAttr.attrs[i];
        if (cf.cp.utf8Equals(slot.nameCp, kLocalVariableTable)
            || cf.cp.utf8Equals(slot.nameCp, kLocalVariableTypeTable)) {
            codeAttr.attrs.erase(codeAttr.attrs.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
        }
    }
    return removed;
}

/// 把重建行挂为新的 LVT 槽（行按 (startPc, slot) 排序落，输出形稳定）。
void attachRows(ClassFile& cf, CodeAttr& codeAttr, const std::vector<EmitRow>& rows)
{
    auto table = std::make_unique<LocalVariableTableAttr>();
    table->rows.reserve(rows.size());
    for (const EmitRow& row : rows) {
        LocalVariableTableAttr::Row out;
        out.startPc = row.startPc;
        out.length  = row.length;
        out.nameCp  = internUtf8(cf, row.name);
        out.descCp  = internUtf8(cf, row.desc);
        out.slot    = row.slot;
        table->rows.push_back(out);
    }
    codeAttr.attrs.push_back(
        AttrSlot::makeNew(internUtf8(cf, kLocalVariableTable), std::move(table)));
}

/// 一个方法的重建（Rebuild 口径）。返回是否改动 backing；@p outSkipped 区分
/// 「升格/解码失败跳过」与「幂等零改动」；@p outStrictFailure 收帧降级；
/// @p outRows/@p outCarried/@p outVar 收行数与名称来源计数。
bool rebuildMethodCode(jbc::ir::ClassSession& session, const jbc::ir::MethodUnit& method,
                       ca::usize methodIdx, ja::ITypeResolver& resolver,
                       const LocalVariableTableRebuilderOptions& options, bool& outSkipped,
                       bool& outStrictFailure, ca::usize& outRows, ca::usize& outCarried,
                       ca::usize& outVar)
{
    using jbc::ir::Insn;

    outSkipped = outStrictFailure = false;
    outRows = outCarried = outVar = 0;

    ClassFile& cf        = session.backing();
    AttrSlot*  codeSlot  = findSlot(cf, cf.methods[method.backingIndex].attrs, kCode);
    if (codeSlot == nullptr) {
        outSkipped = true;
        return false;
    }
    auto decodedCode = cf.decodeAttr(*codeSlot);
    if (decodedCode.is_err()) {
        outSkipped = true;
        return false;
    }
    auto* codeAttr = cast<CodeAttr>(decodedCode.unwrap());
    if (codeAttr == nullptr) {
        outSkipped = true;
        return false;
    }

    // 既有调试表快照：名称/描述符沿用源 + 幂等比对源。
    std::vector<DeclaredRow> declared;
    bool                     hadLvtt  = false;
    const bool               hadTable = snapshotDeclared(cf, *codeAttr, declared, hadLvtt);

    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) {
        outSkipped = true;
        return false;
    }
    jbc::ir::CodeUnit& code = *std::move(promoted).unwrap();

    const ca::u32 codeLength = static_cast<ca::u32>(codeAttr->code.size());

    // 帧计算：Strict 优先（精确引用类型），失败降级 BestEffort（仅形状）。
    ja::MethodSig sig;
    sig.isStatic  = (method.access & kAccStatic) != 0;
    sig.desc      = session.unit().symbols().str(method.desc);
    sig.name      = session.unit().symbols().str(method.name);
    sig.thisClass = session.unit().symbols().str(session.unit().thisClass);

    std::vector<ja::FrameAt> frames;
    bool                     framesOk = false;
    {
        ja::FrameEngine engine(cf.cp, resolver, ja::FramePolicy::Strict);
        framesOk = engine.compute(code, sig, frames).ok;
    }
    if (!framesOk) {
        outStrictFailure = true;
        frames.clear();
        ja::FrameEngine engine(cf.cp);
        framesOk = engine.compute(code, sig, frames).ok;
    }

    const std::vector<jbc::ir::InsnId>& order = code.dom.order();

    // 真实 pc 序列（合成指令 kNoPc 不在其中），用于求「存储后的下一个 pc」。
    std::vector<ca::u32> pcs;
    pcs.reserve(order.size());
    for (const jbc::ir::InsnId id : order) {
        const Insn* in = code.dom.peek(id);
        if (in != nullptr && in->srcPc != jbc::ir::kNoPc) pcs.push_back(in->srcPc);
    }
    std::sort(pcs.begin(), pcs.end());
    auto pcAfter = [&](ca::u32 pc) -> ca::u32 {
        const auto it = std::upper_bound(pcs.begin(), pcs.end(), pc);
        return it == pcs.end() ? codeLength : *it;
    };

    const ca::u32          maxLocals = std::max<ca::u32>(code.maxLocals, codeAttr->maxLocals);
    std::vector<Segment>   segments;
    std::vector<ca::usize> current(maxLocals, kNoSegment);

    auto startSegment = [&](ca::u32 slot, ca::u32 startPc, std::string type) {
        if (slot >= maxLocals || startPc > codeLength) return;
        Segment segment;
        segment.slot    = slot;
        segment.startPc = startPc;
        segment.endPc   = codeLength;
        segment.type    = std::move(type);
        segments.push_back(std::move(segment));
        current[slot] = segments.size() - 1;
    };

    // 形参（含 this）是入口即存在的变量：整段有效，被异型覆盖时再切分。
    ca::u32 slotCursor = 0;
    if (!sig.isStatic) {
        const std::string thisType =
            sig.thisClass.empty() ? std::string{} : "L" + std::string(sig.thisClass) + ";";
        startSegment(slotCursor, 0, thisType);
        ++slotCursor;
    }
    std::vector<std::pair<ca::u32, std::string>> args;
    seedArgSlots(sig.desc, slotCursor, args);
    for (auto& [slot, desc] : args) startSegment(slot, 0, std::move(desc));

    for (ca::usize i = 0; i < order.size(); ++i) {
        const Insn* in = code.dom.peek(order[i]);
        if (in == nullptr) continue;
        const LocalOp local = localOpOf(*in);
        if (local.use == LocalUse::None || local.slot >= maxLocals) continue;

        std::string stackType;
        if (framesOk && i < frames.size() && !frames[i].stack.empty()) {
            stackType = vtypeDesc(frames[i].stack.back());
        }
        std::string localType;
        if (framesOk && i < frames.size() && local.slot < frames[i].locals.size()) {
            localType = vtypeDesc(frames[i].locals[local.slot]);
        }

        if (local.use == LocalUse::Load) {
            ca::usize seg = current[local.slot];
            if (seg == kNoSegment) {
                startSegment(local.slot, in->srcPc, {});
                seg = current[local.slot];
            }
            if (seg != kNoSegment && segments[seg].type.empty() && !localType.empty()) {
                segments[seg].type = localType;
            }
            continue;
        }
        if (local.use == LocalUse::Inc) {
            ca::usize seg = current[local.slot];
            if (seg == kNoSegment) {
                startSegment(local.slot, in->srcPc, "I");
                seg = current[local.slot];
            }
            if (seg != kNoSegment && segments[seg].type.empty()) segments[seg].type = "I";
            continue;
        }

        // Store：赋入类型与当前段异型时切段（槽复用即独立变量）。
        std::string storeType = stackType;
        if (storeType.empty()) storeType = kindDesc(local.kind);
        const ca::u32 after = pcAfter(in->srcPc);
        ca::usize     seg   = current[local.slot];
        if (seg == kNoSegment) {
            startSegment(local.slot, after, std::move(storeType));
            continue;
        }
        Segment& target = segments[seg];
        // 空类型 = 未判定（含 null 存储）；其余必须精确同型才视为同一变量。
        if (target.type.empty() || storeType.empty() || storeType == target.type) {
            if (target.type.empty()) target.type = storeType;
            continue;
        }
        target.endPc = in->srcPc;
        startSegment(local.slot, after, std::move(storeType));
    }

    // 每槽分段按起点序编组，逐段定名：槽上可信名唯一 → 沿用（复用段
    // 原名_2/原名_3 序号）；可信名 ≥2（位置对应不可证）或 0 → var 形态兜底。
    std::map<ca::u32, std::vector<const Segment*>> bySlot;
    for (const Segment& segment : segments) bySlot[segment.slot].push_back(&segment);

    std::vector<EmitRow> computed;
    for (auto& [slot, list] : bySlot) {
        std::stable_sort(list.begin(), list.end(),
                         [](const Segment* a, const Segment* b) { return a->startPc < b->startPc; });
        const std::vector<std::string> trusted = trustedNamesOf(slot, declared);
        const std::vector<std::string> descs   = declaredDescsOf(slot, declared);
        const bool                     carry   = trusted.size() == 1;
        for (ca::usize k = 0; k < list.size(); ++k) {
            const Segment& segment = *list[k];
            if (segment.startPc >= segment.endPc || segment.startPc >= codeLength) continue;
            EmitRow row;
            row.slot    = slot;
            row.startPc = segment.startPc;
            row.length  = segment.endPc - segment.startPc;
            if (carry) {
                row.name = trusted.front();
                if (k > 0) row.name += "_" + std::to_string(k + 1);
                ++outCarried;
            } else {
                row.name = options.prefix + std::to_string(slot);
                if (k > 0) row.name += "_" + std::to_string(k + 1);
                ++outVar;
            }
            row.desc = reconcileDesc(segment.type, descs);
            computed.push_back(std::move(row));
        }
    }

    // 幂等门：重算行集与既有 LVT 逐行一致且无 LVTT → 零改动。
    if (!computed.empty()) {
        if (sameRowSet(declared, computed) && !hadLvtt) return false;
    } else if (!hadTable && !hadLvtt) {
        return false;   // 无表且无存活段：无事可做
    }

    (void)stripDebugTables(cf, *codeAttr);
    if (!computed.empty()) attachRows(cf, *codeAttr, computed);
    (void)codeSlot->mutate();
    outRows = computed.size();
    return true;
}

}   // namespace

LocalVariableTableRebuilder::LocalVariableTableRebuilder(LocalVariableTableRebuilderOptions options)
    : options_(std::move(options))
{}

mjbase::spi::PreservedAnalyses LocalVariableTableRebuilder::run(
    jbc::ir::ClassSession& session, mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    stats_ = Stats{};

    // 层次图只用于 Strict 帧合并；本 pass 是类级单元，只有单会话可登记
    // （跨类同名词合并需求弱于容器级 Annotator，合并失败自然降级 BestEffort）。
    mjbase::KeyPool                       pool;
    mjbase::hierarchy::ClassHierarchy     hierarchy{pool};
    (void)hierarchy.addClass(mjbase::hierarchy::fillSummary(session, pool));
    mjbase::spi::CommonSuperClassResolver  superResolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{superResolver};

    jbc::ir::ClassUnit& unit       = session.unit();
    bool                anyChanged = false;
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (!jbc::ir::hasCode(session, idx)) continue;
        const jbc::ir::MethodUnit& method = unit.methods[idx];

        if (options_.mode == LvtRebuildMode::Drop) {
            ClassFile& cf       = session.backing();
            AttrSlot*  codeSlot = findSlot(cf, cf.methods[method.backingIndex].attrs, kCode);
            if (codeSlot == nullptr) continue;
            auto decodedCode = cf.decodeAttr(*codeSlot);
            if (decodedCode.is_err()) {
                ++stats_.skippedMethods;
                continue;
            }
            auto* codeAttr = cast<CodeAttr>(decodedCode.unwrap());
            if (codeAttr == nullptr) {
                ++stats_.skippedMethods;
                continue;
            }
            ++stats_.methods;
            if (stripDebugTables(cf, *codeAttr)) {
                (void)codeSlot->mutate();
                ++stats_.droppedMethods;
                anyChanged = true;
            }
            continue;
        }

        bool      strictFailure = false;
        bool      skipped       = false;
        ca::usize rows          = 0;
        ca::usize carried       = 0;
        ca::usize varForm       = 0;
        bool      changed       = false;
        try {
            changed = rebuildMethodCode(session, method, idx, typeResolver, options_, skipped,
                                        strictFailure, rows, carried, varForm);
        } catch (const std::exception&) {
            ++stats_.skippedMethods;
            continue;
        }
        if (skipped) {
            ++stats_.skippedMethods;
            continue;
        }
        ++stats_.methods;
        if (strictFailure) ++stats_.strictFailures;
        if (changed) {
            ++stats_.rebuiltMethods;
            stats_.rows += rows;
            stats_.carriedNames += carried;
            stats_.varNames += varForm;
            anyChanged = true;
        }
    }
    return anyChanged ? mjbase::spi::PreservedAnalyses::none()
                      : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
