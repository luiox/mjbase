#include "mjbase/spi/frame_rewire.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::spi {

namespace frame_rewire {

namespace {

using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;

/// 会话内方法定位；找不到即管线内部契约破坏（promote 过的方法必然在骨架）。
const jbc::ir::MethodUnit& methodById(const jbc::ir::ClassSession& s, jbc::ir::MethodId method)
{
    const jbc::ir::MethodUnit* mu = s.unit().method(method);
    if (mu == nullptr) {
        throw std::logic_error("frame_rewire: method identity not attached to session");
    }
    return *mu;
}

/// Code 属性槽定位；缺失即 NotPromoted 语义（管线保证 promote 过，异常路径
/// 只可能来自越权改动）。
AttrSlot& codeSlotOf(ClassFile& cf, const jbc::ir::MethodUnit& mu)
{
    for (AttrSlot& slot : cf.methods[mu.backingIndex].attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, "Code")) return slot;
    }
    throw std::runtime_error("frame_rewire: Code attribute missing for method '"
                             + std::string(cf.nameOf(cf.methods[mu.backingIndex])) + "'");
}

/// 存活指令旧 pc 集合（升序）。合成指令（srcPc = kNoPc）不在内。
std::vector<ca::u32> survivingPcs(const jbc::ir::CodeDom& dom)
{
    std::vector<ca::u32> pcs;
    for (const jbc::ir::InsnId id : dom.order()) {
        const jbc::ir::Insn* in = dom.peek(id);
        if (in != nullptr && in->srcPc != jbc::ir::kNoPc) pcs.push_back(in->srcPc);
    }
    std::sort(pcs.begin(), pcs.end());
    return pcs;
}

/// 严格大于 pc 的下一个存活 pc；不存在返回 false。
bool nextSurvivingPc(const std::vector<ca::u32>& pcs, ca::u32 pc, ca::u32& out)
{
    const auto it = std::upper_bound(pcs.begin(), pcs.end(), pc);
    if (it == pcs.end()) return false;
    out = *it;
    return true;
}

/// SMT 帧链的绝对 pc 序列（与 ir::relocateSmt 同一链算术，共享 attribute.hpp
/// 的单一知识源）。
std::vector<ca::u32> frameAbsPcs(const jbc::core::StackMapTableAttr& smt)
{
    std::vector<ca::u32> abs(smt.frames.size(), 0);
    for (ca::usize i = 0; i < smt.frames.size(); ++i) {
        abs[i] = i == 0 ? smt.frames[i].offset : jbc::core::smtAbsPc(abs[i - 1], smt.frames[i].offset);
    }
    return abs;
}

/// 无落穿控制转移：执行后线性扫描不再落穿到下一指令（HotSpot split
/// verifier 对这类指令置 no_control_flow）。条件分支（Branch2/4 的 if 家族）
/// 有落穿，不在列；jsr/ret 在 Strict 域先行拒绝，不会走到此处。
bool cutsControlFlow(jbc::core::Op op)
{
    using jbc::core::Op;
    switch (op) {
    case Op::GOTO:
    case Op::GOTO_W:
    case Op::TABLESWITCH:
    case Op::LOOKUPSWITCH:
    case Op::IRETURN:
    case Op::LRETURN:
    case Op::FRETURN:
    case Op::DRETURN:
    case Op::ARETURN:
    case Op::RETURN:
    case Op::ATHROW:
        return true;
    default:
        return false;
    }
}

/// 帧链条目的等效全帧展开：abs pc + 有效 locals（append 并回/chop 截去）
/// + stack。紧凑形态（same/same1）只随 prev 携带 locals。
struct ChainEntry
{
    ca::u32 abs = 0;
    std::vector<jbc::core::StackMapTableAttr::VType> locals;
    std::vector<jbc::core::StackMapTableAttr::VType> stack;
};

