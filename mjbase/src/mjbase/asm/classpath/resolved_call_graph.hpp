/// @file        resolved_call_graph.hpp
/// @brief       层次感知调用图：CHA 虚/接口分派扩展 + RTA 精度档 + 反射织入挂点。
///
/// 移植自旧栈同名件（CHA 档 + 查询面 + appendEdge 织入挂点 + RTA 精度档；
/// .rcg 持久化不迁，旧栈独立测试承载，后续批次补）。与旧版的差异：
///
/// - 基座换 mjbase 件：调用点来自 ReferenceIndex（CodeDom order() 坐标的
///   ReferenceSite），层次查询走 mjbase::hierarchy 的 ClassHierarchy/
///   MethodHierarchy（键面 API，无旧栈的条目下标制）。
/// - 输入层次以 HierarchyBundle 交付（共享驻留池 + 类/方法层次图同生共死）：
///   mjbase MethodHierarchy 的查询键必须经类图绑定的池驻留，束把「池 + 两图」
///   的配对关系收口为单一载体。
/// - RTA 档（buildRta）语义与旧栈当前实现对齐，两处口径适配 mjbase 类图：
///   类链「走满」判定（旧栈未登记 java/lang/Object 时自动合成补位条目并以
///   objectIndex 为链终点；mjbase 不自动合成，按链尾名字等价判定，其余悬挂
///   尾部仍按「解析不可知」保守放行组件全部条目）；方法层次的组件经
///   overrideFamily / sourceDeclaration 键面表达（旧栈为并查集下标），祖先
///   闭包由 super 链 + 接口闭包两查询拼合（旧栈 ancestors() 单查询含接口）。
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
/// ## 扩展规则（RTA 档）
///
/// `buildRta()` 在 CHA 之上提供 RTA（rapid type analysis）精度档，语义与旧栈
/// 当前实现对齐：
///
/// - 入口可达性驱动：从 `RtaOptions` 根方法（显式根 + 可选 main 发现）做
///   worklist 闭包，只有入口可达方法的方法体被展开，不可达方法的边整体缺席；
/// - 已实例化类型集合收窄虚分派：入口可达方法体内 `new T` 把 T 加入已实例化
///   集合（fixpoint，晚发现的实例化会重估早先挂起的虚调用点）；虚/接口调用点
///   按 canonical RTA 的方法解析口径收窄——从已实例化类型 T 沿父类链找第一个
///   拥有组件条目的类，dispatch 落在该类名下条目。子类继承父类/接口 default
///   具体方法且不覆写时，祖先名下条目可行（继承未覆写不是欠近似）；子类覆写
///   时解析停在子类，父类条目不出边；组件在册但闭包零实例化的调用点零边
///   （相对 CHA 的精度收益来源，不回退编码目标）。类链走满 = 终止于
///   java/lang/Object（旧栈未登记 Object 时自动合成补位条目，mjbase 类图不
///   合成，按链尾名字等价判定）——走满且无类命中时，组件中 T 祖先闭包（super
///   链 + 接口闭包）内的接口条目可行（接口 default 方法经 superinterface 继承）；
///   其余悬挂尾部（容器外缺失父类）解析不可知，保守放行组件全部条目（宁过
///   近似不丢真实执行边）；
/// - 调用点粒度接收者静态类型门控：per-root feasible 按组件共享，落在目标
///   owner 上的边还要求 ∃ 已实例化类型 S：S <: R（接收者声明类型）且
///   owner ∈ ancestors(S)——「具体父类 × 无关接口」菱形下确定执行的真边由此
///   保留（宁过近似不丢真实执行边）；另保留 R <: owner 的保守补遗（子类型
///   静态接收者直调/接口 default 经祖先条目）。接收者类型在容器外/未知时
///   fail-open 维持原口径。该门控仅筛选调用点目标（挂起与物化同口径），不改
///   实例化集合与 fixpoint 传播机制；
/// - 外部/缺失虚目标仍降级为编码目标（与 CHA 档相同），编码 owner 未声明时
///   与 CHA 档同源沿链落真实声明者（仍受实例化集约束），空描述符反射查找按
///   名落解口径与 GlobalReachability 对齐。
///
/// 铁律（旧栈原口径，必须保留）：RTA 档的结果不应直接作为删除依据——它是
/// 保守根来源；反射实例化、native、classpath 外实例化（反序列化）按欠近似
/// 处理（宁多入根，不漏）。消费入口见 reference/keep_roots 的
/// applyRtaInstantiatedRoots：已实例化的容器内声明类作为保守类根追加进
/// GlobalReachabilityOptions 三槽的 rootClasses。
///
/// 代价：需要入口根集合（roots 变化即边集变化）；本档 opt-in，关闭时
/// build() 的 CHA 行为零变化。
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

