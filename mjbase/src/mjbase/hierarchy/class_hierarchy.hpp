/// @file        class_hierarchy.hpp
/// @brief       类层次内存图：登记 ClassSummary 建图，super 链与接口闭包查询。
///
/// 设计要点：
/// - 输入是 ClassSummary 值（来自 fillSummary 或手工构造）；图内只驻留
///   键与计数，不持有容器引用或查找回调；
/// - 未登记的父类/接口按「悬挂键」处理——名字已驻留（键可比较）但没有
///   摘要，链上行到悬挂键即截断；
/// - java/lang/Object 不自动合成：未登记则 objectClass() 返回 invalid；
/// - 改名映射与 .ihc 持久化不在本期范围。
///
/// 畸形输入纪律：父类成环、自指父类、接口环在 addClass 时被拒绝
///（Err(Cycle)，不静默建图）；所有链/闭包遍历带访问集，任何情况下不挂死。
/// validate() 提供全图复查（兜底乱序登记漏检的环）。
#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/keys.hpp"
#include "summary.hpp"

namespace mjbase::hierarchy {

using ca::core::Result;

/// @brief 层次图构建错误。
enum class HierarchyError : ca::u8
{
    BadKey,          ///< self 键未设置，或其文本在绑定池中不可解析
    DuplicateClass,  ///< 同名类重复登记
    Cycle,           ///< 父类/接口边成环（含自指）
};

const char* to_string(HierarchyError e) noexcept;

class ClassHierarchy
{
public:
    /// @brief 绑定驻留池构造空图；摘要中的键与按名查询都借用该池，
    ///        池必须存活到本图死亡（「一个分析任务持有一个池」）。
    explicit ClassHierarchy(const KeyPool& pool) noexcept
        : pool_(&pool)
    {}

    // ── 构建 ──

    /// @brief 登记一个类摘要（拷贝入库）。
    ///
    /// 错误：BadKey（self 无效/文本不可解析）、DuplicateClass（重复登记）、
    /// Cycle（本条边闭合父类链或接口闭包的环——拒绝登记，图保持原状）。
    Result<void, HierarchyError> addClass(const ClassSummary& s);

    /// @brief 全图复查：任何已登记类的 super 链 / 接口闭包包含自身即 Cycle。
    ///
    /// addClass 的环检查对「闭合边的加入时刻」生效；此入口兜底极端乱序
    /// 登记路径（防御位，常规调用方可省略）。
    Result<void, HierarchyError> validate() const;

    // ── 基本查询 ──

    ca::usize classCount() const noexcept { return order_.size(); }
    bool contains(ClassKey cls) const noexcept;
    /// @brief 按内部名查类；未登记返回 invalid ClassKey。
    ClassKey findClass(std::string_view internalName) const noexcept;
    /// @brief java/lang/Object 的键（若已登记）；否则 invalid。
    ClassKey objectClass() const noexcept;
    /// @brief 类内部名（池视图）；未知键返回空视图。
    std::string_view nameOf(ClassKey cls) const noexcept;
    /// @brief 类摘要；未知键返回 nullptr。
    const ClassSummary* summary(ClassKey cls) const noexcept;
    /// @brief 登记序（决定 descendants 等遍历输出顺序）。
    const std::vector<ClassKey>& registrationOrder() const noexcept { return order_; }
    const KeyPool& pool() const noexcept { return *pool_; }

    // ── 边查询 ──

    /// @brief 直接父类；无父类（Object）或未知键返回 invalid。
    ClassKey superClass(ClassKey cls) const noexcept;
    /// @brief 直接接口列表；未知键返回空。
    const std::vector<ClassKey>& interfaces(ClassKey cls) const noexcept;

    /// @brief super 链祖先（自反：含自身，最近在前，直到无父类或悬挂键）。
    ///
    /// 链尾的悬挂键（名字已驻留但未登记摘要）计入列表后截断；
    /// 未知键返回空表。迭代实现，环安全。
    std::vector<ClassKey> ancestors(ClassKey cls) const;

    /// @brief 沿 super 边的严格后代（不含自身），按登记序输出。
    std::vector<ClassKey> descendants(ClassKey cls) const;

    /// @brief sub 的 super 链可达 super（自反 true）。仅 extends 边，
    ///        不含接口（接口语义走 isSubtype）。
    bool isSubclass(ClassKey sub, ClassKey super) const;

    /// @brief JVM 子类型：isSubclass 或 super 落在 sub 的接口闭包中
    ///        （含 super 链上各类继承的接口及其传递超接口）。
    bool isSubtype(ClassKey sub, ClassKey super) const;

    /// @brief super 链最近公共祖先；两链无交（含悬挂截断）返回 invalid。
    ClassKey commonAncestor(ClassKey a, ClassKey b) const;

    /// @brief 接口闭包：super 链上各类的直接接口 + 各接口的传递超接口，
    ///        BFS 序（自身接口先于父类的接口）。悬挂接口键计入后截断。
    std::vector<ClassKey> interfaceClosure(ClassKey cls) const;

    // ── 类别谓词（读 access 位；未知键一律 false）──
    bool isInterface(ClassKey cls) const noexcept;
    bool isEnum(ClassKey cls) const noexcept;

private:
    /// @brief 从任意键起沿 super 边走到头（含起点；悬挂键计入后停）。
    ///
    /// 不校验登记状态（环检查需要在「新类尚未入库」时走旧图）；
    /// 访问集防环，永不挂死。
    std::vector<ClassKey> superChainOf(ClassKey start) const;
    static bool chainContains(const std::vector<ClassKey>& chain,
                              ClassKey key) noexcept;

    const KeyPool* pool_ = nullptr;
    std::vector<ClassKey> order_;                        ///< 登记序
    std::unordered_map<ca::u32, ClassSummary> classes_;  ///< self.id → 摘要
    std::unordered_map<std::string_view, ClassKey> byName_;  ///< 视图借用池
};

}   // namespace mjbase::hierarchy
