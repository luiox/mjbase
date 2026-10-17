/// @file        global_reachability.hpp
/// @brief       全局可达性闭包：声明/引用事实 + 调用图边的保守闭包。
///
/// 移植自 mbase/reference/global_reachability。闭包规则与旧版逐条同构：
///
/// - Class 可达只扩展声明级类型事实（直接超类/接口/字段声明描述符/Signature/
///   Annotation/结构元数据），不自动展开方法体；
/// - Method 可达扩展 owner class、调用图出边（direct 或 resolved）、精确方法
///   引用、方法体字段访问、方法体与描述符类型引用；空描述符反射查找按名落解；
/// - Field 可达扩展 owner class；
/// - 根策略：显式根 + 可选 Java main 发现 + 类初始化器保留。
///
/// ## 键契约
///
/// 闭包集合以驻留 MemberKey/ClassKey 为键（reachability 自有 KeyPool，mutable
/// 查询 intern）；reachableMethods/reachableFields/entryMethods 的文本视图随
/// reachability 存活。与 ReferenceIndex / ResolvedCallGraph / ReflectionBinding
/// 的互通按文本内容等值（不跨池比 id）。
///
/// ## 停泊件：CallGraphAnalysis
///
/// mjbase asm/classpath 的 CallGraph 是 call_graph 批的既有件（本批不改其目录），
/// 但 GAM 包装缺失。CallGraphAnalysis 在此停泊：容器全类喂 builder 建图；建图
/// Err（fail-fast 语义）时退化为空图并保留诊断开关——直接事实仍可从
/// ReferenceIndex 获得。主线裁决点：call_graph 批交付自己的 GAM 包装后应迁回
/// 其目录（见迁移报告）。
#pragma once

#include <memory>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/asm/classpath/resolved_call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/reference/reflection_binding_analyzer.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::reference {

using mjbase::spi::GlobalAnalysisManager;
using mjbase::spi::PreservedAnalyses;

/// @brief GlobalAnalysisManager 包装：mjbase CallGraph（容器全类建图）。
///
/// 停泊件（归属裁决见文件头注）。建图 fail-fast：单类 promote/操作数解析失败
/// 时 build 返回 Err——包装退化为空图（直接事实由 ReferenceIndex 承载，可达性
/// 闭包少边不小看）。
struct CallGraphAnalysis
{
    using Result = mjbase::asm_classpath::CallGraph;

    Result run(mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam);
};

/// @brief Method-call expansion policy for GlobalReachability.
enum class GlobalReachabilityCallExpansion
{
    /// @brief Use exact encoded direct call graph edges only.
    Direct,
    /// @brief Use `ResolvedCallGraph` virtual/interface target expansion.
    Resolved
};

/// @brief Root policy for GlobalReachability.
///
/// The reachability model is intentionally policy-light. Production cleanup
/// passes should provide explicit roots from CLI configuration, entry-point
/// discovery, keep rules, or product-specific contracts. The built-in main
/// discovery is only a safe default for ordinary application jars.
struct GlobalReachabilityOptions
{
    /// @brief Classes that must be considered live before closure expansion.
    std::vector<std::string_view> rootClasses;

    /// @brief Methods that must be considered live before closure expansion.
    std::vector<MethodRefKey> rootMethods;

    /// @brief Fields that must be considered live before closure expansion.
    std::vector<FieldRefKey> rootFields;

    /// @brief Treat Java `public static main(String[])` declarations as roots.
    ///
    /// This keeps the default GlobalReachabilityAnalysis useful for application
    /// jars while still allowing library-oriented passes to disable the policy
    /// and supply explicit roots.
    bool includeMainMethods = true;

    /// @brief Mark a reachable class initializer as live with its class.
    ///
    /// Class initialization is a side-effect boundary. Keeping `<clinit>` with
    /// its class prevents later passes from deleting static initialization code
    /// merely because no direct call instruction targets it.
    bool includeClassInitializers = true;

    /// @brief Select whether reachable method bodies expand direct or resolved calls.
    ///
    /// The default keeps historical direct `CallGraph` behavior. Passes that
    /// need hierarchy-aware liveness can call the resolved build overload or use
    /// `ResolvedGlobalReachabilityAnalysis`.
    GlobalReachabilityCallExpansion callExpansion = GlobalReachabilityCallExpansion::Direct;

