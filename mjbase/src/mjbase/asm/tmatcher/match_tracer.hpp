/// @file        match_tracer.hpp
/// @brief       匹配过程追踪器（调试辅助）。
///
/// 事件携带动态拼接的节点标识与诊断文本——运行期临时诊断数据，
/// 按 Utf8String 驻留（诊断文本经 diagnosticText 按原始字节收进），
/// 拼接/渲染走本地字节缓冲。
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::asm_tmatcher {

/// @brief 诊断文本按原始字节收进为驻留 Utf8String（不校验、字节保真）。
///
/// 追踪事件与 step 调试名都是运行期拼接的临时文本，仅作展示，不走
/// UTF-8 校验/错误通道；空视图（含 nullptr data）返回空串。
inline ca::str::Utf8String diagnosticText(std::string_view text)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(text.data()), text.size());
}

enum class TracePhase : ca::u8 {
    Enter = 0,
    Success = 1,
    Fail = 2,
};

struct TraceEvent
{
    TracePhase          phase = TracePhase::Enter;
    ca::str::Utf8String nodeId;
    ca::i32             depth = 0;
    ca::i32             cursor = 0;
    ca::i32             nextCursor = 0;
    ca::str::Utf8String detail;

    TraceEvent(TracePhase phase, std::string_view nodeId, ca::i32 depth,
               ca::i32 cursor, ca::i32 nextCursor, std::string_view detail)
        : phase(phase)
        , nodeId(diagnosticText(nodeId))
        , depth(depth)
        , cursor(cursor)
        , nextCursor(nextCursor)
        , detail(diagnosticText(detail))
    {}

    /// 显式深拷贝（Utf8String 成员禁拷贝后的唯一复制方式）。
    TraceEvent clone() const
    {
        return TraceEvent{phase, nodeId.ref(), depth, cursor, nextCursor, detail.ref()};
    }
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
    ///
    /// 调试渲染出口：本地字节缓冲拼接，按原始字节收进返回。
    ca::str::Utf8String toPrettyString() const
    {
        std::string sb;
        for (const auto& e : events_) {
            sb.append(static_cast<std::size_t>(e.depth) * 2, ' ');
            switch (e.phase) {
            case TracePhase::Enter: sb += "ENTER "; break;
            case TracePhase::Success: sb += "OK    "; break;
            case TracePhase::Fail: sb += "FAIL  "; break;
            }
            sb += e.nodeId.ref();
            sb += " @";
            sb += std::to_string(e.cursor);
            if (e.phase == TracePhase::Success) {
                sb += " -> ";
                sb += std::to_string(e.nextCursor);
            }
            if (!e.detail.is_empty()) {
                sb += " : ";
                sb += e.detail.ref();
            }
            sb += '\n';
        }
        return diagnosticText(sb);
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

    void enter(std::string_view nodeId, ca::i32 depth, ca::i32 cursor,
               std::string_view detail = {})
    {
        if (!enabled_) {
            return;
        }
        events_.push_back(TraceEvent{TracePhase::Enter, nodeId, depth, cursor, cursor, detail});
    }

    void success(std::string_view nodeId, ca::i32 depth, ca::i32 cursor, ca::i32 nextCursor,
                 std::string_view detail = {})
    {
        if (!enabled_) {
            return;
        }
        events_.push_back(TraceEvent{TracePhase::Success, nodeId, depth, cursor, nextCursor, detail});
    }

    void fail(std::string_view nodeId, ca::i32 depth, ca::i32 cursor,
              std::string_view detail = {})
    {
        if (!enabled_) {
            return;
        }
        events_.push_back(TraceEvent{TracePhase::Fail, nodeId, depth, cursor, cursor, detail});
    }

    /// @brief 事件序列快照（Utf8String 成员禁拷贝，逐事件 clone）。
    MatchTrace snapshot() const
    {
        std::vector<TraceEvent> copied;
        copied.reserve(events_.size());
        for (const auto& e : events_) {
            copied.push_back(e.clone());
        }
        return MatchTrace(std::move(copied));
    }
};

}   // namespace mjbase::asm_tmatcher
