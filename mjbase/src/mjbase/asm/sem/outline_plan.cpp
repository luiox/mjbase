/// @file        outline_plan.cpp
/// @brief       OutlinePlan 识别（planOutline）与落地（apply）。
///
/// 判据实现对照 luiox/mjbase #115 §1.1/§3.2/§3.3 与头注 v1 口径。出口侧
/// 帧与切面事实一律取区域末条 goto 的到达帧——区域路径独占、未经出口锚
/// 汇合点合并；出口锚本身的帧是多路径汇合结果，不做切面依据。
#include "mjbase/asm/sem/outline_plan.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "jbc/analysis/frame.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/method_editor.hpp"

#include "mjbase/asm/insn/insn_op.hpp"
#include "mjbase/asm/sem/frame_view.hpp"
#include "mjbase/asm/sem/liveness_view.hpp"
#include "mjbase/asm/sem/slot_provenance.hpp"

namespace mjbase::asm_sem {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

using jbc::core::Op;

/// 描述符首字符的槽位宽（J/D = 2）。
ca::u32 descSlots(char head) noexcept
{
    return (head == 'J' || head == 'D') ? 2u : 1u;
}

/// VType → 描述符片段。v1 边界语义：Null 升格 Object（入参安全——直线体
/// 无汇合点，Object 比 Null 严格，不引入新的合并行为）；Uninitialized/
/// ReturnAddress/Top/Bottom 不可跨边界，返回 Err 由调用方映射为对应判据码。
Result<std::string, OutlineError> descOf(const jbc::analysis::VType& v,
                                         OutlineError uninitCode,
                                         OutlineError unknownCode)
{
    using jbc::analysis::VKind;
    switch (v.k) {
    case VKind::Int: return Ok(std::string("I"));
    case VKind::Long: return Ok(std::string("J"));
    case VKind::Float: return Ok(std::string("F"));
    case VKind::Double: return Ok(std::string("D"));
    case VKind::Null: return Ok(std::string("Ljava/lang/Object;"));
    case VKind::Ref: {
        if (v.name.empty()) {
            return Err(unknownCode);   /// Strict Ref 必带名；缺失 = 帧不自洽
        }
        const std::string name(v.name);
        if (name[0] == '[') {
            return Ok(name);           /// 数组型内部名即描述符形态
        }
        return Ok("L" + name + ";");
    }
    case VKind::UninitThis:
    case VKind::Uninitialized:
        return Err(uninitCode);
    case VKind::ReturnAddress:
    case VKind::Top:
    case VKind::Bottom:
    default:
        return Err(unknownCode);
    }
}

/// 值类型族：装载/返回指令的 opcode 选择轴。
enum class ValueFamily : ca::u8 { Int, Long, Float, Double, Ref };

ValueFamily familyOfDesc(char head) noexcept
{
    switch (head) {
    case 'J': return ValueFamily::Long;
    case 'F': return ValueFamily::Float;
    case 'D': return ValueFamily::Double;
    case 'L':
    case '[': return ValueFamily::Ref;
    default:  return ValueFamily::Int;   /// I/Z/C/S/B
    }
}

Op loadOpOf(ValueFamily f) noexcept
{
    switch (f) {
    case ValueFamily::Long:   return Op::LLOAD;
    case ValueFamily::Float:  return Op::FLOAD;
    case ValueFamily::Double: return Op::DLOAD;
    case ValueFamily::Ref:    return Op::ALOAD;
    default:                  return Op::ILOAD;
    }
}

Op returnOpOf(ValueFamily f) noexcept
{
    switch (f) {
    case ValueFamily::Long:   return Op::LRETURN;
    case ValueFamily::Float:  return Op::FRETURN;
    case ValueFamily::Double: return Op::DRETURN;
    case ValueFamily::Ref:    return Op::ARETURN;
    default:                  return Op::IRETURN;
    }
}

/// 紧凑 load/store 形态 → 显式形态（重编号后统一显式编码，a = 新槽号）。
Op explicitLocalOp(Op op) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(op);
    if (v >= static_cast<ca::u8>(Op::ILOAD_0) && v <= static_cast<ca::u8>(Op::ILOAD_3)) return Op::ILOAD;
    if (v >= static_cast<ca::u8>(Op::LLOAD_0) && v <= static_cast<ca::u8>(Op::LLOAD_3)) return Op::LLOAD;
    if (v >= static_cast<ca::u8>(Op::FLOAD_0) && v <= static_cast<ca::u8>(Op::FLOAD_3)) return Op::FLOAD;
    if (v >= static_cast<ca::u8>(Op::DLOAD_0) && v <= static_cast<ca::u8>(Op::DLOAD_3)) return Op::DLOAD;
    if (v >= static_cast<ca::u8>(Op::ALOAD_0) && v <= static_cast<ca::u8>(Op::ALOAD_3)) return Op::ALOAD;
    if (v >= static_cast<ca::u8>(Op::ISTORE_0) && v <= static_cast<ca::u8>(Op::ISTORE_3)) return Op::ISTORE;
    if (v >= static_cast<ca::u8>(Op::LSTORE_0) && v <= static_cast<ca::u8>(Op::LSTORE_3)) return Op::LSTORE;
    if (v >= static_cast<ca::u8>(Op::FSTORE_0) && v <= static_cast<ca::u8>(Op::FSTORE_3)) return Op::FSTORE;
    if (v >= static_cast<ca::u8>(Op::DSTORE_0) && v <= static_cast<ca::u8>(Op::DSTORE_3)) return Op::DSTORE;
    if (v >= static_cast<ca::u8>(Op::ASTORE_0) && v <= static_cast<ca::u8>(Op::ASTORE_3)) return Op::ASTORE;
    return op;
}

/// 局部操作数指令的槽号（紧凑形态按 opcode 解出，与 asm/analyzer 同口径）。
ca::u32 localOperandSlot(const jbc::ir::Insn& in) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(in.op);
    const bool compact = (v >= static_cast<ca::u8>(Op::ILOAD_0)
                          && v <= static_cast<ca::u8>(Op::ALOAD_3))
                         || (v >= static_cast<ca::u8>(Op::ISTORE_0)
                             && v <= static_cast<ca::u8>(Op::ASTORE_3));
    if (compact) {
        const ca::u8 base = v <= static_cast<ca::u8>(Op::ALOAD_3)
                                ? static_cast<ca::u8>(Op::ILOAD_0)
                                : static_cast<ca::u8>(Op::ISTORE_0);
        return static_cast<ca::u32>(v - base) % 4u;
    }
    return static_cast<ca::u32>(in.a);
}

/// goto 字节长（区域 pc 右端计算用；v1 出口只认 goto 形态）。
ca::u32 gotoByteSize(Op op) noexcept
{
    return op == Op::GOTO_W ? 5u : 3u;
}

/// 局部槽宽（区域写集槽位排布用：L/D 族占 2 槽）。
ca::u32 localOpSlotWidth(Op op) noexcept
{
    return (op == Op::LLOAD || op == Op::DLOAD || op == Op::LSTORE || op == Op::DSTORE) ? 2u : 1u;
}

/// 方法名 → 命名简单段（<init>/<clinit> 的 '<' '>' 不可入新方法名，
/// JVMS 4.6 保留形态）。
std::string simpleMethodName(std::string_view name) noexcept
{
    if (!name.empty() && name.front() == '<') {
        return "ctor";
    }
    return std::string(name);
}

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

