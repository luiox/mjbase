#include "dos_time_util.hpp"

#include <chrono>
#include <ctime>

namespace mjbase::jar {

// DOS 时间格式：小时(5) << 分(5) << 秒/2(5)
ca::u16 dos_time_from_tm(const struct tm* t)
{
    return static_cast<ca::u16>((t->tm_hour << 11) | (t->tm_min << 5) | (t->tm_sec / 2));
}

// DOS 日期格式：年-1980(7) << 月(4) << 日(5)
ca::u16 dos_date_from_tm(const struct tm* t)
{
    return static_cast<ca::u16>(((t->tm_year - 80) << 9) | ((t->tm_mon + 1) << 5) | t->tm_mday);
}

ca::u16 current_dos_time()
{
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    std::tm time_info{};
#ifdef _WIN32
    // Windows
    localtime_s(&time_info, &now);
#else
    // Linux / macOS / Unix
    localtime_r(&now, &time_info);
#endif
    auto dosTime = dos_time_from_tm(&time_info);
    return dosTime;
}

ca::u16 current_dos_date()
{
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    std::tm time_info{};
#ifdef _WIN32
    // Windows
    localtime_s(&time_info, &now);
#else
    // Linux / macOS / Unix
    localtime_r(&now, &time_info);
#endif
    auto dosDate = dos_date_from_tm(&time_info);
    return dosDate;
}

}   // namespace mjbase::jar
