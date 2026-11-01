/// @file        local_op.hpp
/// @brief       局部变量指令解码：显式/紧凑形态 + iinc 的槽位与类型族。
///
/// IR 保留紧凑操作码（iload_0 … astore_3），其槽位来自操作码本身而不是
/// `Insn::a`；需要改写槽位的 pass 必须先经本解码，改后统一落显式形态。
#pragma once

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

namespace mjbase::pass {

enum class LocalUse : ca::u8
{
    None = 0,
    Load,
    Store,
    Inc,
};

struct LocalOp
{
    LocalUse use  = LocalUse::None;
    ca::u32  slot = 0;
    char     kind = 'A';   ///< I/L/F/D/A
};

/// @brief 解码一条指令的局部变量访问面；非局部指令返回 use = None。
inline LocalOp localOpOf(const jbc::ir::Insn& in) noexcept
{
    static constexpr char kKinds[] = {'I', 'L', 'F', 'D', 'A'};
    const ca::u32         raw      = static_cast<ca::u32>(in.op);
    if (raw >= 0x15 && raw <= 0x19) {
        return {LocalUse::Load, static_cast<ca::u32>(in.a), kKinds[raw - 0x15]};
    }
    if (raw >= 0x36 && raw <= 0x3a) {
        return {LocalUse::Store, static_cast<ca::u32>(in.a), kKinds[raw - 0x36]};
    }
    // 紧凑形态：每型 4 条，槽位 = 序号 % 4，类型族 = 序号 / 4。
    if (raw >= 0x1a && raw <= 0x2d) {
        const ca::u32 d = raw - 0x1a;
        return {LocalUse::Load, d % 4, kKinds[d / 4]};
    }
    if (raw >= 0x3b && raw <= 0x4e) {
        const ca::u32 d = raw - 0x3b;
        return {LocalUse::Store, d % 4, kKinds[d / 4]};
    }
    if (in.op == jbc::core::Op::IINC) {
        return {LocalUse::Inc, static_cast<ca::u32>(in.a), 'I'};
    }
    return {};
}

/// @brief 类型族的槽宽（long/double 占 2 槽）。
inline ca::u32 localWidth(char kind) noexcept
{
    return (kind == 'L' || kind == 'D') ? 2u : 1u;
}

}   // namespace mjbase::pass
