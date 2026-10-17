/// @file        resolved_call_graph.hpp
/// @brief       层次感知调用图：CHA 虚/接口分派扩展 + 反射织入挂点。
///
/// 移植自 mbase/asm/classpath/resolved_call_graph（CHA 档 + 查询面 + appendEdge
/// 织入挂点）。与旧版的差异：
///
/// - 基座换 mjbase 件：调用点来自 ReferenceIndex（CodeDom order() 坐标的
///   ReferenceSite），层次查询走 mjbase::hierarchy 的 ClassHierarchy/
///   MethodHierarchy（键面 API，无 mbase 的条目下标制）。
/// - 输入层次以 HierarchyBundle 交付（共享驻留池 + 类/方法层次图同生共死）：
///   mjbase MethodHierarchy 的查询键必须经类图绑定的池驻留，束把「池 + 两图」
///   的配对关系收口为单一载体。
/// - RTA 精度档（buildRta）与 .rcg 持久化不在本批范围（旧栈独立测试承载，
///   迁移批次未含其用例；后续批次补），见迁移报告。
///
/// ## 扩展规则（CHA 档）
///
/// 对 ReferenceIndex 的每条方法引用：
///
/// - invokestatic / invokespecial 保持精确编码目标；
/// - invokevirtual / invokeinterface 先把编码目标落解到声明者（编码 owner 自身
///   声明 → 沿 super 链首中即停 → 接口继承闭包首中；环防御经类图自身的环拒绝），
///   再按声明者的 override 等价族扩展；
/// - owner 不在容器或全链无声明、格式错误、外部目标降级为原始编码目标。
///
/// 每条边同时保留编码目标与扩展目标（expanded 标记区分），消费方自行选择
/// 字节码表面或保守运行时目标集口径。
///
/// ## 键契约
///
/// 边文本（site/target）一律驻留进图自有池（appendEdge 时重驻留，随图存活）；
/// 查询面收 MemberKey 与文本三段双形态（文本入参 intern 进图池）。与
/// ReferenceIndex / HierarchyBundle 的互通按文本内容等值（不跨池比 id）。
#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/units.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/method_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace jbc::ir {
class ClassSession;
}   // namespace jbc::ir

namespace mjbase::asm_classpath {

// GAM/PA 别名（mjbase::spi 真身；包装面在模块内统一裸名使用）。
using mjbase::spi::GlobalAnalysisManager;
using mjbase::spi::PreservedAnalyses;

class CallGraph;

/// @brief Dispatch category used when expanding one encoded call site.
///
/// `ResolvedCallGraph` keeps the direct bytecode target and records why extra
/// targets were added. Consumers can use this to distinguish exact static calls
/// from conservative virtual/interface expansion without re-reading bytecode.
enum class ResolvedCallKind
{
    Direct,
    Special,
    Static,
    Virtual,
    Interface,
    Unresolved,
    /// @brief Dynamic edge woven in after build (reflection binding), not
    /// backed by an encoded call-site target. Provenance marker only: build()
    /// never emits this kind, and statistics keep it separate from the static
    /// CHA buckets above.
    Reflect
};

/// @brief One resolved edge from a source method to a possible runtime target.
///
/// `encodedTarget` is the owner/name/descriptor stored in the original invoke
/// instruction; `resolvedTarget` is one declared or external method that may be
/// reached after hierarchy expansion. For static/special/private or unresolved
/// calls the two keys are normally identical. 文本字段驻留图自有池视图。
struct ResolvedCallEdge
{
    mjbase::reference::ReferenceSite site;
    mjbase::reference::MethodRefKey  encodedTarget;
    mjbase::reference::MethodRefKey  resolvedTarget;
    ResolvedCallKind                 kind     = ResolvedCallKind::Unresolved;
    bool                             expanded = false;
};

/// @brief 层次分析束：共享驻留池 + 类/方法层次图的最小配对载体。
///
/// 停泊件：mbase 的 ClassHierarchyAnalysis/MethodHierarchyAnalysis（GAM 包装）
/// 归层次批所有，本批不改其目录；resolved 图需要「查询键驻留池与两图绑定池
/// 同一」的载体，此处给最小实现（build 从容器扫描 + fillSummary 逐类登记，
/// 同名首到先得，登记 Err 的类跳过）。主线裁决点：层次批交付 GAM 包装后，
/// 本束应改为其 Result 载体或被替换（见迁移报告）。
class HierarchyBundle
{
public:
    HierarchyBundle() = default;
    HierarchyBundle(const HierarchyBundle&)            = delete;
    HierarchyBundle& operator=(const HierarchyBundle&) = delete;

    /// @brief 从容器构建：扫描类集合 → 逐类 fillSummary 登记（先到先得）→
    ///        方法层次在类图上构建。
    static std::shared_ptr<const HierarchyBundle> build(
        const mjbase::model::ResourceContainer& container);

    /// @brief 束自有驻留池：查询键经此驻留（内容寻址幂等；图/束存活期有效）。
    KeyPool& pool() const noexcept { return pool_; }

    const mjbase::hierarchy::ClassHierarchy& classHierarchy() const noexcept { return ch_; }
    const mjbase::hierarchy::MethodHierarchy& methodHierarchy() const noexcept { return mh_; }

private:
    mutable mjbase::KeyPool pool_;
    mjbase::hierarchy::ClassHierarchy ch_{pool_};
    mjbase::hierarchy::MethodHierarchy mh_;
};

/// @brief Hierarchy-aware method connectivity view.
///
/// This graph is layered above the direct `CallGraph`. It does not change direct
/// graph semantics; instead it reads `ReferenceIndex` call sites and expands
/// `invokevirtual` / `invokeinterface` targets through `MethodHierarchy`
/// override families. Missing or external targets degrade to the original
/// encoded method key.
class ResolvedCallGraph
{
public:
    /// @brief Rebuild resolved edges from direct references and hierarchy data.
    ///
    /// The direct `CallGraph` parameter is currently unused and kept in the
    /// signature so future policies can choose whether to retain external direct
    /// nodes（与旧版 (void)callGraph 同口径）。
    void build(const mjbase::reference::ReferenceIndex& index, const CallGraph& callGraph,
               const HierarchyBundle& hierarchy);

