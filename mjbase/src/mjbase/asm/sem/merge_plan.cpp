/// @file        merge_plan.cpp
/// @brief       MergePlan 识别（planInline）与落地（apply）。
///
/// 判据实现对照 merge_plan.hpp 文件头注的识别口径。槽逆映射的栈参
/// 边界在「装载链 ⊆ 参数值」内取保守最大剥离（min(V, 装载链长)）——链上
/// 原代码装载与 N 体序幕装载互为对偶，边界滑动不改变合并结果；
/// retIsEntryCopy 判定同见该头注。
#include "mjbase/asm/sem/merge_plan.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "jbc/analysis/frame.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/ir/code_editor.hpp"

#include "mjbase/asm/insn/insn_op.hpp"
#include "mjbase/asm/sem/frame_view.hpp"
#include "mjbase/asm/sem/insert_prim.hpp"

namespace mjbase::asm_sem {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

using jbc::core::Op;

const jbc::core::AttrSlot* findAttr(const jbc::core::ClassFile& cf,
                                    const std::vector<jbc::core::AttrSlot>& attrs,
                                    std::string_view name)
{
    for (const jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) {
            return &slot;
        }
    }
    return nullptr;
}

jbc::core::AttrSlot* findAttr(jbc::core::ClassFile&              cf,
                              std::vector<jbc::core::AttrSlot>& attrs,
                              std::string_view                  name)
{
    for (jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) {
            return &slot;
        }
    }
    return nullptr;
}

/// 代码锚定嵌套属性归类：LNT / LVT / LVTT / SMT / 盖章可识别，
/// 其余 fail-closed。
enum class NestedKind : ca::u8 { Other, Lnt, Lvt, Lvtt, Smt, Stamp };

NestedKind nestedKindOf(const jbc::core::ClassFile& cf, const jbc::core::AttrSlot& slot)
{
    if (cf.cp.utf8Equals(slot.nameCp, "LineNumberTable")) return NestedKind::Lnt;
    if (cf.cp.utf8Equals(slot.nameCp, "LocalVariableTable")) return NestedKind::Lvt;
    if (cf.cp.utf8Equals(slot.nameCp, "LocalVariableTypeTable")) return NestedKind::Lvtt;
    if (cf.cp.utf8Equals(slot.nameCp, "StackMapTable")) return NestedKind::Smt;
    if (cf.cp.utf8Equals(slot.nameCp, kOutlineStampAttr)) return NestedKind::Stamp;
    return NestedKind::Other;
}

/// 盖章体解析：u16 计数 + (nSlot, parentSlot) 对序列（u16 大端）。
/// 半截/超长章与重复 nSlot 均为非本设施产物，fail-closed。
Result<std::unordered_map<ca::u32, ca::u32>, OutlineError> parseStampBody(
    const std::vector<ca::u8>& b)
{
    if (b.size() < 2u) {
        return Err(OutlineError::CalleeShapeImpure);
    }
    const ca::u32 count = (static_cast<ca::u32>(b[0]) << 8) | static_cast<ca::u32>(b[1]);
    if (b.size() != 2u + count * 4u) {
        return Err(OutlineError::CalleeShapeImpure);
    }
    std::unordered_map<ca::u32, ca::u32> out;
    for (ca::u32 i = 0; i < count; ++i) {
        const ca::u32 nSlot      = (static_cast<ca::u32>(b[2u + i * 4u]) << 8)
                                   | static_cast<ca::u32>(b[3u + i * 4u]);
        const ca::u32 parentSlot = (static_cast<ca::u32>(b[4u + i * 4u]) << 8)
                                   | static_cast<ca::u32>(b[5u + i * 4u]);
        if (!out.emplace(nSlot, parentSlot).second) {
            return Err(OutlineError::CalleeShapeImpure);   /// 重复 nSlot
        }
    }
    return Ok(std::move(out));
}

/// 帧值 → 装载/返回族（调用点参数族门的取值侧；Top/Bottom/Uninit/
/// ReturnAddress 不可跨界，fail-closed）。
Result<ValueFamily, OutlineError> familyOfVType(const jbc::analysis::VType& v)
{
    using jbc::analysis::VKind;
    switch (v.k) {
    case VKind::Int: return Ok(ValueFamily::Int);
    case VKind::Long: return Ok(ValueFamily::Long);
    case VKind::Float: return Ok(ValueFamily::Float);
    case VKind::Double: return Ok(ValueFamily::Double);
    case VKind::Null:
    case VKind::Ref: return Ok(ValueFamily::Ref);
    case VKind::UninitThis:
    case VKind::Uninitialized:
    case VKind::ReturnAddress:
    case VKind::Top:
    case VKind::Bottom:
    default:
        return Err(OutlineError::CallSiteShapeImpure);
    }
}

/// 方法描述符解析：参数表（出现序）+ 返回类型。
struct MethodDesc
{
    std::vector<std::string> params;
    std::string              ret;
};

