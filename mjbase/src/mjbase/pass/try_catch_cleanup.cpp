#include "mjbase/pass/try_catch_cleanup.hpp"

#include <string_view>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/null_domain.hpp"
#include "mjbase/hierarchy/summary.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

constexpr ca::u16 kAccSynchronized = 0x0020;

/// 条目视图：异常表条目的位置解析（start/end/handler 的 order 下标；
/// end 悬空 = 方法末尾语义，取 order.size()；start/handler 悬空 = 非法）。
struct EntryView
{
    ca::usize start = 0;
    ca::usize end   = 0;
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

/// 两个有效区间是否相交。
bool rangesOverlap(ca::usize as, ca::usize ae, ca::usize bs, ca::usize be)
{
    return as < be && bs < ae;
}

/// 普通控制流后继（不含异常派发边）。JSR/RET 子例程、分支目标悬空、
/// 非终结指令悬空在方法末尾一律 fail（调用方放弃依赖 CFG 的规则）。
bool normalSuccessors(const CodeDom& dom, const std::vector<InsnId>& ids,
                      const std::vector<std::pair<LabelId, ca::usize>>& labelPos,
                      ca::usize i, std::vector<ca::usize>& out)
{
    const Insn* in = dom.peek(ids[i]);
    if (in == nullptr) return false;
    const ca::usize n = ids.size();
    switch (in->op) {
    case Op::GOTO:
    case Op::GOTO_W: {
        const ca::usize* t = findLabelPos(labelPos, in->target);
        if (t == nullptr) return false;
        out.push_back(*t);
        return true;
    }
    case Op::JSR:
    case Op::JSR_W:
    case Op::RET:
        return false;
    case Op::IFEQ:
    case Op::IFNE:
    case Op::IFLT:
    case Op::IFGE:
    case Op::IFGT:
    case Op::IFLE:
    case Op::IF_ICMPEQ:
    case Op::IF_ICMPNE:
    case Op::IF_ICMPLT:
    case Op::IF_ICMPGE:
    case Op::IF_ICMPGT:
    case Op::IF_ICMPLE:
    case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE:
    case Op::IFNULL:
    case Op::IFNONNULL: {
        const ca::usize* t = findLabelPos(labelPos, in->target);
        if (t == nullptr) return false;
        out.push_back(*t);
        if (i + 1 < n) out.push_back(i + 1);
        return true;
    }
    case Op::TABLESWITCH:
    case Op::LOOKUPSWITCH: {
        const ca::usize* d = findLabelPos(labelPos, in->target);
        if (d == nullptr) return false;
        out.push_back(*d);
        for (const LabelId l : in->targets) {
            const ca::usize* t = findLabelPos(labelPos, l);
            if (t == nullptr) return false;
            out.push_back(*t);
        }
        return true;
    }
    case Op::ATHROW:
    case Op::IRETURN:
    case Op::LRETURN:
    case Op::FRETURN:
    case Op::DRETURN:
    case Op::ARETURN:
    case Op::RETURN:
        return true;
    default:
        if (i + 1 >= n) return false;
        out.push_back(i + 1);
        return true;
    }
}

/// 从方法入口做普通流可达 BFS（不含异常派发边）。结构不支持时 false。
bool buildNormalReach(const CodeDom& dom, const std::vector<InsnId>& ids,
                      const std::vector<std::pair<LabelId, ca::usize>>& labelPos,
                      std::vector<bool>& reachable)
{
    const ca::usize n = ids.size();
    if (n == 0) return false;
    std::vector<std::vector<ca::usize>> succs(n);
    for (ca::usize i = 0; i < n; ++i) {
        if (!normalSuccessors(dom, ids, labelPos, i, succs[i])) return false;
    }
    reachable.assign(n, false);
    reachable[0] = true;
    std::vector<ca::usize> queue = {0};
    for (ca::usize head = 0; head < queue.size(); ++head) {
        for (const ca::usize s : succs[queue[head]]) {
            if (!reachable[s]) {
                reachable[s] = true;
                queue.push_back(s);
            }
        }
    }
    return true;
}

/// 「可能抛出异常」的保守指令集合（对齐旧栈 insnCanThrowAny 口径：
/// 调用/分配/类型解析/字段与数组访问/监视器/除法；IALOAD..SASTORE 整段
/// 计入——普通局部 store 也落在区间内，方向保守多保留条目）。
bool insnCanThrowAny(const Insn& in)
{
    switch (in.op) {
    case Op::ATHROW:
    case Op::INVOKEVIRTUAL:
    case Op::INVOKESPECIAL:
    case Op::INVOKESTATIC:
    case Op::INVOKEINTERFACE:
    case Op::INVOKEDYNAMIC:
    case Op::NEW:
    case Op::ANEWARRAY:
    case Op::NEWARRAY:
    case Op::MULTIANEWARRAY:
    case Op::CHECKCAST:
    case Op::INSTANCEOF:
    case Op::GETFIELD:
    case Op::PUTFIELD:
    case Op::ARRAYLENGTH:
    case Op::MONITORENTER:
    case Op::MONITOREXIT:
    case Op::IDIV:
    case Op::IREM:
    case Op::LDIV:
    case Op::LREM:
    case Op::FDIV:
    case Op::FREM:
    case Op::DDIV:
    case Op::DREM:
        return true;
    default:
        return in.op >= Op::IALOAD && in.op <= Op::SASTORE;
    }
}

/// catch 类型内部名（catchCp = 0 = any → 空）。引用 CP 视图借用会话。
std::string_view catchTypeName(const jbc::core::ConstantPool& cp, ca::u32 catchCp)
{
    if (catchCp == 0 || !cp.valid(catchCp) || cp.tagOf(catchCp) != jbc::core::CpTag::Class) {
        return {};
    }
    return cp.utf8Of(cp.at(catchCp).a);
}

/// 恒等静态包装判定：当前类中唯一同名同描述符静态方法，非 synchronized、
/// 无异常表、体恰为 `aload 0; areturn`。
bool refLoadSlot(const Insn& in, ca::u32& slot);

bool isIdentityStaticWrapper(jbc::ir::ClassSession& session, std::string_view name,
                             std::string_view desc)
{
    jbc::ir::ClassUnit& unit = session.unit();
    ca::usize           resolved = jbc::ir::kNoMethod;
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        const jbc::ir::MethodUnit& mu = unit.methods[i];
        if (unit.symbols().str(mu.name) != name || unit.symbols().str(mu.desc) != desc) {
            continue;
        }
        if ((mu.access & mjbase::hierarchy::kAccStatic) == 0) continue;
        if (resolved != jbc::ir::kNoMethod) return false;   // 重复定义保守放弃
        resolved = i;
    }
    if (resolved == jbc::ir::kNoMethod) return false;
    const jbc::ir::MethodUnit& mu = unit.methods[resolved];
    if ((mu.access & kAccSynchronized) != 0) return false;
    auto promoted = jbc::ir::promoteMethod(session, resolved);
    if (promoted.is_err()) return false;
    const jbc::ir::CodeUnit& code = *promoted.unwrap();
    if (!code.exc.empty()) return false;
    const std::vector<InsnId> ids = code.dom.order();
    if (ids.size() != 2) return false;
    const Insn* load = code.dom.peek(ids[0]);
    const Insn* ret = code.dom.peek(ids[1]);
    if (load == nullptr || ret == nullptr) return false;
    ca::u32 slot = 0;
    if (!refLoadSlot(*load, slot) || slot != 0) return false;
    if (ret->op != Op::ARETURN) return false;
    return true;
}

