#include "mjbase/pass/local_variable_annotator.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/local_op.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace mjbase::pass {

namespace {

namespace ja = jbc::analysis;

using ca::core::tag_cast::cast;
using ca::core::tag_cast::isa;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::LocalVariableTableAttr;
using jbc::core::Op;

constexpr std::string_view kCode                     = "Code";
constexpr std::string_view kLocalVariableTable       = "LocalVariableTable";
constexpr std::string_view kLocalVariableTypeTable   = "LocalVariableTypeTable";
constexpr ca::u16          kAccStatic                = 0x0008;
constexpr ca::usize        kNoSegment                = static_cast<ca::usize>(-1);

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
    ca::usize pos = 1;   // '('
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

/// 原 LVT 行快照（用于名字保留与声明类型兜底）。
struct DeclaredRow
{
    ca::u32     slot = 0;
    ca::u32     startPc = 0;
    ca::u32     length = 0;
    std::string name;
    std::string desc;
};

/// 变量生命周期段：一个槽在两次异型赋值之间的一段。
struct Segment
{
    ca::u32     slot = 0;
    ca::u32     startPc = 0;
    ca::u32     endPc = 0;
    std::string type;       ///< 推断描述符；空 = 未知
    std::string declared;   ///< 原 LVT 声明描述符
    std::string declaredName;
    std::string name;       ///< 产物名
    bool        parameter = false;
};

struct MethodResult
{
    std::vector<LocalVariableTableAttr::Row> rows;
    ca::usize                                promoted = 0;
    bool                                     changed = false;
};

/// 名字：slot 的首段 var<slot>，复用段 var<slot>_2、var<slot>_3……
void assignNames(std::vector<Segment>& segments, std::string_view prefix)
{
    std::unordered_map<ca::u32, ca::usize> seen;
    for (Segment& segment : segments) {
        const ca::usize ordinal = seen[segment.slot]++;
        segment.name = std::string(prefix) + std::to_string(segment.slot);
        if (ordinal > 0) segment.name += "_" + std::to_string(ordinal + 1);
    }
}

/// 一个方法的局部变量分析与表重建。
std::optional<MethodResult> annotateMethod(jbc::ir::ClassSession& session, ca::usize methodIdx,
                                           ja::ITypeResolver& resolver,
                                           const LocalVariableAnnotatorOptions& options,
                                           LocalVariableAnnotator::Stats& stats)
{
    using jbc::ir::Insn;

    jbc::ir::ClassUnit& unit = session.unit();
    if (methodIdx >= unit.methods.size()) return std::nullopt;
    jbc::ir::MethodUnit& method = unit.methods[methodIdx];

    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) return std::nullopt;
    jbc::ir::CodeUnit& code = *std::move(promoted).unwrap();

    ClassFile& cf = session.backing();
    AttrSlot*  codeSlot = findSlot(cf, cf.methods[method.backingIndex].attrs, kCode);
    if (codeSlot == nullptr) return std::nullopt;
    auto decodedCode = cf.decodeAttr(*codeSlot);
    if (decodedCode.is_err()) return std::nullopt;
    auto* codeAttr = cast<CodeAttr>(decodedCode.unwrap());
    if (codeAttr == nullptr) return std::nullopt;

