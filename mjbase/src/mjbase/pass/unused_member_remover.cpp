#include "mjbase/pass/unused_member_remover.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/class_resource_pending_commit.hpp"

namespace mjbase::pass {

namespace {

// JVM 访问标志位（JVMS 表；libjbc 未提供旗标常量）。
constexpr ca::u16 kAccPublic    = 0x0001;
constexpr ca::u16 kAccPrivate   = 0x0002;
constexpr ca::u16 kAccStatic    = 0x0008;
constexpr ca::u16 kAccNative    = 0x0100;
constexpr ca::u16 kAccAbstract  = 0x0400;

/// 装载完成的类：DOM + 写回目标（成员删除在低层 DOM 上进行，无需会话）。
struct LoadedClass
{
    std::string                           name;      ///< 类内部名（classfile 头声明名）
    mjbase::model::ClassResource*         resource = nullptr;
    std::shared_ptr<jbc::core::ClassFile> cf;
};

/// 装载单个 class 资源；空 bytes / 不可解析 / 无类名返回 false（资源保持原样）。
bool loadClass(mjbase::model::ClassResource& resource, LoadedClass& out)
{
    const auto& bytes = resource.get();
    if (bytes.empty()) return false;
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return false;
    out.cf = std::move(read).unwrap().file;
    out.name.assign(out.cf->thisName());
    if (out.name.empty()) return false;
    out.resource = &resource;
    return true;
}

/// 容器内全部 class 资源可解析性预检：
/// 任一不可解析整 pass 放弃（返回 all()），不做部分删除。
bool allClassResourcesReadable(mjbase::model::ResourceContainer& container)
{
    for (const auto& [uri, resource] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr) continue;
        if (cls->get().empty()) return false;
        auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
        if (read.is_err()) return false;
        jbc::core::ReadResult readResult = std::move(read).unwrap();
        if (std::string_view(readResult.file->thisName()).empty()) return false;
    }
    return true;
}

/// 属性表按名查找（CP 只追加，索引比较不可靠，逐槽比名字字节）。
const jbc::core::AttrSlot* findAttrByName(const jbc::core::ClassFile&             cf,
                                          const std::vector<jbc::core::AttrSlot>& attrs,
                                          std::string_view                        name)
{
    for (const jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// 类是否携带 Module 描述符（module-info keep 根）。
bool isModuleDescriptor(const jbc::core::ClassFile& cf)
{
    return findAttrByName(cf, cf.attrs, "Module") != nullptr;
}

/// 字段 keep metadata：Signature 或任一注解族属性在场。
bool hasFieldKeepMetadata(const jbc::core::ClassFile& cf, const jbc::core::MemberInfo& field)
{
    static constexpr std::string_view kKeepAttrs[] = {
        "Signature",
        "RuntimeVisibleAnnotations",
        "RuntimeInvisibleAnnotations",
        "RuntimeVisibleTypeAnnotations",
        "RuntimeInvisibleTypeAnnotations",
    };
    for (const std::string_view attrName : kKeepAttrs) {
        if (findAttrByName(cf, field.attrs, attrName) != nullptr) return true;
    }
    return false;
}

/// 方法 keep metadata：字段面 + 参数注解 + AnnotationDefault；局部变量注解在 libjbc 同属 Runtime*TypeAnnotations
/// 属性，在场即 keep，判定等价）。
bool hasMethodKeepMetadata(const jbc::core::ClassFile& cf, const jbc::core::MemberInfo& method)
{
    static constexpr std::string_view kKeepAttrs[] = {
        "Signature",
        "RuntimeVisibleAnnotations",
        "RuntimeInvisibleAnnotations",
        "RuntimeVisibleTypeAnnotations",
        "RuntimeInvisibleTypeAnnotations",
        "RuntimeVisibleParameterAnnotations",
        "RuntimeInvisibleParameterAnnotations",
        "AnnotationDefault",
    };
    for (const std::string_view attrName : kKeepAttrs) {
        if (findAttrByName(cf, method.attrs, attrName) != nullptr) return true;
    }
    return false;
}

/// 资源 keep 类名单：module 描述符类 + Manifest 入口键 + META-INF/services
/// 文件名与内容行。
/// 任一 class 资源不可解析返回 false（调用方整 pass 放弃）。
bool collectResourceKeepClasses(mjbase::model::ResourceContainer& container,
                                std::unordered_set<std::string>&       keepNames)
{
    constexpr std::string_view kServicesPrefix = "META-INF/services/";
    constexpr std::string_view kManifestKeys[] = {
        "Main-Class", "Premain-Class", "Agent-Class", "Launcher-Agent-Class",
        "JavaFX-Application-Class",
    };

    const auto normalizeBinaryClassName = [](std::string value) {
        const auto trim = [](std::string& text) {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                text.clear();
                return;
            }
            const auto last = text.find_last_not_of(" \t\r\n");
            text            = text.substr(first, last - first + 1);
        };
        trim(value);
        if (value.empty()) return std::string{};
        for (char& ch : value) {
            if (ch == '.') ch = '/';
        }
        return value;
    };

    for (const auto& [uri, resource] : container.getResources()) {
        if (const auto* cls =
                ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get())) {
            if (cls->get().empty()) return false;
            auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
            if (read.is_err()) return false;
            jbc::core::ReadResult readResult = std::move(read).unwrap();
            const jbc::core::ClassFile& cf = *readResult.file;
            if (isModuleDescriptor(cf)) keepNames.insert(std::string(cf.thisName()));
            continue;
        }
        if (const auto* manifest = ca::core::tag_cast::dyn_cast<const mjbase::model::ManifestResource>(
                resource.get())) {
            for (const std::string_view key : kManifestKeys) {
                const std::string name = normalizeBinaryClassName(
                    manifest->get().getMainAttribute(
                        ca::str::Utf8StringRef::from_cstr(key.data())).to_std_string());
                if (!name.empty()) keepNames.insert(name);
            }
            continue;
        }
        if (uri.rfind(kServicesPrefix, 0) != 0) continue;
        const std::string serviceName =
            normalizeBinaryClassName(std::string(uri.substr(kServicesPrefix.size())));
        if (!serviceName.empty()) keepNames.insert(serviceName);
        if (const auto* unknown =
                ca::core::tag_cast::dyn_cast<const mjbase::model::UnknownResource>(resource.get())) {
            // 内容逐行：'#' 注释截断、CRLF 归一、二名 '.' → '/'。
            std::string content(unknown->get().begin(), unknown->get().end());
            std::size_t cursor = 0;
            while (cursor <= content.size()) {
                const std::size_t end  = content.find('\n', cursor);
                std::string       line = content.substr(
                    cursor, end == std::string::npos ? std::string::npos : end - cursor);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto comment = line.find('#');
                if (comment != std::string::npos) line.resize(comment);
                const std::string provider = normalizeBinaryClassName(std::move(line));
                if (!provider.empty()) keepNames.insert(provider);
                if (end == std::string::npos) break;
                cursor = end + 1;
            }
        }
    }
    return true;
}

/// 闭包根收集器：classes/methods/fields 的 string_view 指向自身 strings 驻留集
/// （unordered_set 节点地址稳定，存活至 run() 结束——对齐 UnusedClassRemover 的
/// rootClasses 驻留纪律）。
struct RetainedRoots
{
    std::unordered_set<std::string>             strings;
    std::vector<std::string_view>               classes;
    std::vector<mjbase::reference::MethodRefKey> methods;
    std::vector<mjbase::reference::FieldRefKey>  fields;