    /// @brief Remove every edge and lookup table.
    void clear();

    /// @brief Return all resolved outgoing edges emitted by one source method.
    const std::vector<ResolvedCallEdge>& edgesFrom(MemberKey source) const;
    const std::vector<ResolvedCallEdge>& edgesFrom(std::string_view owner, std::string_view name,
                                                   std::string_view desc) const;

    /// @brief Return all resolved incoming edges targeting one possible method.
    const std::vector<ResolvedCallEdge>& edgesTo(MemberKey target) const;
    const std::vector<ResolvedCallEdge>& edgesTo(std::string_view owner, std::string_view name,
                                                 std::string_view desc) const;

    /// @brief Return unique possible targets called by one source method.
    ///
    /// Ordering follows first-seen edge order. Repeated call sites to the same
    /// possible target are collapsed in this convenience view.
    std::vector<mjbase::reference::MethodRefKey> resolvedTargetsFrom(
        MemberKey source) const;
    std::vector<mjbase::reference::MethodRefKey> resolvedTargetsFrom(
        std::string_view owner, std::string_view name, std::string_view desc) const;

    /// @brief Return the reverse resolved-edge transitive closure to one method.
    ///
    /// The result lists methods from which a resolved edge path can reach the
    /// requested target, excluding the target itself. Walk follows
    /// `resolvedTarget` backwards through the call-site sources, so virtual and
    /// interface expansion participate in the closure. Missing targets return an
    /// empty list; cycles are safe. Ordering follows first-seen discovery order.
    std::vector<mjbase::reference::MethodRefKey> reverseReachableTo(
        MemberKey target) const;
    std::vector<mjbase::reference::MethodRefKey> reverseReachableTo(
        std::string_view owner, std::string_view name, std::string_view desc) const;

    /// @brief Append one pre-built edge to the graph.
    ///
    /// Build-time code uses this to register encoded call-site edges. It stays
    /// public so post-build augmentation (the reflection binding weave) can add
    /// `ResolvedCallKind::Reflect` edges without re-running build; the caller
    /// owns dedup and provenance policy. The next build() discards appended
    /// edges. 边文本重驻留进图自有池，调用方缓冲可随即释放。
    void appendEdge(const ResolvedCallEdge& edge);

    /// @brief Return true if a hierarchy-expanded edge exists.
    bool hasResolvedEdge(MemberKey source, MemberKey target) const;
    bool hasResolvedEdge(std::string_view sourceOwner, std::string_view sourceName,
                         std::string_view sourceDesc, std::string_view targetOwner,
                         std::string_view targetName, std::string_view targetDesc) const;

    /// @brief Number of resolved edges, including per-call-site duplicates.
    ca::usize edgeCount() const { return edges_.size(); }

    /// @brief Return every resolved edge in build order.
    ///
    /// Edges are per encoded call site: a virtual/interface site expanded to N
    /// targets contributes N entries. Consumers that need stable output should
    /// sort by (site, resolvedTarget) themselves; the stored order follows
    /// ReferenceIndex scan order and is deterministic per input.
    const std::vector<ResolvedCallEdge>& edges() const { return edges_; }

private:
    mutable mjbase::KeyPool pool_;

    std::vector<ResolvedCallEdge>                                  edges_;
    std::unordered_map<MemberKey, std::vector<ResolvedCallEdge>, MemberKeyHash> edgesBySource_;
    std::unordered_map<MemberKey, std::vector<ResolvedCallEdge>, MemberKeyHash> edgesByTarget_;
};

/// @brief GlobalAnalysisManager 包装：层次束结果（池 + 两图同生共死）。
struct HierarchyBundleAnalysis
{
    using Result = std::shared_ptr<const HierarchyBundle>;

    Result run(mjbase::model::ResourceContainer& container, GlobalAnalysisManager&)
    {
        return HierarchyBundle::build(container);
    }
};

}   // namespace mjbase::asm_classpath

/// @brief Global analysis wrapper for `ResolvedCallGraph`.
// 跨模块包装类型：真身在 mjbase::reference（CallGraphAnalysis 停泊在
// mjbase/reference/global_reachability.hpp）。此处只需类型名（analysisID
// 惰性实例化），前向声明 + using 引入——注意 namespace 定义不可嵌在
// asm_classpath 内（会嵌套出 asm_classpath::mjbase 遮蔽全局名）。
namespace mjbase::reference {
struct ReferenceIndexAnalysis;
struct CallGraphAnalysis;
}   // namespace mjbase::reference

namespace mjbase::asm_classpath {

using mjbase::reference::ReferenceIndexAnalysis;
using mjbase::reference::CallGraphAnalysis;

struct ResolvedCallGraphAnalysis
{
    using Result = ResolvedCallGraph;

    Result run(mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam);

    /// 依赖 ReferenceIndex / CallGraph / 层次束：任一被失效则本图连锁失效。
    static bool dependencyInvalidated(mjbase::model::ResourceContainer& container,
                                      const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ReferenceIndexAnalysis>(container, pa) ||
               inv.invalidate<CallGraphAnalysis>(container, pa) ||
               inv.invalidate<HierarchyBundleAnalysis>(container, pa);
    }
};

}   // namespace mjbase::asm_classpath
