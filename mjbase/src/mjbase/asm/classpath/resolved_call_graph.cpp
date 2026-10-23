#include "mjbase/asm/classpath/resolved_call_graph.hpp"

#include <cassert>
#include <optional>
#include <unordered_set>
#include <utility>

#include "jbc/core/opcode.hpp"

#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/reference/entry_points.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::asm_classpath {

using mjbase::reference::MethodRefKey;
using mjbase::reference::MethodReferenceKind;
using mjbase::reference::ReferenceSite;
using mjbase::reference::TypeReferenceKind;

namespace {

MemberKey toMemberKey(KeyPool& pool, std::string_view owner, std::string_view name,
                      std::string_view desc)
{
    return memberKeyOf(pool, classKeyOf(pool, owner), name, desc);
}

const std::vector<ResolvedCallEdge>& emptyEdges()
{
    static const std::vector<ResolvedCallEdge> empty;
    return empty;
}

ResolvedCallKind classifyOpcode(ca::i32 opcode)
{
    switch (static_cast<jbc::core::Op>(opcode)) {
    case jbc::core::Op::INVOKESTATIC: return ResolvedCallKind::Static;
    case jbc::core::Op::INVOKESPECIAL: return ResolvedCallKind::Special;
    case jbc::core::Op::INVOKEVIRTUAL: return ResolvedCallKind::Virtual;
    case jbc::core::Op::INVOKEINTERFACE: return ResolvedCallKind::Interface;
    default: return ResolvedCallKind::Unresolved;
    }
}

bool shouldExpand(ResolvedCallKind kind)
{
    return kind == ResolvedCallKind::Virtual || kind == ResolvedCallKind::Interface;
}

/// @brief 把调用点编码目标落解到声明者（键面重写；旧栈 resolveHierarchyMethod
/// 三级口径照迁）：
///
/// 1. 精确：编码 owner 自身声明（override 等价族非空即已声明，含非虚单例）；
/// 2. 继承落边：owner 是容器内已知类型但未声明该方法时，沿 super 链首中即停
///    （ancestors 自反序），类链无命中再沿接口继承闭包首中。子类型静态接收者
///    直调与子接口未重声明由此落真实声明者；
/// 3. 缺失：owner 不在层次图或全链无声明 → nullopt，调用方降级编码目标。
std::optional<MemberKey> resolveDeclarationOwner(const mjbase::hierarchy::ClassHierarchy& ch,
                                                 const mjbase::hierarchy::MethodHierarchy& mh,
                                                 KeyPool&                pool,
                                                 const MethodRefKey&     target)
{
    const mjbase::ClassKey owner = ch.findClass(target.owner);
    if (!owner.valid()) return std::nullopt;

    auto declaredHere = [&](mjbase::ClassKey cls) -> std::optional<MemberKey> {
        MemberKey m = memberKeyOf(pool, cls, target.name, target.desc);
        return mh.overrideFamily(m).empty() ? std::nullopt : std::optional<MemberKey>(m);
    };

    // 父类链：首中即停（ancestors 自反，最近在前）。
    for (const mjbase::ClassKey cur : ch.ancestors(owner)) {
        if (auto hit = declaredHere(cur)) return hit;
    }
    // 接口继承链：超类已全部落空，剩余命中必为接口条目。
    for (const mjbase::ClassKey iface : ch.interfaceClosure(owner)) {
        if (auto hit = declaredHere(iface)) return hit;
    }
    return std::nullopt;
}

void appendUnique(std::vector<MethodRefKey>& targets, const MethodRefKey& target)
{
    for (const auto& existing : targets) {
        if (existing.owner == target.owner && existing.name == target.name &&
            existing.desc == target.desc) {
            return;
        }
    }
    targets.push_back(target);
}

}   // namespace

// ── HierarchyBundle ──────────────────────────────────────────

std::shared_ptr<const HierarchyBundle> HierarchyBundle::build(
    const mjbase::model::ResourceContainer& container)
{
    auto bundle = std::shared_ptr<HierarchyBundle>(new HierarchyBundle());
    const auto scanned = mjbase::analysis::scanClassResources(container);
    for (const auto& scannedClass : scanned) {
        const mjbase::hierarchy::ClassSummary summary =
            mjbase::hierarchy::fillSummary(*scannedClass.session, bundle->pool_);
        // 同名首到先得（与 ReferenceIndex 的 first-seen-wins 同口径）；登记
        // Err（重复/环）的类跳过，不拖垮束。
        (void)bundle->ch_.addClass(summary);
    }
    bundle->mh_.build(bundle->ch_);
    return bundle;
}

