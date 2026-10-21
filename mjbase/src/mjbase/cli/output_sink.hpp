#pragma once

// ── 输出抽象（FILE* 封装）──────────────────────────────────────────
//
// 替代散落在各 cmd_*.cpp 的 fprintf(stdout/stderr, ...)。默认指向 stdout/stderr
// （行为与现状一致），但可指向任意 FILE*（测试捕获、重定向、换输出后端），
// 让输出可重定向/可测试。
//
// 用法：
//   mjbase::cli::out().printf("  Version: %d.%d\n", major, minor);
//   mjbase::cli::err().printf("Error: %s\n", msg);
//   mjbase::cli::out().puts("done");
//
// 测试捕获：
//   mjbase::cli::OutputSink sink(tmpfile);  // 或注入到命令
//   sink.printf(...);
//

#include <cstdio>
#include <cstdarg>
#include <string>

namespace mjbase::cli {

/// FILE* 封装的输出 sink。printf 签名与 fprintf 一致（首参为 sink 自身），
/// 故 fprintf(stdout, fmt, ...) → out().printf(fmt, ...) 是 1:1 机械替换。
class OutputSink
{
public:
    explicit OutputSink(FILE* f)
        : file_(f)
    {}

    /// 变参格式化输出（与 fprintf 同语义，写到 sink 持有的 FILE*）。
    void printf(const char* fmt, ...) const
    {
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(file_, fmt, ap);
        va_end(ap);
    }

    /// 输出一行（自动加换行）。
    void puts(const std::string& s) const
    {
        std::fputs(s.c_str(), file_);
        std::fputc('\n', file_);
    }

    /// 输出原始字符串（不加换行）。
    void write(const std::string& s) const { std::fputs(s.c_str(), file_); }

    FILE* file() const { return file_; }

private:
    FILE* file_;
};

/// 默认 stdout sink（进程级单例，避免每个 cmd 都构造）。
inline OutputSink& out()
{
    static OutputSink sink(stdout);
    return sink;
}

/// 默认 stderr sink。
inline OutputSink& err()
{
    static OutputSink sink(stderr);
    return sink;
}

// ── format 解析（消除 classes/strings/xref/jar list 各手搓的 --format 校验）──

enum class Format
{
    Plain,
    Json,
    Csv
};

/// 把 "plain"/"json"/"csv" 解析为 Format；非法返回 Plain（调用方按需报错）。
inline Format parseFormat(const std::string& s)
{
    if (s == "json") return Format::Json;
    if (s == "csv") return Format::Csv;
    return Format::Plain;
}

}   // namespace mjbase::cli
