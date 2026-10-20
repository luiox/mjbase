/// @file        method_hierarchy.hpp
/// @brief       方法层次：同 name+desc 的 override 等价族与单继承链虚分派。
///
/// 语义来源（旧栈 method_hierarchy 的公开语义，按键重写）：
/// - 虚方法 = 非 private、非 static、名字非 '<' 开头；只有虚方法参与
///   override 族（private/static/'<init>' 族退化为自身单例）；
/// - override 族 = 旧并查集连通分量的等价语义：同一 name+desc 在
///   super 链与接口闭包上声明的全部虚方法互相连通（含后代方向）；
///   本实现在 build 时做一次并查集合并，族成员按登记序返回；
/// - sourceDeclaration（旧 componentRoot/sourceMethod）：族内「最浅声明」，
///   平级接口所有者优先，再按类名字典序、声明序决胜（跨平台确定性）；
/// - virtualDispatch：receiver 起沿 super 链取最近虚声明；接口 default
///   方法与多继承冲突一律 Unresolved，不裁决（单继承链边界）。
/// 旧 canRename/isExternalOverride 依赖 broken/phantom 机制，本轮不迁。
#pragma once

#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "class_hierarchy.hpp"

namespace mjbase::hierarchy {

/// @brief 虚分派解析结果。
struct DispatchResult
{
    enum class Status : ca::u8
    {
        NoMethod,    ///< 链与接口闭包均无该签名（receiver 未登记同此）
        Abstract,    ///< 最近声明为 abstract（链上或纯接口 abstract 声明）
        Unresolved,  ///< 仅接口 default 方法承载实现：单继承链不裁决
        Resolved,    ///< 唯一确定目标（target 有效）
    };

    Status status = Status::NoMethod;
    MemberKey target{};   ///< Resolved/Abstract/Unresolved 时为最近/首个声明
};

class MethodHierarchy
{
public:
    MethodHierarchy() = default;

    /// @brief 在类图上建方法索引与 override 等价族（登记全部方法，含非虚）。
    ///
    /// 前置条件：ch 存活期覆盖本对象后续查询（借用指针，不拷贝图）。
    void build(const ClassHierarchy& ch);

    ca::usize methodCount() const noexcept { return decls_.size(); }

    /// @brief 该方法是否参与多态（非 private、非 static、名字非 '<' 开头）。
    ///
    /// 未登记的方法返回 false；键文本经绑定的池解析。
    bool isVirtual(MemberKey m) const;

    /// @brief override 等价族：与 m 同 name+desc 且经 super 链/接口闭包
    ///        连通的全部虚声明（含后代方向），按登记序返回。
    ///
    /// 非虚方法（private/static/'<'）族退化为自身单例；owner 未声明该
    /// 签名返回空。
    std::vector<MemberKey> overrideFamily(MemberKey m) const;

    /// @brief 族内最浅声明（旧 sourceMethod 语义，决胜序见文件头）。
    ///
    /// 族为空（未登记/无声明）返回全零 MemberKey。
    MemberKey sourceDeclaration(MemberKey m) const;

    /// @brief 单继承链虚分派：receiver 起沿 super 链取最近虚声明；
    ///        static/private/'<' 声明跳过继续上行（不参与分派）。
    ///
    /// 类链优先于接口（classes-win）：类链命中即 Resolved/Abstract；
    /// 类链无声明时接口闭包内非 abstract（default）声明 → Unresolved；
    /// 仅 abstract 声明 → Abstract；无声明 → NoMethod。sig 只取其
    /// name/desc，sig.cls 不限定起点。
    DispatchResult virtualDispatch(ClassKey receiver, MemberKey sig) const;

private:
    /// @brief 类内一条方法声明（含访问位、声明下标与所属等价族编号）。
    struct Decl
    {
        MemberKey key;
        ca::u32 access    = 0;
        ca::u32 index     = 0;   ///< 在 ClassSummary::methods 中的下标（决胜用）
        ca::u32 component = 0;   ///< 等价族编号（build 时赋值；非虚为单例族）
    };

    static ca::u64 packSig(ca::u32 name, ca::u32 desc) noexcept
    {
        return (static_cast<ca::u64>(name) << 32) | desc;
    }

    const Decl* findDecl(ClassKey cls, ca::u32 name, ca::u32 desc) const noexcept;
    bool isVirtualDecl(const Decl& d) const;
    /// @brief owner 到链顶（无父/悬挂）的步数；未登记返回 0。
    ca::u32 depthOf(ClassKey cls) const;
    bool shallowerOrStable(MemberKey a, MemberKey b) const;

    const ClassHierarchy* ch_ = nullptr;
    const KeyPool* pool_     = nullptr;
    /// 全部声明，登记序 × 类内声明序（族成员与计数的数据源）。
    std::vector<Decl> decls_;
    /// component id → decls_ 下标集合（族成员，decls_ 序）。
    std::vector<std::vector<ca::u32>> comps_;
    /// cls.id → (name<<32|desc) → decls_ 下标（点查用）。
    std::unordered_map<ca::u32, std::unordered_map<ca::u64, ca::u32>> table_;
};

}   // namespace mjbase::hierarchy