/// 属性模型帧链 → 等效全帧条目列（升序）。非法形态标签（保留域 128..246）
/// 返回空——只可能来自越权字节，调用方跳过增强保持现状。
std::vector<ChainEntry> expandChain(const jbc::core::StackMapTableAttr& smt)
{
    using SmtFrame = jbc::core::StackMapTableAttr::Frame;
    std::vector<ChainEntry> out;
    out.reserve(smt.frames.size());
    std::vector<jbc::core::StackMapTableAttr::VType> prev;
    ca::u32 prevAbs = 0;
    for (ca::usize i = 0; i < smt.frames.size(); ++i) {
        const SmtFrame& f = smt.frames[i];
        ChainEntry e;
        e.abs = i == 0 ? f.offset : jbc::core::smtAbsPc(prevAbs, f.offset);
        if (f.type <= SmtFrame::kSameMax) {
            e.locals = prev;
        }
        else if (f.type <= SmtFrame::kSame1Max || f.type == SmtFrame::kSame1Ext) {
            e.locals = prev;
            e.stack  = f.stack;
        }
        else if (f.type >= SmtFrame::kChopMax && f.type <= SmtFrame::kChopMin) {
            const ca::usize k = static_cast<ca::usize>(251 - f.type);
            if (k > prev.size()) return {};
            e.locals.assign(prev.begin(), prev.end() - static_cast<std::ptrdiff_t>(k));
        }
        else if (f.type == SmtFrame::kSameExt) {
            e.locals = prev;
        }
        else if (f.type >= SmtFrame::kAppendMin && f.type <= SmtFrame::kAppendMax) {
            e.locals = prev;
            e.locals.insert(e.locals.end(), f.locals.begin(), f.locals.end());
        }
        else if (f.type == SmtFrame::kFull) {
            e.locals = f.locals;
            e.stack  = f.stack;
        }
        else {
            return {};   // 保留域（128..246）：不增强
        }
        prev    = e.locals;
        prevAbs = e.abs;
        out.push_back(std::move(e));
    }
    return out;
}

/// 等效全帧条目列 → 属性模型帧链（smt_build 同一选形器与链算术；首帧
/// prev = 初始帧条目，delta 链式语义同源）。delta 超出 u2 编码域返回 false。
bool reencodeChain(const std::vector<ChainEntry>& entries,
                   const std::vector<jbc::core::StackMapTableAttr::VType>& initLocals,
                   jbc::core::StackMapTableAttr& out)
{
    std::vector<jbc::core::StackMapTableAttr::VType> prev = initLocals;
    ca::u32 prevAbs = 0;
    for (ca::usize i = 0; i < entries.size(); ++i) {
        const ca::u32 delta = i == 0 ? entries[i].abs
                                     : jbc::core::smtDelta(prevAbs, entries[i].abs);
        if (delta > 0xFFFFu) return false;
        out.frames.push_back(jbc::analysis::smt_detail::chooseSmtFrame(
            delta, prev, entries[i].locals, entries[i].stack));
        prev    = entries[i].locals;
        prevAbs = entries[i].abs;
    }
    return true;
}

/// 线性扫描栅栏信息：位置 + 从后继到块终支（下一无落穿转移，含）的指令数
/// （maxStack 抬升的上界估算用）。
struct FenceInfo
{
    ca::u32  pc = 0;
    ca::usize blockInsns = 0;
};

/// 线性扫描栅栏位置：无落穿控制转移的布局后继（升序）。已带帧的后继
/// （数据流目标）不在列——split verifier 只对「帧链跳过的线性位置」索帧。
std::vector<FenceInfo> fencePcsOf(const jbc::ir::CodeUnit& cu,
                                  const std::vector<ChainEntry>& chain)
{
    const auto framed = [&chain](ca::u32 pc) {
        return std::any_of(chain.begin(), chain.end(),
                           [pc](const ChainEntry& e) { return e.abs == pc; });
    };
    std::vector<FenceInfo> fences;
    const std::vector<jbc::ir::InsnId>& order = cu.dom.order();
    for (ca::usize i = 0; i + 1 < order.size(); ++i) {
        const jbc::ir::Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr || !cutsControlFlow(in->op)) continue;
        const jbc::ir::Insn* nx = cu.dom.peek(order[i + 1]);
        if (nx == nullptr || nx->srcPc == jbc::ir::kNoPc) continue;
        if (framed(nx->srcPc)) continue;
        // 块长 = 后继起，到下一无落穿转移（含）——栈上界估算的跨度。
        ca::usize end = order.size();
        for (ca::usize k = i + 1; k < order.size(); ++k) {
            const jbc::ir::Insn* b = cu.dom.peek(order[k]);
            if (b != nullptr && cutsControlFlow(b->op)) {
                end = k + 1;
                break;
            }
        }
        fences.push_back({nx->srcPc, end - (i + 1)});
    }
    std::sort(fences.begin(), fences.end(),
              [](const FenceInfo& a, const FenceInfo& b) { return a.pc < b.pc; });
    fences.erase(std::unique(fences.begin(), fences.end(),
                             [](const FenceInfo& a, const FenceInfo& b) {
                                 return a.pc == b.pc;
                             }),
                 fences.end());
    return fences;
}

