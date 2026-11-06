#pragma once

#include "jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"

#include <string>

namespace mjbase::jasm {

/// @brief 将完整 class Jasm 操作事务式接入 ResourceContainer。
///
/// 汇编仅在名称校验和候选工作区验证全部成功后替换 ClassResource，失败不会修改
/// 资源内容。
///
/// 与 mbase 版的差异：mbase 版经 GlobalAnalysisManager 挂 ClassHierarchyAnalysis
/// 缓存，并在成功替换后 clear 缓存；本服务不经分析缓存，层次图每次 assemble
/// 按容器当前内容即时重建——不持任何跨调用缓存，「成功替换后不得使用陈旧层次」
/// 的契约由构造保证。候选工作区验证同样用候选容器即时重建的层次图。
class ResourceJasmService final
{
public:
    explicit ResourceJasmService(model::ResourceContainer& container)
        : container_(&container)
    {}

    /// @brief 按 URI 反汇编 ClassResource。
    Result<JvmJasmDisassembly> disassemble(const std::string& uri) const;

    /// @brief 按 URI 事务式汇编并替换已有 ClassResource。
    Result<JvmJasmClass> assemble(const std::string& uri, ca::str::Utf16StringRef sourceName,
                                  ca::str::Utf16StringRef source, JvmJasmContext context = {});

private:
    model::ResourceContainer* container_;
    JvmJasmService            jasm_;
};

}   // namespace mjbase::jasm