// ── ResolvedCallGraph ────────────────────────────────────────

void ResolvedCallGraph::clear()
{
    // 池只增不减（内容寻址驻留）：仅清边表，文本驻留随图存活期复用。
    edges_.clear();
    edgesBySource_.clear();
    edgesByTarget_.clear();
    instantiatedClasses_.clear();
    rtaInstantiatedCount_    = 0;
    rtaReachableMethodCount_ = 0;
}

void ResolvedCallGraph::appendEdge(const ResolvedCallEdge& edge)
{
    // 边文本重驻留进图自有池：调用方（织入端/构建端）缓冲可随即释放。
    ResolvedCallEdge stored;
    stored.site.sourceClass  = pool_.str(pool_.intern(edge.site.sourceClass));
    stored.site.sourceMethod = pool_.str(pool_.intern(edge.site.sourceMethod));
    stored.site.sourceDesc   = pool_.str(pool_.intern(edge.site.sourceDesc));
    stored.site.opcode       = edge.site.opcode;
    stored.site.insnIndex    = edge.site.insnIndex;
    stored.encodedTarget     = MethodRefKey{pool_.str(pool_.intern(edge.encodedTarget.owner)),
                                pool_.str(pool_.intern(edge.encodedTarget.name)),
                                pool_.str(pool_.intern(edge.encodedTarget.desc))};
    stored.resolvedTarget    = MethodRefKey{pool_.str(pool_.intern(edge.resolvedTarget.owner)),
                                 pool_.str(pool_.intern(edge.resolvedTarget.name)),
                                 pool_.str(pool_.intern(edge.resolvedTarget.desc))};
    stored.kind              = edge.kind;
    stored.expanded          = edge.expanded;

    edges_.push_back(stored);
    edgesBySource_[toMemberKey(pool_, stored.site.sourceClass, stored.site.sourceMethod,
                               stored.site.sourceDesc)]
        .push_back(stored);
    edgesByTarget_[toMemberKey(pool_, stored.resolvedTarget.owner, stored.resolvedTarget.name,
                               stored.resolvedTarget.desc)]
        .push_back(stored);
}

void ResolvedCallGraph::build(const mjbase::reference::ReferenceIndex& index,
                              const CallGraph& callGraph, const HierarchyBundle& hierarchy)
{
    (void)callGraph;
    clear();
    const mjbase::hierarchy::ClassHierarchy&    ch = hierarchy.classHierarchy();
    const mjbase::hierarchy::MethodHierarchy&   mh = hierarchy.methodHierarchy();
    KeyPool& hierarchyPool = hierarchy.pool();

    for (const auto& ref : index.methodReferences()) {
        const ResolvedCallKind    kind = classifyOpcode(ref.site.opcode);
        std::vector<MethodRefKey> targets;

        if (shouldExpand(kind)) {
            if (auto decl = resolveDeclarationOwner(ch, mh, hierarchyPool, ref.target)) {
                for (const MemberKey member : mh.overrideFamily(*decl)) {
                    MethodRefKey target;
                    target.owner = pool_.str(pool_.intern(ch.nameOf(mjbase::ClassKey{member.cls})));
                    target.name  = pool_.str(pool_.intern(hierarchyPool.str(member.name)));
                    target.desc  = pool_.str(pool_.intern(hierarchyPool.str(member.desc)));
                    appendUnique(targets, target);
                }
            }
        }

        if (targets.empty()) {
            MethodRefKey encoded;
            encoded.owner = pool_.str(pool_.intern(ref.target.owner));
            encoded.name  = pool_.str(pool_.intern(ref.target.name));
            encoded.desc  = pool_.str(pool_.intern(ref.target.desc));
            targets.push_back(encoded);
        }

        for (const auto& target : targets) {
            ResolvedCallEdge edge;
            edge.site.sourceClass  = pool_.str(pool_.intern(ref.site.sourceClass));
            edge.site.sourceMethod = pool_.str(pool_.intern(ref.site.sourceMethod));
            edge.site.sourceDesc   = pool_.str(pool_.intern(ref.site.sourceDesc));
            edge.site.opcode       = ref.site.opcode;
            edge.site.insnIndex    = ref.site.insnIndex;
            edge.encodedTarget     = MethodRefKey{pool_.str(pool_.intern(ref.target.owner)),
                                      pool_.str(pool_.intern(ref.target.name)),
                                      pool_.str(pool_.intern(ref.target.desc))};
            edge.resolvedTarget    = target;
            edge.kind              = kind;
            edge.expanded          = !(target.owner == ref.target.owner &&
                              target.name == ref.target.name && target.desc == ref.target.desc);
            appendEdge(edge);
        }
    }
}

