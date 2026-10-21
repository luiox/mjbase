#pragma once

// ── 声明式 CLI 参数解析器（header-only）────────────────────────────
//
// 消除各线 cmd_*.cpp 与 pass 驱动里重复的「for(i..argc) strcmp(argv) {
// if(++i>=argc) err } + help 文本 + 必填校验」模板。声明式注册 option 表，
// 统一支持 -x v / --name v / --name=v 三种写法，自动生成 help，自动校验必填。
//
// 用法：
//   mjbase::cli::ArgParser p("mjbase opt -i <jar> -o <out>", "处理字节码");
//   p.add({"--input","-i"}, mjbase::cli::OptKind::String, "<jar>", "输入 jar/class", true)
//    .add({"--output","-o"}, mjbase::cli::OptKind::String, "<out>", "输出路径", true)
//    .add({"--verbose","-v"}, mjbase::cli::OptKind::Flag, "", "详细输出");
//   auto r = p.parse(argc, argv);
//   if (!r) return 1;            // 错误或 -h 已打印，直接退出
//   auto in = r->getString("--input");
//
// 设计约束：不依赖 RTTI/异常；纯标准库 + libca 类型（遵循 AGENTS.md）。
// .lang 表随 mjbase target 原样嵌入，见 cli/i18n_tables.hpp）。

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libca/i18n/i18n.hpp"

namespace mjbase::cli {

/// Option 取值类型。
enum class OptKind
{
    Flag,         // 无值开关：-v / --verbose
    String,       // 单字符串：-i jar
    Int,          // 整数：--length 16
    CommaList,    // 逗号分隔列表：-p A,B,C（也接受 -p A -p B 多次追加）
    Positional,   // 位置参数（裸 argv，非 - 开头）：grep <pattern>
};

/// 一个 option 的声明。
struct OptSpec
{
    std::vector<std::string> names;   // 别名列表；第一个为 canonical 名（约定长名 --xxx 在前）
    OptKind     kind = OptKind::String;
    std::string metavar;            // help 里的占位符 <jar>；Positional 用作位置参数名
    std::string help;               // 该 option 描述
    bool        required = false;   // 缺失则报错（Flag/Positional 忽略此字段）
};

/// 解析结果。所有 getter 按 canonical 名（OptSpec.names[0]）取值。
class ArgResult
{
public:
    ArgResult() = default;

    /// Flag 是否出现。
    bool has(const std::string& canonical) const
    {
        return present_.find(canonical) != present_.end();
    }

    /// 取 String 值；未出现返回 def。
    std::string getString(const std::string& canonical, std::string def = {}) const
    {
        auto it = strValues_.find(canonical);
        return it == strValues_.end() ? def : it->second;
    }

    /// 取 Int 值；未出现或解析失败返回 def。
    int getInt(const std::string& canonical, int def = 0) const
    {
        auto it = intValues_.find(canonical);
        return it == intValues_.end() ? def : it->second;
    }

    /// 取 CommaList 值；未出现返回空。多次出现会累加。
    std::vector<std::string> getList(const std::string& canonical) const
    {
        auto it = listValues_.find(canonical);
        return it == listValues_.end() ? std::vector<std::string>{} : it->second;
    }

    /// 所有位置参数（按命令行顺序）。
    const std::vector<std::string>& positionals() const { return positionals_; }

