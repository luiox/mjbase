/// @file        resource_container.hpp
/// @brief       资源容器：IResourceContainer 接口 + unordered_map 默认实现。
///
/// URI 键语义（字符串约定）：键文本驻留在容器自有的
/// Utf8StringArena（只增不移、内容去重），对外一律以 std::string_view 借用——
/// addResource 的入参视图在调用返回后即可释放；visitResources /
/// duplicateResourceUris 发出的视图指向 arena，容器存活期内有效。
///
/// 重复条目纪律：addResource 对已知 URI first-wins
/// 保留首条目（对齐 JVM 类加载），后条目丢弃并记录；有意替换走
/// replaceResource / getResources()。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "resource_model.hpp"

namespace mjbase::model {

class IResourceImporter;
class IResourceExporter;

/// @brief 资源容器接口，统一管理多种类型的资源。
class IResourceContainer
{
public:
    virtual ~IResourceContainer() = default;
    /// @brief 添加资源，按 URI 索引；重复 URI first-wins 并记录（见实现）。
    virtual void addResource(std::string_view uri, std::unique_ptr<IResource> resource) = 0;
    /// @brief 检查 URI 对应的资源是否存在。
    virtual bool hasResource(std::string_view uri) const = 0;
    /// @brief 获取 URI 对应的资源（原始指针，不转移所有权）；不存在返回 nullptr。
    virtual IResource* getResource(std::string_view uri) const = 0;
    /// @brief 获取容器中资源总数。
    virtual ca::usize size() const = 0;
    /// @brief 清空所有资源。
    virtual void clear() = 0;
    /// @brief 遍历所有资源；uri 视图借用容器 arena，回调期内有效。
    virtual void visitResources(
        const std::function<void(std::string_view, const IResource&)>& visitor) const = 0;
    /// @brief 使用导入器向容器中导入资源（自动 beginImport）。
    virtual ModelResult<void> importResource(IResourceImporter* importer) = 0;
    /// @brief 使用导出器将容器中资源导出。
    virtual ModelResult<void> exportResource(IResourceExporter* exporter) = 0;
};

/// @brief 基于 unordered_map 的资源容器默认实现。
class ResourceContainer : public IResourceContainer
{
public:
    void addResource(std::string_view uri, std::unique_ptr<IResource> resource) override;
    /// @brief 显式替换（或新增）资源，不触发重复导入告警。
    ///
    /// 候选重建、事务预览、undo/redo 回滚等内部流程对已知 URI 的有意替换
    /// 不属于「重复导入」语义，不应记入 duplicateResourceUris_/日志告警；
    /// 外部导入路径（导入器、一次性导入）请继续使用 addResource。
    void replaceResource(std::string_view uri, std::unique_ptr<IResource> resource)
    {
        resources_[internUri(uri)] = std::move(resource);
    }
    bool       hasResource(std::string_view uri) const override;
    IResource* getResource(std::string_view uri) const override;
    ca::usize  size() const override { return resources_.size(); }
    void       clear() override { resources_.clear(); }
    void       visitResources(
              const std::function<void(std::string_view, const IResource&)>& visitor) const override;
    ModelResult<void> importResource(IResourceImporter* importer) override;
    ModelResult<void> exportResource(IResourceExporter* exporter) override;
    /// @brief 标记一次新导入的开始：清空上一轮的重复条目记录。
    ///
    /// duplicateResourceUris() 只反映最近一次导入。importResource 会自动
    /// 调用；进度导入路径（JarFileImporter::importResourceWithProgress）
    /// 不经过本容器方法，导入方（ResourceHelper）须先调用本方法。
    void beginImport() { duplicateResourceUris_.clear(); }
    /// @brief 获取底层资源映射表（可修改）。
    ///
    /// 键视图借用容器 arena：经本表手工插入新条目时，键必须指向存活期覆盖
    /// 容器使用期的存储（常规路径请走 addResource / replaceResource）。
    std::unordered_map<std::string_view, std::unique_ptr<IResource>>& getResources()
    {
        return resources_;
    }

    /// @brief 导入期间重复出现的条目名，按出现顺序记录（issue #962）。
    ///
    /// 重复条目按 JVM 类加载语义 first-wins 保留首条目，后条目丢弃并记录
    /// 在此。此前 last-wins 导入会让「工具分析的对象」与「JVM 实际加载的
    /// 对象」错配。导入方应检查此列表并向用户告警。每次导入开始时清空
    /// （见 beginImport）。视图借用容器 arena，容器存活期内有效。
    [[nodiscard]] const std::vector<std::string_view>& duplicateResourceUris() const noexcept
    {
        return duplicateResourceUris_;
    }

private:
    /// @brief URI 驻留进 arena 并返回稳定视图（内容去重，同内容同视图）。
    std::string_view internUri(std::string_view uri);

    std::unordered_map<std::string_view, std::unique_ptr<IResource>> resources_;
    std::vector<std::string_view>                                    duplicateResourceUris_;
    ca::str::Utf8StringArena                                         arena_;
};

// ── Multi-Release 族视图（JEP 238）─────────────────────────────────────────
//
// 版本化条目（META-INF/versions/<N>/ 下的 class）与 base 条目共用同一类内部名，
// 语义上是同一个类的多个 release 变体（一个「族」）。容器层按 URI 形状提供族
// 视图，供改名/删除/选择器各面把「按内名结算」从巧合固化为契约——任何按内名
// 键控的结算都必须对族内全部成员一致生效。

/// @brief class URI 的版本段解析。
///
/// `META-INF/versions/<纯数字>/…` 形返回版本段数值；其余形状（前缀不符、版本
/// 段缺失或非纯数字、数值溢出）返回 -1。这是形状级判定（任何数字段都算版本化
/// 形状）；JEP 238 的语义口径（版本 ≥ 9 才被 JVM 识别为版本化类条目）由调用
/// 方叠加 `version >= 9` 判定。
ca::i64 multiReleaseVersionOfClassUri(std::string_view uri);

/// @brief class URI → 类内部名。
///
/// 剥 `META-INF/versions/<N>/` 前缀（JEP 238 口径：N 为 ≥ 9 的纯数字）与
/// `.class` 后缀；其余形状（含 N < 9 或非数字版本段）返回空串——它们不是
/// JVM 可识别的 canonical class URI。与 jar 层条目名判定一致按大小写敏感
/// 处理后缀。
std::string internalNameFromClassUri(std::string_view uri);

/// @brief 同内名 versioned 族视图（非拥有）。
struct ClassFamilyView
{
    /// base 形条目 URI（借用容器 arena 视图）；容器无 base 形条目时为空视图。
    std::string_view baseUri;

    /// 版本化条目：版本号 + URI 借用视图。
    struct Variant
    {
        ca::i64          version;   ///< 版本段数值（≥ 9）
        std::string_view uri;       ///< 条目 URI（借用容器 arena 视图）
    };
    /// 版本化条目列表，按 (版本号, URI) 升序。
    std::vector<Variant> variants;
};

/// @brief 容器 class 资源的同内名族索引（URI 形状推导，不解析字节）。
///
/// 键 = internalNameFromClassUri 推导的内部名（自有存储）；值中的 URI 视图
/// 借用容器 arena，容器存活期内有效。推导不出内名的条目（非 class 资源、非
/// class 形状、版本段 N < 9）不入索引。base 缺失而仅存版本化变体的族原样
/// 呈现（baseUri 空视图）——本层不做「最小变体升格 base」的布局重构。
std::unordered_map<std::string, ClassFamilyView> buildClassFamilyIndex(
    const IResourceContainer& container);

}   // namespace mjbase::model