// ── RTA（rapid type analysis）精度档 ─────────────────────────────
//
// 入口可达性驱动 + 已实例化类型集合收窄虚分派。两个阶段（语义与旧栈当前实现
// 对齐）：
//
// 1. fixpoint：worklist 闭包同时增长「可达方法集」与「已实例化类型集」。
//    可达方法体内的 `new T`（TypeInstruction + NEW）激活 T；T 激活时重估组件
//    条目 owner 落在 T 祖先闭包上的全部已询问虚/接口调用点（晚发现的实例化
//    不丢边）。虚/接口调用点的可行 dispatch 口径是 canonical RTA 的方法解析：
//    从已实例化类型 T 沿父类链向上找第一个拥有该组件条目的类，运行时 dispatch
//    即落在该类名下的组件条目——子类继承父类具体方法且不覆写时执行的是父类
//    条目，故祖先名下条目可行（继承未覆写不再欠近似）；类链命中即停，子类
//    覆写时父类条目不可达（「父类已实例化」场景行为不变）；类链无命中且走满
//    到 java/lang/Object 时组件中的接口条目可行（接口 default 方法经
//    superinterface 继承，同类根因）；类链因其余悬挂尾部（容器外缺失父类）
//    截断时解析不可知，保守放行组件全部条目（宁过近似不丢真实执行边）。
//    组件在册但整个闭包零实例化时调用点零边，不回退编码目标——相对 CHA 的
//    精度收益来源。外部/缺失虚目标照 CHA 降级为编码目标。
//    在 per-root feasible 之上另有调用点粒度接收者静态类型门控
//    （siteReceiverAllows，站点级健全判据）：保留边 iff ∃ 已实例化类型 S：
//    S <: R（调用点接收者声明类型）且 owner ∈ ancestors(S)，另保留 R <: owner
//    保守补遗（子类型静态接收者直调/接口 default 经祖先条目），容器外类型
//    fail-open。「具体父类 × 无关接口」菱形的真实执行边由此不丢（宁过近似
//    不丢真实执行边）。挂起 enqueue 与物化两条路径同口径；实例化闭包传播
//    机制不受影响。
// 2. 物化：按可达方法发现序 × 引用扫描序产出确定性边表；虚/接口组件按最终
//    可行 dispatch 集过滤后逐目标出边（expanded 标记与 CHA 同口径），不可达
//    方法的边整体缺席。
void ResolvedCallGraph::buildRta(const mjbase::reference::ReferenceIndex& index,
                                 const CallGraph& callGraph, const HierarchyBundle& hierarchy,
                                 const RtaOptions& options)
{
    (void)callGraph;
    clear();

    const mjbase::hierarchy::ClassHierarchy&  ch = hierarchy.classHierarchy();
    const mjbase::hierarchy::MethodHierarchy& mh = hierarchy.methodHierarchy();
    KeyPool& hierarchyPool = hierarchy.pool();

    // ── fixpoint 状态 ──
    std::unordered_set<mjbase::MemberKey, mjbase::MemberKeyHash> reachableKeys;
    std::vector<MethodRefKey>                                    reachableOrder;
    std::vector<MethodRefKey>                                    worklist;
    std::unordered_set<std::string_view>                         instantiatedSet;
    // 已实例化类型集合（发现序）：文本重驻留进图自有池——本图死亡前视图稳定，
    // keep_roots 的 applyRtaInstantiatedRoots 直接消费。
    std::vector<std::string_view>& instantiated = instantiatedClasses_;
    // instantiated 的 ClassKey 镜像（同序追加）：接收者门控逐 S 查询用，免去
    // 门控热路径上对已实例化类型反复按名 findClass；外部类型记 invalid
    // ClassKey，由门控跳过（外部实例的分派链全在外部，见 closureOf 口径）。
    std::vector<mjbase::ClassKey> instantiatedKeys;

    // 覆盖族（override family）的询价状态：族条目、owner 集与「可行 dispatch
    // owner」集。feasible 的增长口径是方法解析（见上方 RTA 注释）：实例化类型
    // 沿父类链命中的第一个组件 owner；接口成员与悬挂截断按注释口径放行。族由
    // MethodHierarchy 按覆写关系闭包，同一族的全部调用点共享一份（键取族内
    // 最浅声明 sourceDeclaration，跨调用点稳定）。
    struct RootState
    {
        std::vector<MethodRefKey>            component;
        std::unordered_set<std::string_view> ownerSet;
        std::unordered_set<std::string_view> feasible;
        ca::usize                            folded = 0;   ///< 已折入的 instantiated 前缀
    };
    std::unordered_map<mjbase::MemberKey, RootState, mjbase::MemberKeyHash> rootStates;
    std::unordered_map<std::string_view, std::vector<mjbase::MemberKey>>    rootsByOwner;

    auto enqueue = [&](const MethodRefKey& method) {
        if (method.owner.empty() || method.name.empty() || method.desc.empty()) return;
        const mjbase::MemberKey key =
            mjbase::memberKeyOf(hierarchyPool, mjbase::classKeyOf(hierarchyPool, method.owner),
                                method.name, method.desc);
        if (!reachableKeys.insert(key).second) return;
        reachableOrder.push_back(method);
        worklist.push_back(method);
    };

    // 类链解析：从 type 沿 super 链向上找第一个 ownerSet 命中类。
    // second = 是否走满（终止于 java/lang/Object）；未走满说明父链有其他悬挂
    // 尾部（容器外缺失父类），解析结果不可知，调用方须保守放行。Object 判定
    // 按链尾名字等价——旧栈未登记 Object 时自动合成补位条目、以 objectIndex
    // 为链终点；mjbase 类图不自动合成，未登记的 Object 以悬挂键出现在链尾，
    // 名字即身份。
    auto resolveClassChain = [&](std::string_view type,
                                 const std::unordered_set<std::string_view>& ownerSet) {
        constexpr std::string_view kObjectName = "java/lang/Object";
        std::pair<std::string_view, bool> result{"", false};
        const mjbase::ClassKey start = ch.findClass(type);
        if (!start.valid()) return result;
        const std::vector<mjbase::ClassKey> chain = ch.ancestors(start);
        for (const mjbase::ClassKey cur : chain) {
            const std::string_view name = ch.nameOf(cur);
            if (ownerSet.count(name) != 0) return std::pair<std::string_view, bool>{name, true};
        }
        result.second = !chain.empty() && ch.nameOf(chain.back()) == kObjectName;
        return result;
    };

    // landed owner 名下的组件条目补入闭包（继承未覆写的目标由此进边）。
    //
    // 口径固化（自旧栈评审）：此处不受调用点接收者静态类型门控——可达方法集
    // 按落点 owner 全量入闭包，是偏保守的有意设计（门控仅收窄边表的目标选取，
    // 不收缩 worklist，从而不影响实例化发现）。边表收窄后会出现「方法可达但无
    // 任何入边」的表里差异，属设计选择，勿当缺陷修。
    auto enqueueFeasible = [&](RootState& rs, std::string_view owner) {
        for (const auto& target : rs.component) {
            if (target.owner == owner) enqueue(target);
        }
    };

    // T 的祖先闭包（ClassKey id 集）：super 链（含自身）+ 接口闭包——旧栈
    // ancestors() 单查询即含接口；mjbase 两查询拆分，此处拼合同一口径。外部
    // 类型返回空集——外部实例的分派链全在外部，落不到容器声明条目（外部实例
    // 化不激活任何组件）。
    auto closureOf = [&](std::string_view type) {
        std::unordered_set<ca::u32> closure;
        const mjbase::ClassKey tKey = ch.findClass(type);
        if (!tKey.valid()) return closure;
        closure.insert(tKey.id);
        for (const mjbase::ClassKey ancestor : ch.ancestors(tKey)) closure.insert(ancestor.id);
        for (const mjbase::ClassKey iface : ch.interfaceClosure(tKey)) closure.insert(iface.id);
        return closure;
    };

    // 调用点粒度接收者静态类型门控（站点级健全判据）。per-root feasible 按
    // 组件共享，"接收者静态类型没有任何已实例化子类型"的调用点也会经共享
    // feasible 落出与接收者无关 owner 的边；落目标前按站点逐边裁决，保留边
    // iff：
    //
    //   1. ∃ 已实例化类型 S：S <: R（调用点接收者声明类型）且 owner ∈
    //      ancestors(S)（ancestors 自含，即 S <: owner）。这是健全判据：运行时
    //      接收者必为某已实例化 S 的实例且同时是 R 的子型，而分派只能落在该
    //      实例祖先闭包内的组件条目上——「具体父类 × 无关接口」菱形（owner 与
    //      R 互不为子型、但经已实例化 S 的具体父类链可达，JVMS 5.4.5 具体父类
    //      方法优先）由此保留。成立的是更强的不变量：不丢任何真实执行边。
    //   2. R <: owner（保守补遗）：接收者静态类型本身是 owner 子型时，任何
    //      运行时接收者（R 的子型）的祖先闭包都含 owner——调用点真执行时
    //      分派可落 owner，即便当前尚无 R 的已实例化子型。过近似方向的安全
    //      放宽。
    //
    // 任一类型不在容器（外部/缺失）fail-open 维持原口径不收窄。裁决作用于
    // 挂起 enqueue 与物化两条路径（同口径）：中间快照可能存在瞬态掉边，物化
    // 按终态已实例化集重门控，终局边表健全。enqueueFeasible 不过门控，可达
    // 方法集不受扰；实例化闭包传播机制不受影响。纯层级查询 +
    // instantiatedKeys 顺序扫描，结果与扫描序无关，确定性保持（同输入同边表）。
    auto siteReceiverAllows = [&](std::string_view receiverType, std::string_view owner) {
        if (receiverType == owner) return true;
        const mjbase::ClassKey receiverKey = ch.findClass(receiverType);
        const mjbase::ClassKey ownerKey    = ch.findClass(owner);
        if (!receiverKey.valid() || !ownerKey.valid()) return true;
        if (ch.isSubtype(receiverKey, ownerKey)) return true;
        for (const mjbase::ClassKey sKey : instantiatedKeys) {
            if (!sKey.valid()) continue;
            if (ch.isSubtype(sKey, receiverKey) && ch.isSubtype(sKey, ownerKey)) return true;
        }
        return false;
    };

    // 用单个已实例化类型重估一个 root 的可行 dispatch 集（三口径见 RTA 注释）。
    // closure 为 T 的祖先闭包（含自身）；空集（外部类型）直接跳过。
    auto evaluateResolution = [&](RootState& rs, std::string_view type,
                                  const std::unordered_set<ca::u32>& closure) {
        if (closure.empty()) return;
        const auto [landed, complete] = resolveClassChain(type, rs.ownerSet);
        if (!landed.empty()) {
            if (rs.feasible.insert(landed).second) enqueueFeasible(rs, landed);
            return;
        }
        if (!complete) {
            // 悬挂尾部截断：解析不可知，保守放行组件全部条目。
            for (const auto& target : rs.component) {
                if (rs.feasible.insert(target.owner).second) enqueue(target);
            }
            return;
        }
        // 类链无声明且走满：组件中 T 祖先闭包内的接口条目可行——接口 default
        // 方法经 superinterface 继承且容器类未覆写时，运行时执行的就是接口
        // 条目。（T 未实现的组件接口不可能承接其分派，不得放行；多接口菱形
        // 下不区分 max-specific，闭包内保守全收。）
        for (const auto& target : rs.component) {
            const mjbase::ClassKey ownerKey = ch.findClass(target.owner);
            if (!ownerKey.valid() || !ch.isInterface(ownerKey)) continue;
            if (closure.count(ownerKey.id) == 0) continue;
            if (rs.feasible.insert(target.owner).second) enqueue(target);
        }
    };

    // 折入 instantiated[rs.folded..]：首次询问为全量，其后为增量。
    //
    // 不变量（自旧栈评审固化）：循环体内 evaluateResolution → enqueueFeasible
    // → enqueue 链只增长可达集/worklist，绝不触发 instantiate——instantiate
    // 仅由 process() 的类型引用扫描调用（与 foldRoot 无调用边），物化阶段更是
    // fixpoint 结束后 instantiated 已封口。故循环内不增长：end 在进入前快照
    // 即可。debug 断言固化该推理——若未来重构在 evaluateResolution 链上引入
    // 实例化，调试构建立即失火而不是静默漏折。
    auto foldRoot = [&](RootState& rs) {
        const ca::usize end = instantiated.size();
        for (ca::usize i = rs.folded; i < end; ++i) {
            evaluateResolution(rs, instantiated[i], closureOf(instantiated[i]));
            assert(instantiated.size() == end &&
                   "foldRoot 迭代期间 instantiated 不得增长（见上不变量）");
        }
        rs.folded = end;
    };

    auto instantiate = [&](std::string_view type) {
        if (type.empty()) return;
        const ca::u32 id = pool_.intern(type);
        const std::string_view pooled = pool_.str(id);
        if (!instantiatedSet.insert(pooled).second) return;
        instantiated.push_back(pooled);
        // 门控镜像（同序追加，含外部类型的 invalid 占位，见上声明处）。
        instantiatedKeys.push_back(ch.findClass(pooled));

        // 候选 root：组件条目 owner 落在 T 的祖先闭包（super 链 + 接口闭包）
        // 内。owner 全在闭包外的 root 不可能被 T 的实例 dispatch 命中，无需
        // 重估。收集按 ancestors()/interfaceClosure() 链序（确定性），不按闭包
        // 集迭代序——候选评估序进入 enqueue 次序，须保持边表确定性。外部类型
        // 不在层次图内：容器组件条目 owner 均为容器内声明类，无候选可激活。
        auto closure = closureOf(pooled);
        if (closure.empty()) return;

        std::vector<mjbase::MemberKey>                               candidates;
        std::unordered_set<mjbase::MemberKey, mjbase::MemberKeyHash> seenRoots;
        auto collect = [&](std::string_view owner) {
            auto it = rootsByOwner.find(owner);
            if (it == rootsByOwner.end()) return;
            for (const mjbase::MemberKey root : it->second) {
                if (seenRoots.insert(root).second) candidates.push_back(root);
            }
        };
        collect(pooled);
        const mjbase::ClassKey tKey = ch.findClass(pooled);
        for (const mjbase::ClassKey ancestor : ch.ancestors(tKey)) {
            collect(ch.nameOf(ancestor));
        }
        for (const mjbase::ClassKey iface : ch.interfaceClosure(tKey)) {
            collect(ch.nameOf(iface));
        }

        for (const mjbase::MemberKey root : candidates) {
            auto rsIt = rootStates.find(root);
            if (rsIt == rootStates.end()) continue;
            evaluateResolution(rsIt->second, pooled, closure);
            rsIt->second.folded = instantiated.size();
        }
    };

    auto process = [&](const MethodRefKey& method) {
        for (const auto& tref : index.typeReferencesFrom(method.owner, method.name, method.desc)) {
            if (tref.kind == mjbase::reference::TypeReferenceKind::TypeInstruction &&
                tref.site.opcode == static_cast<ca::i32>(jbc::core::Op::NEW)) {
                instantiate(tref.targetType);
            }
        }

        for (const auto& ref : index.methodReferencesFrom(method.owner, method.name, method.desc)) {
            // 空描述符反射查找按名落解，口径与 GlobalReachability 对齐。
            if (ref.target.desc.empty()) {
                if (ref.kind == mjbase::reference::MethodReferenceKind::Reflection ||
                    ref.kind == mjbase::reference::MethodReferenceKind::ReflectionExecution) {
                    for (const auto& declaration : index.declaredMethodsIn(ref.target.owner)) {
                        if (declaration.method.name == ref.target.name) {
                            enqueue(declaration.method);
                        }
                    }
                }
                continue;
            }

            const ResolvedCallKind kind = classifyOpcode(ref.site.opcode);
            if (!shouldExpand(kind)) {
                enqueue(ref.target);
                continue;
            }

            // 编码 owner 未声明时沿类链/接口链落真实声明者（与 CHA 档同源），
            // RTA 侧照旧受实例化集约束；owner 不在容器/全链无声明维持降级。
            const std::optional<mjbase::MemberKey> decl =
                resolveDeclarationOwner(ch, mh, hierarchyPool, ref.target);
            if (!decl.has_value()) {
                enqueue(ref.target);   // 外部/缺失目标：降级精确边
                continue;
            }

            const mjbase::MemberKey rootId = mh.sourceDeclaration(*decl);
            auto [rsIt, inserted]          = rootStates.try_emplace(rootId);
            if (inserted) {
                for (const mjbase::MemberKey member : mh.overrideFamily(*decl)) {
                    MethodRefKey target;
                    target.owner = ch.nameOf(mjbase::ClassKey{member.cls});
                    target.name  = hierarchyPool.str(member.name);
                    target.desc  = hierarchyPool.str(member.desc);
                    rsIt->second.component.push_back(target);
                    rsIt->second.ownerSet.insert(target.owner);
                    rootsByOwner[target.owner].push_back(rootId);
                }
            }
            foldRoot(rsIt->second);
            for (const auto& target : rsIt->second.component) {
                if (rsIt->second.feasible.count(target.owner) == 0) continue;
                // 接收者静态类型门控：与物化同口径。
                if (!siteReceiverAllows(ref.target.owner, target.owner)) continue;
                enqueue(target);
            }
        }
    };

    // 根集合：显式 rootMethods + 可选 main 发现。
    for (const MethodRefKey& root : options.rootMethods) {
        enqueue(root);
    }
    if (options.includeMainMethods) {
        for (const auto& decl : index.methodDeclarations()) {
            if (mjbase::reference::isJavaMainMethod(decl)) enqueue(decl.method);
        }
    }

    for (ca::usize cursor = 0; cursor < worklist.size(); ++cursor) {
        // 值快照：process 内 enqueue 会让 worklist 重分配，引用传参在增长后
        // 即悬垂（旧栈 ASan 实证 UAF 的同款修复）；三个 string_view 的拷贝
        // 成本可忽略，fixpoint 语义不变。
        const MethodRefKey current = worklist[cursor];
        process(current);
    }
    rtaInstantiatedCount_    = instantiated.size();
    rtaReachableMethodCount_ = reachableOrder.size();

    // ── 物化：按可达发现序 × 引用扫描序产出边表 ──
    for (const auto& source : reachableOrder) {
        for (const auto& ref : index.methodReferencesFrom(source.owner, source.name, source.desc)) {
            const ResolvedCallKind    kind = classifyOpcode(ref.site.opcode);
            std::vector<MethodRefKey> targets;

            if (shouldExpand(kind)) {
                // 与 fixpoint 同一解析口径：编码 owner 未声明时沿链落真实
                // 声明者；同一可达方法集的同一引用必解析出同一 root，故
                // fixpoint 对该 root 必已询问，find 不到按零可行口径处理。
                const std::optional<mjbase::MemberKey> decl =
                    resolveDeclarationOwner(ch, mh, hierarchyPool, ref.target);
                if (!decl.has_value()) {
                    MethodRefKey encoded;
                    encoded.owner = pool_.str(pool_.intern(ref.target.owner));
                    encoded.name  = pool_.str(pool_.intern(ref.target.name));
                    encoded.desc  = pool_.str(pool_.intern(ref.target.desc));
                    targets.push_back(encoded);   // 外部/缺失：降级编码目标
                }
                else {
                    const mjbase::MemberKey rootId = mh.sourceDeclaration(*decl);
                    auto                rsIt = rootStates.find(rootId);
                    if (rsIt != rootStates.end()) {
                        foldRoot(rsIt->second);
                        for (const auto& target : rsIt->second.component) {
                            if (rsIt->second.feasible.count(target.owner) == 0) continue;
                            // 接收者静态类型门控：与挂起路径同口径。
                            if (!siteReceiverAllows(ref.target.owner, target.owner)) continue;
                            appendUnique(targets, target);
                        }
                    }
                    // 组件在册但零可行 dispatch（闭包零实例化）：零边
                    //（RTA 精度语义，不回退编码目标）。
                }
            }
            else {
                MethodRefKey encoded;
                encoded.owner = pool_.str(pool_.intern(ref.target.owner));
                encoded.name  = pool_.str(pool_.intern(ref.target.name));
                encoded.desc  = pool_.str(pool_.intern(ref.target.desc));
                targets.push_back(encoded);
            }

            for (const auto& target : targets) {
                ResolvedCallEdge edge;
                edge.site.sourceClass  = pool_.str(pool_.intern(ref.site.sourceClass));
                edge.site.sourceMethod = pool_.str(pool_.intern(ref.site.sourceMethod));
                edge.site.sourceDesc   = pool_.str(pool_.intern(ref.site.sourceDesc));
                edge.site.opcode       = ref.site.opcode;
                edge.site.insnIndex    = ref.site.insnIndex;
                edge.encodedTarget     = MethodRefKey{pool_.str(pool_.intern(ref.target.owner)),
                                          pool_.str(pool_.intern(ref.target.name)),
                                          pool_.str(pool_.intern(ref.target.desc))};
                edge.resolvedTarget    = MethodRefKey{pool_.str(pool_.intern(target.owner)),
                                                   pool_.str(pool_.intern(target.name)),
                                                   pool_.str(pool_.intern(target.desc))};
                edge.kind              = kind;
                edge.expanded          = !(target.owner == ref.target.owner &&
                                 target.name == ref.target.name &&
                                 target.desc == ref.target.desc);
                appendEdge(edge);
            }
        }
    }
}

