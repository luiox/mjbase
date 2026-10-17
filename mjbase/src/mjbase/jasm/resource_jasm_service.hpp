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
/// 与 mbase 的差异（本批有意重设计）：mbase 版经 GlobalAnalysisManager 挂
/// ClassHierarchyAnalysis 缓存，并在成功替换后 clear 缓存；mjbase 无 spi 域，
/// 层次图改为每次 assemble 按容器当前内容即时重建——服务不持任何跨调用缓存，
/// 「成功替换后不得使用陈旧层次」的契约由构造保证。候选工作区验证仍用候选
/// 容器即时重建的层次图，事务语义不变。
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
