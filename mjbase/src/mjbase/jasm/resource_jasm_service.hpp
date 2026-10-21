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
/// 层次图不设跨调用缓存：每次 assemble 按容器当前内容即时重建，候选工作区
/// 验证也用候选容器即时重建的层次图——「成功替换后不得使用陈旧层次」的契约
/// 由构造保证，事务语义不变。
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