    // ── 内部累积接口（ArgParser::parse 用）──
    void markPresent(const std::string& canonical) { present_.insert(canonical); }
    void setString(const std::string& canonical, std::string v)
    {
        strValues_[canonical] = std::move(v);
        present_.insert(canonical);
    }
    void setInt(const std::string& canonical, int v)
    {
        intValues_[canonical] = v;
        present_.insert(canonical);
    }
    void appendList(const std::string& canonical, std::vector<std::string> items)
    {
        auto& dst = listValues_[canonical];
        for (auto& it : items) dst.push_back(std::move(it));
        present_.insert(canonical);
    }
    void appendPositional(std::string v) { positionals_.push_back(std::move(v)); }

private:
    std::unordered_map<std::string, std::string>              strValues_;
    std::unordered_map<std::string, int>                      intValues_;
    std::unordered_map<std::string, std::vector<std::string>> listValues_;
    std::unordered_set<std::string>                           present_;   // 仅用 key 集合
    std::vector<std::string>                                  positionals_;
};

/// ArgParser 解析失败的稳定类别。
///
/// 现有调用方可继续使用 parse() 的 optional 接口；需要稳定退出码的命令应使用
/// parseDetailed()，而不要从错误文本推断失败原因。
enum class ArgParseError
{
    None,
    HelpRequested,
    UnknownOption,
    FlagHasValue,
    MissingValue,
    EmptyValue,
    UnexpectedArgument,
    MissingRequired,
    InvalidInteger,
    MutexConflict,
    MutexRequired,
};

/// ArgParser 的带分类解析结果。
class ArgParseOutcome
{
public:
    /// 解析是否成功。
    bool ok() const { return value_.has_value(); }

    /// 是否因 help 请求提前结束。
    bool helpRequested() const { return error_ == ArgParseError::HelpRequested; }

    /// 失败类别；成功时为 None。
    ArgParseError error() const { return error_; }

    /// 成功后的不可变参数结果；失败时为 nullptr。
    const ArgResult* value() const { return value_ ? &*value_ : nullptr; }

    /// 转移成功的参数结果，供保持 optional API 的兼容包装使用。
    std::optional<ArgResult> takeValue() { return std::move(value_); }

private:
    friend class ArgParser;

    static ArgParseOutcome success(ArgResult value)
    {
        ArgParseOutcome outcome;
        outcome.value_ = std::move(value);
        return outcome;
    }

    static ArgParseOutcome failure(ArgParseError error)
    {
        ArgParseOutcome outcome;
        outcome.error_ = error;
        return outcome;
    }

    std::optional<ArgResult> value_;
    ArgParseError            error_ = ArgParseError::None;
};

/// 声明式参数解析器。
class ArgParser
{
public:
    ArgParser(std::string usageLine, std::string description)
        : usageLine_(std::move(usageLine))
        , description_(std::move(description))
    {
        // 内置 -h/--help，不占 user spec 位
    }

    /// 链式注册。OptSpec.names[0] 为 canonical 名。
    ArgParser& add(OptSpec spec)
    {
        // 第一个 spec 决定 canonical 名；建立别名→canonical 映射
        if (spec.names.empty()) return *this;
        std::string canonical = spec.names[0];
        for (const auto& nm : spec.names) alias_[nm] = canonical;
        specs_.push_back(std::move(spec));
        return *this;
    }
    /// 便捷重载：从 names+kind 构造。
    ArgParser& add(std::vector<std::string> names, OptKind kind, std::string metavar,
                   std::string help, bool required = false)
    {
        return add(OptSpec{std::move(names), kind, std::move(metavar), std::move(help), required});
    }

    /// 注册互斥组：组内 canonical 名最多出现一个。
    /// @param canonicalNames 组内成员的 canonical 名（OptSpec.names[0]）
    /// @param required true 表示组内至少出现一个（如 subs 的 -s/--interface 二选一）
    ArgParser& addMutexGroup(std::vector<std::string> canonicalNames, bool required = false)
    {
        mutexGroups_.push_back({std::move(canonicalNames), required});
        return *this;
    }

    /// 打印 help（自动从 specs 拼接）。usage 行 + 描述 + options 表。
    /// 框架文案经 mbase.cli.* 单源表本地化(mjbase 自带 .lang 嵌入,显式 init 接入,
    /// 见 cli/i18n_tables.hpp);OptSpec.help/metavar 由调用方在注册时用 tr/trf 渲染。
    void printHelp() const
    {
        std::fprintf(stdout, "%s:\n  %s\n\n", ca::i18n::tr("mbase.cli.usage"), usageLine_.c_str());
        if (!description_.empty()) {
            std::fprintf(stdout, "%s\n\n", description_.c_str());
        }
        std::fprintf(stdout, "%s\n", ca::i18n::tr("mbase.cli.options_header"));
        std::fprintf(stdout, "  -h, --help%*s%s\n", 16, "", ca::i18n::tr("mbase.cli.help_flag"));
        for (const auto& s : specs_) {
            printOptionRow(s);
        }
    }