    std::string_view intern(std::string text)
    {
        return *strings.insert(std::move(text)).first;
    }
    void addClass(std::string_view name) { classes.push_back(intern(std::string(name))); }
    void addMethod(std::string_view owner, std::string_view name, std::string_view desc)
    {
        methods.push_back({intern(std::string(owner)), intern(std::string(name)),
                           intern(std::string(desc))});
    }
    void addField(std::string_view owner, std::string_view name, std::string_view desc)
    {
        fields.push_back({intern(std::string(owner)), intern(std::string(name)),
                          intern(std::string(desc))});
    }
};

/// 无条件保留面提升为闭包根（删除安全不变量的前提，见模块头注「删除安全不变量
/// 与『调用点收尾』裁定」）：凡因策略面而非可达性保留的成员，其出边必须参与
/// 闭包传播，否则「保留但不可达」代码引用的死成员会被删出悬垂引用。
///
/// - 资源 keep 类（module/Manifest/services）与 keep 规则命中类：类 + 全部声明
///   成员（类在即成员全在，对齐 UnusedClassRemover 的声明方法提升策略；成员删除
///   两件另提升声明字段——ServiceLoader/agent 等容器外入口面 fail-closed）。
///   声明面取自 ReferenceIndex（与删除谓词同源口径）。
/// - keep metadata（Signature/注解族）成员、keep 规则命中成员（DOM 属性面判定）。
/// - native/abstract 方法（无方法体语义承载）与全部 `<init>`/`<clinit>`
///   （`<clinit>` 与 includeClassInitializers 双保险，`<init>` 从不候选删除）。
void collectRetainedRoots(const std::vector<LoadedClass>&          classes,
                          const mjbase::reference::ReferenceIndex& index,
                          const std::unordered_set<std::string>&   resourceKeepNames,
                          const mjbase::reference::KeepRuleSet*    keepRules,
                          RetainedRoots&                           roots)
{
    for (const LoadedClass& loaded : classes) {
        const std::string& owner = loaded.name;
        const bool wholeClassKeep = resourceKeepNames.count(owner) != 0 ||
                                    (keepRules != nullptr && keepRules->matchesClass(owner));
        if (wholeClassKeep) {
            roots.addClass(owner);
            for (const mjbase::reference::MethodDeclaration& method :
                 index.declaredMethodsIn(owner)) {
                roots.addMethod(method.method.owner, method.method.name, method.method.desc);
            }
            for (const mjbase::reference::FieldDeclaration& field :
                 index.declaredFieldsIn(owner)) {
                roots.addField(field.field.owner, field.field.name, field.field.desc);
            }
            continue;
        }
        for (const jbc::core::MemberInfo& method : loaded.cf->methods) {
            const std::string name(loaded.cf->nameOf(method));
            const std::string desc(loaded.cf->descOf(method));
            const bool classInit   = name == "<init>" || name == "<clinit>";
            const bool noBody      = (method.access & (kAccNative | kAccAbstract)) != 0;
            const bool ruleKeep =
                keepRules != nullptr && keepRules->matchesMember(owner, name, desc);
            if (classInit || noBody || hasMethodKeepMetadata(*loaded.cf, method) || ruleKeep) {
                roots.addMethod(owner, name, desc);
            }
        }
        for (const jbc::core::MemberInfo& field : loaded.cf->fields) {
            const std::string name(loaded.cf->nameOf(field));
            const std::string desc(loaded.cf->descOf(field));
            if (hasFieldKeepMetadata(*loaded.cf, field) ||
                (keepRules != nullptr && keepRules->matchesMember(owner, name, desc))) {
                roots.addField(owner, name, desc);
            }
        }
    }
}

/// 自建闭包（Resolved 展开）：共享根策略（main、`<clinit>`）+ 保留面根。闭包对象
/// 为 pass 局部——根集合是 pass 策略面，不与共享 ResolvedGlobalReachabilityAnalysis
/// 缓存混用；索引/调用图/解析图仍取自 GAM（与共享分析同源）。
mjbase::reference::GlobalReachability buildRetainedClosure(
    mjbase::spi::GlobalAnalysisManager& am, mjbase::model::ResourceContainer& container,
    const RetainedRoots& roots)
{
    auto&             index            = am.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);
    auto&             callGraph        = am.getResult<mjbase::reference::CallGraphAnalysis>(container);
    auto&             resolvedCallGraph = am.getResult<mjbase::asm_classpath::ResolvedCallGraphAnalysis>(container);

