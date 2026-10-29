#include "mjbase/reference/global_reachability.hpp"

#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/reference/entry_points.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"

namespace mjbase::reference {

using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::reference::ResolvedCallEdge;
using mjbase::reference::ResolvedCallGraph;
using mjbase::reference::ResolvedCallKind;

namespace {

MemberKey toMemberKey(KeyPool& pool, std::string_view owner, std::string_view name,
                      std::string_view desc)
{
    return memberKeyOf(pool, classKeyOf(pool, owner), name, desc);
}

bool isDeclarationTypeReference(TypeReferenceKind kind)
{
    return kind == TypeReferenceKind::ClassSuper || kind == TypeReferenceKind::ClassInterface ||
           kind == TypeReferenceKind::FieldDeclarationDescriptor ||
           kind == TypeReferenceKind::Signature || kind == TypeReferenceKind::Annotation ||
           kind == TypeReferenceKind::StructuralMetadata;
}

}   // namespace

// ── CallGraphAnalysis（停泊件）───────────────────────────────

CallGraphAnalysis::Result CallGraphAnalysis::run(mjbase::model::ResourceContainer& container,
                                                 GlobalAnalysisManager& gam)
{
    (void)gam;
    CallGraphBuilder builder;
    for (const auto& scannedClass : mjbase::analysis::scanClassResources(container)) {
        builder.addSession(scannedClass.session);
    }
    auto built = builder.build();
    if (built.is_err()) {
        // fail-fast 建图失败：退化为空图（直接事实由 ReferenceIndex 承载，
        // 可达性闭包少边不小看）。诊断面随 call_graph 批的 GAM 包装补齐。
        return CallGraph{};
    }
    return std::move(built).unwrap();
}

// ── GlobalReachability ───────────────────────────────────────

void GlobalReachability::clear()
{
    // 池只增不减（内容寻址驻留）：仅清闭包集合。
    reachableMethodKeys_.clear();
    reachableFieldKeys_.clear();
    reachableClassKeys_.clear();
    reachableClasses_.clear();
    reachableMethods_.clear();
    reachableFields_.clear();
    entryMethods_.clear();
}

bool GlobalReachability::markClass(std::string_view owner)
{
    if (owner.empty()) return false;
    const ca::u32 id = classKeyOf(pool_, owner).id;
    if (!reachableClassKeys_.insert(id).second) return false;
    reachableClasses_.push_back(pool_.str(id));
    return true;
}

bool GlobalReachability::markMethod(const MethodRefKey& method)
{
    if (method.owner.empty() || method.name.empty() || method.desc.empty()) return false;
    const MemberKey key = toMemberKey(pool_, method.owner, method.name, method.desc);
    if (!reachableMethodKeys_.insert(key).second) return false;
    reachableMethods_.push_back(MethodRefKey{pool_.str(key.cls), pool_.str(key.name),
                                             pool_.str(key.desc)});
    return true;
}

bool GlobalReachability::markField(const FieldRefKey& field)
{
    if (field.owner.empty() || field.name.empty() || field.desc.empty()) return false;
    const MemberKey key = toMemberKey(pool_, field.owner, field.name, field.desc);
    if (!reachableFieldKeys_.insert(key).second) return false;
    reachableFields_.push_back(FieldRefKey{pool_.str(key.cls), pool_.str(key.name),
                                           pool_.str(key.desc)});
    return true;
}

void GlobalReachability::build(const ReferenceIndex& index, const CallGraph& callGraph,
                               const GlobalReachabilityOptions& options)
{
    buildInternal(index, callGraph, nullptr, options);
}

void GlobalReachability::build(const ReferenceIndex& index, const CallGraph& callGraph,
                               const ResolvedCallGraph& resolvedCallGraph,
                               const GlobalReachabilityOptions& options)
{
    buildInternal(index, callGraph, &resolvedCallGraph, options);
}

void GlobalReachability::buildInternal(const ReferenceIndex& index, const CallGraph& callGraph,
                                       const ResolvedCallGraph* resolvedCallGraph,
                                       const GlobalReachabilityOptions& options)
{
    clear();

    const KeyPool& callGraphPool = callGraph.pool();

    std::vector<std::string_view> classQueue;
    std::vector<MethodRefKey>     methodQueue;
    std::vector<FieldRefKey>      fieldQueue;

    auto enqueueClass = [&](std::string_view owner) {
        if (markClass(owner)) classQueue.push_back(pool_.str(classKeyOf(pool_, owner).id));
    };
    auto enqueueMethod = [&](const MethodRefKey& method) {
        if (markMethod(method)) {
            methodQueue.push_back(reachableMethods_.back());
        }
    };
    auto enqueueField = [&](const FieldRefKey& field) {
        if (markField(field)) {
            fieldQueue.push_back(reachableFields_.back());
        }
    };

    for (const std::string_view owner : options.rootClasses) {
        enqueueClass(owner);
    }
    for (const MethodRefKey& method : options.rootMethods) {
        enqueueMethod(method);
    }
    for (const FieldRefKey& field : options.rootFields) {
        enqueueField(field);
    }
    if (options.includeMainMethods) {
        for (const auto& decl : index.methodDeclarations()) {
            if (isJavaMainMethod(decl)) enqueueMethod(decl.method);
        }
    }

    // methodQueue 此时恰好是全部方法级根（显式 rootMethods + 发现的 main），
    // 快照为入口清单供报告方查询；闭包展开从下面才开始。
    entryMethods_ = methodQueue;

    ca::usize classCursor  = 0;
    ca::usize methodCursor = 0;
    ca::usize fieldCursor  = 0;

    while (classCursor < classQueue.size() || methodCursor < methodQueue.size() ||
           fieldCursor < fieldQueue.size()) {
        while (classCursor < classQueue.size()) {
            const std::string_view owner = classQueue[classCursor++];

            // 可达类会保留其直接声明类型事实；这里不主动展开方法体，方法体必须
            // 由可达方法根或调用边进入闭包。
            for (const auto& ref : index.typeReferencesFromClass(owner)) {
                if (isDeclarationTypeReference(ref.kind)) {
                    enqueueClass(ref.targetType);
                }
            }

            if (options.includeClassInitializers) {
                if (const auto* clinit = index.declaredMethod(owner, "<clinit>", "()V")) {
                    enqueueMethod(clinit->method);
                }
            }
        }

        while (methodCursor < methodQueue.size()) {
            const MethodRefKey method = methodQueue[methodCursor++];
            enqueueClass(method.owner);

            if (options.callExpansion == GlobalReachabilityCallExpansion::Resolved &&
                resolvedCallGraph != nullptr) {
                for (const auto& edge :
                     resolvedCallGraph->edgesFrom(method.owner, method.name, method.desc)) {
                    enqueueMethod(edge.resolvedTarget);
                }
            }
            else {
                // 查询键按内容驻留进图池（const 接口下唯一入口；内容寻址幂等，
                // 只增不减无副作用面）。
                for (const MemberKey callee :
                     callGraph.calleesOf(toMemberKey(const_cast<KeyPool&>(callGraphPool),
                                                     method.owner, method.name, method.desc))) {
                    enqueueMethod(MethodRefKey{callGraphPool.str(callee.cls),
                                               callGraphPool.str(callee.name),
                                               callGraphPool.str(callee.desc)});
                }
            }

            // ReferenceIndex 还记录 MethodHandle、invokedynamic 引导参数和已支持
            // 的反射事实；这些引用不一定出现在任一调用图实现中。
            // resolvedEdgesExclusive（RTA 档）时编码目标改由解析边表承载——raw
            // 循环再入队会把被收窄掉的 owner 拉回闭包。空描述符反射按名落解
            // 保留（与 RTA fixpoint 同口径）。
            const bool resolvedExclusive =
                options.resolvedEdgesExclusive &&
                options.callExpansion == GlobalReachabilityCallExpansion::Resolved &&
                resolvedCallGraph != nullptr;
            for (const auto& ref :
                 index.methodReferencesFrom(method.owner, method.name, method.desc)) {
                if (!ref.target.desc.empty()) {
                    if (!resolvedExclusive) enqueueMethod(ref.target);
                    continue;
                }
                if (ref.kind != MethodReferenceKind::Reflection &&
                    ref.kind != MethodReferenceKind::ReflectionExecution) {
                    continue;
                }
                for (const auto& declaration : index.declaredMethodsIn(ref.target.owner)) {
                    if (declaration.method.name == ref.target.name) {
                        enqueueMethod(declaration.method);
                    }
                }
            }
            for (const auto& ref :
                 index.fieldReferencesFrom(method.owner, method.name, method.desc)) {
                if (!ref.target.desc.empty()) {
                    enqueueField(ref.target);
                    continue;
                }
                if (ref.kind != FieldReferenceKind::Reflection &&
                    ref.kind != FieldReferenceKind::ReflectionAccess) {
                    continue;
                }
                for (const auto& declaration : index.declaredFieldsIn(ref.target.owner)) {
                    if (declaration.field.name == ref.target.name) {
                        enqueueField(declaration.field);
                    }
                }
            }
            for (const auto& ref :
                 index.typeReferencesFrom(method.owner, method.name, method.desc)) {
                enqueueClass(ref.targetType);
            }
        }

        while (fieldCursor < fieldQueue.size()) {
            const FieldRefKey field = fieldQueue[fieldCursor++];
            enqueueClass(field.owner);
        }
    }
}

bool GlobalReachability::isClassReachable(std::string_view owner) const
{
    if (owner.empty()) return false;
    const ca::u32 id = classKeyOf(pool_, owner).id;
    return reachableClassKeys_.find(id) != reachableClassKeys_.end();
}

bool GlobalReachability::isMethodReachable(std::string_view owner, std::string_view name,
                                           std::string_view desc) const
{
    return reachableMethodKeys_.find(toMemberKey(pool_, owner, name, desc)) !=
           reachableMethodKeys_.end();
}

bool GlobalReachability::isFieldReachable(std::string_view owner, std::string_view name,
                                          std::string_view desc) const
{
    return reachableFieldKeys_.find(toMemberKey(pool_, owner, name, desc)) !=
           reachableFieldKeys_.end();
}

std::vector<ClassDeclaration> GlobalReachability::unreachableDeclaredClasses(
    const ReferenceIndex& index) const
{
    std::vector<ClassDeclaration> result;
    for (const auto& decl : index.classDeclarations()) {
        if (!isClassReachable(decl.name)) result.push_back(decl);
    }
    return result;
}

std::vector<MethodDeclaration> GlobalReachability::unreachableDeclaredMethods(
    const ReferenceIndex& index) const
{
    std::vector<MethodDeclaration> result;
    for (const auto& decl : index.methodDeclarations()) {
        if (!isMethodReachable(decl.method.owner, decl.method.name, decl.method.desc)) {
            result.push_back(decl);
        }
    }
    return result;
}

std::vector<FieldDeclaration> GlobalReachability::unreachableDeclaredFields(
    const ReferenceIndex& index) const
{
    std::vector<FieldDeclaration> result;
    for (const auto& decl : index.fieldDeclarations()) {
        if (!isFieldReachable(decl.field.owner, decl.field.name, decl.field.desc)) {
            result.push_back(decl);
        }
    }
    return result;
}

// ── GAM 包装 ─────────────────────────────────────────────────

GlobalReachabilityAnalysis::Result GlobalReachabilityAnalysis::run(
    mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam)
{
    auto& index     = gam.getResult<ReferenceIndexAnalysis>(container);
    auto& callGraph = gam.getResult<CallGraphAnalysis>(container);

    GlobalReachability reachability;
    reachability.build(index, callGraph);
    return reachability;
}

ResolvedGlobalReachabilityAnalysis::Result ResolvedGlobalReachabilityAnalysis::run(
    mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam)
{
    auto& index             = gam.getResult<ReferenceIndexAnalysis>(container);
    auto& callGraph         = gam.getResult<CallGraphAnalysis>(container);
    auto& resolvedCallGraph = gam.getResult<mjbase::reference::ResolvedCallGraphAnalysis>(container);

    GlobalReachabilityOptions options;
    options.callExpansion = GlobalReachabilityCallExpansion::Resolved;

    GlobalReachability reachability;
    reachability.build(index, callGraph, resolvedCallGraph, options);
    return reachability;
}

ReflectionWeaveReport weaveReflectionBindings(ResolvedCallGraph&                    graph,
                                              const ReferenceIndex&                 index,
                                              const std::vector<ReflectionBinding>& bindings)
{
    ReflectionWeaveReport          report;
    std::unordered_set<std::string_view> resolvedClassNames;

    for (const auto& binding : bindings) {
        switch (binding.api) {
        case ReflectionApiKind::MethodInvoke:
        case ReflectionApiKind::ConstructorNewInstance:
        case ReflectionApiKind::ClassNewInstance:
        {
            ++report.sites;
            if (binding.state != ReflectionBindingState::Resolved) {
                // 静态无法落解：没有可织入目标，计入未落图，不静默丢。
                ++report.dropped;
                continue;
            }
            // resolved 计分析器结论为 Resolved 的绑定数（含织入复核未过者），
            // 与 edges（实际织入）分离：Resolved-but-dropped 时 resolved > edges。
            ++report.resolved;

            // 织入端复核：Resolved 绑定的目标以 ReferenceIndex 声明为准再验一次。
            // 分析器验证与织入之间可能存在容器边界或描述符口径差异，落不进声明
            // 集的目标宁可丢弃并计量。
            if (index.declaredMethod(binding.target.owner, binding.target.name,
                                     binding.target.desc) == nullptr) {
                ++report.dropped;
                continue;
            }

            ResolvedCallEdge edge;
            edge.site.sourceClass  = binding.site.sourceClass;
            edge.site.sourceMethod = binding.site.sourceMethod;
            edge.site.sourceDesc   = binding.site.sourceDesc;
            edge.site.opcode       = binding.site.opcode;
            edge.site.insnIndex    = binding.site.insnIndex;
            edge.encodedTarget  = MethodRefKey{binding.target.owner, binding.target.name,
                                               binding.target.desc};
            edge.resolvedTarget = edge.encodedTarget;
            edge.kind           = ResolvedCallKind::Reflect;
            edge.expanded       = false;
            graph.appendEdge(edge);

            ++report.edges;
            break;
        }
        case ReflectionApiKind::ClassForName:
        case ReflectionApiKind::ClassLoaderLoadClass:
            // 类加载不产生调用边：Resolved 目标类名去重计量。
            if (binding.state == ReflectionBindingState::Resolved && !binding.className.empty()) {
                resolvedClassNames.insert(binding.className);
            }
            break;
        case ReflectionApiKind::MemberLookup:
        case ReflectionApiKind::FieldAccess:
        case ReflectionApiKind::LookupMethodHandle:
            // 成员查找 / 字段访问 / Lookup 目录项不产生调用边。
            break;
        }
    }

    report.classes = resolvedClassNames.size();
    return report;
}

}   // namespace mjbase::reference
