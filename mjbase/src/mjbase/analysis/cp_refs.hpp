/// @file        cp_refs.hpp
/// @brief       analysis 模块内共享的 CP 成员/类解析（域钩子与走查件共用）。
///
/// libjbc 会话的 memberOf/classOf 是「指令语义层」入口；域钩子只拿到
/// Insn + ConstantPool（与 DomainEngine 的接线面一致），此处提供同语义的
/// 纯池解析：成员引用（Field/Method/InterfaceMethodref → NaT 两跳）与
/// 类名（Class 条目 → name_index）。解析失败返回 false/空，不抛错。
#pragma once

#include <string_view>

#include "jbc/core/constant_pool.hpp"

namespace mjbase::analysis {

/// @brief 成员引用三段（owner/name/desc）；失败时全部为空。
struct MemberRefSem
{
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

/// @brief 解析 Fieldref/Methodref/InterfaceMethodref 条目的三段文本。
inline bool resolveMemberRef(const jbc::core::ConstantPool& cp, ca::u32 cpIdx, MemberRefSem& out)
{
    out = {};
    if (!cp.valid(cpIdx)) return false;
    const auto& e = cp.at(cpIdx);
    if (e.tag != jbc::core::CpTag::Methodref && e.tag != jbc::core::CpTag::InterfaceMethodref &&
        e.tag != jbc::core::CpTag::Fieldref) {
        return false;
    }
    if (!cp.valid(e.a) || !cp.valid(e.b)) return false;
    const auto& cls = cp.at(e.a);
    const auto& nat = cp.at(e.b);
    if (cls.tag != jbc::core::CpTag::Class) return false;
    if (nat.tag != jbc::core::CpTag::NameAndType) return false;
    if (!cp.isUtf8(cls.a) || !cp.isUtf8(nat.a) || !cp.isUtf8(nat.b)) return false;
    out.owner = cp.utf8Of(cls.a);
    out.name  = cp.utf8Of(nat.a);
    out.desc  = cp.utf8Of(nat.b);
    return true;
}

/// @brief 解析 Class 条目的内部名；失败返回空视图。
inline std::string_view resolveClassName(const jbc::core::ConstantPool& cp, ca::u32 cpIdx)
{
    if (!cp.valid(cpIdx)) return {};
    const auto& e = cp.at(cpIdx);
    if (e.tag != jbc::core::CpTag::Class || !cp.isUtf8(e.a)) return {};
    return cp.utf8Of(e.a);
}

}   // namespace mjbase::analysis
