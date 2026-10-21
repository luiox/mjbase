#pragma once

#include <map>
#include <string_view>
#include <utility>

#include "libca/core/datatype.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"

namespace mjbase::spi {

/// @brief 公共父类求解适配件：挂接 mjbase::hierarchy::ClassHierarchy 的
///        JVM 子类型骨架，产出 getCommonSuperClass 语义。
///
/// 按层次图底座实现：
///   - 求解用 ClassHierarchy 的 commonAncestor（super 链最近公共祖先，
///     super-chain-only）；
///   - 不可判定的回退语义逐条如下：
///       1) 任一类型未在图中登记 → 回退 fallback；
///       2) 两链无交（含悬挂键截断——super 名已驻留但未登记摘要）→ 回退
///          fallback；
///       3) 继承链成环：本适配件的输入图在 addClass
///          时即拒绝环（Err(Cycle)，不静默建图），求解面看到的前置条件是
///          无环图——环防护由建图承接，resolver 不再自检；
///       4) fallback 缺省 "java/lang/Object"，可经构造参数/setFallback 改；
///          触发回退时返回 fallback 文本本身。
///   - 查询结果缓存：归一化 (t1,t2)/(t2,t1) 等价对，一次求解后续命中
///     （COMPUTE_FRAMES 式逐类查询下，相同类型对被反复查询，缓存避免重复
///     链遍历）。
///
/// @par 字符串与寿命
/// 查询键 intern 进 resolver 自有的 queryPool_（内容去重，池存活期内视图
/// 稳定）；返回视图指向层次图驻留池（h.nameOf）或 queryPool_（fallback），
/// 寿命 = 层次图与 resolver 两者中先死亡者。调用方不得把视图带过该死亡点；
/// 需跨期持有则深拷贝或 intern 进自己的池。
///
/// @par 数据供给
/// 数据获取是调用方的事——对扫描集合的会话逐个 fillSummary +
/// ClassHierarchy::addClass 即成图。resolver 只消费已建好的图。
class CommonSuperClassResolver
{
public:
    /// @param hierarchy 已建好的类层次图（引用借用，须存活到 resolver 死亡）。
    /// @param fallback  不可判定时的回退内部名（缺省 java/lang/Object）。
    explicit CommonSuperClassResolver(const mjbase::hierarchy::ClassHierarchy& hierarchy,
                                      std::string_view fallback = "java/lang/Object");

    /// 改设回退内部名（驻留进查询池）。
    void setFallback(std::string_view fallback);

    /// @brief 求解两个内部名类型的公共父类。
    ///
    /// 结果视图指向层次图驻留池（或查询池的 fallback 文本）；两类型相同、
    /// 一方是另一方祖先、正常公共链尾均按最近公共祖先语义返回。
    /// 不可判定（未登记/无交）返回 fallback 文本。
    std::string_view getCommonSuperClass(std::string_view type1, std::string_view type2);

    /// 已缓存的查询对数（测试/诊断用）。
    ca::usize cachedQueryCount() const noexcept { return cache_.size(); }

private:
    /// 缓存值：命中键（层次图键）+ 是否回退（回退键在查询池，nameOf 口径不同）。
    struct Cached
    {
        mjbase::ClassKey key;
        bool             fromFallback = false;
    };

    Cached resolve(std::string_view type1, std::string_view type2);

    const mjbase::hierarchy::ClassHierarchy* hierarchy_;
    mjbase::KeyPool queryPool_;   ///< 查询键 + fallback 文本驻留（自有 arena）
    mjbase::ClassKey fallback_;
    std::map<std::pair<ca::u32, ca::u32>, Cached> cache_;   ///< 归一化 (minId,maxId) → 结果
};

}   // namespace mjbase::spi
