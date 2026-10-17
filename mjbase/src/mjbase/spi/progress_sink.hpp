#pragma once

#include <string_view>
#include <vector>

#include "libca/str/utf8_string.hpp"

#include "mjbase/util/time_util.hpp"

namespace mjbase::spi {

/// @brief 作业级进度反馈 sink。
///
/// 解决「大 JAR 跑起来像死了一样」。两个阶段共用一个 sink：
/// - loading 阶段（import jar）：`loading resource [i/total] name time`
/// - pipeline 阶段（跑 pass）：`[pass 标记] class [i/total] pct% time`
///
/// 输出策略：
/// | stdout 目标 | 默认行为 |
/// |---|---|
/// | TTY        | `\r` 单行原地刷新 |
/// | 非 TTY     | 完全静默，仅 finish() 的总结行（避免 CI 日志洪泛） |
/// quiet 强制静默；verbose 在非 TTY 下输出换行式阶段日志。
///
/// SIGINT 兜底：析构时若末动作是 `\r` 且未 finish，补一个 `\n`，
/// 避免 shell 提示符粘在进度行末尾。
class ProgressSink
{
public:
    /// 阶段标识：loading 和 pipeline 用不同标题。
    enum class Phase
    {
        Loading,
        Pipeline
    };

    /// 当前进度快照（值语义，update 时整体传入）。
    struct State
    {
        Phase phase = Phase::Loading;
        int   total = 0;     ///< class/entry 总数
        int   index = 0;     ///< 当前序号（1-based）
        /// 当前 class/entry 名。契约：由调用方负责展示层转义（mjbase::spi 不持
        /// 转义设施）；sink 原样输出。
        ca::str::Utf8String currentName;
        // pipeline 阶段的 pass 标记信息（PR-A 用占位，PR-B 填真实名）。
        int passIndex = 0;   ///< 当前 pass（1-based），0 表示未进 pipeline 内层
        int passTotal = 0;
        std::vector<ca::str::Utf8String> passNames;   ///< PR-B 才填充；PR-A 为空走占位
    };

    ProgressSink();
    ~ProgressSink();

    /// 配置输出模式。quiet 强制静默；verbose 让非 TTY 也输出换行式日志。
    /// TTY 检测在构造时做一次（避免反复 isatty），结果也可由 setTty 覆盖（测试用）。
    void setQuiet(bool q) noexcept { quiet_ = q; }
    void setVerbose(bool v) noexcept { verbose_ = v; }
    void setTty(bool t) noexcept { isTty_ = t; }

    /// 记录开始时刻（重置计时）。
    void start() noexcept;

    /// 刷新一行进度（TTY 最快每 100ms 节流；非 TTY + verbose 则每阶段换行）。
    /// 虚函数：子类可拦截快照流（测试/自定渲染），默认实现走 stdout 策略。
    virtual void update(State s);

    /// 阶段结束：强制把 finalState 刷成 100% 进度条（绕过节流），换行，
    /// 再打印 `<label> done in <sec>`。finalState.index 应已是 total（满进度）。
    virtual void finishPhase(State finalState, std::string_view label);

private:
    // 是否实际产生输出（TTY 刷新或 verbose 换行）。quiet / 非 TTY 非 verbose → 不输出。
    bool shouldOutput() const noexcept;
    // \r 刷新一行（TTY 模式）。
    void refreshLine(const State& s);
    // 换行打印一行（非 TTY + verbose 模式）。
    void printLine(const State& s);
    // 把 State 格式化成单行文本（两模式共用；stdout 输出面，允许 std::string）。
    std::string format(const State& s) const;

    bool quiet_         = false;
    bool verbose_       = false;
    bool isTty_         = true;
    bool lastWasReturn_ = false;   ///< 末动作是否为 \r（析构判断要不要补 \n）
    int  lastLineLen_   = 0;       ///< 上次 \r 行写入的字符数（抹残留用，防窄终端换行）
    mjbase::util::TimeUtil::TimePoint start_{};
    mjbase::util::TimeUtil::TimePoint lastFlush_{};
};

}   // namespace mjbase::spi
