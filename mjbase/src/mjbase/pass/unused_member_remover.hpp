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
/// - `UnusedFieldRemover` / `GlobalUnusedMethodRemover`：自建 GlobalReachability
///   闭包（Resolved 展开，根策略见下），按 @ref UnusedMemberAccessTier 档位删除
///   可达类内不可达字段/方法。
///
/// 共享策略：根策略 = 资源 keep 名单（module 描述符、Manifest 的
/// Main/Premain/Agent/Launcher-Agent/JavaFX 入口、META-INF/services 文件名与内容）
/// + Java `public static main(String[])` + 类初始化器保留；方法体展开用
/// ResolvedCallGraph，删除前保留虚调用和接口调用的覆写目标。可达类闭包为空
/// （library jar、无根）时整 pass 原样保留输入。分析未注册时抛 `logic_error`
/// （配置错误，稳定失败，对齐旧栈）。
///
/// ## 删除安全不变量与「调用点收尾」裁定
///
/// 成员删除两件的安全论证与商业化 trim 工具同构：其 trim 主流程（mark 相
/// work-queue 不动点 → sweep 二分）从不挂接调用点收尾件——保留代码引用到的成员
/// 在 mark 相必然已入保留集，sweep 后保留代码天然无悬垂引用；其 removeMethodCalls
/// 设施是与 trim 平行的 spec 驱动件（用户点名成员 → 全程序收集调用点 → 按名单
/// 删调用指令），不是 sweep 的兜底清扫。本仓同构：删除候选 = 容器内声明 ∧ 不在
/// 闭包；闭包是容器内全部引用事实的过近似（ReferenceIndex 覆盖 invoke 家族、
/// 字段访问、MethodHandle、invokedynamic 引导参数、反射查找/执行、类型引用），
/// 只从根前向展开——**可达性判死 ⇒ 全容器代码（含保留代码）无引用 ⇒ 删除后
/// invoke 指令面无需清扫**。`MethodCallRemover` 保持独立 spec 驱动设施定位：
/// 不是删除族的正确性前置，可选地作为删除族的前置增益（删掉最后一个调用点使
/// 非私有成员变死，供下一轮删除族受益）或独立用户面（删保留成员的无用调用点）；
/// 管线无跨 pass 正确性顺序约束。
///
/// 删除安全不变量成立的前提是**闭包根 ⊇ 无条件保留面**。凡因策略面（而非可达性）
/// 保留的成员——keep metadata（Signature/注解族）、keep 规则命中、native/abstract、
/// 构造器、资源 keep 类与 keep 规则类的全部声明成员——其出边必须参与闭包传播，
/// 否则「保留但不可达」成员引用的死成员会被删出悬垂引用。成员删除两件因此自建
/// 闭包并把上述保留面整体提升为根（对齐 `UnusedClassRemover` 的资源 keep 提升
/// 策略）；删除谓词内的 keep metadata / keep 规则守卫保留为纵深防御。这也是
/// fail-closed 判定路径的主体：跨容器不可见的入边（库消费方、外部子类、
/// ServiceLoader/agent 等容器外入口）落在 public 面（默认档保留）与资源 keep
/// 根（提升保留）两道闸上。
///
/// ## 可见性档位（UnusedMemberAccessTier）
///
/// - `PrivateOnly`：旧策略，只候选 private 死成员。
/// - `NonPublic`（默认）：private/package-private/protected 死成员全候选；
///   **public 面保守保留**——public 是库消费方无需任何类关系即可引用的面，容器
///   引用闭包看不见跨 JAR 入边，fail-closed 保留（@ref UnusedClassRemover 删除
///   整类的信任级与 keep 规则兜底同样适用于 protected 面；外部子类覆写/split
///   package 残余风险由 keep 规则收口，见头注风险段）。
/// - `All`：public 死成员也候选。仅适用于闭程序（应用 JAR，全程序在容器内）；
///   库型 JAR 开启会破坏外部链接。
///
/// ## keep 规则面（审计 P0-1/P0-2）
///
/// 四个 pass 的构造器接受可选 `KeepRuleSet`（默认 nullptr = 无规则）。规则集是
/// **纯加法**输入：命中的类/成员在既有硬底线之上再保留一份，无法关闭任何硬底线
/// （keep_rule.hpp「规则只加不减」契约）。类规则语义对齐商业化 trim 工具的排除
/// 语义：命中类整体保留，其声明方法提升为可达性根（与资源 keep 同名类的提升
/// 策略一致，成员删除两件另提升其声明字段）；成员规则对「否则会被删除」的任意
/// 可见性成员产生可观测差异（private-only 时代只覆盖 private，档位放开后覆盖
/// 面随档位走）。运行时供给面（CLI/配置解析）属 cli 批，本层只提供程序化注入点。
///
/// 载体重定型注记：成员删除在 libjbc 低层 DOM 上进行（erase MemberInfo，成员
/// 在 classfile 中按 name/desc 经 CP 引用、无槽位索引依赖，删除结构安全）；
/// 常量池不压缩，失引条目保留（append-only 契约，writer 级差异由 golden
/// 声明类目收口）。需要 JVM 真跑门的产品化使用方为 `opt-in`。
/// @{

