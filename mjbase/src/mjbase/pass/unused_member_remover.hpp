#pragma once

#include <memory>

#include "mjbase/model/resource_container.hpp"
#include "mjbase/reference/keep_rule.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @defgroup unused_member_cleanup 未使用成员清理家族（移植自 mbase
/// pass/opt/unused_member_remover 的容器级直写四件）。
///
/// 本家族把「可达性 / 引用索引分析证明不可达」的类、字段、方法转换为字节码删除。
/// 四个 pass 按分析来源分层：
///
/// - `EmptyClassRemover`：最朴素，只看单个 class 是否无字段无方法，不做可达性分析。
/// - `UnusedClassRemover`：消费 ReferenceIndex + CallGraph + ResolvedCallGraph 建
///   GlobalReachability 闭包（Resolved 展开），删除闭包外的 class 资源。
/// - `UnusedFieldRemover` / `GlobalUnusedMethodRemover`：消费
///   ResolvedGlobalReachabilityAnalysis，删除可达类内不可达的 private 字段/方法。
///
/// 共享策略：根策略 = 资源 keep 名单（module 描述符、Manifest
/// 的 Main/Premain/Agent/Launcher-Agent/JavaFX 入口、META-INF/services 文件名与
/// 内容）+ Java `public static main(String[])` + 类初始化器保留；方法体展开用
/// ResolvedCallGraph，删除前保留虚调用和接口调用的覆写目标。可达类闭包为空
/// （library jar、无根）时整 pass 原样保留输入。构造器、`<clinit>`、非 private
/// 成员、keep metadata（Signature/注解族）成员均保留。分析未注册时抛
/// `logic_error`（配置错误，稳定失败）。
///
/// ## keep 规则面（审计 P0-1/P0-2）
///
/// 四个 pass 的构造器接受可选 `KeepRuleSet`（默认 nullptr = 旧行为）。规则集是
/// **纯加法**输入：命中的类/成员在既有硬底线之上再保留一份，无法关闭任何硬底线
/// （keep_rule.hpp「规则只加不减」契约）。类规则语义对齐 商业化 trim 工具的排除语义：命中类
/// 整体保留，其声明方法提升为可达性根（与资源 keep 同名类的提升策略一致）；成员
/// 规则只对「否则会被删除」的 private 成员产生可观测差异（非私有成员本就地保留）。
/// 运行时供给面（CLI/配置解析）属 cli 批，本层只提供程序化注入点。
///
/// 载体重定型注记：成员删除在 libjbc 低层 DOM 上进行（erase MemberInfo，成员
/// 在 classfile 中按 name/desc 经 CP 引用、无槽位索引依赖，删除结构安全）；
/// 常量池不压缩，失引条目保留（append-only 契约，writer 级差异由 golden
/// 声明类目收口）。需要 JVM 真跑门的产品化使用方为 `opt-in`。
/// @{

/// @brief 删除没有字段和方法的空 class 资源。
///
/// 只接受能够完整解析的 class 资源；非 class 资源、畸形字节和带字段/方法的类
/// 保持不变。不执行可达性分析。仅在确实删除资源时返回 none()。
///
/// @param keepRules 可选 keep 规则集（默认无）：规则命中的类不删。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class EmptyClassRemover : public mjbase::spi::GlobalPass
{
public:
    explicit EmptyClassRemover(
        std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules = nullptr);

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules_;
};

/// @brief 根据全局可达性删除不可达 class 资源。
///
/// 比较的是 classfile 头中的声明名而不是资源路径（JAR 条目可能被重命名或与
/// classfile 头不一致）。任一类名读取失败立即整 pass 放弃（返回 all()，无部分
/// 删除）；所有解析完成后才提交删除。
///
/// @param keepRules 可选 keep 规则集（默认无）：规则命中的类进 keep 名单，其
///         声明方法同步提升为根方法（与资源 keep 同名类策略一致）。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class UnusedClassRemover : public mjbase::spi::GlobalPass
{
public:
    explicit UnusedClassRemover(
        std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules = nullptr);

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules_;
};

/// @brief 从可达类中删除明确不可达的 private 字段。
///
/// public/protected 字段一律保留（keep 规则落地前不破坏公共 API、反射与框架
/// 注入）。带 Signature/注解族 keep metadata 的字段保留。所有 class 先完成解析
/// 和序列化，再一次提交（两阶段提交，作业级原子）；任一解析或序列化失败保持
/// 全部原始 class bytes。
///
/// @param keepRules 可选 keep 规则集（默认无）：规则命中的类整类跳过；规则命中的
///         字段保留（两类判定均只加不减）。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class UnusedFieldRemover : public mjbase::spi::GlobalPass
{
public:
    explicit UnusedFieldRemover(
        std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules = nullptr);

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules_;
};

/// @brief 使用全局可达性删除不可达的 private 方法。
///
/// 构造器与 `<clinit>` 始终保留；native/abstract（无方法体语义承载）与带
/// keep metadata 的方法保守保留。两阶段提交语义同 UnusedFieldRemover。
///
/// @param keepRules 可选 keep 规则集（默认无）：规则命中的类整类跳过；规则命中的
///         方法保留（两类判定均只加不减）。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class GlobalUnusedMethodRemover : public mjbase::spi::GlobalPass
{
public:
    explicit GlobalUnusedMethodRemover(
        std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules = nullptr);

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules_;
};

/// @}

}   // namespace mjbase::pass
