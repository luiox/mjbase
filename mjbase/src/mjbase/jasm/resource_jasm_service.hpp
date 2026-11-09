#pragma once

#include "jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"

#include <string_view>

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
    Result<JvmJasmDisassembly> disassemble(std::string_view uri) const;

    /// @brief 按 URI 事务式汇编并替换已有 ClassResource。
    ///
    /// frameMode（issue #190）：Regenerate（默认）走数据流帧重生成；PreserveOriginal
    /// 保留原 class 的 StackMapTable 字节——汇编面跳过帧生成（产物临时以 v49
    /// 装配，替换前回填原类版本），候选字节与原类逐方法比对 code[] 字节：
    /// 完全一致才原样挂回原 StackMapTable（RawAttr 原样副本）；指令面有任何
    /// 改动的方法按 fail-closed 拒绝并逐方法申报边界（错误诊断点名方法），不落
    /// 改动。边界面：jhasm 数据流分析（engine.compute）仍运行——数据流本身
    /// 失败的类（栈深不一致、无不动点等）两档同败；本档只豁免帧序列化
    /// （conflict/unnamed slot 等坏帧面）。
    Result<JvmJasmClass> assemble(std::string_view uri, ca::str::Utf16StringRef sourceName,
                                  ca::str::Utf16StringRef source, JvmJasmContext context = {},
                                  FrameMode frameMode = FrameMode::Regenerate);

private:
    model::ResourceContainer* container_;
    JvmJasmService            jasm_;
};

}   // namespace mjbase::jasm