    mjbase::reference::GlobalReachabilityOptions options;
    options.callExpansion = mjbase::reference::GlobalReachabilityCallExpansion::Resolved;
    options.rootClasses   = roots.classes;
    options.rootMethods   = roots.methods;
    options.rootFields    = roots.fields;

    mjbase::reference::GlobalReachability reachability;
    reachability.build(index, callGraph, resolvedCallGraph, options);
    return reachability;
}

/// 档位可见性谓词：候选访问面。NonPublic 的「非 public」= private /
/// package-private / protected（无标志位即 package-private）。
bool matchesTier(ca::u16 access, UnusedMemberAccessTier tier)
{
    switch (tier) {
    case UnusedMemberAccessTier::PrivateOnly: return (access & kAccPrivate) != 0;
    case UnusedMemberAccessTier::NonPublic:   return (access & kAccPublic) == 0;
    case UnusedMemberAccessTier::All:         return true;
    }
    return false;
}

/// 预载容器全部 class 资源 DOM（保留面 guard 收集与删除改写共用一轮解析）；
/// 任一不可解析返回 false（调用方整 pass 放弃）。
bool loadAllClasses(mjbase::model::ResourceContainer& container, std::vector<LoadedClass>& out)
{
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) continue;
        auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource.get());
        LoadedClass loaded;
        if (!loadClass(*cls, loaded)) return false;
        out.push_back(std::move(loaded));
    }
    return true;
}

}   // namespace