/// SMT 条目栈的槽位数（cat2 = Long/Double 占 2 槽，其余 1）。
ca::usize stackSlots(const std::vector<jbc::core::StackMapTableAttr::VType>& stack)
{
    using VType = jbc::core::StackMapTableAttr::VType;
    ca::usize n = 0;
    for (const VType& v : stack) {
        n += (v.tag == VType::kLong || v.tag == VType::kDouble) ? 2 : 1;
    }
    return n;
}

/// CP 查找/追加 Utf8 属性名（buildStackMapTable 不落属性名，挂回时归此）。
ca::u32 findOrAddUtf8(ClassFile& cf, std::string_view name)
{
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (cf.cp.isUtf8(i) && cf.cp.utf8Equals(i, name)) return i;
    }
    return cf.cp.addUtf8(name);
}

/// 嵌套属性是否为 LNT / LVT(LVTT) / SMT（有布局敏感字段的三族）。
enum class NestedKind { Other, Lnt, Lvt, Smt };

NestedKind kindOf(ClassFile& cf, const AttrSlot& slot)
{
    if (cf.cp.utf8Equals(slot.nameCp, "LineNumberTable")) return NestedKind::Lnt;
    if (cf.cp.utf8Equals(slot.nameCp, "LocalVariableTable")) return NestedKind::Lvt;
    if (cf.cp.utf8Equals(slot.nameCp, "LocalVariableTypeTable")) return NestedKind::Lvt;
    if (cf.cp.utf8Equals(slot.nameCp, "StackMapTable")) return NestedKind::Smt;
    return NestedKind::Other;
}

}   // namespace

