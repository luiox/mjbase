#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjbase::cli {

/// @brief CLI 通用参数（-i / -o / --jdk / -c / -m）。
///
/// input/output 为必填，jdkPath 为空表示不启用 COMPUTE_FRAMES。
///
/// targetClass（-c）非空时只限制处理集合：匹配类会被处理，
/// 未匹配 class 和非 class 资源仍会保留并写入输出 JAR。targetMethod（-m）
/// 非空时只处理匹配方法，类本身仍参与读写 roundtrip。
struct CliOptions
{
    std::string input;
    std::string output;
    std::string jdkPath;
    std::string targetClass;       // -c: internal name 子串匹配（org/example/Demo）
    std::string targetMethod;      // -m: "name" 或 "name()I"
    bool        quiet   = false;   // --quiet: 完全静默，仅最终总结行
    bool        verbose = false;   // --verbose: 非 TTY 下也输出换行式阶段日志
    // keep 配置（审计 P0#1：keep 规则的 CLI 暴露面）。runCli 不消费——它们服务
    // 于跑全局收缩管线的命令（规则文件经消费方既有 IO 路径读入；档位文本经
    // mjbase::cli::keepTierOf 解析成 reference::KeepTier）。
    std::string keepFile;          // --keep <file>: keep 规则 DSL 文件
    std::string keepTier;          // --keep-tier <name>: none|public|public-protected
};

/// @brief 单个 class 的处理结果。
struct ClassProcessResult
{
    bool        ok = true;
    std::string message;
};

/// @brief class 字节处理器：输入 URI + class 字节，原地修改字节并返回处理状态。
///
/// @param uri      资源 URI（jar 内路径或 class 文件路径，借用视图，回调期内有效）
/// @param bytes    class 字节（引用，处理器直接原地改写）
/// @return 处理结果（ok=false 时计入失败计数，message 用于日志）
using ClassProcessor =
    std::function<ClassProcessResult(std::string_view uri, std::vector<ca::u8>& bytes)>;

/// CLI 运行失败的稳定分类，供命令行和发布 smoke test 使用。
enum class CliFailureKind
{
    None,
    InvalidArguments,
    UnsupportedInput,
    Read,
    NoClass,
    NoMatch,
    Processing,
    Write,
    Verification,
};

/// @brief 整体运行报告。
///
/// `failure` 和 `failureMessage` 是稳定的机器可读失败契约；调用方不得只依据 stderr
/// 文本判断失败原因。成功写出后 runner 会重新读取输出，并确认每个 class 都能重新解析；
/// JAR 中后缀为 `.class` 但未被识别为 class 资源的条目同样会导致 `Verification`。
struct CliRunReport
{
    ca::i32 processed = 0;   // 成功处理的 class 数
    ca::i32 failed    = 0;   // 失败的 class 数
    CliFailureKind failure = CliFailureKind::None;
    std::string    failureMessage;
    // 成功 = 处理了至少一个 class 且无失败（空输入/不支持的类型/全部失败都算失败）
    bool ok() const { return processed > 0 && failed == 0; }
};

/// @brief CLI 公共骨架：import → 对每个 ClassResource 跑 processor → export。
///
/// 消除各工具重复的「解析参数 → 读资源 → 转换 → 写出」流程。文件 IO 走
/// ca::fs::FileUtil / ResourceHelper。输出先写入同目录临时文件，逐个重新解析
/// class 验证后才发布到此前不存在的目标路径。
///
/// 输入类型由 JarUtil::isJarFile / isClassFile 自动分发：
///   - jar：importFromJar → 遍历所有 ClassResource → exportToJar
///   - class：importFromClassFile → 处理 → exportToClassFile
///
/// @param options   CLI 参数
/// @param processor class 字节处理器
/// @return 运行报告；输入非法（空路径/不支持的类型/无 class）时 report.ok()=false
///         且 processed=0，调用方据此返回非零退出码。
CliRunReport runCli(const CliOptions& options, const ClassProcessor& processor);

/// @brief 把 targetMethod（"name" 或 "name()I"）解析成方法过滤器。
///
/// - "name"      → name 精确匹配，descriptor 任意
/// - "name()I"   → name 精确匹配 AND descriptor 精确匹配
/// - ""          → 返回 nullopt（不过滤）
///
/// 旧栈 的 buildMethodPredicate 架在 jbytecode::tree::MethodNode 上；mjbase
/// 底座无该类型，过滤契约改落在 (name, descriptor) 借用视图上，后续 pass 域
/// 接线时由消费方把成员元数据投到视图（语义等价：匹配规则不变）。
class MethodFilter
{
public:
    /// @brief 解析 targetMethod 文本；空串返回 nullopt（不过滤）。
    static std::optional<MethodFilter> parse(const std::string& targetMethod)
    {
        if (targetMethod.empty()) return std::nullopt;

        MethodFilter filter;
        // 拆 "name(desc)"：左括号前是 name，括号内（含到结尾）是 desc
        auto paren = targetMethod.find('(');
        if (paren == std::string::npos) {
            filter.name_ = targetMethod;   // 纯名字，desc 任意
        }
        else {
            filter.name_ = targetMethod.substr(0, paren);
            filter.desc_ = targetMethod.substr(paren);   // 含 '('
        }
        return filter;
    }

    /// @brief 成员是否命中：name 精确匹配；desc 规格非空时还需精确匹配。
    bool matches(std::string_view name, std::string_view desc) const
    {
        if (name != std::string_view(name_)) return false;   // name 精确匹配
        if (!desc_.empty() && desc != std::string_view(desc_)) return false;
        return true;
    }

    /// @brief 注册时的名字规格（诊断/展示用）。
    const std::string& name() const { return name_; }
    /// @brief 注册时的描述符规格（含 '('，空表示不限定）。
    const std::string& descriptor() const { return desc_; }

private:
    std::string name_;   ///< 方法名规格
    std::string desc_;   ///< 描述符规格（"" = 任意）
};

}   // namespace mjbase::cli
