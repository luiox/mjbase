/// @file        summary.hpp
/// @brief       类摘要值结构：层次图的输入单元（自持值，可复制）。
///
/// ClassSummary 是「一个类对层次分析可见的全部骨架信息」的纯值快照：
/// this/super/接口键 + 成员（名字/描述符/访问位）。来源有二——
/// fillSummary 从 jbc 会话骨架填充（只读骨架，不 promote 方法体），
/// 或调用方手工构造。键一律来自调用方的 KeyPool（池存活期间视图稳定）。
#pragma once

#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/units.hpp"
#include "mjbase/keys.hpp"

namespace mjbase::hierarchy {

// ── JVM access flags（类与成员共用的低位段；JVMS 表 4.1/4.5/4.7）──
inline constexpr ca::u32 kAccPublic    = 0x0001;
inline constexpr ca::u32 kAccPrivate   = 0x0002;   ///< 成员：不参与虚分派
inline constexpr ca::u32 kAccProtected = 0x0004;   ///< 成员：子类可见（keep 档位判定用）
inline constexpr ca::u32 kAccStatic    = 0x0008;   ///< 成员：静态
inline constexpr ca::u32 kAccFinal     = 0x0010;
inline constexpr ca::u32 kAccSuper     = 0x0020;   ///< 类：invokespecial 语义
inline constexpr ca::u32 kAccInterface = 0x0200;   ///< 类：接口
inline constexpr ca::u32 kAccAbstract  = 0x0400;   ///< 方法：无实现体
inline constexpr ca::u32 kAccSynthetic = 0x1000;
inline constexpr ca::u32 kAccEnum      = 0x4000;   ///< 类/字段：枚举

/// @brief 成员（方法或字段）摘要：名字/描述符为 KeyPool 驻留 id。
struct MemberSummary
{
    ca::u32 name    = 0;   ///< 名字（KeyPool id）
    ca::u32 desc    = 0;   ///< 描述符（KeyPool id）
    bool    isStatic = false;   ///< access & kAccStatic 的快捷位
    ca::u32 access  = 0;   ///< JVM access flags 原样保留（含 isStatic 之外的位）

    bool operator==(const MemberSummary& o) const noexcept
    {
        return name == o.name && desc == o.desc && isStatic == o.isStatic
               && access == o.access;
    }
    bool operator!=(const MemberSummary& o) const noexcept { return !(*this == o); }
};

/// @brief 类摘要：层次图的登记单元（自持值；vector 拷贝即快照）。
struct ClassSummary
{
    ClassKey self;                     ///< 本类（内部名驻留键）
    ClassKey super;                    ///< 父类；无父类（java/lang/Object）时 invalid
    std::vector<ClassKey> interfaces;  ///< 直接接口（superinterfaces）
    std::vector<MemberSummary> methods;
    std::vector<MemberSummary> fields;
    ca::u32 access = 0;                ///< 类 access flags（kAccInterface 等）
};

/// @brief 成员摘要 → 全键（所属类取自摘要的 self）。
inline MemberKey memberKeyIn(const ClassSummary& owner, const MemberSummary& m) noexcept
{
    return MemberKey{owner.self.id, m.name, m.desc};
}

/// @brief 从 jbc 会话骨架填充类摘要：只读 this/super/接口/成员元数据，
///        不 promote 任何方法体。
///
/// 名字与描述符逐段 intern 进 pool；Object 的「无父类」在会话侧是空文本
/// （ClassSession 对缺失 super_name 物化空串），这里映射回 invalid ClassKey。
/// 前置条件：session 与 pool 在本调用期间有效；返回值借用 pool 的驻留文本。
ClassSummary fillSummary(const jbc::ir::ClassSession& session, KeyPool& pool);

}   // namespace mjbase::hierarchy