const std::vector<ResolvedCallEdge>& ResolvedCallGraph::edgesFrom(MemberKey source) const
{
    const auto it = edgesBySource_.find(source);
    return it == edgesBySource_.end() ? emptyEdges() : it->second;
}

const std::vector<ResolvedCallEdge>& ResolvedCallGraph::edgesFrom(std::string_view owner,
                                                                  std::string_view name,
                                                                  std::string_view desc) const
{
    return edgesFrom(toMemberKey(pool_, owner, name, desc));
}

const std::vector<ResolvedCallEdge>& ResolvedCallGraph::edgesTo(MemberKey target) const
{
    const auto it = edgesByTarget_.find(target);
    return it == edgesByTarget_.end() ? emptyEdges() : it->second;
}

const std::vector<ResolvedCallEdge>& ResolvedCallGraph::edgesTo(std::string_view owner,
                                                                std::string_view name,
                                                                std::string_view desc) const
{
    return edgesTo(toMemberKey(pool_, owner, name, desc));
}

std::vector<MethodRefKey> ResolvedCallGraph::resolvedTargetsFrom(MemberKey source) const
{
    std::vector<MethodRefKey> targets;
    for (const auto& edge : edgesFrom(source)) {
        appendUnique(targets, edge.resolvedTarget);
    }
    return targets;
}

