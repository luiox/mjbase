#include "mjbase/cli/cli_runner.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>

#include "jbc/core/reader.hpp"
#include "libca/fs/file_util.hpp"
#include "mjbase/i18n.hpp"
#include "mjbase/jar/jar_util.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjbase::cli {

namespace {

using ca::fs::FileUtil;
using mjbase::tr;

namespace model = mjbase::model;

// 判断输入是 jar、class 还是其它
enum class InputKind
{
    Jar,
    Class,
    Unsupported
};

void setFailure(CliRunReport& report, CliFailureKind kind, std::string message)
{
    report.failure        = kind;
    report.failureMessage = std::move(message);
}

// 重新导入输出文件，确认其中每个 Class 资源都能被 classfile 读取器完整解析
// （mbase 侧以 ClassReader 异常判定；mjbase 换 jbc::core::readClass——
// Err / 非 Validated / 类名为空都视为验证失败）。
bool verifyOutput(const std::string& output, InputKind kind)
{
    model::ResourceContainer check;
    if (kind == InputKind::Jar) {
        if (!model::ResourceHelper::importFromJar(check, output).is_ok()) return false;
    }
    else if (kind == InputKind::Class) {
        if (!model::ResourceHelper::importFromClassFile(check, output).is_ok())
            return false;
    }
    else {
        return false;
    }
    bool hasClass = false;
    for (const auto& [uri, resource] : check.getResources()) {
        if (!resource) {
            std::fprintf(stderr, "[verify] null resource for %s\n",
                         std::string(uri).c_str());
            return false;
        }
        if (resource->getType() != model::ResourceType::Class) {
            // 病态 jar 可能含 0 字节 / 非 class 内容但以 .class 命名的 entry
            // （如被混淆器改坏的 module-info.class）。此时按内容分拣已是
            // UnknownResource，说明其字节本身不构成可解析 class，跳过即可。
            continue;
        }

        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(resource.get());
        if (!classResource || classResource->get().empty()) {
            std::fprintf(stderr, "[verify] empty class bytes for %s\n",
                         std::string(uri).c_str());
            return false;
        }

        const auto& bytes = classResource->get();
        auto        read  = jbc::core::readClass(jbc::core::ByteView(bytes));
        if (read.is_err()) {
            std::fprintf(stderr, "[verify] parse failed for %s: %s\n",
                         std::string(uri).c_str(),
                         jbc::core::to_string(std::move(read).unwrap_err()));
            return false;
        }
        auto readResult = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
        if (readResult.state != jbc::core::ParseState::Validated) {
            std::fprintf(stderr, "[verify] degraded class for %s: %s\n",
                         std::string(uri).c_str(), jbc::core::to_string(readResult.state));
            return false;
        }
        if (readResult.file->thisName().empty()) {
            std::fprintf(stderr, "[verify] empty class name for %s\n",
                         std::string(uri).c_str());
            return false;
        }
        hasClass = true;
    }
    return hasClass;
}

bool validateOutputTarget(const CliOptions& options, CliRunReport& report)
{
    if (!FileUtil::exists(options.output)) return true;
    setFailure(report, CliFailureKind::InvalidArguments,
               tr("mbase.cli.runner.err.output_exists"));
    std::fprintf(stderr, "Error: refusing to overwrite existing output %s\n",
                 options.output.c_str());
    return false;
}

std::string createStagedOutput(const std::string& output)
{
    // output 持 argv 值（D36 UTF-8 契约）：显式 u8path（u8path 清扫）；base/回传
    // 串一律 generic_u8string 保持 UTF-8 字节域——原 generic_string 按 ACP 再编码，
    // 非 UTF-8 ACP 进程里 UTF-8 路径会被转 mojibake 且 create_file（libca UTF-8
    // 契约）二次解码放大。
    const std::filesystem::path destination = std::filesystem::u8path(output);
    const auto parent = destination.has_parent_path() ? destination.parent_path()
                                                       : std::filesystem::path(".");
    const auto base = destination.filename().generic_u8string();
    static std::atomic<ca::u64> sequence{0};
    const auto timestamp = static_cast<ca::u64>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (ca::u32 attempt = 0; attempt < 128; ++attempt) {
        const auto suffix = std::to_string(timestamp) + "." +
                            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        const auto candidate =
            parent / std::filesystem::u8path(base + ".morpher-stage." + suffix);
        if (FileUtil::create_file(candidate.generic_u8string()))
            return candidate.generic_u8string();
    }
    return {};
}

bool publishStagedOutput(std::string& stagedOutput, const CliOptions& options,
                         CliRunReport& report)
{
    if (FileUtil::move(stagedOutput, options.output, false)) {
        stagedOutput.clear();
        return true;
    }
    setFailure(report, CliFailureKind::Write, tr("mbase.cli.runner.err.publish_failed"));
    report.failed    = report.processed;
    report.processed = 0;
    std::fprintf(stderr, "Error: failed to publish %s\n", options.output.c_str());
    return false;
}

InputKind detectInputKind(const std::string& path)
{
    const auto view = ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(path.data()), path.size());
    if (mjbase::jar::JarUtil::isJarFile(view)) return InputKind::Jar;
    if (mjbase::jar::JarUtil::isClassFile(view)) return InputKind::Class;
    return InputKind::Unsupported;
}