Result<MethodDesc, OutlineError> parseMethodDesc(std::string_view desc)
{
    if (desc.empty() || desc.front() != '(') {
        return Err(OutlineError::CalleeShapeImpure);
    }
    const ca::usize close = desc.find(')');
    if (close == std::string_view::npos) {
        return Err(OutlineError::CalleeShapeImpure);
    }
    MethodDesc out;
    ca::usize  i = 1;
    while (i < close) {
        const ca::usize start = i;
        const char      c     = desc[i];
        if (c == 'L') {
            const ca::usize semi = desc.find(';', i);
            if (semi == std::string_view::npos || semi >= close) {
                return Err(OutlineError::CalleeShapeImpure);
            }
            i = semi + 1;
        } else if (c == '[') {
            while (i < close && desc[i] == '[') {
                ++i;
            }
            if (i >= close) {
                return Err(OutlineError::CalleeShapeImpure);
            }
            if (desc[i] == 'L') {
                const ca::usize semi = desc.find(';', i);
                if (semi == std::string_view::npos || semi >= close) {
                    return Err(OutlineError::CalleeShapeImpure);
                }
                i = semi + 1;
            } else {
                ++i;
            }
        } else if (c == 'J' || c == 'D' || c == 'I' || c == 'Z' || c == 'B' || c == 'C'
                   || c == 'S' || c == 'F') {
            ++i;
        } else {
            return Err(OutlineError::CalleeShapeImpure);
        }
        out.params.emplace_back(desc.substr(start, i - start));
    }
    out.ret = std::string(desc.substr(close + 1));
    if (out.ret.empty()) {
        return Err(OutlineError::CalleeShapeImpure);
    }
    return Ok(std::move(out));
}

}   // namespace

// ---------------------------------------------------------------------------
// 识别侧：planInline
// ---------------------------------------------------------------------------

