#pragma once

// ── 进程级日志门面初始化（#971）─────────────────────────────────────
//
// 诊断输出统一走 ca::log 门面：CLI / agent 入口在进入业务前调用一次
// initDefaultLogger()，库与子模块经 CA_LOG_WARN / CA_LOG_ERROR 等宏
// （"default" target）输出诊断。门面在未注册 target 时静默丢弃，
// 因此入口不初始化 = 诊断不可见——各入口必须调用。
//
// 迁移动机：subprocess.hpp 会 #undef stderr，任何在它之后解析的
// TU 里裸用 stderr 宏直接编译失败（MSVC C2065）；统一走门面后
// 诊断路径不再依赖 stderr 标识符。

#include <memory>

#include "libca/log/log_macros.hpp"
#include "libca/log/simple_log_backend.hpp"

namespace mjbase::log {

/// 注册进程级 "default" 日志 target（SimpleLogBackend：stderr、彩色、HH:MM:SS，
/// 不带 file:line——与既有日志门面范式一致）。幂等，可重复调用。
///
/// 运行期级别取 Trace：诊断可见性与迁移前的 fprintf/stderr 打平，
/// 输出开关仍由各调用点的既有门控（env 探针 / verbose 标志）负责。
inline void initDefaultLogger()
{
    static const bool initialized = [] {
        ca::log::SimpleLogConfig cfg;
        cfg.stream        = ca::log::SimpleLogConfig::Stream::Stderr;
        cfg.color         = true;
        cfg.show_target   = false;
        cfg.show_location = false;
        cfg.time_format   = "%H:%M:%S";

        auto logger = std::make_shared<ca::log::Logger>(
            std::make_shared<ca::log::SimpleLogBackend>(cfg));
        logger->set_level(ca::log::Level::Trace);
        ca::log::LoggerRegistry::register_logger("default", logger);
        return true;
    }();
    (void)initialized;
}

}   // namespace mjbase::log
