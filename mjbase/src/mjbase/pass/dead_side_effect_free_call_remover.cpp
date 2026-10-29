#include "mjbase/pass/dead_side_effect_free_call_remover.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefUseEdge;
using mjbase::asm_analyzer::DefUseSlot;
using mjbase::asm_analyzer::MethodDefUse;

/// 白名单声明表（旧栈 issue #678 + UnusedResultCallEliminater 白名单合并）：
/// 可安全弹弃的调用。零参精确条目 + Math/StrictMath 纯函数名家族条目；
/// 扩展只增条目，不改判定逻辑。
struct DiscardableCall
{
    jbc::core::Op    op;
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

constexpr DiscardableCall kExactDiscardableCalls[] = {
    {jbc::core::Op::INVOKESTATIC, "java/lang/System", "nanoTime", "()J"},
    {jbc::core::Op::INVOKESTATIC, "java/lang/System", "currentTimeMillis", "()J"},
    {jbc::core::Op::INVOKESTATIC, "java/lang/Math", "random", "()D"},
    {jbc::core::Op::INVOKEVIRTUAL, "java/lang/String", "length", "()I"},
    {jbc::core::Op::INVOKEVIRTUAL, "java/lang/String", "isEmpty", "()Z"},
};

/// 家族条目：Math/StrictMath 纯函数名集（旧栈 isSafeMathName 对齐——
/// 全参确定、无副作用、对任意输入值（含 NaN/Inf）不抛不写）。
constexpr std::string_view kMathFamilyOwners[] = {"java/lang/Math", "java/lang/StrictMath"};
constexpr std::string_view kSafeMathNames[] = {
    "abs",  "max",       "min",      "sin",      "cos",           "tan",       "asin",
    "acos", "atan",      "atan2",    "sqrt",     "cbrt",          "ceil",      "floor",
    "rint", "round",     "exp",      "expm1",    "log",           "log10",     "log1p",
    "pow",  "signum",    "copySign", "hypot",    "IEEEremainder", "toRadians", "toDegrees",
    "ulp",  "nextAfter", "nextUp",   "nextDown", "scalb",
};

/// 描述符参数值个数（cat2 参数计一值；数组前缀归并到元素类型）；
/// 畸形描述符返回 -1。
ca::i32 paramValueCount(std::string_view desc)
{
    ca::usize i = 0;
    if (desc.empty() || desc[0] != '(') return -1;
    ++i;
    ca::i32 n = 0;
    while (i < desc.size() && desc[i] != ')') {
        while (i < desc.size() && desc[i] == '[') ++i;
        if (i >= desc.size()) return -1;
        if (desc[i] == 'L') {
            ++i;
            while (i < desc.size() && desc[i] != ';') ++i;
        }
        ++i;
        ++n;
    }
    if (i >= desc.size() || desc[i] != ')') return -1;
    return n;
}

/// 白名单命中时给出期望输入值个数（静态 = 参数值数，虚调用 = 接收者 +
/// 参数值数）；未命中返回 -1。arity 同时是缺失边守卫：输入使用边数少于
/// 期望即存在看不见的来源（未写局部装载等），不可删。
ca::i32 discardableCallArity(const ClassSession& session, const Insn& in)
{
    if (in.op != Op::INVOKESTATIC && in.op != Op::INVOKEVIRTUAL) return -1;
    ClassSession::MemberSem m;
    if (!session.memberOf(in, m)) return -1;
    if (m.kind != ClassSession::MemberSem::Kind::Method
        && m.kind != ClassSession::MemberSem::Kind::InterfaceMethod) {
        return -1;
    }
    bool matched = false;
    for (const DiscardableCall& c : kExactDiscardableCalls) {
        if (in.op == c.op && m.owner == c.owner && m.name == c.name && m.desc == c.desc) {
            matched = true;
            break;
        }
    }
    if (!matched && in.op == Op::INVOKESTATIC) {
        const bool mathOwner = std::find(std::begin(kMathFamilyOwners),
                                         std::end(kMathFamilyOwners),
                                         m.owner)
                               != std::end(kMathFamilyOwners);
        matched = mathOwner
                  && std::find(std::begin(kSafeMathNames), std::end(kSafeMathNames), m.name)
                         != std::end(kSafeMathNames);
    }
    if (!matched) return -1;
    const ca::i32 params = paramValueCount(m.desc);
    if (params < 0) return -1;
    return params + (in.op == Op::INVOKEVIRTUAL ? 1 : 0);
}

/// 链内纯计算生产者的期望弹值个数（arity）；-1 = 不入链。
///
/// LDC 族常量类别需 CP 判定（class/MethodType/MethodHandle 不认领）。
/// arity 同时是缺失边守卫：输入使用边数少于期望即存在看不见的来源
/// （未写局部装载等），不可删。
ca::i32 chainOpArity(const ClassSession& session, const Insn& in)
{
    const auto inRange = [](Op v, Op first, Op last) { return v >= first && v <= last; };
    switch (in.op) {
    case Op::LDC: case Op::LDC_W: case Op::LDC2_W: {
        ca::i32         iv = 0;
        ca::i64         lv = 0;
        float           fv = 0.0F;
        double          dv = 0.0;
        std::string_view sv;
        return method_rewrite_util::intValueOf(session, in, iv)
                       || method_rewrite_util::longValueOf(session, in, lv)
                       || method_rewrite_util::floatValueOf(session, in, fv)
                       || method_rewrite_util::doubleValueOf(session, in, dv)
                       || method_rewrite_util::stringValueOf(session, in, sv)
                   ? 0
                   : -1;
    }
    default: break;
    }
    if (in.op == Op::ACONST_NULL || in.op == Op::BIPUSH || in.op == Op::SIPUSH
        || inRange(in.op, Op::ICONST_M1, Op::ICONST_5) || inRange(in.op, Op::LCONST_0, Op::LCONST_1)
        || inRange(in.op, Op::FCONST_0, Op::FCONST_2) || inRange(in.op, Op::DCONST_0, Op::DCONST_1)) {
        return 0;
    }
    if (inRange(in.op, Op::IADD, Op::DREM)) return 2;
    if (inRange(in.op, Op::INEG, Op::DNEG)) return 1;
    if (inRange(in.op, Op::ISHL, Op::LXOR)) return 2;
    if (inRange(in.op, Op::I2L, Op::I2S)) return 1;
    if (in.op == Op::ARRAYLENGTH || in.op == Op::INSTANCEOF) return 1;
    return -1;
}

/// 链闭包：从种子定义出发，白名单调用（叶子）与「纯计算且全部输入
/// 定义均入链」的生产者递归入链。纯栈上生产者不经局部回传，静态图
/// 无环；visiting 兜底环（防御）。返回定义 → 是否入链的备忘。
std::map<DefId, bool> closeChain(const ClassSession& session, const CodeDom& dom,
                                 const MethodDefUse& du, const std::vector<DefId>& seeds)
{
    std::map<DefId, bool> memo;
    std::set<DefId>       visiting;
    const std::function<bool(const DefId&)> visit = [&](const DefId& d) -> bool {
        const auto hit = memo.find(d);
        if (hit != memo.end()) return hit->second;
        if (!visiting.insert(d).second) return false;
        bool inChain = false;
        if (d.kind == DefId::Kind::Instruction) {
            const InsnId* iid = du.instructionOf(d);
            const Insn*   in  = iid != nullptr ? dom.peek(*iid) : nullptr;
            if (in != nullptr) {
                // 白名单调用与纯计算同构：期望输入值个数 + 全部输入定义
                // 入链（白名单带参条目由此进链，零参条目退化为叶子）。
                ca::i32 arity = discardableCallArity(session, *in);
                if (arity < 0) arity = chainOpArity(session, *in);
                if (arity >= 0 && iid != nullptr) {
                    ca::usize edgeCount = 0;
                    inChain = true;
                    for (const DefUseEdge* e : du.usesAt(*iid)) {
                        if (e->slot != DefUseSlot::Stack) continue;
                        ++edgeCount;
                        if (!visit(e->def)) {
                            inChain = false;
                            break;
                        }
                    }
                    // 输入边少于期望 = 存在无定义来源（看不见即不可删）。
                    if (edgeCount != static_cast<ca::usize>(arity)) inChain = false;
                }
            }
        }
        visiting.erase(d);
        memo[d] = inChain;
        return inChain;
    };
    for (const DefId& d : seeds) visit(d);
    return memo;
}

}   // namespace