bool prepareNestedAttrsForLayout(jbc::ir::ClassSession& session, const jbc::ir::CodeUnit& code)
{
    ClassFile&                 cf = session.backing();
    const jbc::ir::MethodUnit& mu = methodById(session, code.method);
    AttrSlot&                  codeSlot = codeSlotOf(cf, mu);

    const std::vector<ca::u32> pcs = survivingPcs(code.dom);
    const ca::u32              oldCodeLen = [&] {
        auto d = cf.decodeAttr(codeSlot);
        if (d.is_err()) {
            throw std::runtime_error("frame_rewire: Code attribute undecodable");
        }
        return static_cast<ca::u32>(static_cast<CodeAttr*>(d.unwrap())->code.size());
    }();

    bool smtDropped = false;
    for (AttrSlot& nested : static_cast<CodeAttr*>(cf.decodeAttr(codeSlot).unwrap())->attrs) {
        const NestedKind kind = kindOf(cf, nested);
        if (kind == NestedKind::Other) continue;
        auto body = cf.decodeAttr(nested);
        if (body.is_err()) {
            throw std::runtime_error("frame_rewire: nested attribute undecodable for method '"
                                     + std::string(cf.nameOf(cf.methods[mu.backingIndex])) + "'");
        }
        bool touched = false;
        switch (kind) {
        case NestedKind::Lnt: {
            auto& lnt = *static_cast<jbc::core::LineNumberTableAttr*>(body.unwrap());
            std::vector<jbc::core::LineNumberTableAttr::Row> kept;
            kept.reserve(lnt.rows.size());
            for (auto& row : lnt.rows) {
                const bool dangles =
                    std::binary_search(pcs.begin(), pcs.end(), row.startPc) == false;
                if (!dangles) {
                    kept.push_back(row);
                    continue;
                }
                // 行号节点留原位语义：行起点重映射到下一个存活 pc；
                // 之后无存活指令则整行截去（该行号所辖代码不复存在）。
                ca::u32 remapped = 0;
                if (nextSurvivingPc(pcs, row.startPc, remapped)) {
                    row.startPc = remapped;
                    kept.push_back(row);
                    touched = true;
                }
                else {
                    touched = true;
                }
            }
            if (touched) {
                auto* mut = static_cast<jbc::core::LineNumberTableAttr*>(nested.mutate());
                if (mut == nullptr) {
                    throw std::runtime_error("frame_rewire: LNT slot not decoded");
                }
                mut->rows = std::move(kept);
            }
            break;
        }
        case NestedKind::Lvt: {
            auto& lvt = *static_cast<jbc::core::LocalVariableTableAttr*>(body.unwrap());
            for (auto& row : lvt.rows) {
                const ca::u32 oldEnd = row.startPc + row.length;
                ca::u32       newStart = row.startPc;
                bool          changed  = false;
                if (std::binary_search(pcs.begin(), pcs.end(), row.startPc) == false) {
                    if (!nextSurvivingPc(pcs, row.startPc, newStart)) {
                        newStart = oldCodeLen;   // 区间整体悬垂：收敛到空区间
                    }
                    changed = true;
                }
                ca::u32 newEnd = oldEnd;
                if (oldEnd != oldCodeLen
                    && std::binary_search(pcs.begin(), pcs.end(), oldEnd) == false)
                {
                    if (!nextSurvivingPc(pcs, oldEnd, newEnd)) newEnd = oldCodeLen;
                    changed = true;
                }
                if (changed) {
                    row.length  = newEnd > newStart ? newEnd - newStart : 0;
                    row.startPc = newStart;
                    touched     = true;
                }
            }
            if (touched) {
                auto* mut = static_cast<jbc::core::LocalVariableTableAttr*>(nested.mutate());
                if (mut == nullptr) {
                    throw std::runtime_error("frame_rewire: LVT slot not decoded");
                }
            }
            break;
        }
        case NestedKind::Smt: {
            // 帧内容无法跨删除搬运：任一目标/载荷 pc 悬垂即整体摘除，交给
            // rewireFramesAfterLower 再生（调用方保证 rewire 后随或显式失败）。
            const auto& smt = *static_cast<const jbc::core::StackMapTableAttr*>(body.unwrap());
            bool        dangles = false;
            for (const ca::u32 pc : frameAbsPcs(smt)) {
                if (std::binary_search(pcs.begin(), pcs.end(), pc) == false) {
                    dangles = true;
                    break;
                }
            }
            if (!dangles) {
                const auto uninitDangles = [&](const jbc::core::StackMapTableAttr::VType& v) {
                    return v.tag == jbc::core::StackMapTableAttr::VType::kUninitialized
                           && std::binary_search(pcs.begin(), pcs.end(), v.cp) == false;
                };
                for (const auto& f : smt.frames) {
                    for (const auto& v : f.locals) {
                        if (uninitDangles(v)) dangles = true;
                    }
                    for (const auto& v : f.stack) {
                        if (uninitDangles(v)) dangles = true;
                    }
                }
            }
            if (dangles) {
                smtDropped = true;
                touched    = true;
            }
            if (touched) {
                // 摘除在调用方遍历的 attrs 序列上进行——先标记，由循环外的
                // erase 收口（避免迭代中删除）。
                nested.state = jbc::core::AttrSlot::State::Modified;
                nested.body.reset();
            }
            break;
        }
        case NestedKind::Other:
            break;
        }
    }

    if (smtDropped) {
        auto& attrs = static_cast<CodeAttr*>(cf.decodeAttr(codeSlot).unwrap())->attrs;
        attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                                   [&](const AttrSlot& s) {
                                       return cf.cp.utf8Equals(s.nameCp, "StackMapTable")
                                              && s.body == nullptr;
                                   }),
                    attrs.end());
    }
    return smtDropped;
}

