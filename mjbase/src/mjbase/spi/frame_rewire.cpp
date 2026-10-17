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
                // 旧栈行号节点留原位语义：行起点重映射到下一个存活 pc；
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

    // maxes 显式采用（A2 裁决：Strict 重算 = 覆盖）→ 第二次 lower 落 maxes
    // （布局未变，写回只触 maxStack/maxLocals 两字段）。
    if (!ja::applyComputedMaxes(cu, summary, sig)) {
        throw std::runtime_error("frame_rewire: computed maxes out of u16 range for method '"
                                 + std::string(unit.symbols().str(mu->name)) + "'");
    }
    const auto lowered = jbc::ir::lowerMethod(session, scope.method);
    if (lowered.is_err()) {
        throw std::runtime_error(std::string("frame_rewire: maxes write-back failed: ")
                                 + jbc::ir::to_string(lowered.unwrap_err()));
    }

    // SMT 再生：needed=false（方法无控制流目标）= 不写属性（摘除后维持缺席）。
    auto built = ja::buildStackMapTable(session, cu, frames);
    if (built.is_err()) {
        throw std::runtime_error(std::string("frame_rewire: stack map regeneration failed (")
                                 + ja::to_string(built.unwrap_err()) + ") for method '"
                                 + std::string(unit.symbols().str(mu->name)) + "'");
    }
    auto result = std::move(built).unwrap();
    if (!result.needed) {
        return;
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
                          std::unique_ptr<jbc::core::AttrBody>(std::move(result.attr))));
    // Code 属性重编码由 lower 置脏保证；嵌套槽 State::New 强制重编码。
}

}   // namespace frame_rewire

}   // namespace mjbase::spi
