/// @file        keys.hpp
/// @brief       跨类身份原语：KeyPool 驻留池 + ClassKey/MemberKey。
///
/// libjbc 的 owner-scoped 句柄（InsnId/LabelId/MethodId）只在单类 DOM 或
/// 单个会话内有效；跨类引用（xref 边、调用图、层次图、去重键）使用本
/// 文件的进程内驻留键。键是纯值：id 指向 KeyPool 中的驻留字符串，池
/// 存活期间视图稳定；同类内容恒等（同一 id），不改写、不回收。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/result.hpp"
#include "libca/str/utf8_string_arena.hpp"

namespace mjbase {

/// @brief 字符串驻留池：内容寻址、只增不减。
///
/// 一个分析任务持有一个池；类内部名、成员名、描述符统一经 intern 拿 id，
/// 后续比较退化为 u32 比较。arena 只增不移，str() 视图在池存活期有效。
class KeyPool
{
public:
    /// @brief 构造时烧掉 0 号槽（空内容哨兵）：真实内容从 id 1 起，
    ///        id 0 恒表示「未设置」，与 ClassKey{}/MemberKey{} 默认值一致。
    KeyPool() noexcept;

    /// @brief 驻留一个字符串（字节序看待）；相同内容返回相同 id。
    ca::u32 intern(std::string_view s);

    /// @brief 取驻留文本；无效 id 返回空视图（探测式，不抛错）。
    std::string_view str(ca::u32 id) const noexcept;

    /// @brief 已驻留条目数（也是下一个 id 的取值）。
    ca::usize size() const noexcept { return slots_.size(); }

private:
    ca::str::Utf8StringArena arena_;
    std::vector<ca::str::Utf8StringRef> slots_;
    std::unordered_map<std::string_view, ca::u32> byStr_;
};

/// @brief 类身份：id 是内部名（如 "java/lang/String"）在 KeyPool 的驻留号。
struct ClassKey
{
    ca::u32 id = 0;   ///< 0保留给「未设置」（首个驻留 id 从 0 起，配合 == 使用）

    bool operator==(const ClassKey& o) const noexcept { return id == o.id; }
    bool operator!=(const ClassKey& o) const noexcept { return id != o.id; }
    bool valid() const noexcept { return id != 0; }
};

/// @brief 成员身份：所属类 + 名字 + 描述符三段驻留 id。
struct MemberKey
{
    ca::u32 cls  = 0;
    ca::u32 name = 0;
    ca::u32 desc = 0;

    bool operator==(const MemberKey& o) const noexcept
    {
        return cls == o.cls && name == o.name && desc == o.desc;
    }
    bool operator!=(const MemberKey& o) const noexcept { return !(*this == o); }
};

/// @brief MemberKey 的 std::unordered_* 哈希（三段混合）。
struct MemberKeyHash
{
    std::size_t operator()(const MemberKey& k) const noexcept;
};

/// @brief 便捷构造：驻留内部名并返回类键。
ClassKey classKeyOf(KeyPool& pool, std::string_view internalName);

/// @brief 便捷构造：驻留三段并返回成员键。
MemberKey memberKeyOf(KeyPool& pool,
                      ClassKey owner,
                      std::string_view name,
                      std::string_view desc);

/// @brief 调试/输出用：成员键展开为 "owner.name desc" 形式（拼接进调用方缓冲）。
void formatMember(const KeyPool& pool, MemberKey key, std::string& out);

}   // namespace mjbase