// ── EmptyClassRemover ────────────────────────────────────────────────────────

EmptyClassRemover::EmptyClassRemover(
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules)
    : keepRules_(std::move(keepRules))
{}

mjbase::spi::PreservedAnalyses EmptyClassRemover::run(mjbase::model::ResourceContainer&     container,
                                                      mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    std::vector<std::string> toRemove;
    for (const auto& [uri, resource] : container.getResources()) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr) continue;
        if (cls->get().empty()) continue;
        auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
        if (read.is_err()) continue;
        jbc::core::ReadResult readResult = std::move(read).unwrap();
        const jbc::core::ClassFile& cf = *readResult.file;
        if (std::string_view(cf.thisName()).empty()) continue;
        // keep 规则（只加不减）：命中的类不删。
        if (keepRules_ != nullptr && keepRules_->matchesClass(std::string_view(cf.thisName()))) {
            continue;
        }
        if (cf.fields.empty() && cf.methods.empty()) {
            toRemove.emplace_back(uri);
        }
    }

    if (toRemove.empty()) return mjbase::spi::PreservedAnalyses::all();
    auto& resources = container.getResources();
    for (const std::string& uri : toRemove) {
        resources.erase(uri);
    }
    return mjbase::spi::PreservedAnalyses::none();
}

// ── UnusedClassRemover ───────────────────────────────────────────────────────

UnusedClassRemover::UnusedClassRemover(
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules)
    : keepRules_(std::move(keepRules))
{}

