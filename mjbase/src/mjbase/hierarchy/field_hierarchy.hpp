/// @file        field_hierarchy.hpp
/// @brief       字段层次：沿 super 链的可见性解析（找到定义类 / shadow 判定）。
///
/// 语义来源：
/// - resolveField：receiver 起 super 链自近及远，第一个 name+desc 完全
///   匹配的声明即解析结果（JVM 字段解析不区分 static/instance）；
/// - shadowsAncestor：实例字段遮蔽祖先链上的同名实例字段（static 字段
///   不构成遮蔽，与旧 isSourceField 的反面语义一致——旧 canRename 不迁）。
#pragma once

#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "class_hierarchy.hpp"

namespace mjbase::hierarchy {

class FieldHierarchy
{
public:
    FieldHierarchy() = default;

    /// @brief 在类图上建字段索引（登记全部字段，含 static）。
    ///
    /// 前置条件：ch 存活期覆盖本对象后续查询（借用指针，不拷贝图）。
    void build(const ClassHierarchy& ch);

    ca::usize fieldCount() const noexcept { return count_; }

    /// @brief 字段解析结果。
    struct FieldResolution
    {
        enum class Status : ca::u8
        {
            NotFound,   ///< 链上无该 name+desc 声明（receiver 未登记同此）
            Found,      ///< 解析到定义类（target 有效）
        };

        Status status = Status::NotFound;
        MemberKey target{};   ///< Found 时为最近的声明
    };

    /// @brief 在 receiver 的 super 链上解析字段引用：最近的 name+desc
    ///        声明胜出（找到定义类）。环安全，悬挂键截断。
    FieldResolution resolveField(ClassKey receiver, ca::u32 name, ca::u32 desc) const;

    /// @brief field（owner 声明的实例字段）是否遮蔽祖先链上的同名实例字段。
    ///
    /// static 字段恒 false；owner 未声明该字段或未登记返回 false。
    bool shadowsAncestor(MemberKey field) const;

private:
    struct Decl
    {
        MemberKey key;
        ca::u32 access = 0;
    };

    static ca::u64 packSig(ca::u32 name, ca::u32 desc) noexcept
    {
        return (static_cast<ca::u64>(name) << 32) | desc;
    }

    const Decl* findDecl(ClassKey cls, ca::u32 name, ca::u32 desc) const noexcept;

    const ClassHierarchy* ch_ = nullptr;
    ca::usize count_          = 0;
    /// cls.id → (name<<32|desc) → Decl；只含已登记类的已声明字段。
    std::unordered_map<ca::u32, std::unordered_map<ca::u64, Decl>> table_;
};

}   // namespace mjbase::hierarchy
