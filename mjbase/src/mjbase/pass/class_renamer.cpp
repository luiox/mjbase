#include "mjbase/pass/class_renamer.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
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
/// 与 renamedClassBase 逐字节对齐（新名 = 基段 + 全局递增编号）。
std::string renamedClassBase(const std::string& oldName)
{
    const auto slash = oldName.rfind('/');
    if (slash == std::string::npos) return "Class";
    return oldName.substr(0, slash + 1) + "Class";
}

/// 装载完成的类：会话（backing + IR 同生共死）+ 类名视图 + 写回目标。
struct LoadedClass
{
    std::string                           name;      ///< 类内部名（会话解析面）
    mjbase::model::ClassResource*         resource = nullptr;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 装载单个 class 资源；空 bytes / 不可解析 / 无类名跳过。
bool loadClass(mjbase::model::ClassResource& resource, LoadedClass& out)
{
    const auto& bytes = resource.get();
    if (bytes.empty()) return false;
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (sessionResult.is_err()) return false;
    out.session = std::move(sessionResult).unwrap();
    out.name.assign(out.session->backing().thisName());
    if (out.name.empty()) return false;
    out.resource = &resource;
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses ClassRenamer::run(mjbase::model::ResourceContainer&     container,
                                                 mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    // 阶段零：装载与准入，按类内部名字节序排序（编号确定性的来源）。
    std::vector<LoadedClass> loaded;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!resource ||
            !ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) {
            continue;
        }
        auto*       cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource.get());
        LoadedClass entry;
        if (loadClass(*cls, entry)) loaded.push_back(std::move(entry));
    }
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    std::sort(loaded.begin(), loaded.end(),
              [](const LoadedClass& lhs, const LoadedClass& rhs) { return lhs.name < rhs.name; });

    // 阶段一：命名计划。编号 N 全局递增不按类重置，集合查重取首个未占用名
    //（单计数器语义：容器按旧名序得到 Class0/Class1/...）。
    mjbase::asm_remapper::MemberRemapPlan plan;
    std::set<std::string>                 allocatedNames;
    int                                   id = 0;
    for (const LoadedClass& entry : loaded) {
        const std::string base = renamedClassBase(entry.name);
        std::string       mappedName;
        do {
            mappedName = base + std::to_string(id++);
        } while (!allocatedNames.insert(mappedName).second);
        plan.mapClass(entry.name, mappedName);
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
        item.oldUri = entry.resource->getLocation().to_std_string();
        item.newUri = std::string(plan.mapInternalName(entry.name)) + ".class";
        item.bytes  = std::move(written).unwrap();
        updated.push_back(std::move(item));
    }

    if (updated.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 阶段三：提交。先删全部旧条目，再插全部新条目（避免
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