    /// @brief Skip the raw encoded method-reference expansion in Resolved mode.
    ///
    /// Under a profile-narrowed resolved graph (RTA), the encoded call-site
    /// target may be absent from the edge table (dispatch narrowed to zero
    /// feasible targets). Re-enqueuing raw encoded targets would re-import
    /// owners the profile removed, making the reachable closure disagree with
    /// the resolved edge table. When set together with
    /// `callExpansion == Resolved`, method bodies expand through resolved edges
    /// only; the empty-descriptor reflection by-name expansion is kept because
    /// the RTA build applies the same policy inside its own closure. Direct
    /// mode and the plain Resolved mode (CHA) are unaffected.
    bool resolvedEdgesExclusive = false;
};

/// @brief 保守的全局可达性闭包。
///
/// 结合 ReferenceIndex 声明/引用事实和调用图边，为后续清理 pass 提供稳定的
/// 查询：哪些 class/method/field 从选定根可达。模块不删除字节码，只给删除前
/// 的稳定查询。发现序（first-seen）仅够诊断与测试断言，不是语义排序契约。
class GlobalReachability
{
public:
    /// @brief Rebuild the closure from direct reference and call graph analyses.
    ///
    /// Existing results are discarded. The resulting vectors preserve first-seen
    /// discovery order, which is stable enough for diagnostics and focused tests
    /// but should not be treated as a semantic ordering contract.
    void build(const ReferenceIndex& index, const mjbase::asm_classpath::CallGraph& callGraph,
               const GlobalReachabilityOptions& options = {});

    /// @brief Rebuild the closure with optional hierarchy-aware call expansion.
    ///
    /// When `options.callExpansion` is `Resolved`, reachable method bodies use
    /// `resolvedCallGraph` edges instead of exact direct edges. Other closure
    /// rules are identical to the direct build overload.
    void build(const ReferenceIndex& index, const mjbase::asm_classpath::CallGraph& callGraph,
               const mjbase::asm_classpath::ResolvedCallGraph& resolvedCallGraph,
               const GlobalReachabilityOptions&                options = {});

    /// @brief Remove all reachable sets and snapshots.
    void clear();

    /// @brief Return true if a class name is in the reachable closure.
    bool isClassReachable(std::string_view owner) const;

    /// @brief Return true if a method key is in the reachable closure.
    bool isMethodReachable(std::string_view owner, std::string_view name,
                           std::string_view desc) const;

    /// @brief Return true if a field key is in the reachable closure.
    bool isFieldReachable(std::string_view owner, std::string_view name,
                          std::string_view desc) const;

    /// @brief Reachable classes in first-seen discovery order.
    const std::vector<std::string_view>& reachableClasses() const { return reachableClasses_; }

    /// @brief Reachable methods in first-seen discovery order.
    const std::vector<MethodRefKey>& reachableMethods() const { return reachableMethods_; }

    /// @brief Reachable fields in first-seen discovery order.
    const std::vector<FieldRefKey>& reachableFields() const { return reachableFields_; }

    /// @brief Root methods that seeded the closure, in enqueue order.
    ///
    /// This is the union of `GlobalReachabilityOptions::rootMethods` and the
    /// `main(String[])` declarations discovered when `includeMainMethods` is
    /// enabled. It lets callers report entry counts and entry lists without
    /// re-running main discovery. Valid only after build(); empty after clear().
    const std::vector<MethodRefKey>& entryMethods() const { return entryMethods_; }

    /// @brief Declared classes not present in the reachable class closure.
    std::vector<ClassDeclaration> unreachableDeclaredClasses(const ReferenceIndex& index) const;

    /// @brief Declared methods not present in the reachable method closure.
    std::vector<MethodDeclaration> unreachableDeclaredMethods(const ReferenceIndex& index) const;

    /// @brief Declared fields not present in the reachable field closure.
    std::vector<FieldDeclaration> unreachableDeclaredFields(const ReferenceIndex& index) const;

private:
    void buildInternal(const ReferenceIndex& index,
                       const mjbase::asm_classpath::CallGraph& callGraph,
                       const mjbase::asm_classpath::ResolvedCallGraph* resolvedCallGraph,
                       const GlobalReachabilityOptions& options);

    bool markClass(std::string_view owner);
    bool markMethod(const MethodRefKey& method);
    bool markField(const FieldRefKey& field);

    mutable mjbase::KeyPool pool_;

