#pragma once

#include <libca/core/datatype.hpp>
#include <libca/str/utf16_string.hpp>
#include <libca/str/utf8_string.hpp>

#include <optional>
#include <vector>

namespace mjbase::hierarchy {
class ClassHierarchy;
}

namespace mjbase::jasm {

/// @brief Jasm 服务诊断级别。
enum class DiagnosticSeverity : ca::u8
{
    Error,
    Warning,
};

/// @brief 产生诊断的稳定服务阶段。
enum class DiagnosticStage : ca::u8
{
    Assemble,
    Disassemble,
    Validate,
    Resource,
};

/// @brief Jasm UTF-16 源位置的拥有型 DTO。
struct SourceLocation final
{
    ca::i32                             line   = -1;
    ca::i32                             column = -1;
    ca::i32                             length = -1;
    std::optional<ca::str::Utf16String> source;
};

/// @brief 不依赖 Jasm 内部错误类型的拥有型诊断 DTO。
struct Diagnostic final
{
    DiagnosticSeverity            severity;
    DiagnosticStage               stage;
    ca::str::Utf16String          message;
    std::optional<SourceLocation> location;
};

/// @brief 带可选值和有序诊断集合的服务结果。
template<typename T>
struct Result final
{
    std::optional<T>        value;
    std::vector<Diagnostic> diagnostics;

    bool hasErrors() const noexcept
    {
        for (const auto& diagnostic : diagnostics) {
            if (diagnostic.severity == DiagnosticSeverity::Error) return true;
        }
        return false;
    }

    bool isOk() const noexcept { return value.has_value() && !hasErrors(); }
};

/// @brief JVM Jasm 操作上下文；所有指针均不拥有。
struct JvmJasmContext final
{
    const mjbase::hierarchy::ClassHierarchy* hierarchy         = nullptr;
    ca::i32                                  classMajorVersion = 61;
    ca::i32                                  classMinorVersion = 0;
    ca::i32                                  maxStackHint      = 128;
};

/// @brief 汇编或验证成功后的 owning class 数据。
struct JvmJasmClass final
{
    ca::str::Utf8String internalName;
    std::vector<ca::u8> bytes;
};

/// @brief 反汇编成功后的 owning class 名称与 Jasm 文本。
struct JvmJasmDisassembly final
{
    ca::str::Utf8String  internalName;
    ca::str::Utf16String source;
};

/// @brief 反汇编成功后的 owning 方法标识与 Jasm 声明。
struct JvmJasmMethodDisassembly final
{
    ca::str::Utf8String  internalName;
    ca::str::Utf8String  name;
    ca::str::Utf8String  descriptor;
    ca::str::Utf16String source;
};

/// @brief 面向 mjbase 消费者的完整 JVM class Jasm 门面。
///
/// 首版仅处理完整 class，不提供 member/annotation overlay、Android、snippet、
/// UI 配置或后台任务。返回值不会暴露 Jasm AST 或 BLW model。
///
/// 移植自 mbase/jasm/jvm_jasm_service（语义照旧）；两处底座替换：
/// - 内部名读取换 jbc::core::readClass（原 jbytecode ClassReader）；
/// - 层次查询换 mjbase::hierarchy::ClassHierarchy（原 mbase ClassHierarchy）。
class JvmJasmService final
{
public:
    /// @brief 汇编完整 Jasm class，并返回 class internal name 与字节。
    Result<JvmJasmClass> assemble(ca::str::Utf16StringRef sourceName,
                                  ca::str::Utf16StringRef source,
                                  const JvmJasmContext&   context = {}) const;

    /// @brief 将完整 class 字节反汇编为规范化 Jasm。
    Result<JvmJasmDisassembly> disassemble(const std::vector<ca::u8>& classBytes) const;

    /// @brief 将精确选择的方法反汇编为规范化 `.method` Jasm 声明。
    Result<JvmJasmMethodDisassembly> disassembleMethod(
        const std::vector<ca::u8>& classBytes, ca::str::Utf8StringRef name,
        ca::str::Utf8StringRef descriptor) const;

    /// @brief 读取并分析完整 class；成功时返回拥有型 class 数据。
    Result<JvmJasmClass> validate(const std::vector<ca::u8>& classBytes,
                                  const JvmJasmContext&      context = {}) const;
};

}   // namespace mjbase::jasm