/// 引用存槽装载槽位（ASTORE 与 ASTORE_0..3 家族）；非家族 false。
bool refStoreSlot(const Insn& in, ca::u32& slot)
{
    if (in.op == Op::ASTORE) {
        slot = in.a;
        return true;
    }
    if (in.op >= Op::ASTORE_0 && in.op <= Op::ASTORE_3) {
        slot = static_cast<ca::u32>(static_cast<ca::u8>(in.op)
                                    - static_cast<ca::u8>(Op::ASTORE_0));
        return true;
    }
    return false;
}

bool refLoadSlot(const Insn& in, ca::u32& slot)
{
    if (in.op == Op::ALOAD) {
        slot = in.a;
        return true;
    }
    if (in.op >= Op::ALOAD_0 && in.op <= Op::ALOAD_3) {
        slot = static_cast<ca::u32>(static_cast<ca::u8>(in.op)
                                    - static_cast<ca::u8>(Op::ALOAD_0));
        return true;
    }
    return false;
}

/// 描述符 (LX;)LX; 判定（单引用入参、返回同类型；数组/多参/异型不认）。
bool isIdentityRefDesc(std::string_view desc)
{
    constexpr ca::usize kNpos = static_cast<ca::usize>(-1);
    if (desc.size() < 6 || desc.front() != '(' || desc[1] != 'L') return false;
    const ca::usize semi = desc.find(';', 2);
    if (semi == kNpos || desc.compare(semi, 3, ";)L") != 0) return false;
    const std::string_view arg = desc.substr(1, semi - 1);
    // 返回面 = "L" + arg + ";"（含尾分号逐段比较）。
    return desc.size() == semi + 2 + arg.size() + 1
           && desc.compare(semi + 2, arg.size(), arg) == 0 && desc.back() == ';';
}

}   // namespace