    /// 解析 argv[1..argc)，并保留失败类别。
    ///
    /// 失败（未知参数/缺值/缺必填/-h）已打印信息到 stdout/stderr；需要区分 help 和
    /// 参数错误的调用方使用 ArgParseOutcome::error() 决定退出码。
    ArgParseOutcome parseDetailed(int argc, char** argv) const
    {
        ArgResult result;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            // help：任何位置出现都触发
            if (arg == "-h" || arg == "--help") {
                printHelp();
                return ArgParseOutcome::failure(ArgParseError::HelpRequested);
            }

            // 是否以 - 开头（option）还是位置参数
            if (arg.size() >= 1 && arg[0] == '-' && arg != "-") {
                // 拆出可能的 =value
                std::string name, inlineVal;
                bool        hasInline = false;
                auto        eq        = arg.find('=');
                if (eq != std::string::npos) {
                    name      = arg.substr(0, eq);
                    inlineVal = arg.substr(eq + 1);
                    hasInline = true;
                }
                else {
                    name = arg;
                }

                const OptSpec* spec = findSpec(name);
                if (!spec) {
                    std::fprintf(stderr, "Error: %s\n",
                                 ca::i18n::trf("mbase.cli.err.unknown_option", {arg}).c_str());
                    return ArgParseOutcome::failure(ArgParseError::UnknownOption);
                }
                std::string canonical = spec->names[0];

                if (spec->kind == OptKind::Flag) {
                    if (hasInline) {
                        std::fprintf(stderr, "Error: %s\n",
                                     ca::i18n::trf("mbase.cli.err.flag_no_value", {name}).c_str());
                        return ArgParseOutcome::failure(ArgParseError::FlagHasValue);
                    }
                    result.markPresent(canonical);
                    continue;
                }

                // 取值：优先 inline，否则取下一个 argv
                std::string value;
                if (hasInline) {
                    value = inlineVal;
                }
                else {
                    if (i + 1 >= argc) {
                        std::fprintf(stderr, "Error: %s\n",
                                     ca::i18n::trf("mbase.cli.err.requires_argument",
                                                           {name}).c_str());
                        return ArgParseOutcome::failure(ArgParseError::MissingValue);
                    }
                    value = argv[++i];
                }

                if (value.empty()) {
                    std::fprintf(stderr, "Error: %s\n",
                                 ca::i18n::trf("mbase.cli.err.empty_argument", {name}).c_str());
                    return ArgParseOutcome::failure(ArgParseError::EmptyValue);
                }

                if (!consumeValue(*spec, canonical, value, result)) {
                    return ArgParseOutcome::failure(spec->kind == OptKind::Int
                                                        ? ArgParseError::InvalidInteger
                                                        : ArgParseError::MissingValue);
                }
            }
            else {
                // 位置参数：必须有对应的 Positional spec（否则报错，避免静默吞）
                if (!hasPositionalSpec()) {
                    std::fprintf(stderr, "Error: %s\n",
                                 ca::i18n::trf("mbase.cli.err.unexpected_argument",
                                                       {arg}).c_str());
                    return ArgParseOutcome::failure(ArgParseError::UnexpectedArgument);
                }
                result.appendPositional(arg);
            }
        }

        // 必填校验
        for (const auto& s : specs_) {
            if (s.required && s.kind != OptKind::Positional && s.kind != OptKind::Flag) {
                std::string canonical = s.names[0];
                if (!result.has(canonical)) {
                    std::fprintf(stderr, "Error: %s\n",
                                 ca::i18n::trf("mbase.cli.err.missing_required",
                                                       {canonical}).c_str());
                    return ArgParseOutcome::failure(ArgParseError::MissingRequired);
                }
            }
        }