    std::unordered_set<MemberKey, MemberKeyHash> reachableMethodKeys_;
    std::unordered_set<MemberKey, MemberKeyHash> reachableFieldKeys_;
    std::unordered_set<ca::u32>                  reachableClassKeys_;   ///< ClassKey.id
    std::vector<std::string_view>                reachableClasses_;
    std::vector<MethodRefKey>                    reachableMethods_;
    std::vector<FieldRefKey>                     reachableFields_;
    std::vector<MethodRefKey>                    entryMethods_;
};

/// @brief GlobalAnalysisManager wrapper for the default reachability policy.
///
/// The default policy includes Java main methods and class initializers. Passes
/// that need keep-rule or library-specific roots should instantiate
/// GlobalReachability directly with their own GlobalReachabilityOptions.
struct GlobalReachabilityAnalysis
{
    using Result = GlobalReachability;

    Result run(mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam);

    /// 依赖 ReferenceIndex / CallGraph：任一被失效则可达性连锁失效（传递失效）。
    static bool dependencyInvalidated(mjbase::model::ResourceContainer& container,
                                      const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ReferenceIndexAnalysis>(container, pa) ||
               inv.invalidate<CallGraphAnalysis>(container, pa);
    }
};

/// @brief GlobalAnalysisManager wrapper using hierarchy-aware call expansion.
///
/// This keeps the default direct analysis stable while giving P3 consumers an
/// explicit analysis type for virtual/interface-aware liveness.
struct ResolvedGlobalReachabilityAnalysis
{
    using Result = GlobalReachability;

    Result run(mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam);

    /// 依赖 ReferenceIndex / CallGraph / ResolvedCallGraph：任一被失效则连锁失效。
    static bool dependencyInvalidated(mjbase::model::ResourceContainer& container,
                                      const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ReferenceIndexAnalysis>(container, pa) ||
               inv.invalidate<CallGraphAnalysis>(container, pa) ||
               inv.invalidate<mjbase::asm_classpath::ResolvedCallGraphAnalysis>(container, pa);
    }
};

/// @brief 计量报告：反射绑定织入调用图的结果分布。
///
/// 各字段口径：
/// - `sites`：方法级执行 sink 绑定数（Method.invoke / Constructor.newInstance /
///   Class.newInstance，含 Pending）。
/// - `resolved`：其中 state == Resolved 的绑定数（含织入复核未过者，与 edges
///   分列：Resolved-but-dropped 时 resolved > edges）。
/// - `edges`：实际织入图的边数（Resolved 且目标命中容器内声明）。
/// - `dropped`：未织入的方法级绑定数 = sites - edges。包含静态无法落解
///   （Pending）与 Resolved 但目标未落入容器声明（容器边界、描述符口径差异）
///   两类，归因不静默丢。
/// - `classes`：类级 sink（Class.forName / ClassLoader.loadClass）Resolved 的
///   去重目标类数。类加载不产生调用边，仅计量。
struct ReflectionWeaveReport
{
    ca::usize sites    = 0;
    ca::usize resolved = 0;
    ca::usize edges    = 0;
    ca::usize dropped  = 0;
    ca::usize classes  = 0;
};

/// @brief 把反射绑定分析器的方法级 Resolved 绑定织入既有解析调用图。
///
/// 对每个 `ReflectionBinding`：
///
/// - 方法级执行 sink（Method.invoke / Constructor.newInstance / Class.newInstance）
///   且 state == Resolved：从绑定调用点所在方法（site.source*）向
///   `binding.target` 追加一条 `ResolvedCallKind::Reflect` 动态边。encoded 与
///   resolved 目标同为反射落解目标（反射边没有字节码编码目标）。目标未命中
///   `ReferenceIndex` 声明时计入 `dropped`，不产生边。
/// - 类级 sink（Class.forName / ClassLoader.loadClass）且 Resolved：目标类名进
///   `classes` 计量，不产生边。
/// - 字段访问与成员查找 sink：不产生边（字段访问不进调用图；查找本身由对应
///   执行 sink 承载）。
///
/// `GlobalReachability` 的 resolved 扩展模式读取 `edgesFrom`，织入后反射边与
/// 静态 CHA 边同路参与可达闭包；不改变两个既有设施任何既有接口语义。
ReflectionWeaveReport weaveReflectionBindings(
    mjbase::asm_classpath::ResolvedCallGraph& graph, const ReferenceIndex& index,
    const std::vector<ReflectionBinding>& bindings);

}   // namespace mjbase::reference