mjbase::spi::PreservedAnalyses UnusedClassRemover::run(mjbase::model::ResourceContainer&     container,
                                                       mjbase::spi::GlobalAnalysisManager& am)
{
    std::unordered_set<std::string> resourceKeepNames;
    if (!collectResourceKeepClasses(container, resourceKeepNames)) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    auto& index             = am.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);
    auto& callGraph         = am.getResult<mjbase::reference::CallGraphAnalysis>(container);
    auto& resolvedCallGraph = am.getResult<mjbase::asm_classpath::ResolvedCallGraphAnalysis>(container);

    mjbase::reference::GlobalReachabilityOptions options;
    options.callExpansion = mjbase::reference::GlobalReachabilityCallExpansion::Resolved;
    for (const std::string& className : resourceKeepNames) {
        options.rootClasses.emplace_back(className);
        for (const mjbase::reference::MethodDeclaration& method :
             index.declaredMethodsIn(className)) {
            options.rootMethods.push_back(method.method);
        }
    }

    // keep 规则（只加不减）：规则命中的类进 keep 名单，其声明方法同步提升为根
    // （与资源 keep 同名类的提升策略逐条对齐——类在即成员全在）。规则预读与主
    // 循环同契约：任一 class 不可解析整 pass 放弃（无部分删除）。
    if (keepRules_ != nullptr) {
        for (const auto& [uri, resource] : container.getResources()) {
            (void)uri;
            const auto* cls = ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(
                resource.get());
            if (cls == nullptr) continue;
            if (cls->get().empty()) return mjbase::spi::PreservedAnalyses::all();
            auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
            if (read.is_err()) return mjbase::spi::PreservedAnalyses::all();
            jbc::core::ReadResult readResult = std::move(read).unwrap();
            const std::string     className(readResult.file->thisName());
            if (className.empty()) return mjbase::spi::PreservedAnalyses::all();
            if (!keepRules_->matchesClass(className)) continue;
            // 注意：rootClasses 持 string_view——必须指向 resourceKeepNames 节点内
            // 的字符串（集合元素引用至 run() 结束稳定），不能指向循环局部 className。
            const auto ins = resourceKeepNames.insert(className);
            if (ins.second) {
                options.rootClasses.emplace_back(*ins.first);
                for (const mjbase::reference::MethodDeclaration& method :
                     index.declaredMethodsIn(className)) {
                    options.rootMethods.push_back(method.method);
                }
            }
        }
    }

    mjbase::reference::GlobalReachability reachability;
    reachability.build(index, callGraph, resolvedCallGraph, options);

    // 没有根（library jar、无 main、无资源根）时保持不变——调用方提供显式保留
    // 策略前不删库型容器。
    if (reachability.reachableClasses().empty()) return mjbase::spi::PreservedAnalyses::all();

    std::vector<std::string> toRemove;
    for (const auto& [uri, resource] : container.getResources()) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr) continue;
        if (cls->get().empty()) return mjbase::spi::PreservedAnalyses::all();
        auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
        if (read.is_err()) return mjbase::spi::PreservedAnalyses::all();
        jbc::core::ReadResult readResult = std::move(read).unwrap();
        const std::string     className(readResult.file->thisName());
        if (className.empty()) {
            return mjbase::spi::PreservedAnalyses::all();
        }
        if (!reachability.isClassReachable(className)) {
            toRemove.emplace_back(uri);
        }
    }

    if (toRemove.empty()) return mjbase::spi::PreservedAnalyses::all();
    auto& resources = container.getResources();
    for (const std::string& uri : toRemove) {
        resources.erase(uri);
    }
    return mjbase::spi::PreservedAnalyses::none();
}

// ── UnusedFieldRemover ───────────────────────────────────────────────────────

UnusedFieldRemover::UnusedFieldRemover(
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules,
    UnusedMemberAccessTier                                tier)
    : keepRules_(std::move(keepRules)), tier_(tier)
{}

mjbase::spi::PreservedAnalyses UnusedFieldRemover::run(mjbase::model::ResourceContainer&     container,
                                                       mjbase::spi::GlobalAnalysisManager& am)
{
    if (!allClassResourcesReadable(container)) return mjbase::spi::PreservedAnalyses::all();

    std::unordered_set<std::string> resourceKeepNames;
    if (!collectResourceKeepClasses(container, resourceKeepNames)) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    std::vector<LoadedClass> classes;
    if (!loadAllClasses(container, classes)) return mjbase::spi::PreservedAnalyses::all();

    auto& index = am.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);
    RetainedRoots roots;
    collectRetainedRoots(classes, index, resourceKeepNames, keepRules_.get(), roots);
    const mjbase::reference::GlobalReachability reachability =
        buildRetainedClosure(am, container, roots);

    // 没有根（library jar、无 main、无资源根）时保持不变——调用方提供显式保留
    // 策略前不删库型容器。
    if (reachability.reachableClasses().empty()) return mjbase::spi::PreservedAnalyses::all();

    mjbase::spi::ClassResourcePendingCommit pending;
    for (LoadedClass& loaded : classes) {
        const std::string& owner = loaded.name;
        if (!reachability.isClassReachable(owner)) continue;
        // keep 规则（只加不减）：类规则命中 → 整类跳过；成员规则命中 → 该字段保留。
        if (keepRules_ != nullptr && keepRules_->matchesClass(owner)) continue;

        bool modifiedClass = false;
        for (auto it = loaded.cf->fields.begin(); it != loaded.cf->fields.end();) {
            const jbc::core::MemberInfo& field = *it;
            const std::string            name(loaded.cf->nameOf(field));
            const std::string            desc(loaded.cf->descOf(field));
            if (matchesTier(field.access, tier_) &&
                !hasFieldKeepMetadata(*loaded.cf, field) &&
                !(keepRules_ != nullptr && keepRules_->matchesMember(owner, name, desc)) &&
                !reachability.isFieldReachable(owner, name, desc)) {
                it = loaded.cf->fields.erase(it);
                modifiedClass = true;
                continue;
            }
            ++it;
        }

        if (!modifiedClass) continue;
        auto written = jbc::core::writeClass(*loaded.cf);
        if (written.is_err()) {
            throw std::runtime_error(std::string("UnusedFieldRemover: write failed for '") +
                                     owner + "'");
        }
        pending.stage(loaded.resource, std::move(written).unwrap());
    }

    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();
    pending.commit();
    return mjbase::spi::PreservedAnalyses::none();
}