        // 互斥组校验：组内最多一个；required 组至少一个
        for (const auto& g : mutexGroups_) {
            int count = 0;
            for (const auto& cn : g.names) {
                if (result.has(cn)) count++;
            }
            if (count > 1) {
                std::fprintf(stderr, "Error: %s\n", ca::i18n::tr("mbase.cli.err.mutex_conflict"));
                return ArgParseOutcome::failure(ArgParseError::MutexConflict);
            }
            if (g.required && count == 0) {
                std::fprintf(stderr, "Error: %s\n", ca::i18n::tr("mbase.cli.err.mutex_required"));
                return ArgParseOutcome::failure(ArgParseError::MutexRequired);
            }
        }

        return ArgParseOutcome::success(std::move(result));
    }

    /// 兼容旧调用方的 optional 接口。
    std::optional<ArgResult> parse(int argc, char** argv) const
    {
        return parseDetailed(argc, argv).takeValue();
    }

private:
    struct MutexGroup
    {
        std::vector<std::string> names;
        bool                     required;
    };
    std::string                                  usageLine_;
    std::string                                  description_;
    std::vector<OptSpec>                         specs_;
    std::unordered_map<std::string, std::string> alias_;   // 别名→canonical
    std::vector<MutexGroup>                      mutexGroups_;

    const OptSpec* findSpec(const std::string& name) const
    {
        auto it = alias_.find(name);
        if (it == alias_.end()) return nullptr;
        const std::string& canonical = it->second;
        for (const auto& s : specs_) {
            if (s.names[0] == canonical) return &s;
        }
        return nullptr;
    }

    bool hasPositionalSpec() const
    {
        for (const auto& s : specs_) {
            if (s.kind == OptKind::Positional) return true;
        }
        return false;
    }

    bool consumeValue(const OptSpec& spec, const std::string& canonical, const std::string& value,
                      ArgResult& result) const
    {
        switch (spec.kind) {
        case OptKind::String: result.setString(canonical, value); return true;
        case OptKind::Int:
        {
            try {
                int v = std::stoi(value);
                result.setInt(canonical, v);
            }
            catch (...) {
                std::fprintf(stderr, "Error: %s\n",
                             ca::i18n::trf("mbase.cli.err.invalid_integer",
                                                   {canonical, value}).c_str());
                return false;
            }
            return true;
        }
        case OptKind::CommaList:
        {
            result.appendList(canonical, splitComma(value));
            return true;
        }
        default:   // Flag/Positional 不应走到这里
            result.setString(canonical, value);
            return true;
        }
    }

    static std::vector<std::string> splitComma(const std::string& s)
    {
        std::vector<std::string> out;
        if (s.empty()) return out;
        std::size_t start = 0;
        while (true) {
            auto comma = s.find(',', start);
            if (comma == std::string::npos) {
                out.push_back(s.substr(start));
                break;
            }
            out.push_back(s.substr(start, comma - start));
            start = comma + 1;
        }
        return out;
    }

    void printOptionRow(const OptSpec& s) const
    {
        // 拼接所有别名，如 "-i, --input"
        std::string left;
        for (std::size_t k = 0; k < s.names.size(); k++) {
            if (k) left += ", ";
            left += s.names[k];
        }
        if (!s.metavar.empty()) {
            left += " " + s.metavar;
        }
        // 对齐到 26 列（与 -h 行宽度接近），超过则换行缩进
        const int   align = 26;
        std::string pad;
        if (static_cast<int>(left.size()) < align) {
            pad = std::string(align - left.size(), ' ');
        }
        else {
            pad = "\n" + std::string(align, ' ');
        }
        std::fprintf(stdout, "  %s%s%s\n", left.c_str(), pad.c_str(), s.help.c_str());
    }
};

}   // namespace mjbase::cli
