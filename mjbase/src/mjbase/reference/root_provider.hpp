/// @file        root_provider.hpp
/// @brief       可达性根提供者：把多种根来源收敛为值语义 RootSet。
///
/// ## 动机
///
/// 可达性闭包的根原先只有两条写入路径：调用方直接往
/// `GlobalReachabilityOptions` 三槽里塞，或调用 `applyKeepRoots`（keep 规则 +
/// 档位）。每新增一种根来源（main 发现、模块描述符、Manifest、services、
/// 产品契约）就多一条平行入口，删除 pass 无法把「还有哪些根来源」当作数据
/// 看待。
///
/// 本面把根来源统一成 `IRootProvider`：只读索引、返回 `RootSet`。组合方式是
/// 调用方持有一个 provider 指针列表并调用 `appendRoots`——没有注册表、没有
/// 全局状态、没有回调，provider 也不持有索引。这与仓内「Result-only API /
/// 无粘性状态」的纪律一致：一次收集是一次纯查询，生命周期由调用方掌握。
///
/// ## 生命周期契约
///
/// `RootSet` 的三槽与 `GlobalReachabilityOptions` 一样持借用视图。内置
/// provider 写入的视图一律指向 `ReferenceIndex` 池内文本（与声明事实同源），
/// 调用方须保证索引存活到 `GlobalReachability::build` 之后。自定义 provider
/// 必须遵守同一契约，或让视图指向自有的、生命周期覆盖消费点的存储。
///
/// ## 只加不减与去重
///
/// `appendRoots` 只追加，不清空既有根、不关闭任何既有保留依据；也不去重——
/// 可达性 build 的 `markClass`/`markMethod`/`markField` 本是集合语义，重复根
/// 只多一次哈希查找。provider 之间的重叠（例如 keep 档位与 main 发现命中同一
/// 方法）是合法且常见的。
#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/keep_rule.hpp"
#include "mjbase/reference/reference_index.hpp"

namespace mjbase::reference {

/// @brief 一次根收集的产物：类 / 方法 / 字段三槽（借用视图）。
struct RootSet
{
    std::vector<std::string_view> classes;   ///< 类内部名视图
    std::vector<MethodRefKey>     methods;
    std::vector<FieldRefKey>      fields;
};

/// @brief 根提供者纯接口。
///
/// 实现必须是 const 可重入的纯查询：不修改索引、不缓存跨调用状态、不依赖
/// 调用顺序。`collect` 返回的视图生命周期见文件头契约。
class IRootProvider
{
public:
    virtual ~IRootProvider() = default;

    /// @brief 从索引声明事实收集根；返回值为纯数据，可自由合并。
    virtual RootSet collect(const ReferenceIndex& index) const = 0;
};

/// @brief keep 规则 + 档位 → 根（包装既有 applyKeepRoots）。
///
/// 规则集按值持有：provider 可独立于配置解析现场存活，避免 `KeepRootSpec`
/// 的借用指针在异步/延迟收集场景悬垂。
class KeepRootProvider final : public IRootProvider
{
public:
    KeepRootProvider() = default;

    /// @brief 只走档位（规则为空）。
    explicit KeepRootProvider(KeepTier tier) noexcept : tier_(tier) {}

    /// @brief 规则 + 档位；两者都空即恒等（不产根）。
    KeepRootProvider(KeepRuleSet rules, KeepTier tier = KeepTier::None)
        : rules_(std::move(rules)), tier_(tier)
    {}

    RootSet collect(const ReferenceIndex& index) const override;

private:
    KeepRuleSet rules_;
    KeepTier    tier_ = KeepTier::None;
};

/// @brief Java `main(String[])` 声明 → 方法根。
///
/// 与 `GlobalReachabilityOptions::includeMainMethods` 不冲突：两者都打开的
/// 时候选集合相同，重复根由可达性集合语义吸收。需要自定义 main 策略（例如
/// 只保留指定类里的 main）的消费方应关闭内建发现、改用自建 provider。
class MainMethodRootProvider final : public IRootProvider
{
public:
    RootSet collect(const ReferenceIndex& index) const override;
};

/// @brief 把一份根集合追加进可达性选项（保留既有根与顺序）。
void appendRoots(const RootSet& roots, GlobalReachabilityOptions& options);

/// @brief 依次收集各 provider 并追加；空指针条目跳过。
///
/// 收集顺序即追加顺序；同一 provider 出现多次会重复追加（合法，见文件头）。
void appendRoots(const std::vector<const IRootProvider*>& providers,
                 const ReferenceIndex& index, GlobalReachabilityOptions& options);

}   // namespace mjbase::reference
