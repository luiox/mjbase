#include "mjbase/pass/class_renamer.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjbase::pass {

namespace {

/// 旧名到新名基段：最后一个 '/' 截断，取 `包前缀 + "Class"`；无 '/' 用 `"Class"`。
/// 与旧栈 renamedClassBase 逐字节对齐（新名 = 基段 + 全局递增编号）。
std::string renamedClassBase(const std::string& oldName)
{
    const auto slash = oldName.rfind('/');
    if (slash == std::string::npos) return "Class";
    return oldName.substr(0, slash + 1) + "Class";
}

/// 写回 URI：base 形 → `新名.class`；`META-INF/versions/<N>/` 形 → 版本前缀
/// （含版本段）原样保留，仅以新名重写其余部分。
///
/// 版本化形状按纯数字版本段判定（任何值，含 JEP 238 语义外的小于 9 段）：
/// 版本化形状的条目一律不得迁出原前缀——被 JVM 忽略的 `versions/8` 条目若迁
/// 出会变为可加载，并与 base 族员的写回 URI 撞车（族坍缩回归）。
std::string renamedClassUri(const std::string& oldUri, std::string_view newName)
{
    if (model::multiReleaseVersionOfClassUri(oldUri) < 0) {
        return std::string(newName) + ".class";
    }
    constexpr std::string_view kPrefix = "META-INF/versions/";
    const auto versionEnd = oldUri.find('/', kPrefix.size());
    return oldUri.substr(0, versionEnd + 1) + std::string(newName) + ".class";
}

/// 装载完成的类：会话（backing + IR 同生共死）+ 类名视图 + 写回目标。
struct LoadedClass
{
    std::string                           name;      ///< 类内部名（会话解析面）
    std::string                           uri;       ///< 容器 URI（族并列序与写回形状的来源）
    mjbase::model::ClassResource*         resource = nullptr;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 装载单个 class 资源；空 bytes / 不可解析 / 无类名跳过（对齐旧栈准入）。
bool loadClass(mjbase::model::ClassResource& resource, const std::string_view uri,
               LoadedClass& out)
{
    const auto& bytes = resource.get();
    if (bytes.empty()) return false;
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (sessionResult.is_err()) return false;
    out.session = std::move(sessionResult).unwrap();
    out.name.assign(out.session->backing().thisName());
    if (out.name.empty()) return false;
    out.uri.assign(uri);
    out.resource = &resource;
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses ClassRenamer::run(mjbase::model::ResourceContainer&     container,
                                                 mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    // 阶段零：装载与准入，按 (类内部名, URI) 字节序排序（编号确定性的来源）。
    // base 与 META-INF/versions/<N>/ 变体内名相同并列时，URI 是唯一的次序
    // 决胜键——只按内名排序则族内相对顺序随容器迭代序漂移。
    std::vector<LoadedClass> loaded;
    for (auto& [uri, resource] : container.getResources()) {
        if (!resource ||
            !ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) {
            continue;
        }
        auto*       cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource.get());
        LoadedClass entry;
        if (loadClass(*cls, uri, entry)) loaded.push_back(std::move(entry));
    }
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    std::sort(loaded.begin(), loaded.end(), [](const LoadedClass& lhs, const LoadedClass& rhs) {
        if (lhs.name != rhs.name) return lhs.name < rhs.name;
        return lhs.uri < rhs.uri;
    });

    // 阶段一：命名计划。编号 N 全局递增不按类重置，集合查重取首个未占用名
    //（旧栈单计数器语义：容器按旧名序得到 Class0/Class1/...）。
    //
    // 同内名族（base + versions/<N> 变体）只消耗一个编号、共享同一新名：
    // 族成员在 JVM 语义上是同一个类的多个 release 变体（内名必须一致，差异
    // 只在条目路径）；命名计划又按内名键控，族内发两个新名既表达不了（后发
    // 覆盖先发）也不可加载。
    mjbase::asm_remapper::MemberRemapPlan plan;
    std::set<std::string>                 allocatedNames;
    int                                   id       = 0;
    std::string                           prevName;
    std::string                           familyName;
    for (const LoadedClass& entry : loaded) {
        if (prevName.empty() || entry.name != prevName) {
            const std::string base = renamedClassBase(entry.name);
            do {
                familyName = base + std::to_string(id++);
            } while (!allocatedNames.insert(familyName).second);
            plan.mapClass(entry.name, familyName);
            prevName = entry.name;
        }
    }

    // 阶段二：逐类应用计划 + 序列化（全部完成后才动容器——作业级原子）。
    struct UpdatedClass
    {
        std::string              oldUri;
        std::string              newUri;
        std::vector<ca::u8>      bytes;
    };
    std::vector<UpdatedClass> updated;
    updated.reserve(loaded.size());

    for (LoadedClass& entry : loaded) {
        auto remapped = mjbase::asm_remapper::applyMemberRemapPlan(*entry.session, plan);
        if (remapped.is_err()) {
            throw std::runtime_error(std::string("ClassRenamer: remap failed for '") +
                                     entry.name + "': " +
                                     mjbase::asm_remapper::to_string(remapped.unwrap_err()));
        }
        if (!remapped.unwrap()) continue;
        auto written = jbc::core::writeClass(entry.session->backing());
        if (written.is_err()) {
            throw std::runtime_error(std::string("ClassRenamer: write failed for '") +
                                     entry.name + "'");
        }
        UpdatedClass item;
        item.oldUri = std::move(entry.uri);
        // 写回保持 URI 形状：base → `新名.class`，版本化 →
        // `META-INF/versions/<N>/新名.class`（同族各成员 URI 互异，不再坍缩）。
        item.newUri = renamedClassUri(item.oldUri, plan.mapInternalName(entry.name));
        item.bytes  = std::move(written).unwrap();
        updated.push_back(std::move(item));
    }

    if (updated.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 阶段三：提交。先删全部旧条目，再插全部新条目（顺序对齐旧栈，避免
    // 「新名恰为另一类旧 URI」时的中间态冲突）。
    auto& resources = container.getResources();
    for (const UpdatedClass& item : updated) {
        resources.erase(item.oldUri);
    }
    for (UpdatedClass& item : updated) {
        container.replaceResource(item.newUri,
                                  std::make_unique<mjbase::model::ClassResource>(
                                      item.newUri, std::move(item.bytes)));
    }

    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