/// 代码锚定嵌套属性归类：LNT / LVT / LVTT / SMT 可识别，其余 fail-closed。
enum class NestedKind : ca::u8 { Other, Lnt, Lvt, Lvtt, Smt };

NestedKind nestedKindOf(const jbc::core::ClassFile& cf, const jbc::core::AttrSlot& slot)
{
    if (cf.cp.utf8Equals(slot.nameCp, "LineNumberTable")) return NestedKind::Lnt;
    if (cf.cp.utf8Equals(slot.nameCp, "LocalVariableTable")) return NestedKind::Lvt;
    if (cf.cp.utf8Equals(slot.nameCp, "LocalVariableTypeTable")) return NestedKind::Lvtt;
    if (cf.cp.utf8Equals(slot.nameCp, "StackMapTable")) return NestedKind::Smt;
    return NestedKind::Other;
}

/// 值语义快照的分支族检查：直线体内禁止一切控制转移（出口 goto 已剥离）。
bool isControlTransfer(Op op) noexcept
{
    return asm_insn::isBranch(op) || asm_insn::isSwitch(op) || asm_insn::isReturn(op)
           || asm_insn::isThrow(op) || op == Op::RET;
}

}   // namespace

const char* to_string(OutlineError e) noexcept
{
    switch (e) {
    case OutlineError::Ok: return "ok";
    case OutlineError::BadRegionHandles: return "bad region handles";
    case OutlineError::ParentNoCode: return "parent has no code";
    case OutlineError::InterfaceOwner: return "interface owner unsupported";
    case OutlineError::RegionEmpty: return "empty region";
    case OutlineError::NotStraightLine: return "region not straight-line";
    case OutlineError::TryCovered: return "region covered by try (v1 closed)";
    case OutlineError::Subroutine: return "subroutine (jsr/ret)";
    case OutlineError::UninitThisLive: return "uninitThis live in region";
    case OutlineError::LastInsnNotGoto: return "last insn not goto";
    case OutlineError::EntryLabelAmbiguous: return "ambiguous entry label";
    case OutlineError::FrameFailed: return "strict frame uncomputable";
    case OutlineError::ProvenanceTruncated: return "provenance truncated";
    case OutlineError::ExitShapeImpure: return "exit shape impure";
    case OutlineError::NoNewTopValue: return "no new top value (v1 closed)";
    case OutlineError::BoundarySlotDirty: return "boundary slot provenance not unique";
    case OutlineError::RetNotProduced: return "return value not produced in region";
    case OutlineError::UninitCrossing: return "uninitialized value crosses cut";
    case OutlineError::RetNullUnsupported: return "null return unsupported (v1)";
    case OutlineError::RetTypeUnknown: return "return type unknown";
    case OutlineError::LocalTypeMissing: return "local type unresolvable";
    case OutlineError::RegionWritesLocal: return "written local live after exit";
    case OutlineError::InternalBranch: return "internal branch in region";
    case OutlineError::LvtRowCrossesCut: return "local variable row crosses cut";
    case OutlineError::UnsupportedAttr: return "code-anchored attribute unsupported";
    case OutlineError::MalformedPlan: return "malformed plan";
    case OutlineError::EditFailed: return "code edit failed";
    case OutlineError::LowerFailed: return "lower failed";
    case OutlineError::MaxesFailed: return "maxes adoption failed";
    case OutlineError::SmtFailed: return "stack map regeneration failed";
    default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// 识别侧：planOutline
// ---------------------------------------------------------------------------

Result<OutlinePlan, OutlineError> planOutline(const OutlineRequest& req)
{
    jbc::ir::ClassUnit&        cu = req.session.unit();
    const jbc::ir::MethodUnit* mu = cu.method(req.parent);
    if (mu == nullptr) {
        return Err(OutlineError::BadRegionHandles);
    }
    if ((cu.access & 0x0200u) != 0) {   /// ACC_INTERFACE：调用点须落
        return Err(OutlineError::InterfaceOwner);   /// InterfaceMethodref 面
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

    if (req.region.members.empty()) {
        return Err(OutlineError::RegionEmpty);
    }
    if (!req.region.straightLine) {
        return Err(OutlineError::NotStraightLine);
    }
    if (!req.region.handlers.empty()) {
        return Err(OutlineError::TryCovered);   /// A2 收紧：v1 只开空 handler 集
    }

    // ---- 区域成员展平（块升序 = 程序序；块间成链由 straightLine 保证）----
    std::vector<jbc::ir::InsnId> flat;
    std::vector<ca::u32>         blockSeq;   /// flat 下标 → 成员块序
    std::set<ca::u32>            memberSlots;   /// InsnId.slot 集（def 归属检查）
    for (ca::usize bi = 0; bi < req.region.members.size(); ++bi) {
        const mjbase::asm_cfg::BasicBlock* b = req.view.cfg().block(req.region.members[bi]);
        if (b == nullptr) {
            return Err(OutlineError::BadRegionHandles);
        }
        for (const jbc::ir::InsnId id : b->insns) {
            if (unit->dom.peek(id) == nullptr) {
                return Err(OutlineError::BadRegionHandles);
            }
            flat.push_back(id);
            blockSeq.push_back(static_cast<ca::u32>(bi));
            memberSlots.insert(id.slot);
        }
    }
    if (flat.empty()) {
        return Err(OutlineError::RegionEmpty);
    }
    const jbc::ir::InsnId entryInsn = flat.front();
    const jbc::ir::InsnId exitGoto  = flat.back();

    // ---- 帧与切面事实（判据 2/3/6 的输入）----
    auto builtFrames = FrameView::build(req.session.backing().cp, *unit, req.sig, req.resolver);
    if (builtFrames.is_err()) {
        return Err(OutlineError::FrameFailed);
    }
    FrameView frames = std::move(builtFrames).unwrap();
    if (frames.hasSubroutine()) {
        return Err(OutlineError::Subroutine);   /// 判据 5（语形兜底）
    }

    SlotProvenanceView prov = SlotProvenanceView::build(req.session.backing().cp, *unit, req.sig);
    if (!prov.ok()) {
        return Err(OutlineError::FrameFailed);
    }
    if (prov.truncated()) {
        /// 负向结论纪律：到达集溢出时「唯一/一致」类结论不可信，保守拒。
        return Err(OutlineError::ProvenanceTruncated);
    }

    // 判据 6：UninitThis 在区域任一成员的到达帧存活 → 该时段区域拒绝。
    for (const jbc::ir::InsnId id : flat) {
        if (frames.hasUninitThis(id)) {
            return Err(OutlineError::UninitThisLive);
        }
    }

    // ---- v1 出口形态：末条指令为指向出口锚的 goto ----
    const jbc::ir::Insn* exitInsnPtr = unit->dom.peek(exitGoto);
    if (exitInsnPtr == nullptr || exitInsnPtr->op != Op::GOTO) {
        return Err(OutlineError::LastInsnNotGoto);   /// goto_w 同拒：保守同形
    }
    const jbc::ir::LabelId exitLabel = exitInsnPtr->target;
    const mjbase::asm_cfg::BasicBlock* exitBlock = req.view.cfg().block(req.region.exitTarget);
    if (exitBlock == nullptr || !unit->dom.validLabel(exitLabel)) {
        return Err(OutlineError::BadRegionHandles);
    }
    auto resolved = unit->dom.resolveLabel(exitLabel);
    if (resolved.is_err() || resolved.unwrap().insn != exitBlock->entry) {
        return Err(OutlineError::LastInsnNotGoto);   /// goto 未指向候选出口锚
    }

    // ---- 区域头标签形态：0 个（fallthrough 入边）或 1 个（分支入边）----
    OutlinePlan plan;
    const std::vector<jbc::ir::LabelId> entryLabels = unit->dom.labelsAt(entryInsn);
    if (entryLabels.size() > 1) {
        return Err(OutlineError::EntryLabelAmbiguous);
    }
    if (entryLabels.size() == 1) {
        plan.entryLabel = entryLabels[0];
    }

    // ---- 布局 pc 面：成员序连续性 + pc 映射（LVT/LNT 归属分类的坐标基）----
    const std::vector<jbc::ir::InsnId>& order = unit->dom.order();
    std::unordered_map<ca::u32, ca::usize> orderIdxOf;   /// InsnId.slot → 程序序
    for (ca::usize i = 0; i < order.size(); ++i) {
        orderIdxOf.emplace(order[i].slot, i);
    }
    auto rawExported = unit->dom.exportRaw();
    if (rawExported.is_err()) {
        return Err(OutlineError::BadRegionHandles);
    }
    const std::vector<jbc::core::RawInsn> raw = std::move(rawExported).unwrap();
    if (raw.size() != order.size()) {
        return Err(OutlineError::BadRegionHandles);   /// 布局导出失配（防御）
    }

    std::vector<ca::usize> memberIdx;
    memberIdx.reserve(flat.size());
    for (const jbc::ir::InsnId id : flat) {
        const auto it = orderIdxOf.find(id.slot);
        if (it == orderIdxOf.end()) {
            return Err(OutlineError::BadRegionHandles);
        }
        memberIdx.push_back(it->second);
    }
    for (ca::usize i = 1; i < memberIdx.size(); ++i) {
        if (memberIdx[i] != memberIdx[i - 1] + 1) {
            /// pc 跨距混入非成员块（goto glue 链形态）：v1 拒绝，成员 pc 区间
            /// 的连续性是 LVT/LNT 行归属分类的坐标前提。
            return Err(OutlineError::NotStraightLine);
        }
    }
    const ca::u32 firstPc     = raw[memberIdx.front()].pc;
    const ca::u32 regionEndPc = raw[memberIdx.back()].pc + gotoByteSize(Op::GOTO);

    // ---- 判据 2：栈切面干净（公共前缀 + 恰 1 个区域内产出的新栈顶值）----
    const ca::u32 dIn  = frames.stackValueCount(entryInsn);
    const ca::u32 dOut = frames.stackValueCount(exitGoto);
    if (dOut == 0) {
        return Err(OutlineError::ExitShapeImpure);   /// 出口须恰 1 个新值
    }
    ca::u32 prefix = 0;
    while (prefix < dIn && prefix < dOut) {
        const ca::u32 inTop  = dIn - 1u - prefix;   /// 自底向上第 prefix 值
        const ca::u32 outTop = dOut - 1u - prefix;
        const jbc::analysis::VType* vin  = frames.stackFromTop(entryInsn, inTop);
        const jbc::analysis::VType* vout = frames.stackFromTop(exitGoto, outTop);
        if (vin == nullptr || vout == nullptr || !vin->sameAs(*vout)) {
            break;
        }
        if (prov.stackDefsAt(entryInsn, inTop) != prov.stackDefsAt(exitGoto, outTop)) {
            break;
        }
        ++prefix;
    }
    if (dOut == prefix) {
        return Err(OutlineError::NoNewTopValue);     /// M == k：v1 不开档
    }
    if (dOut != prefix + 1u) {
        return Err(OutlineError::ExitShapeImpure);
    }

    // 入口栈切片 = 公共前缀之上的后缀（自底向上 → 描述符参数序）。
    for (ca::u32 i = prefix; i < dIn; ++i) {
        const ca::u32 fromTop = dIn - 1u - i;
        if (!prov.stackSlotClean(entryInsn, fromTop)) {
            return Err(OutlineError::BoundarySlotDirty);
        }
        const jbc::analysis::VType* v = frames.stackFromTop(entryInsn, fromTop);
        if (v == nullptr) {
            return Err(OutlineError::ExitShapeImpure);
        }
        auto desc = descOf(*v, OutlineError::UninitCrossing, OutlineError::ExitShapeImpure);
        if (desc.is_err()) {
            return Err(desc.unwrap_err());
        }
        SlotCopy p;
        p.kind    = SlotCopy::Kind::Stack;
        p.fromTop = fromTop;
        p.desc    = plan.arena.intern(std::string_view(desc.unwrap()));
        plan.params.push_back(std::move(p));
    }
    const ca::usize stackParamCount = plan.params.size();   /// 栈参数段长（§3.4
                                                            /// 升格插位用）

    // ---- 判据 3（A3 随 LivenessView 放开）：写回集 = 被写 ∩ 出口后活跃
    //      须为空；读集剔除「恒被重写」槽（读的到达定义全在区域内）----
    auto builtLive = LivenessView::build(*unit);
    if (builtLive.is_err()) {
        return Err(OutlineError::FrameFailed);   /// CFG 不可算：活跃事实缺失
    }
    const LivenessView live = std::move(builtLive).unwrap();

    std::set<ca::u32> readLocals;
    std::set<ca::u32> writtenLocals;
    std::unordered_map<ca::u32, ca::u32> slotWidth;   /// 写集槽宽（cat2 占 2）
    for (const mjbase::asm_analyzer::LocalAccess& access : prov.localAccesses()) {
        if (memberSlots.count(access.insn.slot) == 0u) {
            continue;
        }
        const jbc::ir::Insn* accessInsn = unit->dom.peek(access.insn);
        if (access.kind == mjbase::asm_analyzer::LocalAccess::Kind::Write) {
            writtenLocals.insert(access.local);
            if (accessInsn != nullptr) {
                ca::u32& w = slotWidth[access.local];
                w = std::max(w, localOpSlotWidth(accessInsn->op));
            }
        } else {
            readLocals.insert(access.local);
        }
    }
    for (const ca::u32 slot : writtenLocals) {
        if (live.localLiveAfter(exitGoto, slot)) {
            /// 写回集非空：被写槽出口后仍被读，需多值返回通道（§6 决策点 1
            /// 维持拒绝）。
            return Err(OutlineError::RegionWritesLocal);
        }
    }
    /// 读集剔除（def-use 恒等搬运判据）：某槽在区域内的全部读访问的到达
    /// 定义都落在区域内（先写后读，入口值不可达）→ 不进参数集；到达集
    /// 溢出时负向结论不可信，保守保留参数。
    std::set<ca::u32> paramLocals;
    for (const ca::u32 slot : readLocals) {
        bool entryReaches = false;
        for (const mjbase::asm_analyzer::LocalAccess& access : prov.localAccesses()) {
            if (access.local != slot
                || access.kind != mjbase::asm_analyzer::LocalAccess::Kind::Read
                || memberSlots.count(access.insn.slot) == 0u) {
                continue;
            }
            if (access.defs.truncated()) {
                entryReaches = true;
                break;
            }
            for (ca::u32 di = 0; di < access.defs.size(); ++di) {
                const mjbase::asm_analyzer::DefId d = access.defs.at(di);
                if (d.kind == mjbase::asm_analyzer::DefId::Kind::Parameter
                    || (d.kind == mjbase::asm_analyzer::DefId::Kind::Instruction
                        && memberSlots.count(d.id) == 0u)) {
                    entryReaches = true;
                    break;
                }
            }
            if (entryReaches) {
                break;
            }
        }
        if (entryReaches) {
            paramLocals.insert(slot);
        }
    }
    for (const ca::u32 slot : paramLocals) {
        const jbc::analysis::VType* v = frames.localAt(entryInsn, slot);
        if (v == nullptr) {
            return Err(OutlineError::LocalTypeMissing);
        }
        auto desc = descOf(*v, OutlineError::UninitCrossing, OutlineError::LocalTypeMissing);
        if (desc.is_err()) {
            return Err(desc.unwrap_err());
        }
        SlotCopy p;
        p.kind     = SlotCopy::Kind::Local;
        p.oldLocal = slot;
        p.desc     = plan.arena.intern(std::string_view(desc.unwrap()));
        plan.params.push_back(std::move(p));
    }

    // ---- 返回值 = 出口栈顶 1 槽：干净 + 区域内产出 + 类型可跨界 ----
    if (!prov.stackSlotClean(exitGoto, 0)) {
        return Err(OutlineError::BoundarySlotDirty);
    }
    const jbc::analysis::VType* retv = frames.stackFromTop(exitGoto, 0);
    if (retv == nullptr) {
        return Err(OutlineError::ExitShapeImpure);
    }
    if (retv->k == jbc::analysis::VKind::Null) {
        return Err(OutlineError::RetNullUnsupported);
    }
    const mjbase::asm_analyzer::DefSet retDefs = prov.stackDefsAt(exitGoto, 0);
    bool produced = false;
    if (retDefs.size() == 1u
        && retDefs.at(0).kind == mjbase::asm_analyzer::DefId::Kind::Instruction) {
        const jbc::ir::InsnId* defInsn = prov.instructionOf(retDefs.at(0));
        produced = defInsn != nullptr && memberSlots.count(defInsn->slot) != 0u;
    }
    if (!produced && !retDefs.empty() && !retDefs.truncated()) {
        /// 区域内 load 推入的局部原值形态（def 落在区域外：入参 Parameter 或
        /// 区域前 store）：原值来自 copy-set 读集局部（区域内 load 的产出）且
        /// 不是入口栈上已有值（同原值已驻栈 = 边界 DUP 复制形态，§3.4）时，
        /// 视为区域内产出的新栈值；否则 fail-closed。
        bool fromReadLocal = false;
        for (const ca::u32 slot : readLocals) {
            if (prov.localDefsAt(entryInsn, slot) == retDefs) {
                fromReadLocal = true;
                break;
            }
        }
        bool onEntryStack = false;
        for (ca::u32 i = 0; i < dIn; ++i) {
            if (prov.stackDefsAt(entryInsn, i) == retDefs) {
                onEntryStack = true;
                break;
            }
        }
        produced = fromReadLocal && !onEntryStack;
    }
    if (!produced && retDefs.size() == 1u && !retDefs.truncated() && prefix >= 1u
        && prefix == dIn && flat.size() >= 2u) {
        /// §3.4 落临时档（A3，tier-2）：出口栈顶 = 区域末条 DUP/DUP2（cat2
        /// 形态）对入口栈顶原值的复制——「一条 def 占多槽」跨出口切点。原值
        /// 升格 Stack 入参（公共前缀收缩 1 值），调用点 DUP 复位：副本作
        /// 实参、原值留栈下、返回值落顶插回原位，N 体零改写。dup_x1/SWAP
        /// 族跨切点（复制落栈中段）出口形状结构性不可表达，维持拒绝。
        const jbc::ir::InsnId tailId  = flat[flat.size() - 2u];
        const jbc::ir::Insn* tailInsn = unit->dom.peek(tailId);
        if (tailInsn != nullptr && (tailInsn->op == Op::DUP || tailInsn->op == Op::DUP2)) {
            const mjbase::asm_analyzer::DefSet entryTopDefs = prov.stackDefsAt(entryInsn, 0);
            bool copiedByTail = false;
            for (const mjbase::asm_analyzer::DefUseEdge* e : prov.usesOf(retDefs.at(0))) {
                if (e->use.slot == tailId.slot
                    && e->slot == mjbase::asm_analyzer::DefUseSlot::Stack
                    && e->index == 0u) {
                    copiedByTail = true;
                    break;
                }
            }
            if (copiedByTail && entryTopDefs == retDefs) {
                const jbc::analysis::VType* srcv = frames.stackFromTop(entryInsn, 0);
                if (srcv != nullptr) {
                    auto srcDesc =
                        descOf(*srcv, OutlineError::UninitCrossing, OutlineError::RetTypeUnknown);
                    if (srcDesc.is_err()) {
                        return Err(srcDesc.unwrap_err());
                    }
                    SlotCopy p;
                    p.kind    = SlotCopy::Kind::Stack;
                    p.fromTop = 0;
                    p.desc    = plan.arena.intern(std::string_view(srcDesc.unwrap()));
                    plan.params.insert(plan.params.begin() + static_cast<ca::usize>(stackParamCount),
                                       std::move(p));
                    plan.retIsEntryCopy = true;
                    produced            = true;
                }
            }
        }
    }
    if (!produced) {
        return Err(OutlineError::RetNotProduced);   /// 边界 DUP 复制值形态（tier-3）
    }
    auto retDesc = descOf(*retv, OutlineError::UninitCrossing, OutlineError::RetTypeUnknown);
    if (retDesc.is_err()) {
        return Err(retDesc.unwrap_err());
    }

    // ---- 槽位重排：参数区（栈切片自底向上 + 局部升序）从 0 起，cat2 占 2 槽 ----
    ca::u32 slotCursor = 0;
    for (SlotCopy& p : plan.params) {
        p.newSlot = slotCursor;
        slotCursor += descSlots(std::string_view(p.desc)[0]);
    }
    for (const SlotCopy& p : plan.params) {
        if (p.kind == SlotCopy::Kind::Local) {
            plan.locals.push_back(LocalRemap{p.oldLocal, p.newSlot});
        }
    }
    /// 区域内自足槽（先写后读/恒被重写/纯写，不传参）：排参数区之后，宽度
    /// 按区域写指令的 cat2 形态（N 内 store 占 2 槽）。
    for (const ca::u32 slot : writtenLocals) {
        bool mapped = false;
        for (const LocalRemap& r : plan.locals) {
            if (r.oldSlot == slot) {
                mapped = true;
                break;
            }
        }
        if (mapped) {
            continue;
        }
        ca::u32 w = 1;
        const auto wit = slotWidth.find(slot);
        if (wit != slotWidth.end()) {
            w = std::max(w, wit->second);
        }
        plan.locals.push_back(LocalRemap{slot, slotCursor});
        slotCursor += w;
    }

    plan.hasRet   = true;
    plan.ret.kind    = SlotCopy::Kind::Stack;
    plan.ret.fromTop = 0;
    plan.ret.desc    = plan.arena.intern(std::string_view(retDesc.unwrap()));

    // ---- 方法体值快照：程序序拷贝，局部操作数按重编号表改写为显式形态；
    //      出口 goto 与块间 goto glue 剥离，其余控制转移 fail-closed ----
    std::vector<ca::i32> orderToBody(order.size(), -1);
    /// 栈切片参数序幕：descriptor 参数落在 N 的局部槽，而区域体从栈上消费
    /// 它们（S_in）——先按参数序（自下而上）显式装载，区域体入口栈形与
    /// 原入口切面一致（#115 §3.3 步骤 3 的 Stack 参数承载面）。
    for (const SlotCopy& p : plan.params) {
        if (p.kind != SlotCopy::Kind::Stack) {
            continue;
        }
        jbc::ir::Insn load;
        load.op = loadOpOf(familyOfDesc(std::string_view(p.desc)[0]));
        load.a  = static_cast<ca::i32>(p.newSlot);
        plan.body.push_back(load);
    }
    for (ca::usize p = 0; p < flat.size(); ++p) {
        const jbc::ir::Insn* in = unit->dom.peek(flat[p]);
        if (in == nullptr) {
            return Err(OutlineError::BadRegionHandles);
        }
        const bool lastOfBlock = (p + 1u == flat.size()) || (blockSeq[p + 1u] != blockSeq[p]);
        if (asm_insn::isGoto(in->op)) {
            if (!lastOfBlock) {
                return Err(OutlineError::InternalBranch);
            }
            if (p + 1u == flat.size()) {
                continue;   /// 出口 goto：apply 剥离（转发由调用点 goto 承担）
            }
            auto glue = unit->dom.resolveLabel(in->target);
            if (glue.is_err() || glue.unwrap().insn != flat[p + 1u]) {
                return Err(OutlineError::InternalBranch);   /// 非「跳到下一成员」
            }
            continue;   /// 块间 glue：N 内 fallthrough 承担，剥离
        }
        if (isControlTransfer(in->op)) {
            return Err(OutlineError::InternalBranch);
        }

        jbc::ir::Insn copy = *in;
        if (asm_insn::isLoad(copy.op) || asm_insn::isStore(copy.op) || copy.op == Op::IINC) {
            const ca::u32 oldSlot = localOperandSlot(copy);
            bool mapped = false;
            for (const LocalRemap& r : plan.locals) {
                if (r.oldSlot == oldSlot) {
                    copy.op = explicitLocalOp(copy.op);
                    copy.a  = static_cast<ca::i32>(r.newSlot);
                    mapped  = true;
                    break;
                }
            }
            if (!mapped) {
                return Err(OutlineError::MalformedPlan);   /// 读集与体不一致（防御）
            }
        }
        orderToBody[memberIdx[p]] = static_cast<ca::i32>(plan.body.size());
        plan.body.push_back(copy);
    }
    if (plan.body.empty()) {
        return Err(OutlineError::NoNewTopValue);   /// 纯 goto 区域无新值可返
    }

    // ---- 命名：outline$<原简单名>$<序号>（确定性方案，owner 拍板前占位）----
    const std::string parentName(cu.symbols().str(mu->name));
    plan.newName = plan.arena.intern(std::string_view(
        "outline$" + simpleMethodName(parentName) + "$" + std::to_string(req.sequence)));
    plan.parent = req.parent;

    // ---- LNT/LVT/LVTT 行归属拆分（先于描述符：行分类可能 fail-closed）----
    jbc::core::ClassFile& cf = req.session.backing();
    jbc::core::AttrSlot*  codeSlot =
        findAttr(cf, cf.methods[mu->backingIndex].attrs, "Code");
    if (codeSlot == nullptr) {
        return Err(OutlineError::ParentNoCode);
    }
    auto decoded = cf.decodeAttr(*codeSlot);
    if (decoded.is_err()) {
        return Err(OutlineError::UnsupportedAttr);
    }
    auto* codeAttr = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
    for (jbc::core::AttrSlot& nested : codeAttr->attrs) {
        const NestedKind kind = nestedKindOf(cf, nested);
        if (kind == NestedKind::Smt) {
            continue;   /// apply 期整表再生，识别侧无需行分类
        }
        if (kind == NestedKind::Other) {
            /// TypeAnnotations 族与未知嵌套属性：pc 锚定面未建模，保守拒。
            return Err(OutlineError::UnsupportedAttr);
        }
        auto nestedBody = cf.decodeAttr(nested);
        if (nestedBody.is_err()) {
            return Err(OutlineError::UnsupportedAttr);
        }
        if (kind == NestedKind::Lnt) {
            const auto* lnt = static_cast<jbc::core::LineNumberTableAttr*>(nestedBody.unwrap());
            for (const jbc::core::LineNumberTableAttr::Row& row : lnt->rows) {
                if (row.startPc < firstPc || row.startPc >= regionEndPc) {
                    continue;   /// 区域外：父表保留，lower pcmap 接管
                }
                LntRowMove move;
                move.startPc = row.startPc;
                move.line    = row.line;
                plan.lntMoves.push_back(move);
            }
        } else {
            auto* lvt = static_cast<jbc::core::LocalVariableTableAttr*>(nestedBody.unwrap());
            for (const jbc::core::LocalVariableTableAttr::Row& row : lvt->rows) {
                const ca::u32 rowEnd = row.startPc + row.length;
                /// 端点口径：两端点均存活（区域外）→ 父表保留（lower 按
                /// fail-closed 终点规则重定向，行语义保持诚实——javac 全方法
                /// 参数行即此形态）；两端点均在区域内 → 迁 N 重锚定；恰一
                /// 端点落入区域（真越切点）→ 默认 fail-closed，宽松开关下
                /// 裁剪（父表删除、记 lvtDrops 诊断；#115 §6 决策点 3）。
                const bool startIn = row.startPc >= firstPc && row.startPc < regionEndPc;
                const bool endIn   = rowEnd > firstPc && rowEnd <= regionEndPc;
                if (!startIn && !endIn) {
                    continue;
                }
                if (!startIn || !endIn) {
                    if (!req.allowLvtCrossCut) {
                        return Err(OutlineError::LvtRowCrossesCut);   /// 越切点 fail-closed
                    }
                    LvtRowDrop drop;
                    drop.startPc   = row.startPc;
                    drop.length    = row.length;
                    drop.nameCp    = row.nameCp;
                    drop.descCp    = row.descCp;
                    drop.typeTable = (kind == NestedKind::Lvtt);
                    drop.slot      = row.slot;
                    plan.lvtDrops.push_back(drop);
                    continue;
                }
                LvtRowMove move;
                move.startPc  = row.startPc;
                move.length   = row.length;
                move.nameCp   = row.nameCp;
                move.descCp   = row.descCp;
                move.typeTable = (kind == NestedKind::Lvtt);
                move.oldSlot  = row.slot;
                bool slotMapped = false;
                for (const LocalRemap& r : plan.locals) {
                    if (r.oldSlot == row.slot) {
                        move.newSlot = r.newSlot;
                        slotMapped   = true;
                        break;
                    }
                }
                if (!slotMapped) {
                    /// 行槽不在区域读写集内（槽未入重编号表）：fail-closed。
                    return Err(OutlineError::LvtRowCrossesCut);
                }
                plan.lvtMoves.push_back(move);
            }
        }
    }
    /// 迁移行的 N 体锚点：按行 startPc 定位成员指令 → 体序；锚在已剥离的
    /// goto 上（无体映像）按 fail-closed 拒绝。
    for (LntRowMove& move : plan.lntMoves) {
        bool anchored = false;
        for (ca::usize i = 0; i < memberIdx.size(); ++i) {
            if (raw[memberIdx[i]].pc == move.startPc) {
                if (orderToBody[memberIdx[i]] < 0) {
                    return Err(OutlineError::LvtRowCrossesCut);
                }
                move.bodyIndex = static_cast<ca::u32>(orderToBody[memberIdx[i]]);
                anchored       = true;
                break;
            }
        }
        if (!anchored) {
            return Err(OutlineError::LvtRowCrossesCut);
        }
    }
    for (LvtRowMove& move : plan.lvtMoves) {
        bool anchored = false;
        for (ca::usize i = 0; i < memberIdx.size(); ++i) {
            if (raw[memberIdx[i]].pc == move.startPc) {
                if (orderToBody[memberIdx[i]] < 0) {
                    return Err(OutlineError::LvtRowCrossesCut);
                }
                move.bodyIndex = static_cast<ca::u32>(orderToBody[memberIdx[i]]);
                anchored       = true;
                break;
            }
        }
        if (!anchored) {
            return Err(OutlineError::LvtRowCrossesCut);
        }
    }

    // ---- 描述符与方法级代码锚定属性 ----
    {
        std::string desc = "(";
        for (const SlotCopy& p : plan.params) {
            desc += std::string_view(p.desc);
        }
        desc += ")";
        desc += std::string_view(plan.ret.desc);
        plan.newDesc = plan.arena.intern(std::string_view(desc));
    }
    for (const jbc::core::AttrSlot& slot : cf.methods[mu->backingIndex].attrs) {
        /// 方法级 TypeAnnotations 携带代码偏移目标（localvar/offset 族），
        /// 变换后重定位面未建模，保守拒。
        if (cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleTypeAnnotations")
            || cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleTypeAnnotations")) {
            return Err(OutlineError::UnsupportedAttr);
        }
    }

    // ---- 边界锚 ----
    plan.entryInsn = entryInsn;
    plan.exitGoto  = exitGoto;
    plan.exitLabel = exitLabel;
    plan.members   = flat;
    return Ok(std::move(plan));
}

// ---------------------------------------------------------------------------
// 数据侧：validate
// ---------------------------------------------------------------------------

Result<void, OutlineError> OutlinePlan::validate() const
{
    if (std::string_view(newName).empty() || std::string_view(newDesc).empty() || body.empty()) {
        return Err(OutlineError::MalformedPlan);
    }
    if (!hasRet || std::string_view(ret.desc).empty()) {
        return Err(OutlineError::MalformedPlan);
    }
    const std::string_view descView(newDesc);
    if (descView.empty() || descView.front() != '('
        || descView.find(')') == std::string_view::npos) {
        return Err(OutlineError::MalformedPlan);
    }
    ca::u32 cursor = 0;
    for (const SlotCopy& p : params) {
        const std::string_view d(p.desc);
        if (d.empty() || p.newSlot != cursor) {
            return Err(OutlineError::MalformedPlan);
        }
        cursor += descSlots(d[0]);
    }
    std::set<ca::u32> remapped;
    for (const LocalRemap& r : locals) {
        if (!remapped.insert(r.oldSlot).second) {
            return Err(OutlineError::MalformedPlan);
        }
    }
    /// 体局部操作数的合法槽位 = 参数槽（栈切片序幕装载 + 局部入参）∪
    /// 区域自足槽（重编号表）。
    std::set<ca::u32> bodySlots;
    for (const SlotCopy& p : params) {
        bodySlots.insert(p.newSlot);
    }
    for (const LocalRemap& r : locals) {
        bodySlots.insert(r.newSlot);
    }
    for (const jbc::ir::Insn& in : body) {
        if (isControlTransfer(in.op)) {
            return Err(OutlineError::MalformedPlan);
        }
        if (asm_insn::isLoad(in.op) || asm_insn::isStore(in.op) || in.op == Op::IINC) {
            /// 体载荷已是重映射后的新槽（planOutline 构建期改写）。
            if (bodySlots.count(static_cast<ca::u32>(in.a)) == 0u) {
                return Err(OutlineError::MalformedPlan);
            }
        }
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// 落地侧：apply
// ---------------------------------------------------------------------------

Result<OutlineOutcome, OutlineError> OutlinePlan::apply(jbc::ir::ClassSession& session, jbc::analysis::ITypeResolver& resolver) const
{
    auto checked = validate();
    if (checked.is_err()) {
        return Err(checked.unwrap_err());
    }

    jbc::ir::ClassUnit&  cu = session.unit();
    jbc::ir::MethodUnit* mu = cu.method(parent);
    if (mu == nullptr) {
        return Err(OutlineError::BadRegionHandles);
    }
    jbc::ir::CodeUnit* unit = mu->code.get();
    if (unit == nullptr) {
        auto promoted = jbc::ir::promoteMethod(session, parent);
        if (promoted.is_err()) {
            return Err(OutlineError::ParentNoCode);
        }
        unit = mu->code.get();
    }
    if (unit == nullptr || unit->dom.peek(entryInsn) == nullptr
        || unit->dom.peek(exitGoto) == nullptr || !unit->dom.validLabel(exitLabel)) {
        return Err(OutlineError::BadRegionHandles);
    }
    for (const jbc::ir::InsnId id : members) {
        if (unit->dom.peek(id) == nullptr) {
            return Err(OutlineError::BadRegionHandles);
        }
    }

    const std::string_view thisClass = cu.symbols().str(cu.thisClass);

    // ---- 步骤 1：建合成方法 N（体 append + 返回指令；直线体无标签需求）----
    ca::u32 paramSlots = 0;
    for (const SlotCopy& p : params) {
        paramSlots += descSlots(std::string_view(p.desc)[0]);
    }
    jbc::ir::MethodSeed seed;
    seed.name      = std::string_view(newName);
    seed.desc      = std::string_view(newDesc);
    seed.access    = newAccess;
    seed.withCode  = true;
    seed.maxStack  = 2;
    {
        /// 种子 maxLocals：参数区 + 区域自足槽（含 cat2 高半槽余量；最终由
        /// applyComputedMaxes 收口）。
        ca::u32 nLocals = paramSlots == 0u ? 1u : paramSlots;
        for (const LocalRemap& r : locals) {
            nLocals = std::max(nLocals, r.newSlot + 2u);
        }
        seed.maxLocals = static_cast<ca::u16>(nLocals);
    }
    auto opened    = jbc::ir::MethodEditor::open(session, seed);
    if (opened.is_err()) {
        const jbc::ir::MethodBuildError e = opened.unwrap_err();
        return Err(e.stage == jbc::ir::MethodBuildError::Stage::Edit
                       ? OutlineError::EditFailed
                       : OutlineError::LowerFailed);
    }
    jbc::ir::MethodEditor editor = std::move(opened).unwrap();
    for (const jbc::ir::Insn& in : body) {
        if (editor.append(in).is_err()) {
            return Err(OutlineError::EditFailed);
        }
    }
    const jbc::core::Op retOp = returnOpOf(familyOfDesc(std::string_view(ret.desc)[0]));
    if (editor.append(jbc::ir::CodeEditor::simple(retOp)).is_err()) {
        return Err(OutlineError::EditFailed);
    }
    if (editor.commit().is_err()) {
        return Err(OutlineError::LowerFailed);
    }
    const jbc::ir::MethodId outlined = editor.method();
    jbc::ir::CodeUnit&     nUnit     = editor.unit();

    /// MethodEditor::open 已向 cu.methods 追加成员（vector 可能重排搬移元素），
    /// 此后父方法的 MethodUnit/CodeUnit 句柄一律重取，不得沿用开场指针。
    mu = cu.method(parent);
    if (mu == nullptr) {
        return Err(OutlineError::BadRegionHandles);
    }
    unit = mu->code.get();

    // ---- 步骤 2：调用点替换（边界 DUP 复位 + 局部入参装载 + invokestatic
    //      + 转发 goto）----
    //
    // 入边口径（A3 修复）：goto 入边形态下参数装载块必须随入边走——标签
    // 锚定首条装载（或 retIsEntryCopy 的 DUP），分支入边与 fallthrough 都
    // 先过装载再达调用点。A2 把标签重锚到 invokestatic、装载落在标签之前，
    // 入边绕过装载 → JVM VerifyError「Inconsistent stack height」（#115
    // §3.3 实测缺陷，DupNorm 语料复现）。insertAtLabel 语义（标签恒指新
    // 插入指令、插入点在标签边界）下按执行序逆序逐条插入，终序 = 参数序。
    const ca::u32 callCp = session.internMethodRef(
        thisClass, std::string_view(newName), std::string_view(newDesc));
    jbc::ir::Insn call;
    call.op = jbc::core::Op::INVOKESTATIC;
    call.cp = callCp;

    jbc::ir::CodeEditor ed{*unit};
    jbc::ir::InsnId     callId = jbc::ir::kNoInsn;
    bool anyLocalParams = false;
    for (const SlotCopy& p : params) {
        if (p.kind == SlotCopy::Kind::Local) {
            anyLocalParams = true;
            break;
        }
    }
    const bool emitEntryDup = retIsEntryCopy && !body.empty()
                              && (body.back().op == jbc::core::Op::DUP
                                  || body.back().op == jbc::core::Op::DUP2);
    if (unit->dom.validLabel(entryLabel)) {
        /// 逆序插装载：每条落标签边界、标签重锚到新指令 → 终序 l1..lm。
        if (anyLocalParams) {
            for (auto it = params.rbegin(); it != params.rend(); ++it) {
                if (it->kind != SlotCopy::Kind::Local) {
                    continue;
                }
                jbc::ir::Insn load;
                load.op = loadOpOf(familyOfDesc(std::string_view(it->desc)[0]));
                load.a  = static_cast<ca::i32>(it->oldLocal);
                if (ed.insertAtLabel(entryLabel, load).is_err()) {
                    return Err(OutlineError::EditFailed);
                }
            }
        }
        if (emitEntryDup) {
            /// §3.4 tier-2：DUP 复制入口栈顶（副本作实参、原值留栈下）。
            jbc::ir::Insn dupInsn;
            dupInsn.op = body.back().op;
            if (ed.insertAtLabel(entryLabel, dupInsn).is_err()) {
                return Err(OutlineError::EditFailed);
            }
        }
        if (anyLocalParams || emitEntryDup) {
            auto ins = ed.insertBefore(entryInsn, call);
            if (ins.is_err()) {
                return Err(OutlineError::EditFailed);
            }
            callId = ins.unwrap();
        } else {
            auto ins = ed.insertAtLabel(entryLabel, call);
            if (ins.is_err()) {
                return Err(OutlineError::EditFailed);
            }
            callId = ins.unwrap();
        }
    } else {
        if (emitEntryDup) {
            jbc::ir::Insn dupInsn;
            dupInsn.op = body.back().op;
            if (ed.insertBefore(entryInsn, dupInsn).is_err()) {
                return Err(OutlineError::EditFailed);
            }
        }
        for (const SlotCopy& p : params) {
            if (p.kind != SlotCopy::Kind::Local) {
                continue;   /// 栈切片参数已恰好在栈上（S_in 自底向上 = 参数序）
            }
            jbc::ir::Insn load;
            load.op = loadOpOf(familyOfDesc(std::string_view(p.desc)[0]));
            load.a  = static_cast<ca::i32>(p.oldLocal);
            if (ed.insertBefore(entryInsn, load).is_err()) {
                return Err(OutlineError::EditFailed);
            }
        }
        auto ins = ed.insertBefore(entryInsn, call);
        if (ins.is_err()) {
            return Err(OutlineError::EditFailed);
        }
        callId = ins.unwrap();
    }
    {
        jbc::ir::Insn fwd;
        fwd.op     = jbc::core::Op::GOTO;
        fwd.target = exitLabel;   /// 复用被剥离出口 goto 的目标标签
        if (ed.insertAfter(callId, fwd).is_err()) {
            return Err(OutlineError::EditFailed);
        }
    }
    if (ed.eraseBatch(members, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed).is_err()) {
        return Err(OutlineError::EditFailed);
    }

    // ---- 步骤 3：父方法嵌套属性手术（SMT 摘除待再生；迁移行自父表移除）----
    jbc::core::ClassFile& cf  = session.backing();
    jbc::core::AttrSlot*  codeSlot = findAttr(cf, cf.methods[mu->backingIndex].attrs, "Code");
    if (codeSlot == nullptr) {
        return Err(OutlineError::LowerFailed);
    }
    {
        auto parentCode = cf.decodeAttr(*codeSlot);
        if (parentCode.is_err()) {
            return Err(OutlineError::LowerFailed);
        }
        auto* codeAttr = static_cast<jbc::core::CodeAttr*>(parentCode.unwrap());
        auto& attrs    = codeAttr->attrs;
        attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                                   [&](const jbc::core::AttrSlot& s) {
                                       return cf.cp.utf8Equals(s.nameCp, "StackMapTable");
                                   }),
                    attrs.end());
        bool removedAny = false;
        for (jbc::core::AttrSlot& nested : attrs) {
            const NestedKind kind = nestedKindOf(cf, nested);
            if (kind == NestedKind::Lnt) {
                auto body = cf.decodeAttr(nested);
                if (body.is_err()) {
                    return Err(OutlineError::LowerFailed);
                }
                auto* lnt  = static_cast<jbc::core::LineNumberTableAttr*>(body.unwrap());
                auto& rows = lnt->rows;
                const ca::usize before = rows.size();
                rows.erase(std::remove_if(rows.begin(), rows.end(),
                                          [&](const jbc::core::LineNumberTableAttr::Row& r) {
                                              for (const LntRowMove& m : lntMoves) {
                                                  if (m.startPc == r.startPc && m.line == r.line) {
                                                      return true;
                                                  }
                                              }
                                              return false;
                                          }),
                           rows.end());
                if (rows.size() != before) {
                    (void)nested.mutate();
                    removedAny = true;
                }
            } else if (kind == NestedKind::Lvt || kind == NestedKind::Lvtt) {
                auto body = cf.decodeAttr(nested);
                if (body.is_err()) {
                    return Err(OutlineError::LowerFailed);
                }
                auto* lvt  = static_cast<jbc::core::LocalVariableTableAttr*>(body.unwrap());
                auto& rows = lvt->rows;
                const ca::usize before = rows.size();
                rows.erase(std::remove_if(rows.begin(), rows.end(),
                                          [&](const jbc::core::LocalVariableTableAttr::Row& r) {
                                              for (const LvtRowMove& m : lvtMoves) {
                                                  if (m.typeTable == (kind == NestedKind::Lvtt)
                                                      && m.startPc == r.startPc && m.length == r.length
                                                      && m.nameCp == r.nameCp && m.descCp == r.descCp
                                                      && m.oldSlot == r.slot) {
                                                      return true;
                                                  }
                                              }
                                              for (const LvtRowDrop& d : lvtDrops) {
                                                  /// 宽松开关裁剪的越切点行：父表删除。
                                                  if (d.typeTable == (kind == NestedKind::Lvtt)
                                                      && d.startPc == r.startPc && d.length == r.length
                                                      && d.nameCp == r.nameCp && d.descCp == r.descCp
                                                      && d.slot == r.slot) {
                                                      return true;
                                                  }
                                              }
                                              return false;
                                          }),
                           rows.end());
                if (rows.size() != before) {
                    (void)nested.mutate();
                    removedAny = true;
                }
            }
        }
        if (removedAny) {
            (void)codeSlot->mutate();
        }
    }

    // ---- 步骤 4：父方法 lower → Strict 帧重算 → maxes 采用 → 二次 lower ----
    if (jbc::ir::lowerMethod(session, parent).is_err()) {
        return Err(OutlineError::LowerFailed);
    }
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

    // ---- 步骤 5：父方法 SMT 整表再生（调用点成新分支目标，链式重算不覆盖）----
    if ((cu.version >> 16) >= 50u) {
        auto smt = jbc::analysis::buildStackMapTable(session, *unit, parentFrames);
        if (smt.is_err()) {
            return Err(OutlineError::SmtFailed);
        }
        auto result = std::move(smt).unwrap();
        if (result.needed) {
            jbc::core::AttrSlot* fresh = findAttr(cf, cf.methods[mu->backingIndex].attrs, "Code");
            if (fresh == nullptr) {
                return Err(OutlineError::LowerFailed);
            }
            auto parentCode = cf.decodeAttr(*fresh);
            if (parentCode.is_err()) {
                return Err(OutlineError::LowerFailed);
            }
            auto* codeAttr = static_cast<jbc::core::CodeAttr*>(parentCode.unwrap());
            codeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(
                session.internUtf8("StackMapTable"),
                std::unique_ptr<jbc::core::AttrBody>(std::move(result.attr))));
            (void)fresh->mutate();
        }
    }

    // ---- 步骤 6：N 侧收尾（maxes 采用 + 二次 lower；直线体无控制流目标，
    //      SMT needed=false 跳过）----
    jbc::analysis::MethodSig sigN;
    sigN.isStatic  = true;
    sigN.desc      = std::string_view(newDesc);
    sigN.name      = std::string_view(newName);
    sigN.thisClass = thisClass;
    std::vector<jbc::analysis::FrameAt> nFrames;
    const jbc::analysis::FrameSummary   nSummary = engine.compute(nUnit, sigN, nFrames);
    if (!nSummary.ok) {
        return Err(OutlineError::FrameFailed);
    }
    if (!jbc::analysis::applyComputedMaxes(nUnit, nSummary, sigN)) {
        return Err(OutlineError::MaxesFailed);
    }
    if (jbc::ir::lowerMethod(session, outlined).is_err()) {
        return Err(OutlineError::LowerFailed);
    }
    if ((cu.version >> 16) >= 50u) {
        auto smt = jbc::analysis::buildStackMapTable(session, nUnit, nFrames);
        if (smt.is_err()) {
            return Err(OutlineError::SmtFailed);
        }
        /// needed=false（无分支目标）合法跳过；needed=true 属异常形态，同样挂回。
        auto result = std::move(smt).unwrap();
        if (result.needed) {
            jbc::ir::MethodUnit* nMu = cu.method(outlined);
            if (nMu == nullptr) {
                return Err(OutlineError::LowerFailed);
            }
            jbc::core::AttrSlot* nCode =
                findAttr(cf, cf.methods[nMu->backingIndex].attrs, "Code");
            if (nCode == nullptr) {
                return Err(OutlineError::LowerFailed);
            }
            auto nCodeBody = cf.decodeAttr(*nCode);
            if (nCodeBody.is_err()) {
                return Err(OutlineError::LowerFailed);
            }
            static_cast<jbc::core::CodeAttr*>(nCodeBody.unwrap())
                ->attrs.push_back(jbc::core::AttrSlot::makeNew(
                    session.internUtf8("StackMapTable"),
                    std::unique_ptr<jbc::core::AttrBody>(std::move(result.attr))));
            (void)nCode->mutate();
        }
    }

    // ---- 步骤 7：迁移行挂入 N（LNT 行号原样；LVT 槽重编号、区间覆盖体尾）----
    if (!lntMoves.empty() || !lvtMoves.empty()) {
        auto nRaw = nUnit.dom.exportRaw();
        if (nRaw.is_err()) {
            return Err(OutlineError::LowerFailed);
        }
        const std::vector<jbc::core::RawInsn> nPcs = std::move(nRaw).unwrap();
        jbc::ir::MethodUnit* nMu = cu.method(outlined);
        if (nMu == nullptr) {
            return Err(OutlineError::LowerFailed);
        }
        jbc::core::AttrSlot* nCode = findAttr(cf, cf.methods[nMu->backingIndex].attrs, "Code");
        if (nCode == nullptr) {
            return Err(OutlineError::LowerFailed);
        }
        auto nCodeBody = cf.decodeAttr(*nCode);
        if (nCodeBody.is_err()) {
            return Err(OutlineError::LowerFailed);
        }
        auto* nCodeAttr = static_cast<jbc::core::CodeAttr*>(nCodeBody.unwrap());
        const ca::u32 codeLen = static_cast<ca::u32>(nCodeAttr->code.size());
        const auto pcOf = [&](ca::u32 bodyIndex) -> ca::u32 {
            return bodyIndex < nPcs.size() ? nPcs[bodyIndex].pc : codeLen;
        };

        if (!lntMoves.empty()) {
            auto lnt = std::make_unique<jbc::core::LineNumberTableAttr>();
            for (const LntRowMove& m : lntMoves) {
                jbc::core::LineNumberTableAttr::Row row;
                row.startPc = pcOf(m.bodyIndex);
                row.line    = m.line;
                lnt->rows.push_back(row);
            }
            if (!lnt->rows.empty()) {
                nCodeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(
                    session.internUtf8("LineNumberTable"), std::move(lnt)));
            }
        }
        for (const bool typeTable : {false, true}) {
            auto lvt = std::make_unique<jbc::core::LocalVariableTableAttr>();
            bool any = false;
            for (const LvtRowMove& m : lvtMoves) {
                if (m.typeTable != typeTable) {
                    continue;
                }
                jbc::core::LocalVariableTableAttr::Row row;
                row.startPc = pcOf(m.bodyIndex);
                row.length  = codeLen > row.startPc ? codeLen - row.startPc : 0u;
                row.nameCp  = m.nameCp;
                row.descCp  = m.descCp;
                row.slot    = m.newSlot;
                lvt->rows.push_back(row);
                any = true;
            }
            if (any) {
                nCodeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(
                    session.internUtf8(typeTable ? "LocalVariableTypeTable"
                                                 : "LocalVariableTable"),
                    std::move(lvt)));
            }
        }
        (void)nCode->mutate();
    }

    OutlineOutcome outcome;
    outcome.outlined = outlined;
    outcome.callInsn = callId;
    return Ok(outcome);
}

}   // namespace mjbase::asm_sem