    // 原调试表快照：名字/声明类型兜底；随后两张表整体移除重建。
    std::vector<DeclaredRow> declared;
    bool                     hadDebugTable = false;
    for (AttrSlot& slot : codeAttr->attrs) {
        const bool lvt  = cf.cp.utf8Equals(slot.nameCp, kLocalVariableTable);
        const bool lvtt = cf.cp.utf8Equals(slot.nameCp, kLocalVariableTypeTable);
        if (!lvt && !lvtt) continue;
        hadDebugTable = true;
        if (!lvt) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) continue;
        const auto* table = cast<LocalVariableTableAttr>(slot.peek());
        if (table == nullptr) continue;
        for (const LocalVariableTableAttr::Row& row : table->rows) {
            DeclaredRow entry;
            entry.slot    = row.slot;
            entry.startPc = row.startPc;
            entry.length  = row.length;
            entry.name.assign(cf.cp.utf8Of(row.nameCp));
            entry.desc.assign(cf.cp.utf8Of(row.descCp));
            declared.push_back(std::move(entry));
        }
    }

    const ca::u32 codeLength = static_cast<ca::u32>(codeAttr->code.size());
    auto declaredOf = [&](ca::u32 slot) -> const DeclaredRow* {
        for (const DeclaredRow& row : declared) {
            if (row.slot == slot) return &row;
        }
        return nullptr;
    };
    auto declaredAt = [&](ca::u32 slot, ca::u32 pc) -> const DeclaredRow* {
        for (const DeclaredRow& row : declared) {
            if (row.slot == slot && pc >= row.startPc && pc < row.startPc + row.length) return &row;
        }
        return nullptr;
    };

    // 帧计算：Strict 优先（精确引用类型），失败降级 BestEffort（仅形状）。
    ja::MethodSig sig;
    sig.isStatic  = (method.access & kAccStatic) != 0;
    sig.desc      = unit.symbols().str(method.desc);
    sig.name      = unit.symbols().str(method.name);
    sig.thisClass = unit.symbols().str(unit.thisClass);

    std::vector<ja::FrameAt> frames;
    bool                     framesOk = false;
    {
        ja::FrameEngine engine(cf.cp, resolver, ja::FramePolicy::Strict);
        framesOk = engine.compute(code, sig, frames).ok;
    }
    if (!framesOk) {
        ++stats.strictFailures;
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

    const ca::u32 maxLocals = std::max<ca::u32>(code.maxLocals, codeAttr->maxLocals);
    std::vector<Segment>    segments;
    std::vector<ca::usize>  current(maxLocals, kNoSegment);

    auto startSegment = [&](ca::u32 slot, ca::u32 startPc, std::string type,
                            bool parameter) -> ca::usize {
        if (slot >= maxLocals || startPc > codeLength) return kNoSegment;
        Segment segment;
        segment.slot      = slot;
        segment.startPc   = startPc;
        segment.endPc     = codeLength;
        segment.type      = std::move(type);
        segment.parameter = parameter;
        if (const DeclaredRow* row = declaredOf(slot); row != nullptr) {
            segment.declared     = row->desc;
            segment.declaredName = row->name;
        }
        segments.push_back(std::move(segment));
        current[slot] = segments.size() - 1;
        return current[slot];
    };

    // 形参（含 this）是入口即存在的变量：整段有效，被异型覆盖时再切分。
    ca::u32 slotCursor = 0;
    if (!sig.isStatic) {
        const std::string thisType =
            sig.thisClass.empty() ? std::string{} : "L" + std::string(sig.thisClass) + ";";
        startSegment(slotCursor, 0, thisType, true);
        ++slotCursor;
    }
    std::vector<std::pair<ca::u32, std::string>> args;
    seedArgSlots(sig.desc, slotCursor, args);
    for (auto& [slot, desc] : args) startSegment(slot, 0, std::move(desc), true);

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
            if (seg == kNoSegment) seg = startSegment(local.slot, in->srcPc, {}, false);
            if (seg != kNoSegment && segments[seg].type.empty() && !localType.empty()) {
                segments[seg].type = localType;
            }
            continue;
        }
        if (local.use == LocalUse::Inc) {
            ca::usize seg = current[local.slot];
            if (seg == kNoSegment) seg = startSegment(local.slot, in->srcPc, "I", false);
            if (seg != kNoSegment && segments[seg].type.empty()) segments[seg].type = "I";
            continue;
        }

        // Store：赋入类型与当前段异型时切段（槽复用即独立变量）。
        std::string storeType = stackType;
        if (storeType.empty()) storeType = kindDesc(local.kind);
        const ca::u32 after = pcAfter(in->srcPc);
        ca::usize     seg   = current[local.slot];
        if (seg == kNoSegment) {
            startSegment(local.slot, after, std::move(storeType), false);
            continue;
        }
        Segment& target = segments[seg];
        // 空类型 = 未判定（含 null 存储）；其余必须精确同型才视为同一变量。
        if (target.type.empty() || storeType.empty() || storeType == target.type) {
            if (target.type.empty()) target.type = storeType;
            continue;
        }
        target.endPc = in->srcPc;
        startSegment(local.slot, after, std::move(storeType), false);
    }

    assignNames(segments, options.prefix.ref());

    MethodResult result;
    for (const Segment& segment : segments) {
        if (segment.startPc >= segment.endPc || segment.startPc >= codeLength) continue;
        std::string desc = segment.type;
        if (!options.promoteTypes && !segment.declared.empty()) desc = segment.declared;
        if (desc.empty()) {
            desc = segment.declared.empty() ? "Ljava/lang/Object;" : segment.declared;
        }

        std::string name = segment.name;
        if (!options.replaceNames) {
            const DeclaredRow* row = declaredAt(segment.slot, segment.startPc);
            if (row == nullptr) row = declaredOf(segment.slot);
            if (row != nullptr && !row->name.empty()) name = row->name;
        }

        LocalVariableTableAttr::Row row;
        row.startPc = segment.startPc;
        row.length  = segment.endPc - segment.startPc;
        row.nameCp  = internUtf8(cf, name);
        row.descCp  = internUtf8(cf, desc);
        row.slot    = segment.slot;
        result.rows.push_back(row);

        if (!segment.declared.empty() && desc != segment.declared) ++result.promoted;
    }

    if (result.rows.empty() && !hadDebugTable) return result;   // 无局部变量可标注

    // 重建调试表：两张旧表整体摘除，只挂新的 LVT。
    codeAttr->attrs.erase(
        std::remove_if(codeAttr->attrs.begin(), codeAttr->attrs.end(),
                       [&](const AttrSlot& slot) {
                           return cf.cp.utf8Equals(slot.nameCp, kLocalVariableTable) ||
                                  cf.cp.utf8Equals(slot.nameCp, kLocalVariableTypeTable);
                       }),
        codeAttr->attrs.end());
    if (!result.rows.empty()) {
        auto table = std::make_unique<LocalVariableTableAttr>();
        table->rows = std::move(result.rows);
        codeAttr->attrs.push_back(
            AttrSlot::makeNew(internUtf8(cf, kLocalVariableTable), std::move(table)));
    }
    codeSlot->state = AttrSlot::State::Modified;
    result.changed  = true;
    return result;
}

}   // namespace