/// @brief RTA 构建选项（buildRta）。
struct RtaOptions
{
    /// @brief 显式入口根方法（与 main 发现并集）。
    std::vector<mjbase::reference::MethodRefKey> rootMethods;

    /// @brief 把容器内 `public static void main(String[])` 声明并入根集合。
    bool includeMainMethods = true;
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

    /// @brief Rebuild as an RTA (rapid type analysis) graph.
    ///
    /// Entry-reachability driven: only bodies of methods reachable from
    /// `options` roots (+ optional main discovery) contribute edges. Virtual and
    /// interface expansion follows canonical RTA method resolution: for an
    /// instantiated type T, the feasible dispatch target is the override
    /// family entry under the first class on T's superclass chain that owns
    /// one — so inherited (unoverridden) superclass/interface-default entries
    /// stay reachable, while an overriding subclass stops resolution and keeps
    /// ancestor entries out. A site whose family gains no feasible owner
    /// contributes zero edges. Missing/external virtual targets degrade to the
    /// encoded target exactly like the CHA build. 扩展规则全文见文件头
    /// 「扩展规则（RTA 档）」；语义与旧栈当前实现对齐。
    void buildRta(const mjbase::reference::ReferenceIndex& index, const CallGraph& callGraph,
                  const HierarchyBundle& hierarchy, const RtaOptions& options);

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

    /// @brief RTA 构建后的已实例化类型集合（发现序；图自有池视图）。
    ///
    /// 含外部类名（可达代码中 `new` 出的 JDK/库类型）；仅容器内声明的类参与
    /// 虚分派收窄。仅 buildRta() 后非空；视图随图存活——keep_roots 消费方须
    /// 保证图存活到 GlobalReachability::build 之后（同索引池生命周期契约）。
    /// CHA 档（build()）恒空。
    const std::vector<std::string_view>& instantiatedClasses() const
    {
        return instantiatedClasses_;
    }

    /// @brief RTA 构建后的已实例化类型计数。
    ///
    /// 进程内 buildRta() 后与 instantiatedClasses().size() 恒等。CHA 档恒 0。
    ca::usize rtaInstantiatedCount() const { return rtaInstantiatedCount_; }

    /// @brief RTA 构建后的入口可达方法数（含外部目标）。
    ca::usize rtaReachableMethodCount() const { return rtaReachableMethodCount_; }

private:
    mutable mjbase::KeyPool pool_;

    std::vector<ResolvedCallEdge>                                  edges_;
    std::unordered_map<MemberKey, std::vector<ResolvedCallEdge>, MemberKeyHash> edgesBySource_;
    std::unordered_map<MemberKey, std::vector<ResolvedCallEdge>, MemberKeyHash> edgesByTarget_;
    std::vector<std::string_view>                                  instantiatedClasses_;
    ca::usize                                                      rtaInstantiatedCount_    = 0;
    ca::usize                                                      rtaReachableMethodCount_ = 0;
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

/// @brief RTA 精度档的 GlobalAnalysisManager 包装（opt-in 分析档）。
///
/// 与 CHA 档（ResolvedCallGraphAnalysis）并存、互不替代：本包装 buildRta()
/// 建图（默认根策略 = main 发现，无显式根），CHA 包装 build() 建图。作为可选
/// 保守分析档，只有显式注册/请求本类型的结果才产生 RTA 成本；不注册时既有
/// 消费面行为零变化。产出除边表外携带已实例化类型集合，经
/// reference/keep_roots 的 applyRtaInstantiatedRoots 进 rootClasses 保守根。
struct RtaCallGraphAnalysis
{
    using Result = ResolvedCallGraph;

    Result run(mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam);

    /// 依赖与 CHA 档同源：ReferenceIndex / CallGraph / 层次束任一失效即连锁。
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