mjbase::spi::PreservedAnalyses DeadSideEffectFreeCallRemover::run(mjbase::spi::MethodScope& scope,
                                                                  mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom& dom = scope.code.dom;
    if (dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();
    // 异常表不再整件跳过（原「handler 入口值无定义边」挂账的精细化）：
    // 弹值消费只发生在正常流——异常抛出点栈清空，handler 入口帧由帧引擎
    // 合成为 [exceptionRef]，exceptionRef 无指令定义，以它为种子的窗口在
    // closeChain 处不可入链（负向结论不受异常表影响）。链区间的异常表
    // 语义由两道门保证：区间标签门（try 端点/handler 入口锚定在 pop 或
    // 非头链成员即拒收窗口）+ 应用面 eraseBatch 前置校验（成员被 exc
    // 边界锚定 = 链头形态，整组在任何变更之前拒绝，组级原子无半删）。
    const jbc::analysis::MethodSig sig{
        (scope.access & mjbase::hierarchy::kAccStatic) != 0, scope.desc};
    const MethodDefUse du =
        mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, scope.code, sig);
    if (!du.ok() || du.truncated()) return mjbase::spi::PreservedAnalyses::all();

    const std::vector<InsnId> order = dom.order();
    std::map<ca::u32, ca::usize> idxOf;
    for (ca::usize i = 0; i < order.size(); ++i) idxOf[order[i].slot] = i;

    std::set<ca::u32>                 claimed;
    std::vector<std::vector<InsnId>>  removals;   // 每窗口一个删除组

    for (ca::usize i = 0; i < order.size(); ++i) {
        const InsnId popId = order[i];
        const Insn*  pop   = dom.peek(popId);
        if (pop == nullptr || (pop->op != Op::POP && pop->op != Op::POP2)) continue;
        if (claimed.count(popId.slot) != 0) continue;

        // 弹值定义种子：POP 取 index0；POP2 取 index0/1（cat2 顶值天然
        // 只有 index0 边）。无定义边（看不见来源）直接跳过。
        std::vector<DefId> seeds;
        bool               shapeOk = true;
        for (const DefUseEdge* e : du.usesAt(popId)) {
            if (e->slot != DefUseSlot::Stack) continue;
            if (e->index > 1) {
                shapeOk = false;
                break;
            }
            if (pop->op == Op::POP && e->index != 0) {
                shapeOk = false;
                break;
            }
            seeds.push_back(e->def);
        }
        if (!shapeOk || seeds.empty()) continue;

        std::map<DefId, bool> chain = closeChain(scope.session, dom, du, seeds);
        const auto allSeedsIn = [&]() {
            for (const DefId& d : seeds) {
                const auto hit = chain.find(d);
                if (hit == chain.end() || !hit->second) return false;
            }
            return true;
        };
        if (!allSeedsIn()) continue;

        // 逐出不动点：链成员的全部使用边须指向链内或本 pop；共享消费者
        // 级联逐出（成员被逐后，其输入生产者随之出现集外消费者）。
        std::set<ca::u32> inSlots;
        for (const auto& kv : chain) {
            if (!kv.first.valid()) continue;
            if (!kv.second) continue;
            const InsnId* iid = du.instructionOf(kv.first);
            if (iid != nullptr) inSlots.insert(iid->slot);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& kv : chain) {
                if (!kv.second) continue;
                const InsnId* iid = du.instructionOf(kv.first);
                if (iid == nullptr) continue;
                bool outside = false;
                for (const DefUseEdge* u : du.usesOf(kv.first)) {
                    if (u->use == popId) continue;
                    if (inSlots.count(u->use.slot) == 0) {
                        outside = true;
                        break;
                    }
                }
                if (outside) {
                    chain[kv.first] = false;
                    inSlots.erase(iid->slot);
                    changed = true;
                }
            }
        }
        if (!allSeedsIn()) continue;
        bool hasCall = false;
        for (const auto& kv : chain) {
            if (!kv.second) continue;
            const InsnId* iid = du.instructionOf(kv.first);
            const Insn*   in  = iid != nullptr ? dom.peek(*iid) : nullptr;
            if (in != nullptr && discardableCallArity(scope.session, *in) >= 0) {
                hasCall = true;
                break;
            }
        }
        if (!hasCall) continue;

        // 区间门：[链首, pop] 顺序区间内只允许链成员与 pop，且区间内部
        // （含 pop 前边界）无存活标签——区间外分支不得中途进入链。链
        // 成员必须全部先于 pop（布局 wrap 形态保守拒收）。
        ca::usize headIdx = i;
        ca::usize tailIdx = i;
        bool       wrap    = false;
        for (const InsnId id : order) {
            if (inSlots.count(id.slot) == 0) continue;
            const auto hit = idxOf.find(id.slot);
            if (hit == idxOf.end()) continue;
            if (hit->second < headIdx) headIdx = hit->second;
            if (hit->second > tailIdx) tailIdx = hit->second;
        }
        if (tailIdx > i) wrap = true;
        bool rangeOk = !wrap;
        for (ca::usize j = headIdx; rangeOk && j <= i; ++j) {
            const bool member = j == i || inSlots.count(order[j].slot) != 0;
            if (!member) {
                rangeOk = false;
                break;
            }
            if (j > headIdx && method_rewrite_util::labelsAnchorAt(dom, order[j])) {
                rangeOk = false;
                break;
            }
        }
        if (!rangeOk) continue;

        std::vector<InsnId> group;
        for (ca::usize j = headIdx; j <= i; ++j) group.push_back(order[j]);
        removals.push_back(std::move(group));
        for (const InsnId id : removals.back()) claimed.insert(id.slot);
    }

    if (removals.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 应用：组级原子 eraseBatch——前置全量校验（成员被异常表边界锚定、
    // 批内末位无顺序后继均整组 StillUsed，失败发生在任何变更之前），
    // 校验过单趟重建（批内成员的跳转引用改锚到其后第一条批外存活指令，
    // 链头入跳=链+pop 净零栈效应，直落等价）。被拒窗口保持原样，其余
    // 窗口继续应用。
    bool modified = false;
    jbc::ir::CodeEditor editor(scope.code);
    for (const std::vector<InsnId>& group : removals) {
        if (editor
                .eraseBatch(group,
                            jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                .is_err()) {
            continue;
        }
        modified = true;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
