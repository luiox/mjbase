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
constexpr ca::u16 kAccPrivate  = 0x0002;
constexpr ca::u16 kAccStatic   = 0x0008;
constexpr ca::u16 kAccNative   = 0x0100;
constexpr ca::u16 kAccAbstract = 0x0400;

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

/// 容器内全部 class 资源可解析性预检（旧栈 allClassResourcesReadable 对齐）：
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

/// 类是否携带 Module 描述符（module-info keep 根，对齐旧栈 moduleNode 判定）。
bool isModuleDescriptor(const jbc::core::ClassFile& cf)
{
    return findAttrByName(cf, cf.attrs, "Module") != nullptr;
}

/// 字段 keep metadata：Signature 或任一注解族属性在场（旧栈 hasFieldKeepMetadata）。
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

/// 方法 keep metadata：字段面 + 参数注解 + AnnotationDefault（旧栈
/// hasMethodKeepMetadata；局部变量注解在 libjbc 同属 Runtime*TypeAnnotations
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
/// 文件名与内容行（旧栈 collectResourceKeepClasses 逐条对齐）。
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
            // 内容逐行：'#' 注释截断、CRLF 归一、二名 '.' → '/'（旧栈
            // appendServiceClassNames 对齐）。
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
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules)
    : keepRules_(std::move(keepRules))
{}

mjbase::spi::PreservedAnalyses UnusedFieldRemover::run(mjbase::model::ResourceContainer&     container,
                                                       mjbase::spi::GlobalAnalysisManager& am)
{
    if (!allClassResourcesReadable(container)) return mjbase::spi::PreservedAnalyses::all();
    auto& reachability = am.getResult<mjbase::reference::ResolvedGlobalReachabilityAnalysis>(container);
    if (reachability.reachableClasses().empty()) return mjbase::spi::PreservedAnalyses::all();

    mjbase::spi::ClassResourcePendingCommit pending;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) continue;
        auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource.get());

        LoadedClass loaded;
        if (!loadClass(*cls, loaded)) return mjbase::spi::PreservedAnalyses::all();

        const std::string owner(loaded.cf->thisName());
        if (!reachability.isClassReachable(owner)) continue;
        // keep 规则（只加不减）：类规则命中 → 整类跳过；成员规则命中 → 该字段保留。
        if (keepRules_ != nullptr && keepRules_->matchesClass(owner)) continue;

        bool modifiedClass = false;
        for (auto it = loaded.cf->fields.begin(); it != loaded.cf->fields.end();) {
            const jbc::core::MemberInfo& field = *it;
            const std::string            name(loaded.cf->nameOf(field));
            const std::string            desc(loaded.cf->descOf(field));
            if ((field.access & kAccPrivate) != 0 &&
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
        pending.stage(cls, std::move(written).unwrap());
    }

    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();
    pending.commit();
    return mjbase::spi::PreservedAnalyses::none();
}

// ── GlobalUnusedMethodRemover ────────────────────────────────────────────────

GlobalUnusedMethodRemover::GlobalUnusedMethodRemover(
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules)
    : keepRules_(std::move(keepRules))
{}

mjbase::spi::PreservedAnalyses GlobalUnusedMethodRemover::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    if (!allClassResourcesReadable(container)) return mjbase::spi::PreservedAnalyses::all();
    auto& reachability = am.getResult<mjbase::reference::ResolvedGlobalReachabilityAnalysis>(container);
    if (reachability.reachableClasses().empty()) return mjbase::spi::PreservedAnalyses::all();

    mjbase::spi::ClassResourcePendingCommit pending;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) continue;
        auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource.get());

        LoadedClass loaded;
        if (!loadClass(*cls, loaded)) return mjbase::spi::PreservedAnalyses::all();

        const std::string owner(loaded.cf->thisName());
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
            if ((method.access & kAccPrivate) != 0 && !unsupportedBody &&
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
        pending.stage(cls, std::move(written).unwrap());
    }

    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();
    pending.commit();
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