mjbase::spi::PreservedAnalyses TryCatchCleanup::run(mjbase::spi::MethodScope&       scope,
                                                    mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeUnit&           cu      = scope.code;
    CodeDom&            dom     = cu.dom;
    jbc::ir::CodeEditor editor(cu);
    if (cu.exc.empty()) return mjbase::spi::PreservedAnalyses::all();

    const std::vector<InsnId> ids = dom.order();
    std::vector<std::pair<LabelId, ca::usize>> labelPos;
    buildLabelPos(dom, ids, labelPos);

    // 域分析：可达帧 + 栈深（NPE 形 / 自捕抛 / 恒不抛三规则的门槛面）。
    jbc::analysis::DomainResult<mjbase::asm_analyzer::NullValue> frames;
    const bool framesOk = [&] {
        mjbase::asm_analyzer::NullDomain d(scope.session.backing().cp);
        jbc::analysis::DomainEngine<mjbase::asm_analyzer::NullValue> eng(
            scope.session.backing().cp, d);
        frames = eng.analyze(cu);
        return frames.summary.ok && frames.frames.size() == ids.size();
    }();

    // 普通流可达（透明重抛规则的门槛面）。
    std::vector<bool> normalReach;
    const bool        flowOk = buildNormalReach(dom, ids, labelPos, normalReach);

    struct Plan
    {
        ca::usize index    = 0;   // 待删条目下标
        bool      rewriteGotoToHandler = false;   // 自捕抛：athrow 原位改 GOTO(handler)
        ca::usize athrowPos = 0;
        bool      rewriteNpeJump = false;         // NPE 形：aconst_null 改 GOTO(续点)
        ca::usize nullPos  = 0;
        ca::usize throwPos = 0;
        LabelId   gotoTarget{};
        bool      applied = true;   ///< 改写面失败时条目保留（不删）
    };
    std::vector<Plan> plans;

    for (ca::usize ei = 0; ei < cu.exc.size(); ++ei) {
        const CodeUnit::Exc& e = cu.exc[ei];
        const EntryView      v = viewEntry(cu, e, labelPos, ids.size());

        // 修复面：start/handler 悬空或空区间 → 删。
        if (!v.validLabels || v.emptyRange) {
            plans.push_back({ei});
            continue;
        }
        const Insn* startInsn = dom.peek(ids[v.start]);
        if (startInsn == nullptr) continue;

        // 恒不抛收口：区间内不存在可达且可能抛出的指令 → 删。
        if (framesOk) {
            bool anyThrowing = false;
            for (ca::usize i = v.start; i < v.end; ++i) {
                if (!frames.frames[i].reachable) continue;
                const Insn* in = dom.peek(ids[i]);
                if (in != nullptr && insnCanThrowAny(*in)) {
                    anyThrowing = true;
                    break;
                }
            }
            if (!anyThrowing) {
                plans.push_back({ei});
                continue;
            }
        }

        // 透明重抛删除（依赖普通流可达性）。
        if (flowOk) {
            // 同一 handler 按解析位置计数（不同 LabelId 锚在同一 pc 仍是同一 handler）。
            ca::usize sharedCount = 0;
            for (const CodeUnit::Exc& other : cu.exc) {
                const EntryView ov = viewEntry(cu, other, labelPos, ids.size());
                if (ov.validLabels && !ov.emptyRange && ov.handler == v.handler)
                    ++sharedCount;
            }
            bool overlapping = false;
            for (ca::usize oj = 0; oj < cu.exc.size() && !overlapping; ++oj) {
                if (oj == ei) continue;
                const EntryView o = viewEntry(cu, cu.exc[oj], labelPos, ids.size());
                if (!o.validLabels || o.emptyRange) continue;
                if (rangesOverlap(v.start, v.end, o.start, o.end)) overlapping = true;
            }
            // handler 体：从 handler 起到终结 athrow（攒满 3 条仍未见到即非透明形）。
            std::vector<ca::usize> bodyPos;
            std::vector<const Insn*> body;
            for (ca::usize p = v.handler;
                 p < ids.size() && body.size() < 3; ++p) {
                const Insn* in = dom.peek(ids[p]);
                if (in == nullptr) break;
                bodyPos.push_back(p);
                body.push_back(in);
                if (in->op == Op::ATHROW) break;
            }
            const bool direct = body.size() == 1 && body[0]->op == Op::ATHROW;
            ca::u32 storeSlot = 0;
            ca::u32 loadSlot  = 0;
            bool identityStore = body.size() == 3 && body[2]->op == Op::ATHROW
                                 && refStoreSlot(*body[0], storeSlot)
                                 && refLoadSlot(*body[1], loadSlot)
                                 && storeSlot == loadSlot;
            bool identityWrapper = false;
            if (body.size() == 2 && body[0]->op == Op::INVOKESTATIC
                && body[1]->op == Op::ATHROW) {
                jbc::ir::ClassSession::MemberSem sem;
                if (scope.session.memberOf(*body[0], sem)
                    && sem.owner == scope.session.backing().thisName()
                    && isIdentityRefDesc(sem.desc)
                    && isIdentityStaticWrapper(scope.session, sem.name, sem.desc)) {
                    identityWrapper = true;
                }
            }
            if ((direct || identityStore || identityWrapper) && sharedCount == 1
                && !overlapping) {
                bool bodyUnreachable = true;
                for (const ca::usize p : bodyPos) {
                    if (normalReach[p]) {
                        bodyUnreachable = false;
                        break;
                    }
                }
                if (bodyUnreachable) {
                    plans.push_back({ei});
                    continue;
                }
            }
        }

        // handler 在区间内：改写类规则一律不碰。
        if (v.handler >= v.start && v.handler < v.end) continue;

        // NPE 形改写：保护区恰为 aconst_null; athrow。
        if (framesOk && v.end == v.start + 2 && startInsn->op == Op::ACONST_NULL) {
            const Insn* throwInsn = dom.peek(ids[v.start + 1]);
            if (throwInsn != nullptr && throwInsn->op == Op::ATHROW) {
                const std::string_view catchType = catchTypeName(scope.session.backing().cp,
                                                                 e.catchCp);
                if (catchType.empty() || catchType == "java/lang/NullPointerException") {
                    // 锚入守卫：保护区两条指令上不得有其他标签（分支携带
                    // 栈积压进入会让改写后的 goto 失去派发清栈语义）。
                    bool foreignLabel = false;
                    for (const auto& [label, pos] : labelPos) {
                        if ((pos == v.start && !(label == e.start))
                            || (pos == v.start + 1)) {
                            foreignLabel = true;
                            break;
                        }
                    }
                    // 栈深门槛：保护区入口栈深为零（goto 不清栈）。
                    const bool depthZero = frames.frames[v.start].reachable
                                           && frames.frames[v.start].stack.empty();
                    // handler 形：pop 后接续点（下一指令锚定标签或下一指令
                    // GOTO 的目标）。
                    LabelId continuation{};
                    bool     contOk = false;
                    if (v.handler + 1 < ids.size()) {
                        const Insn* pop = dom.peek(ids[v.handler]);
                        const Insn* nx  = dom.peek(ids[v.handler + 1]);
                        if (pop != nullptr && nx != nullptr && pop->op == Op::POP) {
                            const std::vector<LabelId> anchors = dom.labelsAt(ids[v.handler + 1]);
                            if (!anchors.empty()) {
                                continuation = anchors.front();
                                contOk       = true;
                            }
                            else if (nx->op == Op::GOTO
                                     && findLabelPos(labelPos, nx->target) != nullptr) {
                                continuation = nx->target;
                                contOk       = true;
                            }
                        }
                    }
                    if (!foreignLabel && depthZero && contOk && continuation != e.handler) {
                        bool overlap = false;
                        for (ca::usize oj = 0; oj < cu.exc.size() && !overlap; ++oj) {
                            if (oj == ei) continue;
                            const EntryView o = viewEntry(cu, cu.exc[oj], labelPos, ids.size());
                            if (!o.validLabels || o.emptyRange) continue;
                            if (rangesOverlap(v.start, v.end, o.start, o.end)) overlap = true;
                        }
                        if (!overlap) {
                            Plan p;
                            p.index         = ei;
                            p.rewriteNpeJump = true;
                            p.nullPos       = v.start;
                            p.throwPos      = v.start + 1;
                            p.gotoTarget    = continuation;
                            plans.push_back(p);
                            continue;
                        }
                    }
                }
            }
        }

        // 自捕抛改写：保护区恰为 new E; dup; invokespecial E.<init>()V; athrow。
        if (framesOk && v.end == v.start + 4 && startInsn->op == Op::NEW) {
            const Insn* dup   = dom.peek(ids[v.start + 1]);
            const Insn* init  = dom.peek(ids[v.start + 2]);
            const Insn* thr   = dom.peek(ids[v.start + 3]);
            const std::string_view newType = scope.session.classOf(*startInsn);
            jbc::ir::ClassSession::MemberSem isem;
            bool shapeOk = dup != nullptr && init != nullptr && thr != nullptr
                           && !newType.empty() && dup->op == Op::DUP
                           && init->op == Op::INVOKESPECIAL
                           && scope.session.memberOf(*init, isem)
                           && isem.owner == newType && isem.name == "<init>"
                           && isem.desc == "()V"
                           && thr->op == Op::ATHROW;
            if (shapeOk) {
                const std::string_view catchType = catchTypeName(scope.session.backing().cp,
                                                                 e.catchCp);
                if (catchType.empty() || catchType == newType) {
                    // athrow 处栈深恰为 1（仅异常对象）+ athrow 无其他锚入。
                    const jbc::analysis::FrameV<mjbase::asm_analyzer::NullValue>& fr =
                        frames.frames[v.start + 3];
                    bool foreignLabel = false;
                    for (const auto& [label, pos] : labelPos) {
                        if (pos == v.start + 1 || pos == v.start + 2 || pos == v.start + 3) {
                            foreignLabel = true;
                            break;
                        }
                    }
                    if (fr.reachable && fr.stack.size() == 1 && !foreignLabel) {
                        bool overlap = false;
                        for (ca::usize oj = 0; oj < cu.exc.size() && !overlap; ++oj) {
                            if (oj == ei) continue;
                            const EntryView o = viewEntry(cu, cu.exc[oj], labelPos, ids.size());
                            if (!o.validLabels || o.emptyRange) continue;
                            if (rangesOverlap(v.start, v.end, o.start, o.end)) overlap = true;
                        }
                        if (!overlap) {
                            Plan p;
                            p.index         = ei;
                            p.rewriteGotoToHandler = true;
                            p.athrowPos     = v.start + 3;
                            p.gotoTarget    = e.handler;
                            plans.push_back(p);
                            continue;
                        }
                    }
                }
            }
        }
    }

    if (plans.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 落笔：先做指令改写（改写失败则该条目保留），再按位置降序删条目。
    bool modified = false;
    for (Plan& p : plans) {
        if (p.rewriteNpeJump) {
            auto got = dom.get(ids[p.nullPos]);
            if (got.is_err()) {
                p.applied = false;
                continue;
            }
            Insn* g = std::move(got).unwrap();
            g->op     = Op::GOTO;
            g->target = p.gotoTarget;
            g->a      = 0;
            g->b      = 0;
            g->cp     = 0;
            g->wide   = false;
            g->targets.clear();
            g->keys.clear();
            if (editor.erase(ids[p.throwPos],
                             jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                    .is_err()) {
                p.applied = false;   // 删除失败：条目保留。已改写的 GOTO 语义
                                     // 仍由未删条目承接（GOTO 不抛出，路径
                                     // 行为不变），无半折叠态
                continue;
            }
            p.applied = true;
            modified  = true;
        }
        else if (p.rewriteGotoToHandler) {
            auto got = dom.get(ids[p.athrowPos]);
            if (got.is_err()) {
                p.applied = false;
                continue;
            }
            Insn* g = std::move(got).unwrap();
            g->op     = Op::GOTO;
            g->target = p.gotoTarget;
            g->a      = 0;
            g->b      = 0;
            g->cp     = 0;
            g->wide   = false;
            g->targets.clear();
            g->keys.clear();
            p.applied = true;
            modified  = true;
        }
    }
    for (auto it = plans.rbegin(); it != plans.rend(); ++it) {
        if (it->applied && it->index < cu.exc.size()) {
            cu.exc.erase(cu.exc.begin() + static_cast<ca::usize>(it->index));
        }
    }
    return modified || !plans.empty() ? mjbase::spi::PreservedAnalyses::none()
                                      : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