LocalVariableAnnotator::LocalVariableAnnotator(LocalVariableAnnotatorOptions options)
    : options_(std::move(options))
{}

mjbase::spi::PreservedAnalyses LocalVariableAnnotator::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    stats_ = Stats{};

    struct LoadedClass
    {
        std::string                            uri;
        std::shared_ptr<jbc::ir::ClassSession> session;
    };

    std::vector<LoadedClass> loaded;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!resource || !isa<mjbase::model::ClassResource>(resource.get())) continue;
        auto* cls = cast<mjbase::model::ClassResource>(resource.get());
        if (cls->get().empty()) continue;
        auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (created.is_err()) {
            ++stats_.skippedClasses;
            continue;
        }
        LoadedClass entry;
        entry.uri     = cls->getLocation().to_std_string();
        entry.session = std::move(created).unwrap();
        loaded.push_back(std::move(entry));
    }
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 容器是 unordered_map：按 URI 排序，保证跨平台处理序稳定。
    std::sort(loaded.begin(), loaded.end(),
              [](const LoadedClass& a, const LoadedClass& b) { return a.uri < b.uri; });

    // 层次图只用于 Strict 帧合并；同名引用的合并不依赖登记。
    mjbase::KeyPool                        pool;
    mjbase::hierarchy::ClassHierarchy      hierarchy{pool};
    for (const LoadedClass& entry : loaded) {
        (void)hierarchy.addClass(mjbase::hierarchy::fillSummary(*entry.session, pool));
    }
    mjbase::spi::CommonSuperClassResolver  superResolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{superResolver};

    std::vector<std::pair<std::string, std::vector<ca::u8>>> replacements;
    for (LoadedClass& entry : loaded) {
        ++stats_.classes;
        jbc::ir::ClassUnit& unit = entry.session->unit();
        bool                classChanged = false;
        for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
            if (!jbc::ir::hasCode(*entry.session, idx)) continue;
            std::optional<MethodResult> result;
            try {
                result = annotateMethod(*entry.session, idx, typeResolver, options_, stats_);
            } catch (const std::exception&) {
                ++stats_.skippedMethods;
                continue;
            }
            if (!result.has_value()) {
                ++stats_.skippedMethods;
                continue;
            }
            ++stats_.methods;
            stats_.variables += result->rows.size();
            stats_.promotedTypes += result->promoted;
            classChanged = classChanged || result->changed;
        }
        if (!classChanged) continue;

        auto written = jbc::core::writeClass(entry.session->backing());
        if (written.is_err()) {
            ++stats_.skippedClasses;
            continue;
        }
        replacements.emplace_back(entry.uri, std::move(written).unwrap());
    }

    if (replacements.empty()) return mjbase::spi::PreservedAnalyses::all();
    for (auto& [uri, bytes] : replacements) {
        container.replaceResource(
            uri, std::make_unique<mjbase::model::ClassResource>(uri, std::move(bytes)));
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