/// @brief 成员删除可见性档位（UnusedFieldRemover / GlobalUnusedMethodRemover）。
///
/// 档位只决定「否则会被删除」的成员可见性候选面，不关闭任何既有硬底线：
/// 构造器/`<clinit>`、keep metadata、keep 规则命中、native/abstract、资源 keep
/// 根在全部档位下照常保留。
enum class UnusedMemberAccessTier
{
    /// @brief 旧策略：只候选 private 死成员。
    PrivateOnly,
    /// @brief 默认档：非 public（private/package-private/protected）死成员全候选，
    ///        public 面保守保留（跨容器入边不可见，fail-closed）。
    NonPublic,
    /// @brief 激进档：public 死成员也候选；仅适用于全程序在容器内的闭程序。
    All,
};

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

/// @brief 从可达类中删除不可达字段（档位决定可见性候选面，默认非 public）。
///
/// 自建 GlobalReachability 闭包（Resolved 展开），根 = 共享根策略（资源 keep、
/// main、`<clinit>`）+ 无条件保留面（keep metadata 字段、keep 规则命中、keep
/// 规则/资源 keep 类的全部声明成员、保留方法的字段访问出边经闭包传播）——
/// 「保留但不可达」代码引用的字段不会被删。删除安全不变量与 fail-closed 口径
/// 见模块头注。
///
/// 所有 class 先完成解析和序列化，再一次提交（两阶段提交，作业级原子）；任一
/// 解析或序列化失败保持全部原始 class bytes。
///
/// @param keepRules 可选 keep 规则集（默认无）：规则命中的类整类跳过且其声明
///         字段提升为根；规则命中的字段保留并提升为根（两类判定均只加不减）。
/// @param tier 可见性档位（默认 NonPublic）：PrivateOnly 只删 private；NonPublic
///         删全部非 public；All 连 public 也删（闭程序专用）。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class UnusedFieldRemover : public mjbase::spi::GlobalPass
{
public:
    explicit UnusedFieldRemover(
        std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules = nullptr,
        UnusedMemberAccessTier                                tier = UnusedMemberAccessTier::NonPublic);

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules_;
    UnusedMemberAccessTier                                tier_ = UnusedMemberAccessTier::NonPublic;
};

/// @brief 删除不可达方法（档位决定可见性候选面，默认非 public）。
///
/// 自建 GlobalReachability 闭包（Resolved 展开），根 = 共享根策略 + 无条件保留
/// 面：构造器与 `<clinit>`、native/abstract（无方法体语义承载）、keep metadata
/// 方法、keep 规则命中成员、keep 规则/资源 keep 类的全部声明成员——「保留但
/// 不可达」代码引用的方法不会被删（否则会删出悬垂 invoke）。覆写/被覆写双向
/// 保留由 Resolved 展开承载。删除安全不变量与 fail-closed 口径见模块头注。
/// 两阶段提交语义同 UnusedFieldRemover。
///
/// @param keepRules 可选 keep 规则集（默认无）：规则命中的类整类跳过且其声明
///         方法提升为根；规则命中的方法保留并提升为根（两类判定均只加不减）。
/// @param tier 可见性档位（默认 NonPublic）：PrivateOnly 只删 private；NonPublic
///         删全部非 public；All 连 public 也删（闭程序专用）。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class GlobalUnusedMethodRemover : public mjbase::spi::GlobalPass
{
public:
    explicit GlobalUnusedMethodRemover(
        std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules = nullptr,
        UnusedMemberAccessTier                                tier = UnusedMemberAccessTier::NonPublic);

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    std::shared_ptr<const mjbase::reference::KeepRuleSet> keepRules_;
    UnusedMemberAccessTier                                tier_ = UnusedMemberAccessTier::NonPublic;
};

/// @}

}   // namespace mjbase::pass