// ── GlobalUnusedMethodRemover ────────────────────────────────────────────────

GlobalUnusedMethodRemover::GlobalUnusedMethodRemover(
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules,
    UnusedMemberAccessTier                                tier)
    : keepRules_(std::move(keepRules)), tier_(tier)
{}

mjbase::spi::PreservedAnalyses GlobalUnusedMethodRemover::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    if (!allClassResourcesReadable(container)) return mjbase::spi::PreservedAnalyses::all();

    std::unordered_set<std::string> resourceKeepNames;
    if (!collectResourceKeepClasses(container, resourceKeepNames)) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    std::vector<LoadedClass> classes;
    if (!loadAllClasses(container, classes)) return mjbase::spi::PreservedAnalyses::all();

    auto& index = am.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);
    RetainedRoots roots;
    collectRetainedRoots(classes, index, resourceKeepNames, keepRules_.get(), roots);
    const mjbase::reference::GlobalReachability reachability =
        buildRetainedClosure(am, container, roots);

    // 没有根（library jar、无 main、无资源根）时保持不变——调用方提供显式保留
    // 策略前不删库型容器。
    if (reachability.reachableClasses().empty()) return mjbase::spi::PreservedAnalyses::all();

    mjbase::spi::ClassResourcePendingCommit pending;
    for (LoadedClass& loaded : classes) {
        const std::string& owner = loaded.name;
        if (!reachability.isClassReachable(owner)) continue;
        // keep 规则（只加不减）：类规则命中 → 整类跳过；成员规则命中 → 该方法保留。
        if (keepRules_ != nullptr && keepRules_->matchesClass(owner)) continue;

        bool modifiedClass = false;
        for (auto it = loaded.cf->methods.begin(); it != loaded.cf->methods.end();) {
            const jbc::core::MemberInfo& method = *it;
            const std::string            name(loaded.cf->nameOf(method));
            if (name == "<init>" || name == "<clinit>") {
                ++it;
                continue;
            }
            const std::string desc(loaded.cf->descOf(method));
            const bool unsupportedBody = (method.access & (kAccNative | kAccAbstract)) != 0;
            if (matchesTier(method.access, tier_) && !unsupportedBody &&
                !hasMethodKeepMetadata(*loaded.cf, method) &&
                !(keepRules_ != nullptr && keepRules_->matchesMember(owner, name, desc)) &&
                !reachability.isMethodReachable(owner, name, desc)) {
                it = loaded.cf->methods.erase(it);
                modifiedClass = true;
                continue;
            }
            ++it;
        }

        if (!modifiedClass) continue;
        auto written = jbc::core::writeClass(*loaded.cf);
        if (written.is_err()) {
            throw std::runtime_error(std::string("GlobalUnusedMethodRemover: write failed for '") +
                                     owner + "'");
        }
        pending.stage(loaded.resource, std::move(written).unwrap());
    }

    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();
    pending.commit();
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
