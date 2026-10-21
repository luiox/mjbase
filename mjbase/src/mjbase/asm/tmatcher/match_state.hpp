/// @file        match_state.hpp
/// @brief       匹配状态层：不可变捕获表 + 游标快照。
///
/// 语义对齐 asm/tmatcher 的 match_state.hpp，底座换成 CodeDom：
///   - 捕获值从 `ca::core::Any` 收窄为 `jbc::ir::InsnId`——新 DSL 的绑定
///     语义是「捕获名 → 命中窗口入口处的指令句柄」，调用方经
///     `CodeDom::peek` 读取操作数，常量解释归 asm/insn / evaluator 模块。
///   - 捕获名用 `std::string_view` 借用视图（mjbase 约定）；通常是字面量，
///     调用方须保证规则存活期间名字底层数据有效。
///   - 不可变持久结构（shared_ptr 快照 + 绑定时复制），同名冲突拒绝、
///     同值（同句柄）重复绑定幂等，与旧实现一致。
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "libca/core/datatype.hpp"

#include "jbc/ir/code_dom.hpp"

namespace mjbase::asm_tmatcher {

/// @brief 不可变变量捕获表：捕获名 → InsnId。
///
/// 绑定规则（语义照）：
///   - 名字未绑定 → 新建快照持有绑定；
///   - 名字已绑定且句柄相同 → 返回原快照（幂等）；
///   - 名字已绑定且句柄不同 → nullopt（冲突拒绝）；
///   - 绑定 kNoInsn 视为非法输入，返回 nullopt。
class CaptureStore
{
    using Map = std::unordered_map<std::string_view, jbc::ir::InsnId>;
    std::shared_ptr<const Map> values_;

    explicit CaptureStore(std::shared_ptr<const Map> values)
        : values_(std::move(values))
    {}

public:
    CaptureStore()
        : values_(empty().values_)
    {}

    /// @brief 全局共享的空表（零分配复用）。
    static CaptureStore empty()
    {
        static auto m = std::make_shared<const Map>();
        return CaptureStore(m);
    }

    /// @brief 取绑定句柄；未绑定时返回 kNoInsn（用 contains 区分两种状态）。
    jbc::ir::InsnId get(std::string_view name) const
    {
        auto it = values_->find(name);
        return it != values_->end() ? it->second : jbc::ir::kNoInsn;
    }

    bool contains(std::string_view name) const
    {
        return values_->find(name) != values_->end();
    }

    /// @brief 绑定一个句柄；冲突返回 nullopt（见类注释的绑定规则）。
    std::optional<CaptureStore> bind(std::string_view name, jbc::ir::InsnId value) const
    {
        if (value == jbc::ir::kNoInsn) {
            return std::nullopt;
        }
        auto it = values_->find(name);
        if (it != values_->end()) {
            return it->second == value ? std::optional(*this) : std::nullopt;
        }
        auto newMap       = std::make_shared<Map>(*values_);
        (*newMap)[name]   = value;
        return CaptureStore(std::move(newMap));
    }

    /// @brief 合并两个表；同名不同句柄返回 nullopt，同名同句柄取一侧即可。
    std::optional<CaptureStore> merge(const CaptureStore& other) const
    {
        if (values_->empty()) {
            return other;
        }
        if (other.values_->empty()) {
            return *this;
        }
        auto newMap = std::make_shared<Map>(*values_);
        for (const auto& [key, val] : *other.values_) {
            auto it = newMap->find(key);
            if (it != newMap->end() && !(it->second == val)) {
                return std::nullopt;
            }
            (*newMap)[key] = val;
        }
        return CaptureStore(std::move(newMap));
    }
};

/// @brief 不可变匹配状态：order() 下标游标 + 捕获表快照。
///
/// 每个回溯分支返回独立快照；量词/组合子的多候选即多个 MatchState。
struct MatchState
{
    ca::i32      cursor   = 0;
    CaptureStore captures;

    MatchState withCursor(ca::i32 newCursor) const { return {newCursor, captures}; }

    MatchState withCaptures(const CaptureStore& newCaptures) const
    {
        return {cursor, newCaptures};
    }

    static MatchState initial(ca::i32 cursor) { return {cursor, CaptureStore::empty()}; }
};

}   // namespace mjbase::asm_tmatcher