void rewireFramesAfterLower(jbc::ir::ClassSession& session, const MethodScope& scope,
                            jbc::analysis::ITypeResolver& resolver)
{
    namespace ja = jbc::analysis;

    // 版本门：SMT 自 V50 才有定义，更老的类保持 relocation 后现状。
    if (session.backing().major < 50) {
        return;
    }

    jbc::ir::CodeUnit&         cu = scope.code;
    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::MethodUnit* mu = unit.method(scope.method);
    if (mu == nullptr) {
        throw std::logic_error("frame_rewire: method identity not attached to session");
    }

    ja::MethodSig sig;
    sig.isStatic  = (mu->access & 0x0008) != 0;
    sig.desc      = unit.symbols().str(mu->desc);
    sig.name      = unit.symbols().str(mu->name);
    sig.thisClass = unit.symbols().str(unit.thisClass);

    // Strict 帧重算：lower 已把 srcPc 刷新为新布局，帧的 Uninitialized 载荷
    // 与目标 pc 天然与新 Code 对齐（§10.4 布局一致性由接线方保证——即此处）。
    ja::FrameEngine            engine(session.backing().cp, resolver, ja::FramePolicy::Strict);
    std::vector<ja::FrameAt>   frames;
    const ja::FrameSummary     summary = engine.compute(cu, sig, frames);
    if (!summary.ok) {
        throw std::runtime_error(std::string("frame_rewire: strict frame compute failed (") +
                                 ja::to_string(summary.fail) + ") for method '"
                                 + std::string(unit.symbols().str(mu->name)) + "'");
    }

    // ── 线性扫描栅栏帧（HotSpot split verifier 裁决）──
    // HotSpot 的线性校验循环对无落穿控制转移后的下一条指令索帧
    // （verifier.cpp verify_stackmap_table：no_control_flow 且下一帧
    // offset > bci 即 "Expecting a stack map frame"），与可达性无关
    // （JVMS 允许死代码无帧，实现按线性位置收口——javac 布局下转移的
    // 线性后继恒为目标，该栅栏天然满足；合成死块紧贴转移后就落空）。
    // 数据流目标集不含死块入口，再生表缺栅栏帧。处置：栅栏位置补帧，
    // 状态借链中下一帧的等效全帧条目（死块栈中性且终支目标即该帧时
    // 精确成立——bozar 型插入即此形态；不成立时该死块本就无法通过
    // 线性验证，不劣于缺帧现状）。整链按条目重编码，选形器与链算术
    // 与 smt_build 同源；无栅栏时表原样挂回，既有产物字节不变。
    //
    // 栅栏状态非空时，死块的线性验证从借来的栈出发——逐指令压栈需要
    // maxStack 余量（Strict 重算只覆盖可达面，不含死块）。上界 = 借入
    // 栈槽 + 块内每条指令最大压槽 4（dup2_x2 家族上界；pop 忽略，纯
    // 高估合法——maxStack 是上限语义）。maxes 采用后、lower 落盘前抬升。
    auto built = ja::buildStackMapTable(session, cu, frames);
    if (built.is_err()) {
        throw std::runtime_error(std::string("frame_rewire: stack map regeneration failed (")
                                 + ja::to_string(built.unwrap_err()) + ") for method '"
                                 + std::string(unit.symbols().str(mu->name)) + "'");
    }
    auto result = std::move(built).unwrap();

    std::vector<ChainEntry> chain;
    if (result.needed) chain = expandChain(*result.attr);
    const std::vector<FenceInfo> fences = fencePcsOf(cu, chain);
    if (!result.needed && fences.empty()) {
        return;
    }

    // 栅栏内容 = 链中下一个条目的等效全帧；无后续条目（栅栏在末帧之后）
    // 借初始帧 locals + 空栈（链的起点与引擎播种同源）。
    std::vector<jbc::core::StackMapTableAttr::VType> initLocals;
    auto initR = ja::smt_detail::smtEntriesOf(session, frames[0].locals);
    if (initR.is_err()) {
        throw std::runtime_error(std::string("frame_rewire: initial frame encoding failed (")
                                 + ja::to_string(initR.unwrap_err()) + ") for method '"
                                 + std::string(unit.symbols().str(mu->name)) + "'");
    }
    initLocals = std::move(initR).unwrap();

    ca::usize fenceNeed = 0;
    if (!fences.empty()) {
        ca::usize need = 0;
        for (const FenceInfo& f : fences) {
            const auto nxt = std::upper_bound(chain.begin(), chain.end(), f.pc,
                                              [](ca::u32 v, const ChainEntry& e) {
                                                  return v < e.abs;
                                              });
            const ca::usize slots = nxt != chain.end()
                                        ? stackSlots(nxt->stack)
                                        : 0;   // 末帧之后借空栈
            need = std::max(need, slots + 4 * f.blockInsns);
        }
        fenceNeed = need;
    }

    // maxes 显式采用（A2 裁决：Strict 重算 = 覆盖）→ 第二次 lower 落 maxes
    // （布局未变，写回只触 maxStack/maxLocals 两字段）。栅栏抬升在采用后
    // 叠加（采用会覆盖重算面，先抬必被冲掉），lower 落盘前生效。
    if (!ja::applyComputedMaxes(cu, summary, sig)) {
        throw std::runtime_error("frame_rewire: computed maxes out of u16 range for method '"
                                 + std::string(unit.symbols().str(mu->name)) + "'");
    }
    if (fenceNeed > 0 && fenceNeed <= 0xFFFFu) {
        cu.maxStack = static_cast<ca::u16>(
            std::max<ca::usize>(cu.maxStack, fenceNeed));
    }

    const auto lowered = jbc::ir::lowerMethod(session, scope.method);
    if (lowered.is_err()) {
        throw std::runtime_error(std::string("frame_rewire: maxes write-back failed: ")
                                 + jbc::ir::to_string(lowered.unwrap_err()));
    }

    std::unique_ptr<jbc::core::StackMapTableAttr> attach;
    if (fences.empty()) {
        attach = std::move(result.attr);
    }
    else {
        std::vector<ChainEntry> merged;
        merged.reserve(chain.size() + fences.size());
        for (const FenceInfo& f : fences) {
            ChainEntry e;
            e.abs = f.pc;
            const auto nxt = std::upper_bound(chain.begin(), chain.end(), f.pc,
                                              [](ca::u32 v, const ChainEntry& e) {
                                                  return v < e.abs;
                                              });
            if (nxt != chain.end()) {
                e.locals = nxt->locals;
                e.stack  = nxt->stack;
            }
            else {
                e.locals = initLocals;
            }
            merged.push_back(std::move(e));
        }
        merged.insert(merged.end(), chain.begin(), chain.end());
        std::stable_sort(merged.begin(), merged.end(),
                         [](const ChainEntry& a, const ChainEntry& b) { return a.abs < b.abs; });
        auto attr = std::make_unique<jbc::core::StackMapTableAttr>();
        if (!reencodeChain(merged, initLocals, *attr)) {
            throw std::runtime_error("frame_rewire: fence-augmented frame chain exceeds "
                                     "u2 delta range for method '"
                                     + std::string(unit.symbols().str(mu->name)) + "'");
        }
        attach = std::move(attr);
    }

    ClassFile&      cf       = session.backing();
    AttrSlot&       codeSlot = codeSlotOf(cf, *mu);
    const auto      d        = cf.decodeAttr(codeSlot);
    if (d.is_err()) {
        throw std::runtime_error("frame_rewire: Code attribute undecodable after lower");
    }
    auto* codeAttr = static_cast<CodeAttr*>(d.unwrap());
    auto& attrs    = codeAttr->attrs;
    attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                               [&](const AttrSlot& s) {
                                   return cf.cp.utf8Equals(s.nameCp, "StackMapTable");
                               }),
                attrs.end());
    attrs.push_back(
        AttrSlot::makeNew(findOrAddUtf8(cf, "StackMapTable"),
                          std::unique_ptr<jbc::core::AttrBody>(std::move(attach))));
    // Code 属性重编码由 lower 置脏保证；嵌套槽 State::New 强制重编码。
}

}   // namespace frame_rewire

}   // namespace mjbase::spi
