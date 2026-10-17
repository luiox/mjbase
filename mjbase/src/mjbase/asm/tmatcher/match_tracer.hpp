/// @file        match_tracer.hpp
/// @brief       匹配过程追踪器（调试辅助，语义照旧实现）。
///
/// 事件携带动态拼接的节点标识与诊断文本，因此内部使用 std::string——
/// 这是运行期临时诊断数据，不属于库的持久 API 状态。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjbase::asm_tmatcher {

enum class TracePhase : ca::u8 {
    Enter = 0,
    Success = 1,
    Fail = 2,
};

struct TraceEvent
{
    TracePhase   phase = TracePhase::Enter;
    std::string  nodeId;
    ca::i32      depth = 0;
    ca::i32      cursor = 0;
    ca::i32      nextCursor = 0;
    std::string  detail;
};

/// @brief 一次匹配尝试的事件序列快照。
class MatchTrace
{
    std::vector<TraceEvent> events_;

public:
    MatchTrace() = default;
    explicit MatchTrace(std::vector<TraceEvent> events)
        : events_(std::move(events))
    {}

    const std::vector<TraceEvent>& events() const { return events_; }

    /// @brief 全部事件触及过的最大游标（失败诊断：最远走到哪）。
    ca::i32 furthestCursor() const
    {
        ca::i32 f = -1;
        for (const auto& e : events_) {
            if (e.cursor > f) {
                f = e.cursor;
            }
            if (e.nextCursor > f) {
                f = e.nextCursor;
            }
        }
        return f;
    }

    /// @brief 缩进文本渲染（每事件一行，depth * 2 空格）。
    std::string toPrettyString() const
    {
        std::string sb;
        for (const auto& e : events_) {
            sb.append(static_cast<std::size_t>(e.depth) * 2, ' ');
            switch (e.phase) {
            case TracePhase::Enter: sb += "ENTER "; break;
            case TracePhase::Success: sb += "OK    "; break;
            case TracePhase::Fail: sb += "FAIL  "; break;
            }
            sb += e.nodeId;
            sb += " @" + std::to_string(e.cursor);
            if (e.phase == TracePhase::Success) {
                sb += " -> " + std::to_string(e.nextCursor);
            }
            if (!e.detail.empty()) {
                sb += " : " + e.detail;
            }
            sb += '\n';
        }
        return sb;
    }
};

/// @brief 事件收集器：noop 关闭收集，recording 全量收集。
class MatchTracer
{
    bool                    enabled_;
    std::vector<TraceEvent> events_;

    explicit MatchTracer(bool enabled)
        : enabled_(enabled)
    {}

public:
    static MatchTracer noop() { return MatchTracer(false); }
    static MatchTracer recording() { return MatchTracer(true); }

    void enter(const std::string& nodeId, ca::i32 depth, ca::i32 cursor,
               const std::string& detail = {})
    {
        if (!enabled_) {
            return;
        }
        events_.push_back({TracePhase::Enter, nodeId, depth, cursor, cursor, detail});
    }

    void success(const std::string& nodeId, ca::i32 depth, ca::i32 cursor, ca::i32 nextCursor,
                 const std::string& detail = {})
    {
        if (!enabled_) {
            return;
        }
        events_.push_back({TracePhase::Success, nodeId, depth, cursor, nextCursor, detail});
    }

    void fail(const std::string& nodeId, ca::i32 depth, ca::i32 cursor,
              const std::string& detail = {})
    {
        if (!enabled_) {
            return;
        }
        events_.push_back({TracePhase::Fail, nodeId, depth, cursor, cursor, detail});
    }

    MatchTrace snapshot() const { return MatchTrace(events_); }
};

}   // namespace mjbase::asm_tmatcher