Result<MergePlan, OutlineError> planInline(const InlineRequest& req)
{
    jbc::ir::ClassUnit& cu = req.session.unit();
    jbc::ir::MethodUnit* mu = cu.method(req.parent);
    if (mu == nullptr) {
        return Err(OutlineError::BadRegionHandles);
    }

    /// 类内代码域全量升格：调用点计数扫描的前提（未升格方法的引用不可见，
    /// 漏数会放行多调用点 N）。升格真失败 = 代码域不可完整扫描，保守拒。
    for (const jbc::ir::CodeVisit& visit : jbc::ir::collectCodeUnits(req.session)) {
        if (visit.kind == jbc::ir::CodeVisit::Kind::Failed) {
            return Err(OutlineError::CallSiteCountUnsupported);
        }
    }

    jbc::ir::CodeUnit* unit = mu->code.get();
    if (unit == nullptr) {
        auto promoted = jbc::ir::promoteMethod(req.session, req.parent);
        if (promoted.is_err()) {
            return Err(OutlineError::ParentNoCode);
        }
        unit = mu->code.get();
        if (unit == nullptr) {
            return Err(OutlineError::ParentNoCode);
        }
    }

    const std::string_view thisClass = cu.symbols().str(cu.thisClass);
    const std::vector<jbc::ir::InsnId>& order = unit->dom.order();

    // ---- 候选定位：目标 N（指定或自动发现）+ 父方法内调用点 ----
    jbc::ir::MethodId calleeId = req.callee;
    jbc::ir::InsnId   callId{};
    auto memberMatches = [&](const jbc::ir::Insn& in,
                             std::string_view name,
                             std::string_view desc) {
        jbc::ir::ClassSession::MemberSem sem;
        if (!req.session.memberOf(in, sem)
            || sem.kind != jbc::ir::ClassSession::MemberSem::Kind::Method) {
            return false;
        }
        return sem.owner == thisClass && sem.name == name && sem.desc == desc;
    };
    if (cu.method(calleeId) != nullptr) {
        jbc::ir::MethodUnit* cm = cu.method(calleeId);
        const std::string_view calleeName = cu.symbols().str(cm->name);
        const std::string_view calleeDesc = cu.symbols().str(cm->desc);
        for (const jbc::ir::InsnId id : order) {
            const jbc::ir::Insn* in = unit->dom.peek(id);
            if (in != nullptr && in->op == Op::INVOKESTATIC && memberMatches(*in, calleeName, calleeDesc)) {
                callId = id;
                break;
            }
        }
        if (unit->dom.peek(callId) == nullptr) {
            /// 类内引用不在本消费方（调用方指定错 parent 或单调用点在别处）。
            return Err(OutlineError::CallSiteShapeImpure);
        }
    } else {
        for (const jbc::ir::InsnId id : order) {
            const jbc::ir::Insn* in = unit->dom.peek(id);
            if (in == nullptr || in->op != Op::INVOKESTATIC) {
                continue;
            }
            jbc::ir::ClassSession::MemberSem sem;
            if (!req.session.memberOf(*in, sem)
                || sem.kind != jbc::ir::ClassSession::MemberSem::Kind::Method
                || sem.owner != thisClass) {
                continue;
            }
            std::string_view name(sem.name);
            if (name.size() <= std::string_view("outline$").size()
                || name.substr(0, std::string_view("outline$").size())
                       != std::string_view("outline$")) {
                continue;
            }
            const jbc::ir::MethodId target = cu.findMethodId(sem.name, sem.desc);
            if (cu.method(target) == nullptr) {
                continue;   /// 悬空同名引用（非本设施产物），不入选
            }
            calleeId = target;
            callId   = id;
            break;
        }
        if (cu.method(calleeId) == nullptr) {
            return Err(OutlineError::NoInlineCandidate);
        }
    }
    if (calleeId == req.parent) {
        return Err(OutlineError::CallSiteShapeImpure);   /// 自我内联拒绝
    }

    // ---- 类内单调用点计数（代码域扫描）----
    {
        jbc::ir::MethodUnit* cm = cu.method(calleeId);
        const std::string_view calleeName = cu.symbols().str(cm->name);
        const std::string_view calleeDesc = cu.symbols().str(cm->desc);
        ca::u32 refs = 0;
        for (const jbc::ir::MethodUnit& m : cu.methods) {
            if (m.code == nullptr) {
                continue;
            }
            for (const jbc::ir::InsnId id : m.code->dom.order()) {
                const jbc::ir::Insn* in = m.code->dom.peek(id);
                if (in != nullptr && in->op == Op::INVOKESTATIC
                    && memberMatches(*in, calleeName, calleeDesc)) {
                    ++refs;
                }
            }
        }
        if (refs != 1u) {
            return Err(OutlineError::CallSiteCountUnsupported);
        }
    }

    // ---- N 形态判据：身份 + 体结构 + 描述符 ----
    jbc::ir::MethodUnit* cm = cu.method(calleeId);
    if (cm->access != (0x0002u | 0x0008u | 0x1000u)) {
        return Err(OutlineError::NotOutlineSynthetic);
    }
    const std::string_view calleeName = cu.symbols().str(cm->name);
    const std::string_view prefix("outline$");
    if (calleeName.size() <= prefix.size()
        || calleeName.substr(0, prefix.size()) != prefix) {
        return Err(OutlineError::NotOutlineSynthetic);
    }
    jbc::ir::CodeUnit* nUnit = cm->code.get();
    if (nUnit == nullptr) {
        auto promoted = jbc::ir::promoteMethod(req.session, calleeId);
        if (promoted.is_err() || cm->code.get() == nullptr) {
            return Err(OutlineError::CalleeShapeImpure);
        }
        nUnit = cm->code.get();
    }
    if (!nUnit->exc.empty()) {
        return Err(OutlineError::CalleeTryCovered);   /// outline 产物恒无异常表
    }

    jbc::core::ClassFile& cf = req.session.backing();
    jbc::core::AttrSlot*  nCodeSlot =
        findAttr(cf, cf.methods[cm->backingIndex].attrs, "Code");
    if (nCodeSlot == nullptr) {
        return Err(OutlineError::UnsupportedAttr);
    }
    for (const jbc::core::AttrSlot& slot : cf.methods[cm->backingIndex].attrs) {
        /// 方法级属性白名单：outline 产物仅 Code；其余（TypeAnnotations 族等）
        /// 重定位面未建模，保守拒。
        if (!cf.cp.utf8Equals(slot.nameCp, "Code")) {
            return Err(OutlineError::UnsupportedAttr);
        }
    }

    /// 自足槽盖章：N 的 Code 嵌套 kOutlineStampAttr（RawAttr）承载
    /// outline 侧 (nSlot, parentSlot) 对，槽逆映射消费后自足槽 exact 对回；
    /// 无章回落 fresh（下方，外来 inline 形态兼容）。半截/重复章
    /// 均为非本设施产物，fail-closed。
    std::unordered_map<ca::u32, ca::u32> stampBack;
    {
        auto nStampCode = cf.decodeAttr(*nCodeSlot);
        if (nStampCode.is_err()) {
            return Err(OutlineError::UnsupportedAttr);
        }
        for (jbc::core::AttrSlot& nested :
             static_cast<jbc::core::CodeAttr*>(nStampCode.unwrap())->attrs) {
            if (nestedKindOf(cf, nested) != NestedKind::Stamp) {
                continue;
            }
            auto stampBody = cf.decodeAttr(nested);
            if (stampBody.is_err()) {
                return Err(OutlineError::UnsupportedAttr);
            }
            auto parsed = parseStampBody(
                static_cast<const jbc::core::RawAttr*>(stampBody.unwrap())->bytes());
            if (parsed.is_err()) {
                return Err(parsed.unwrap_err());
            }
            stampBack = std::move(parsed).unwrap();
        }
    }

    const std::vector<jbc::ir::InsnId>& nOrder = nUnit->dom.order();
    if (nOrder.empty()) {
        return Err(OutlineError::CalleeShapeImpure);
    }
    for (ca::usize i = 0; i + 1u < nOrder.size(); ++i) {
        const jbc::ir::Insn* in = nUnit->dom.peek(nOrder[i]);
        if (in == nullptr) {
            return Err(OutlineError::BadRegionHandles);
        }
        if (isControlTransfer(in->op)) {
            return Err(OutlineError::CalleeNotStraightLine);
        }
    }
    const jbc::ir::Insn* nRet = nUnit->dom.peek(nOrder.back());
    if (nRet == nullptr || !asm_insn::isReturn(nRet->op)) {
        return Err(OutlineError::CalleeShapeImpure);
    }

    auto parsedDesc = parseMethodDesc(cu.symbols().str(cm->desc));
    if (parsedDesc.is_err()) {
        return Err(parsedDesc.unwrap_err());
    }
    const MethodDesc desc = std::move(parsedDesc).unwrap();
    if (returnOpOf(familyOfDesc(desc.ret[0])) != nRet->op) {
        return Err(OutlineError::CalleeShapeImpure);   /// 尾返回与描述符不同族
    }

    // ---- N 体可验证性 + 返回点栈深。合并净栈效应等价的前提：调用点净
    //      效应 −V+1；回迁侧 = 剥离 k 幕布装载后跑体，净效应 (retDepth−k)
    //      −k……化简：strip m 条装载与 V−k 参数对消后，汇合深度一致 ⟺
    //      retDepth = 1（retIsEntryCopy 时复位 DUP 与区域尾 DUP 对消，为 2）。
    jbc::analysis::MethodSig sigN;
    sigN.isStatic  = true;
    sigN.desc      = cu.symbols().str(cm->desc);
    sigN.name      = calleeName;
    sigN.thisClass = thisClass;
    auto builtN = FrameView::build(req.session.backing().cp, *nUnit, sigN, req.resolver);
    if (builtN.is_err()) {
        return Err(OutlineError::CalleeShapeImpure);   /// 体不可验证（外来/残缺形态）
    }
    FrameView nFrames = std::move(builtN).unwrap();
    const ca::u32 retDepth = nFrames.stackValueCount(nOrder.back());

    // ---- N 体序幕解析：栈参装载覆盖的槽位排布 + 族匹配（k_max）----
    ca::u32 kMax = 0;
    ca::u32 slotCursor = 0;
    while (kMax < desc.params.size() && kMax < nOrder.size()) {
        const jbc::ir::Insn* in = nUnit->dom.peek(nOrder[kMax]);
        if (in == nullptr || !asm_insn::isLoad(in->op)) {
            break;
        }
        if (localOperandSlot(*in) != slotCursor) {
            break;
        }
        if (loadOpOf(familyOfDesc(desc.params[kMax][0])) != explicitLocalOp(in->op)) {
            break;
        }
        slotCursor += descSlots(desc.params[kMax][0]);
        ++kMax;
    }

    // ---- 调用点形态：帧 + 参数族 + 装载链回溯（标签护栏）----
    jbc::analysis::MethodSig sigP;
    sigP.isStatic  = (mu->access & 0x0008u) != 0;
    sigP.desc      = cu.symbols().str(mu->desc);
    sigP.name      = cu.symbols().str(mu->name);
    sigP.thisClass = thisClass;
    auto builtFrames = FrameView::build(req.session.backing().cp, *unit, sigP, req.resolver);
    if (builtFrames.is_err()) {
        return Err(OutlineError::FrameFailed);
    }
    FrameView frames = std::move(builtFrames).unwrap();

    const ca::u32 valueCount = frames.stackValueCount(callId);
    const ca::u32 vCount     = static_cast<ca::u32>(desc.params.size());
    if (valueCount < vCount) {
        return Err(OutlineError::CallSiteShapeImpure);
    }
    for (ca::u32 j = 0; j < vCount; ++j) {
        const jbc::analysis::VType* v = frames.stackFromTop(callId, vCount - 1u - j);
        if (v == nullptr) {
            return Err(OutlineError::CallSiteShapeImpure);
        }
        auto fam = familyOfVType(*v);
        if (fam.is_err()) {
            return Err(fam.unwrap_err());
        }
        if (fam.unwrap() != familyOfDesc(desc.params[j][0])) {
            return Err(OutlineError::CallSiteShapeImpure);
        }
    }

    ca::usize callIdx = order.size();
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (order[i].slot == callId.slot) {
            callIdx = i;
            break;
        }
    }
    if (callIdx == order.size()) {
        return Err(OutlineError::BadRegionHandles);
    }

    /// 装载链回溯：自调用点线性向前收局部 load；标签锚定的链尾可收（本路
    /// 径末条）但不可越（链前的指令可能在别的入边上）。链外边界指令留给
    /// 复位 DUP 探测。
    std::vector<jbc::ir::InsnId> run;   /// 逆序收集（贴调用点在前）
    const jbc::ir::Insn* boundaryInsn = nullptr;
    jbc::ir::InsnId      boundaryId{};
    for (ca::usize i = callIdx; i > 0;) {
        --i;
        const jbc::ir::InsnId id = order[i];
        const jbc::ir::Insn* in = unit->dom.peek(id);
        if (in == nullptr) {
            return Err(OutlineError::BadRegionHandles);
        }
        if (asm_insn::isLoad(in->op)) {
            run.push_back(id);
            if (!unit->dom.labelsAt(id).empty()) {
                break;
            }
            continue;
        }
        boundaryInsn = in;
        boundaryId   = id;
        break;
    }
    std::reverse(run.begin(), run.end());   /// 转程序序（深 → 贴调用点）

    const ca::usize m = std::min(static_cast<ca::usize>(vCount), run.size());
    const ca::u32   k = static_cast<ca::u32>(vCount - m);
    if (static_cast<ca::usize>(k) > kMax) {
        return Err(OutlineError::CalleeShapeImpure);   /// 体序幕覆盖不了栈参
    }

    MergePlan plan;
    plan.parent   = req.parent;
    plan.callee   = calleeId;
    plan.callInsn = callId;
    for (ca::usize t = 0; t < m; ++t) {
        plan.stripLoads.push_back(run[run.size() - m + t]);   /// 链尾 m 条（程序序）
    }

    // ---- strip 集 + 锚标签（strip 首条的标签 = outline 侧入口标签重锚位）----
    std::vector<jbc::ir::InsnId> stripSet;
    /// 入口栈顶复制形态对偶：strip 集紧邻复位 DUP/DUP2（m == 链全长，
    /// 边界即 strip 前一位）、去序幕后区域尾条为 DUP/DUP2、首参数与返回值
    /// 同族、栈参恰 1（k == 1）。outline 侧该形态的条件（prefix == dIn）
    /// 保证「真切片参数 + 体尾 DUP」的外来同构形态不可能是本设施产物
    /// （该形态 outline 判据 RetNotProduced 拒绝）。
    if (boundaryInsn != nullptr && (boundaryInsn->op == Op::DUP || boundaryInsn->op == Op::DUP2)
        && k == 1u && run.size() == m && nOrder.size() >= 3u) {
        const jbc::ir::Insn* rTail = nUnit->dom.peek(nOrder[nOrder.size() - 2u]);
        if (rTail != nullptr && (rTail->op == Op::DUP || rTail->op == Op::DUP2)
            && familyOfDesc(desc.params[0][0]) == familyOfDesc(desc.ret[0])) {
            plan.retIsEntryCopy = true;
            plan.entryDup       = boundaryId;
        }
    }
    if (plan.retIsEntryCopy) {
        stripSet.push_back(plan.entryDup);
    }
    if (retDepth != (plan.retIsEntryCopy ? 2u : 1u)) {
        return Err(OutlineError::CalleeShapeImpure);   /// 返回点栈深与合并算术不合
    }
    stripSet.insert(stripSet.end(), plan.stripLoads.begin(), plan.stripLoads.end());

    const jbc::ir::InsnId anchorInsn = plan.retIsEntryCopy
                                           ? plan.entryDup
                                           : (plan.stripLoads.empty()
                                                  ? callId
                                                  : plan.stripLoads.front());
    {
        const std::vector<jbc::ir::LabelId> anchorLabels = unit->dom.labelsAt(anchorInsn);
        if (anchorLabels.size() > 1u) {
            return Err(OutlineError::CallSiteShapeImpure);
        }
        if (anchorLabels.size() == 1u) {
            plan.anchorLabel = anchorLabels[0];
        }
        for (ca::usize t = 1u; t < stripSet.size(); ++t) {
            /// strip 中间条带标签 = 参数装载链中跨路径（非本设施产物）。
            if (!unit->dom.labelsAt(stripSet[t]).empty()) {
                return Err(OutlineError::CallSiteShapeImpure);
            }
        }
        if (!stripSet.empty() && !unit->dom.labelsAt(callId).empty()) {
            /// strip 非空时调用点带标签：erase 会孤儿化标签（非本设施产物）。
            return Err(OutlineError::CallSiteShapeImpure);
        }
    }

    // ---- 槽逆映射：参数槽（k..V-1 ← 装载链装载槽）+ 自足槽（盖章 exact
    //      / 无章 fresh）。盖章表已在 N 形态判据后自 Code 嵌套属性解出
    //      （stampBack）；无章条目回落 fresh ----
    plan.freshBase = unit->maxLocals;
    ca::u32 freshCursor = plan.freshBase;
    std::set<ca::u32> selfSlots;
    std::unordered_map<ca::u32, ca::u32> selfWidth;
    for (ca::usize t = 0; t < m; ++t) {
        const jbc::ir::Insn* load = unit->dom.peek(plan.stripLoads[t]);
        ca::u32 paramSlot = 0;
        for (ca::u32 j = 0; j < static_cast<ca::u32>(t); ++j) {
            paramSlot += descSlots(desc.params[k + j][0]);
        }
        SlotBack back;
        back.nSlot          = paramSlot;
        back.parentSlot     = localOperandSlot(*load);
        back.selfSufficient = false;
        plan.slotBacks.push_back(back);
    }
    for (ca::usize i = k; i + 1u < nOrder.size(); ++i) {
        const jbc::ir::Insn* in = nUnit->dom.peek(nOrder[i]);
        if (in == nullptr) {
            return Err(OutlineError::BadRegionHandles);
        }
        if (!asm_insn::isLoad(in->op) && !asm_insn::isStore(in->op) && in->op != Op::IINC) {
            continue;
        }
        const ca::u32 s = localOperandSlot(*in);
        bool isParam = false;
        for (const SlotBack& back : plan.slotBacks) {
            if (back.nSlot == s) {
                isParam = true;
                break;
            }
        }
        if (isParam) {
            continue;
        }
        selfSlots.insert(s);
        ca::u32& w = selfWidth[s];
        w = std::max(w, localOpSlotWidth(in->op));
    }
    for (const ca::u32 s : selfSlots) {
        SlotBack back;
        back.nSlot          = s;
        back.selfSufficient = true;
        const auto stamped = stampBack.find(s);
        if (stamped != stampBack.end()) {
            back.parentSlot = stamped->second;   /// 盖章档：原槽 exact 恢复
        } else {
            back.parentSlot = freshCursor;       /// 无章回落 fresh
            ca::u32 w = 1;
            const auto wit = selfWidth.find(s);
            if (wit != selfWidth.end()) {
                w = std::max(w, wit->second);
            }
            freshCursor += w;
        }
        plan.slotBacks.push_back(back);
    }

    // ---- 回迁体：N 体去序幕/去返回，槽逆映射改写为父槽显式形态 ----
    std::unordered_map<ca::u32, ca::u32> backOf;
    for (const SlotBack& back : plan.slotBacks) {
        backOf.emplace(back.nSlot, back.parentSlot);
    }
    for (ca::usize i = k; i + 1u < nOrder.size(); ++i) {
        const jbc::ir::Insn* in = nUnit->dom.peek(nOrder[i]);
        if (in == nullptr) {
            return Err(OutlineError::BadRegionHandles);
        }
        jbc::ir::Insn copy = *in;
        if (asm_insn::isLoad(copy.op) || asm_insn::isStore(copy.op) || copy.op == Op::IINC) {
            const auto it = backOf.find(localOperandSlot(copy));
            if (it == backOf.end()) {
                return Err(OutlineError::CalleeShapeImpure);   /// 槽引用不可逆映射
            }
            copy.op = explicitLocalOp(copy.op);
            copy.a  = static_cast<ca::i32>(it->second);
        }
        plan.body.push_back(copy);
    }
    if (plan.body.empty()) {
        return Err(OutlineError::CalleeShapeImpure);   /// 纯序幕 + 返回（纯 goto
                                                       /// 区域 outline 已拒）
    }
    plan.leadingStrip = k;

    // ---- N 的 LNT/LVT 行回收（outline 侧迁移表的逆操作；锚点换算回迁体序）----
    auto rawExported = nUnit->dom.exportRaw();
    if (rawExported.is_err() || rawExported.unwrap().size() != nOrder.size()) {
        return Err(OutlineError::BadRegionHandles);
    }
    const std::vector<jbc::core::RawInsn> nRaw = std::move(rawExported).unwrap();
    std::unordered_map<ca::u32, ca::u32> pcToBody;
    for (ca::usize i = 0; i < nRaw.size(); ++i) {
        pcToBody.emplace(nRaw[i].pc, static_cast<ca::u32>(i));
    }
    auto nCodeBody = cf.decodeAttr(*nCodeSlot);
    if (nCodeBody.is_err()) {
        return Err(OutlineError::UnsupportedAttr);
    }
    for (jbc::core::AttrSlot& nested :
         static_cast<jbc::core::CodeAttr*>(nCodeBody.unwrap())->attrs) {
        const NestedKind kind = nestedKindOf(cf, nested);
        if (kind == NestedKind::Smt) {
            continue;   /// 随 N 删除（maxes/SMT 均为再生面）
        }
        if (kind == NestedKind::Stamp) {
            continue;   /// 盖章已在槽逆映射前解出（stampBack）
        }
        if (kind == NestedKind::Other) {
            return Err(OutlineError::UnsupportedAttr);
        }
        auto rowBody = cf.decodeAttr(nested);
        if (rowBody.is_err()) {
            return Err(OutlineError::UnsupportedAttr);
        }
        if (kind == NestedKind::Lnt) {
            const auto* lnt = static_cast<jbc::core::LineNumberTableAttr*>(rowBody.unwrap());
            for (const jbc::core::LineNumberTableAttr::Row& row : lnt->rows) {
                const auto it = pcToBody.find(row.startPc);
                if (it == pcToBody.end() || it->second < k) {
                    return Err(OutlineError::CalleeShapeImpure);
                }
                const ca::u32 rIndex = it->second - k;
                if (rIndex >= plan.body.size()) {
                    return Err(OutlineError::CalleeShapeImpure);   /// 锚在返回指令
                }
                LntRowMove back;
                back.startPc   = row.startPc;
                back.line      = row.line;
                back.bodyIndex = rIndex;
                plan.lntBacks.push_back(back);
            }
        } else {
            const auto* lvt = static_cast<jbc::core::LocalVariableTableAttr*>(rowBody.unwrap());
            for (const jbc::core::LocalVariableTableAttr::Row& row : lvt->rows) {
                const auto it = pcToBody.find(row.startPc);
                if (it == pcToBody.end() || it->second < k) {
                    return Err(OutlineError::CalleeShapeImpure);
                }
                const ca::u32 rIndex = it->second - k;
                if (rIndex >= plan.body.size()) {
                    return Err(OutlineError::CalleeShapeImpure);
                }
                const auto backIt = backOf.find(row.slot);
                if (backIt == backOf.end()) {
                    return Err(OutlineError::CalleeShapeImpure);   /// 行槽不可逆映射
                }
                LvtRowMove back;
                back.startPc   = row.startPc;
                back.length    = row.length;
                back.nameCp    = row.nameCp;
                back.descCp    = row.descCp;
                back.typeTable = (kind == NestedKind::Lvtt);
                back.oldSlot   = row.slot;
                back.newSlot   = backIt->second;
                back.bodyIndex = rIndex;
                plan.lvtBacks.push_back(back);
            }
        }
    }

    return Ok(std::move(plan));
}

