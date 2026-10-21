#include "mjbase/asm/classpath/resolved_call_graph.hpp"

#include <optional>
#include <unordered_set>

#include "jbc/core/opcode.hpp"

#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/hierarchy/summary.hpp"
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

/// @brief 把调用点编码目标落解到声明者（键面重写；）：
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

}   // namespace mjbase::asm_classpath