// uri 形如 "org/example/Demo.class"，targetClass 形如 "org/example/Demo"。
// 子串匹配（去 .class 后缀更稳），空 target 表示不过滤（返回 true）。
bool classUriMatches(std::string_view uri, const std::string& targetClass)
{
    if (targetClass.empty()) return true;
    std::string_view       base   = uri;
    constexpr std::string_view suffix = ".class";
    if (base.size() > suffix.size() &&
        base.substr(base.size() - suffix.size()) == suffix) {
        base.remove_suffix(suffix.size());
    }
    return base.find(targetClass) != std::string_view::npos;
}

model::ModelResult<void> importInput(model::ResourceContainer& container, InputKind kind,
                                     const std::string& path)
{
    if (kind == InputKind::Jar) return model::ResourceHelper::importFromJar(container, path);
    return model::ResourceHelper::importFromClassFile(container, path);
}

model::ModelResult<void> exportOutput(model::ResourceContainer& container, InputKind kind,
                                      const std::string& path,
                                      const std::string& singleClassUri)
{
    if (kind == InputKind::Jar) return model::ResourceHelper::exportToJar(container, path);
    return model::ResourceHelper::exportToClassFile(container, path, singleClassUri);
}

}   // namespace

CliRunReport runCli(const CliOptions& options, const ClassProcessor& processor)
{
    CliRunReport report;

    if (options.input.empty() || options.output.empty()) {
        setFailure(report, CliFailureKind::InvalidArguments, tr("mbase.cli.runner.err.empty_path"));
        return report;   // 调用方负责校验必填项并报错
    }
    if (!validateOutputTarget(options, report)) return report;

    model::ResourceContainer container;

    const auto kind = detectInputKind(options.input);
    if (kind == InputKind::Unsupported) {
        setFailure(report, CliFailureKind::UnsupportedInput,
                   tr("mbase.cli.runner.err.unsupported_input"));
        std::fprintf(stderr,
                     "Error: unsupported input type for %s (expected .jar or .class)\n",
                     options.input.c_str());
        return report;   // 不支持的输入类型
    }
    auto imported = importInput(container, kind, options.input);
    if (imported.is_err()) {
        setFailure(report, CliFailureKind::Read,
                   std::string("import failed: ") +
                       model::to_string(std::move(imported).unwrap_err()));
        std::fprintf(stderr, "Error: failed to read %s: %s\n", options.input.c_str(),
                     report.failureMessage.c_str());
        return report;
    }

    // 遍历所有 ClassResource，对每个跑 processor
    auto&       resources = container.getResources();
    std::string singleClassUri;
    for (auto& [uri, resPtr] : resources) {
        model::IResource* raw = resPtr.get();
        if (raw->getType() != model::ResourceType::Class) continue;
        // -c 过滤：只处理匹配 targetClass 的类
        if (!classUriMatches(uri, options.targetClass)) continue;
        auto* cls = ca::core::tag_cast::cast<model::ClassResource>(raw);

        auto bytes  = cls->get();
        auto result = processor(uri, bytes);
        if (result.ok) {
            cls->set(std::move(bytes));
            report.processed++;
        }
        else {
            setFailure(report, CliFailureKind::Processing, result.message);
            std::fprintf(stderr, "  FAIL: %s - %s\n", std::string(uri).c_str(),
                         result.message.c_str());
            report.failed++;
        }
        singleClassUri = std::string(uri);
    }

    if (report.processed == 0 && report.failed == 0) {
        if (!options.targetClass.empty()) {
            setFailure(report, CliFailureKind::NoMatch, tr("mbase.cli.runner.err.no_class_match"));
            std::fprintf(stderr,
                         "Error: no class files matched --class '%s' in %s\n",
                         options.targetClass.c_str(),
                         options.input.c_str());
        }
        else {
            setFailure(report, CliFailureKind::NoClass, tr("mbase.cli.runner.err.no_class"));
            std::fprintf(stderr, "Error: no class files found in %s\n", options.input.c_str());
        }
        return report;
    }
    if (report.failed != 0) return report;

    std::string stagedOutput = createStagedOutput(options.output);
    if (stagedOutput.empty()) {
        setFailure(report, CliFailureKind::Write, tr("mbase.cli.runner.err.temp_failed"));
        report.failed    = report.processed;
        report.processed = 0;
        return report;
    }

    // 写出
    auto exported = exportOutput(container, kind, stagedOutput, singleClassUri);
    if (exported.is_err()) {
        FileUtil::remove(stagedOutput);
        std::fprintf(stderr, "Error: failed to write %s: %s\n", options.output.c_str(),
                     model::to_string(exported.unwrap_err()));
        setFailure(report, CliFailureKind::Write,
                   std::string("export failed: ") + model::to_string(exported.unwrap_err()));
        report.failed    = report.processed;   // 全部视为失败
        report.processed = 0;
        return report;
    }

    if (!verifyOutput(stagedOutput, kind)) {
        FileUtil::remove(stagedOutput);
        setFailure(report, CliFailureKind::Verification, tr("mbase.cli.runner.err.verify_failed"));
        report.failed    = report.processed;
        report.processed = 0;
        return report;
    }
    if (!publishStagedOutput(stagedOutput, options, report)) {
        FileUtil::remove(stagedOutput);
        return report;
    }

    std::printf("Done: %d classes processed, %d failed -> %s\n",
                report.processed,
                report.failed,
                options.output.c_str());
    return report;
}

}   // namespace mjbase::cli
