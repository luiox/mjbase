/// @file        container_stack.hpp
/// @brief       角色化容器栈：多容器挂载的合并视图、first-wins 解析、角色
///              过滤与复合寻址扩展位。
///
/// 上游依据：mjt 仓 RFC 0001 §5（容器抽象）、§10-P5；工程登记结构见
/// analysis_project.hpp schema v2（role: primary/library/runtime + 挂载树
/// parent）。本类是 model 层能力：只认「容器 + 角色 + 稳定 id」，不知道
/// project.json——登记（AnalysisInput）到挂载的接线归上层消费方（workspace /
/// mjt）。
///
/// 解析序裁决（RFC §5.2 角色化容器栈）：primary > library（按挂载序）>
/// runtime（按挂载序）；同 URI 多容器出现时 first-wins 保留解析序首位条目，
/// 其余记录进 shadowedEntries（对齐 IResourceContainer 的 JVM first-wins 纪律
/// 与 workspace analysisScanContainer 的「主容器 URI 优先」先例）。
///
/// 生命周期纪律：容器对象借用（IResourceContainer* 非拥有）——所有权归调用
/// 方，调用方保证容器存活覆盖本栈使用期。
#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "resource_container.hpp"
#include "resource_model.hpp"

namespace mjbase::model {

/// @brief 工程内容器的角色（与 analysis_project.hpp schema v2 的 role 值域
///        对应；support 遗留值由上层按 library 语义接线，不进本枚举）。
enum class ContainerRole : ca::u8
{
    Primary, ///< 主分析对象（解析序最先）
    Library, ///< 依赖库（解析序居中，按挂载序）
    Runtime, ///< 运行时镜像即 JDK（解析序最后）
};

/// @brief ContainerRole → 小写串（"primary"/"library"/"runtime"；登记/诊断用）。
std::string_view to_string(ContainerRole role) noexcept;

/// @brief 一条挂载记录。
struct MountedContainer
{
    ca::str::Utf8String id;        ///< 工程内唯一（登记 path 或 digest 短形，调用方定）
    ContainerRole       role = ContainerRole::Primary;
    IResourceContainer* container = nullptr; ///< 借用视图（非拥有）
};

/// @brief 重复条目记录：同 URI 在多容器出现时的 first-wins 输家。
struct ShadowedEntryRecord
{
    ca::str::Utf8String uri;        ///< 条目 URI
    ca::str::Utf8String winnerId;   ///< 解析序首位容器（保留条目所在）
    ca::str::Utf8String shadowedId; ///< 被遮蔽容器（其同 URI 条目不参与解析）
};

/// @brief 角色化容器栈：多容器合并视图的解析/枚举/克隆面。
class MountedContainerSet
{
public:
    /// @brief 挂载容器；id 重复、id 空或 container 空返回 BadArgument（不改动）。
    ModelResult<void> mount(std::string_view id, ContainerRole          role,
                            IResourceContainer* container);

    /// @brief 按 id 摘除挂载；命中返回 true 并重建解析序与重复记录（被遮蔽
    ///        副本随重建浮现——对齐 JVM 移除 classpath 条目后的可见性）。
    bool unmount(std::string_view id);

    /// 挂载容器数。
    ca::usize size() const { return containers_.size(); }

    /// 全部挂载记录（挂载序；指针在增删后失效）。
    const std::vector<MountedContainer>& containers() const { return containers_; }

    /// 按 id 查挂载；未命中 nullptr。
    const MountedContainer* find(std::string_view id) const;

    /// @brief URI 解析：沿解析序取首个命中容器内的条目（first-wins）；未命中
    ///        nullptr。
    IResource* resolve(std::string_view uri) const;

    /// URI 是否在解析序任一容器命中。
    bool contains(std::string_view uri) const { return resolve(uri) != nullptr; }

    /// @brief 重复条目记录（卸载/挂载后重建；按 (uri, 发现序) 稳定排列）。
    const std::vector<ShadowedEntryRecord>& shadowedEntries() const { return shadowed_; }

    /// @brief 角色过滤遍历：只走指定角色的容器（角色内原样枚举，不做跨角色
    ///        合并）——primary/library/runtime 可见性区分的查询面。
    void visitRole(ContainerRole                                                   role,
                   const std::function<void(std::string_view, const IResource&)>& visitor) const;

    /// @brief 合并视图：按解析序把条目克隆进新 ResourceContainer（addResource
    ///        first-wins + duplicateResourceUris 记录，resource_container.hpp
    ///        现状语义）；喂既有索引/扫描面（ReferenceIndex::build 等）即
    ///        xref/索引跨容器归并。
    ///
    /// 克隆失败（未知资源类型）返回 BadArgument——合并视图是全量交付，半份
    /// 视图比失败更危险。
    ModelResult<std::unique_ptr<ResourceContainer>> mergedView() const;

    //-- 复合寻址扩展位（RFC §5.3；本期只落接口，不实现解包）------------------

    /// @brief 挂载链地址分隔符（bang 链）：`<containerId>!/entry/path`。
    static constexpr std::string_view kAddressSeparator = "!/";

    /// @brief 装配挂载链地址（containerId / entryUri 任一空 = 空串，调用方
    ///        自行处置；本层不校验）。
    static ca::str::Utf8String mountAddress(std::string_view containerId,
                                            std::string_view entryUri);

    /// @brief 地址解析结果：定位容器 + 容器内条目 URI。
    struct ResolvedAddress
    {
        const MountedContainer* container = nullptr;
        ca::str::Utf8String     entryUri; ///< 剥去首段容器 id 后的条目寻址
    };

    /// @brief 解析挂载链地址：按首段分隔符拆出容器 id，在其容器内定位条目
    ///        （地址钉死容器，不做跨栈 first-wins；条目未命中 = NotFound）。
    ///
    /// 本期只支持单容器形态；entryUri 内残留 `!/`（嵌套 WAR/JAR 钻井）按
    /// BadArgument 拒绝并指路扩展位（TODO 注记，doc_ai 设计文档同题）。
    /// 未知容器 id = NotFound；无分隔符/空段 = BadArgument。
    ModelResult<ResolvedAddress> resolveAddress(std::string_view address) const;

private:
    /// 重建重复记录（挂载/卸载后；按解析序扫描，first-wins 输家逐对记录）。
    void rebuildShadowedEntries();

    std::vector<MountedContainer>    containers_;   ///< 挂载序
    std::vector<ShadowedEntryRecord> shadowed_;    ///< (uri, 发现序) 稳定
};

}   // namespace mjbase::model
