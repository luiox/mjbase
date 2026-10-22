/// @file        frame_query.hpp
/// @brief       DomainResult 的句柄查询面：按指令句柄取到达帧的栈/局部变量域值。
///
/// DomainEngine 交付的 DomainResult<V>.frames 与 CodeUnit::dom.order() 按下标
/// 对齐；本模板收口两件定位杂务：InsnId → 帧下标（slot 索引表），以及「行是
/// 否为值行」（cat2 底槽随值整体移动、不承载域值）。查询全部探测式：nullptr
/// 表示不可达/句柄不属于该方法/越界/底槽，不抛错——与 CodeDom::peek 同款纪律。
#pragma once

#include <unordered_map>
#include <utility>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::asm_analyzer {

/// @brief 域分析结果的句柄查询器（值语义、自持有）。
///
/// 与 `Analyzer::getFrames()` 的借用形态相对：结果整体拷贝进来，构造后
/// CodeUnit、域对象与引擎均可释放。载荷若间接借用外部资源（如 Str 常量的
/// CP 索引要读出内容），解引用时仍需相应常量池存活——借用范围由各域文档说明。
///
/// 结果是否可信以 summary() 为准：ok=false 时引擎不交付帧（frames 为空），
/// 全部查询退化为 nullptr。
template <typename V>
class FrameQuery
{
public:
    /// @param u 参与方法（只读取 order() 建索引表，不保留引用）。
    /// @param r 引擎交付的分析结果（移入持有）。
    FrameQuery(const jbc::ir::CodeUnit& u, jbc::analysis::DomainResult<V> r)
        : r_(std::move(r))
    {
        const std::vector<jbc::ir::InsnId>& order = u.dom.order();
        posOf_.reserve(order.size() * 2);
        for (ca::usize i = 0; i < order.size(); ++i) {
            posOf_.emplace(order[i].slot, i);
        }
    }

    /// 引擎 summary（失败码/熔断诊断）。
    const jbc::analysis::FrameSummary& summary() const noexcept { return r_.summary; }

    /// 数据流是否无异常完成；false 时所有帧查询返回 nullptr。
    bool ok() const noexcept { return r_.summary.ok; }

    /// 指令是否可达（死代码无到达帧）。
    bool reachable(jbc::ir::InsnId id) const noexcept { return frameOf(id) != nullptr; }

    /// @brief 到达帧的局部变量域值。
    /// @return nullptr：不可达 / 越界 / cat2 底槽行（第二槽不承载域值）。
    const V* localAt(jbc::ir::InsnId id, ca::u32 idx) const noexcept
    {
        const jbc::analysis::FrameV<V>* f = frameOf(id);
        if (f == nullptr || idx >= f->locals.size() || f->locals[idx].bottom) {
            return nullptr;
        }
        return &f->locals[idx].val;
    }

    /// @brief 到达帧的操作数栈域值（fromTop=0 为栈顶值）。
    /// @return nullptr：不可达 / 越过栈深 / 命中 cat2 底槽行。
    const V* stackFromTop(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept
    {
        const jbc::analysis::FrameV<V>* f = frameOf(id);
        if (f == nullptr || fromTop >= f->stack.size()) {
            return nullptr;
        }
        const jbc::analysis::SlotV<V>& row = f->stack[f->stack.size() - 1 - fromTop];
        return row.bottom ? nullptr : &row.val;
    }

private:
    /// InsnId → 可达帧；句柄不属于该方法（slot 未建索引）或帧不可达返回 nullptr。
    /// 同一 CodeDom 内 slot 唯一，索引表按 slot 寻址即可。
    const jbc::analysis::FrameV<V>* frameOf(jbc::ir::InsnId id) const noexcept
    {
        const auto it = posOf_.find(id.slot);
        if (it == posOf_.end() || it->second >= r_.frames.size()) {
            return nullptr;
        }
        const jbc::analysis::FrameV<V>& f = r_.frames[it->second];
        return f.reachable ? &f : nullptr;
    }

    jbc::analysis::DomainResult<V> r_;
    std::unordered_map<ca::u32, ca::usize> posOf_;   ///< InsnId.slot → order 下标
};

}   // namespace mjbase::asm_analyzer