std::vector<MethodRefKey> ResolvedCallGraph::resolvedTargetsFrom(std::string_view owner,
                                                                 std::string_view name,
                                                                 std::string_view desc) const
{
    return resolvedTargetsFrom(toMemberKey(pool_, owner, name, desc));
}

std::vector<MethodRefKey> ResolvedCallGraph::reverseReachableTo(MemberKey target) const
{
    std::unordered_set<MemberKey, MemberKeyHash> visited;
    visited.insert(target);
    std::vector<MemberKey>    queue;
    std::vector<MethodRefKey> result;
    queue.push_back(target);

    ca::usize cursor = 0;
    while (cursor < queue.size()) {
        const MemberKey current = queue[cursor++];
        const auto      it      = edgesByTarget_.find(current);
        if (it == edgesByTarget_.end()) continue;
        for (const auto& edge : it->second) {
            const MemberKey sourceKey = toMemberKey(pool_, edge.site.sourceClass,
                                                    edge.site.sourceMethod, edge.site.sourceDesc);
            if (!visited.insert(sourceKey).second) continue;
            result.push_back(MethodRefKey{edge.site.sourceClass, edge.site.sourceMethod,
                                          edge.site.sourceDesc});
            queue.push_back(sourceKey);
        }
    }
    return result;
}