// ---------------------------------------------------------------------------
// 数据侧：validate
// ---------------------------------------------------------------------------

Result<void, OutlineError> MergePlan::validate() const
{
    if (body.empty()) {
        return Err(OutlineError::MalformedPlan);
    }
    std::set<ca::u32> mapped;
    for (const SlotBack& back : slotBacks) {
        mapped.insert(back.parentSlot);
    }
    for (const jbc::ir::Insn& in : body) {
        if (isControlTransfer(in.op)) {
            return Err(OutlineError::MalformedPlan);
        }
        if (asm_insn::isLoad(in.op) || asm_insn::isStore(in.op) || in.op == Op::IINC) {
            /// 体载荷已逆映射到父槽（planInline 构建期改写）。
            if (mapped.count(static_cast<ca::u32>(in.a)) == 0u) {
                return Err(OutlineError::MalformedPlan);
            }
        }
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// 落地侧：apply
// ---------------------------------------------------------------------------

Result<InlineOutcome, OutlineError> MergePlan::apply(jbc::ir::ClassSession& session,
                                                     jbc::analysis::ITypeResolver& resolver) const
{
    auto checked = validate();
    if (checked.is_err()) {
        return Err(checked.unwrap_err());
    }

    jbc::ir::ClassUnit&  cu = session.unit();
    jbc::ir::MethodUnit* mu = cu.method(parent);
    jbc::ir::MethodUnit* cm = cu.method(callee);
    if (mu == nullptr || cm == nullptr) {
        return Err(OutlineError::BadRegionHandles);
    }
    jbc::ir::CodeUnit* unit = mu->code.get();
    if (unit == nullptr) {
        return Err(OutlineError::ParentNoCode);
    }
    if (unit->dom.peek(callInsn) == nullptr) {
        return Err(OutlineError::BadRegionHandles);
    }
    if (retIsEntryCopy && unit->dom.peek(entryDup) == nullptr) {
        return Err(OutlineError::BadRegionHandles);
    }
    for (const jbc::ir::InsnId id : stripLoads) {
        if (unit->dom.peek(id) == nullptr) {
            return Err(OutlineError::BadRegionHandles);
        }
    }

    // ---- 步骤 1：回迁体插入（标签锚定逆序 / fallthrough 直插正序）----
    //
    // 标签口径与 outline 落地镜像：strip 首条（复位 DUP 或首条装载）所锚标签 =
    // outline 入口标签重锚位，回迁体按执行序逆序逐条插到标签边界，终序 =
    // body 程序序、标签重锚到回迁体首条——strip 集随之失去标签承载，可安全
    // 删除。fallthrough 形态直插 strip 首条（或调用点）之前。机械复用
    // insert_prim：标签重锚走 insertAnchoredAtLabel、fallthrough 走
    // insertBeforeInsn（与 OutlinePlan::apply 步骤 2 同原语）。
    jbc::ir::CodeEditor ed{*unit};
    std::vector<jbc::ir::InsnId> rIds;
    rIds.reserve(body.size());
    const jbc::ir::InsnId anchorInsn = retIsEntryCopy
                                           ? entryDup
                                           : (stripLoads.empty() ? callInsn
                                                                 : stripLoads.front());
    if (unit->dom.validLabel(anchorLabel)) {
        auto anchored = insertAnchoredAtLabel(ed, anchorLabel, body);
        if (anchored.is_err()) {
            return Err(anchored.unwrap_err());
        }
        rIds = std::move(anchored).unwrap();
    } else {
        auto placed = insertBeforeInsn(ed, anchorInsn, body);
        if (placed.is_err()) {
            return Err(placed.unwrap_err());
        }
        rIds = std::move(placed).unwrap();
    }

    // ---- 步骤 2：strip 集（复位 DUP + 装载链）与调用点删除 ----
    {
        std::vector<jbc::ir::InsnId> erase;
        if (retIsEntryCopy) {
            erase.push_back(entryDup);
        }
        erase.insert(erase.end(), stripLoads.begin(), stripLoads.end());
        erase.push_back(callInsn);
        if (ed.eraseBatch(erase, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed).is_err()) {
            return Err(OutlineError::EditFailed);
        }
    }

    // ---- 步骤 3：父方法 SMT 摘除（再生前；LNT/LVT 父表行不动——N 的行
    //      随 N 删除，回迁行在步骤 5 挂入）----
    jbc::core::ClassFile& cf = session.backing();
    {
        jbc::core::AttrSlot* codeSlot =
            findAttr(cf, cf.methods[mu->backingIndex].attrs, "Code");
        if (codeSlot == nullptr) {
            return Err(OutlineError::LowerFailed);
        }
        auto parentCode = cf.decodeAttr(*codeSlot);
        if (parentCode.is_err()) {
            return Err(OutlineError::LowerFailed);
        }
        auto* codeAttr = static_cast<jbc::core::CodeAttr*>(parentCode.unwrap());
        auto& attrs    = codeAttr->attrs;
        const ca::usize before = attrs.size();
        attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                                   [&](const jbc::core::AttrSlot& s) {
                                       return cf.cp.utf8Equals(s.nameCp, "StackMapTable");
                                   }),
                    attrs.end());
        if (attrs.size() != before) {
            (void)codeSlot->mutate();
        }
    }

    // ---- 步骤 4：父方法 lower → Strict 帧重算 → maxes 采用 → 二次 lower
    //      → SMT 整表再生（与 OutlinePlan::apply 步骤 4/5 同流水）----
    if (jbc::ir::lowerMethod(session, parent).is_err()) {
        return Err(OutlineError::LowerFailed);
    }
    const std::string_view thisClass = cu.symbols().str(cu.thisClass);
    jbc::analysis::MethodSig sigP;
    sigP.isStatic  = (mu->access & 0x0008u) != 0;
    sigP.desc      = cu.symbols().str(mu->desc);
    sigP.name      = cu.symbols().str(mu->name);
    sigP.thisClass = thisClass;

    jbc::analysis::FrameEngine engine(session.backing().cp, resolver,
                                      jbc::analysis::FramePolicy::Strict);
    std::vector<jbc::analysis::FrameAt> parentFrames;
    const jbc::analysis::FrameSummary   summary = engine.compute(*unit, sigP, parentFrames);
    if (!summary.ok) {
        return Err(OutlineError::FrameFailed);
    }
    if (!jbc::analysis::applyComputedMaxes(*unit, summary, sigP)) {
        return Err(OutlineError::MaxesFailed);
    }
    if (jbc::ir::lowerMethod(session, parent).is_err()) {
        return Err(OutlineError::LowerFailed);
    }
    if ((cu.version >> 16) >= 50u) {
        auto smt = jbc::analysis::buildStackMapTable(session, *unit, parentFrames);
        if (smt.is_err()) {
            return Err(OutlineError::SmtFailed);
        }
        auto result = std::move(smt).unwrap();
        if (result.needed) {
            jbc::core::AttrSlot* fresh =
                findAttr(cf, cf.methods[mu->backingIndex].attrs, "Code");
            if (fresh == nullptr) {
                return Err(OutlineError::LowerFailed);
            }
            auto parentCode = cf.decodeAttr(*fresh);
            if (parentCode.is_err()) {
                return Err(OutlineError::LowerFailed);
            }
            static_cast<jbc::core::CodeAttr*>(parentCode.unwrap())
                ->attrs.push_back(jbc::core::AttrSlot::makeNew(
                    session.internUtf8("StackMapTable"),
                    std::unique_ptr<jbc::core::AttrBody>(std::move(result.attr))));
            (void)fresh->mutate();
        }
    }

    // ---- 步骤 5：N 的 LNT/LVT 行回迁（锚定回迁体窗口；LVT 槽已按
    //      slotBacks 逆映射、区间覆盖回迁体跨度）----
    if (!lntBacks.empty() || !lvtBacks.empty()) {
        auto rawExported = unit->dom.exportRaw();
        if (rawExported.is_err()) {
            return Err(OutlineError::LowerFailed);
        }
        const std::vector<jbc::core::RawInsn> raw = std::move(rawExported).unwrap();
        const std::vector<jbc::ir::InsnId>& ord = unit->dom.order();
        std::unordered_map<ca::u32, ca::usize> idxOf;
        for (ca::usize i = 0; i < ord.size(); ++i) {
            idxOf.emplace(ord[i].slot, i);
        }
        auto pcOf = [&](jbc::ir::InsnId id, ca::u32& out) {
            const auto it = idxOf.find(id.slot);
            if (it == idxOf.end() || it->second >= raw.size()) {
                return false;
            }
            out = raw[it->second].pc;
            return true;
        };
        ca::u32 rEnd = 0;
        {
            const auto it = idxOf.find(rIds.back().slot);
            if (it == idxOf.end()) {
                return Err(OutlineError::LowerFailed);
            }
            rEnd = it->second + 1u < raw.size() ? raw[it->second + 1u].pc
                                                : raw[it->second].pc + 1u;
        }

        jbc::core::AttrSlot* codeSlot =
            findAttr(cf, cf.methods[mu->backingIndex].attrs, "Code");
        if (codeSlot == nullptr) {
            return Err(OutlineError::LowerFailed);
        }
        auto parentCode = cf.decodeAttr(*codeSlot);
        if (parentCode.is_err()) {
            return Err(OutlineError::LowerFailed);
        }
        auto* codeAttr = static_cast<jbc::core::CodeAttr*>(parentCode.unwrap());

        if (!lntBacks.empty()) {
            auto lnt = std::make_unique<jbc::core::LineNumberTableAttr>();
            for (const LntRowMove& m : lntBacks) {
                if (m.bodyIndex >= rIds.size()) {
                    return Err(OutlineError::MalformedPlan);
                }
                jbc::core::LineNumberTableAttr::Row row;
                if (!pcOf(rIds[m.bodyIndex], row.startPc)) {
                    return Err(OutlineError::LowerFailed);
                }
                row.line = m.line;
                lnt->rows.push_back(row);
            }
            codeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(
                session.internUtf8("LineNumberTable"), std::move(lnt)));
        }
        for (const bool typeTable : {false, true}) {
            auto lvt = std::make_unique<jbc::core::LocalVariableTableAttr>();
            bool any = false;
            for (const LvtRowMove& m : lvtBacks) {
                if (m.typeTable != typeTable) {
                    continue;
                }
                if (m.bodyIndex >= rIds.size()) {
                    return Err(OutlineError::MalformedPlan);
                }
                jbc::core::LocalVariableTableAttr::Row row;
                if (!pcOf(rIds[m.bodyIndex], row.startPc)) {
                    return Err(OutlineError::LowerFailed);
                }
                row.length = rEnd > row.startPc ? rEnd - row.startPc : 0u;
                row.nameCp = m.nameCp;
                row.descCp = m.descCp;
                row.slot   = m.newSlot;
                lvt->rows.push_back(row);
                any = true;
            }
            if (any) {
                codeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(
                    session.internUtf8(typeTable ? "LocalVariableTypeTable"
                                                 : "LocalVariableTable"),
                    std::move(lvt)));
            }
        }
        (void)codeSlot->mutate();
    }

    // ---- 步骤 6：删除 N（outline 建序追加在 methods 尾，删除不扰动在先身份）----
    if (session.removeMethod(callee).is_err()) {
        return Err(OutlineError::EditFailed);
    }

    return Ok(InlineOutcome{});
}

}   // namespace mjbase::asm_sem
