#include "mjbase/spi/progress_sink.hpp"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#    include <io.h>
#    define STDOUT_IS_TTY() (_isatty(_fileno(stdout)) != 0)
#else
#    include <unistd.h>
#    define STDOUT_IS_TTY() (isatty(fileno(stdout)) != 0)
#endif

namespace mjbase::spi {

namespace {

// 节流：TTY 下最快每 100ms 刷新一次。
constexpr long kRefreshIntervalMs = 100;

// 智能时长格式化（避免大 jar 跑完只显 0.0s / 长任务显 12345s 不可读）：
//   < 1ms   → "123us"
//   < 1s    → "12.3ms"
//   < 60s   → "1.2s"
//   >= 60s  → "2m 3s"
std::string formatDuration(double sec)
{
    char buf[32];
    if (sec < 0.001) {
        // 微秒：sec*1000*1000，整数 us。
        std::snprintf(buf, sizeof(buf), "%lldus", static_cast<long long>(sec * 1'000'000));
    }
    else if (sec < 1.0) {
        std::snprintf(buf, sizeof(buf), "%.1fms", sec * 1000.0);
    }
    else if (sec < 60.0) {
        std::snprintf(buf, sizeof(buf), "%.1fs", sec);
    }
    else {
        const long total = static_cast<long>(sec);
        const long m     = total / 60;
        const long s     = total % 60;
        std::snprintf(buf, sizeof(buf), "%ldm %lds", m, s);
    }
    return std::string(buf);
}

}   // namespace

ProgressSink::ProgressSink()
{
    isTty_ = STDOUT_IS_TTY();
}

ProgressSink::~ProgressSink()
{
    // SIGINT / 异常退出兜底：末动作若为 \r 且未 finish，补 \n，
    // 避免 shell 提示符粘在进度行末尾。
    // 正常 finishPhase 已置 lastWasReturn_=false，不会重复输出。
    if (lastWasReturn_) {
        std::fputc('\n', stdout);
        std::fflush(stdout);
        lastWasReturn_ = false;
    }
}

void ProgressSink::start() noexcept
{
    start_     = mjbase::util::TimeUtil::now();
    lastFlush_ = start_;
}

bool ProgressSink::shouldOutput() const noexcept
{
    if (quiet_) return false;
    // TTY 默认输出 \r 刷新；非 TTY 仅 verbose 才输出（换行式）。
    return isTty_ || verbose_;
}

void ProgressSink::update(State s)
{
    if (!shouldOutput()) return;

    // TTY：按时间节流（强制刷新点由调用方直接调 finishPhase / 单独保证）。
    if (isTty_) {
        const long sinceLast =
            mjbase::util::TimeUtil::diffMs(lastFlush_, mjbase::util::TimeUtil::now());
        if (sinceLast < kRefreshIntervalMs) return;   // 未到节流间隔，跳过本次刷新
        refreshLine(s);
    }
    else {
        // 非 TTY + verbose：换行打印（不做 \r，避免日志残留控制字符）。
        printLine(s);
    }
}

void ProgressSink::finishPhase(State finalState, std::string_view label)
{
    if (!shouldOutput()) {
        lastWasReturn_ = false;   // 静默模式没输出过 \r，析构不补
        lastFlush_     = mjbase::util::TimeUtil::now();
        return;
    }
    // TTY：节流可能让最后一次 update（100%）没刷出来，这里强制补刷满进度条再换行。
    // 非 TTY + verbose：update 的每条都已换行打印（含最后一条 100%），无需重复打。
    if (isTty_) {
        refreshLine(finalState);    // 显示满进度条
        std::fputc('\n', stdout);   // 换行，进度条定格
    }
    // 总结行：label done in <智能时长>
    std::string summary(label);
    summary += " done in ";
    summary += formatDuration(mjbase::util::TimeUtil::elapsedSec(start_));
    std::fputs(summary.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    lastWasReturn_ = false;   // 已换行，析构不再补
    lastLineLen_   = 0;       // 阶段结束，重置行长度
    lastFlush_     = mjbase::util::TimeUtil::now();
}

void ProgressSink::refreshLine(const State& s)
{
    std::string line = format(s);
    // \r 回到行首重写。抹掉上次的残字符：只补空格到上次写入的长度，
    // 不固定填充——否则在窄终端上填充的空格会换行成空白行。
    std::fputc('\r', stdout);
    std::fputs(line.c_str(), stdout);
    const int newLen = static_cast<int>(line.size());
    for (int i = newLen; i < lastLineLen_; ++i) {
        std::fputc(' ', stdout);
    }
    std::fflush(stdout);
    lastLineLen_   = newLen;
    lastWasReturn_ = true;
    lastFlush_     = mjbase::util::TimeUtil::now();
}

void ProgressSink::printLine(const State& s)
{
    std::string line = format(s);
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    lastWasReturn_ = false;
    lastFlush_     = mjbase::util::TimeUtil::now();
}

std::string ProgressSink::format(const State& s) const
{
    std::string out;
    out.reserve(128);

    if (s.phase == Phase::Loading) {
        // loading resource [23/1422] com/example/Foo 1.24s
        out += "loading resource [";
        out += std::to_string(s.index);
        out += '/';
        out += std::to_string(s.total);
        out += "] ";
        out += s.currentName.to_std_string();   // stdout 输出面
        out += ' ';
        out += formatDuration(mjbase::util::TimeUtil::elapsedSec(start_));
    }
    else {
        // pipeline: [===> ] class [i/total] pct% time
        // 视觉进度条（固定 20 格，按百分比填充）。
        const int     pct       = s.total > 0 ? (s.index * 100 / s.total) : 0;
        constexpr int kBarWidth = 20;
        const int     filled    = pct * kBarWidth / 100;
        out += '[';
        for (int i = 0; i < kBarWidth; ++i) {
            out += (i < filled) ? '=' : (i == filled ? '>' : ' ');
        }
        out += "] ";
        // passTotal==0（pass 名未透传）则不显示 pass 段。
        if (s.passTotal > 0) {
            if (s.passNames.empty()) {
                out += "pass ";
                out += std::to_string(s.passIndex);
                out += '/';
                out += std::to_string(s.passTotal);
                out += ' ';
            }
            else {
                // pass 名标记（*完成 / >当前）。
                for (int i = 0; i < static_cast<int>(s.passNames.size()); ++i) {
                    if (i + 1 < s.passIndex)
                        out += '*';
                    else if (i + 1 == s.passIndex)
                        out += '>';
                    else
                        out += ' ';
                    out += s.passNames[static_cast<ca::usize>(i)].to_std_string();
                    out += ' ';
                }
            }
        }
        out += s.currentName.to_std_string();
        out += " [";
        out += std::to_string(s.index);
        out += '/';
        out += std::to_string(s.total);
        out += "] ";
        out += std::to_string(pct);
        out += "% ";
        out += formatDuration(mjbase::util::TimeUtil::elapsedSec(start_));
    }
    return out;
}

}   // namespace mjbase::spi