std::vector<MethodRefKey> ResolvedCallGraph::reverseReachableTo(std::string_view owner,
                                                                std::string_view name,
                                                                std::string_view desc) const
{
    return reverseReachableTo(toMemberKey(pool_, owner, name, desc));
}

bool ResolvedCallGraph::hasResolvedEdge(MemberKey source, MemberKey target) const
{
    for (const auto& edge : edgesFrom(source)) {
        if (memberKeyOf(pool_, classKeyOf(pool_, edge.resolvedTarget.owner),
                        edge.resolvedTarget.name, edge.resolvedTarget.desc) == target) {
            return true;
        }
    }
    return false;
}

bool ResolvedCallGraph::hasResolvedEdge(std::string_view sourceOwner, std::string_view sourceName,
                                        std::string_view sourceDesc, std::string_view targetOwner,
                                        std::string_view targetName,
                                        std::string_view targetDesc) const
{
    return hasResolvedEdge(toMemberKey(pool_, sourceOwner, sourceName, sourceDesc),
                           toMemberKey(pool_, targetOwner, targetName, targetDesc));
}

// ── GAM 包装 ─────────────────────────────────────────────────

ResolvedCallGraphAnalysis::Result ResolvedCallGraphAnalysis::run(
    mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam)
{
    auto& index    = gam.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);
    auto& directGraph = gam.getResult<CallGraphAnalysis>(container);
    auto& hierarchy = gam.getResult<HierarchyBundleAnalysis>(container);

    ResolvedCallGraph graph;
    graph.build(index, directGraph, *hierarchy);
    return graph;
}

// ── RTA 档 GAM 包装（opt-in：注册本类型才产生 RTA 成本）────────

RtaCallGraphAnalysis::Result RtaCallGraphAnalysis::run(
    mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam)
{
    auto& index       = gam.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);
    auto& directGraph = gam.getResult<CallGraphAnalysis>(container);
    auto& hierarchy   = gam.getResult<HierarchyBundleAnalysis>(container);

    ResolvedCallGraph graph;
    RtaOptions        options;   // 默认根策略：main 发现开，无显式根。
    graph.buildRta(index, directGraph, *hierarchy, options);
    return graph;
}

}   // namespace mjbase::asm_classpath
