#pragma once

#include <chrono>

namespace mjbase::util {

/// @brief chrono 的二次封装，收口计时相关操作。
///
/// 项目内（尤其进度反馈、热路径计时）一律经 TimeUtil，禁止在调用点散用
/// `std::chrono::steady_clock::now()` / `duration_cast` / `time_since_epoch`
/// 这些冗长接口。静态方法类风格（原 `RandomUtil` 已删除，见 dev_plan H）。
///
/// `TimePoint` 虽然是 chrono 类型的别名，但只作为**不透明 token** 在本类
/// 的 API 之间传递，调用方 include 本头即可，无需直接处理 chrono。
///
/// 与 `dos_time_util.hpp` 区分：本类是**经过时长测量**（elapsed），
/// `dos_time_util` 是 jar writer 用的**绝对时刻**（DOS 时间戳）。
class TimeUtil
{
public:
    /// 不透明时刻类型，仅用于本类的 API 之间传递（开始计时、计算 elapsed）。
    using TimePoint = std::chrono::steady_clock::time_point;

    /// 取当前时刻（开始计时用）。
    static TimePoint now() noexcept { return std::chrono::steady_clock::now(); }

    /// 从 start 到现在经过的秒数（浮点，调用方自行格式化小数位）。
    static double elapsedSec(TimePoint start) noexcept
    {
        return std::chrono::duration<double>(now() - start).count();
    }

    /// 从 start 到现在经过的毫秒数（节流判断用，整数足够）。
    static long elapsedMs(TimePoint start) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now() - start).count();
    }

    /// 两个时刻间隔的毫秒数（节流：判断距上次刷新是否满阈值，如 100ms）。
    static long diffMs(TimePoint earlier, TimePoint later) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(later - earlier).count();
    }
};

}   // namespace mjbase::util
