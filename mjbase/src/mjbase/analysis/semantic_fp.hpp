/// @file        semantic_fp.hpp
/// @brief       方法语义指纹（semantic_fp）v1：方法体语义面的规范化哈希。
///
/// 依据 doc_ai/semantic-fp-spec.md（#132）：给每个方法的内容算一个指纹，
/// 方法体语义没变指纹不变，语义变了指纹必变。三步——收集语义面、归一化、
/// 序列化哈希：
///   - 语义面：方法头（原始访问标志 u16、描述符；方法名/类名/Exceptions 不进）
///     + Code 的 max_stack/max_locals/异常表 + 从入口与全部异常处理器入口
///     可达的指令流（可达集合内按原 pc 升序 = 规范序，保序压缩）；行号、
///     LVT、StackMapTable、注解、SourceFile 一律不读。
///   - 归一化：常量加载只有 int 族跨编码折叠（iconst/bipush/sipush/ldc int
///     归一为「int 值」token）；其余指令逐 opcode 精确记录（GOTO 与 GOTO_W
///     不同 token——控制流形态变化必须翻指纹）。分支/switch 目标与异常表
///     范围记存活指令的规范序号（pc 会漂移，序号不会）；类型/字段/方法引用
///     全部解引用为名字（不记 CP 编号）；ldc 全载荷解到语义值（Class/
///     MethodType/MethodHandle/float/double/long/condy）；invokedynamic 记
///     名字、描述符、BSM 句柄与全部静态参数；wide 前缀不进指纹（编码形态
///     而非语义）。
///   - 序列化：类型化 token 流，变长整数 ULEB128、有符号 zigzag、定长字段
///     小端、字符串字节长度前缀，流头一个算法版本字节（kSemanticFpVersion）。
///     哈希 SHA-256 取 32 字节，文本形态 64 位小写 hex。
///
/// 纯函数面：输入 jbc 懒 DOM（ClassFile + 成员条目，analysis 层既有消费
/// 形状），输出指纹值；除懒解码驻留缓存外不修改 DOM、无全局状态。同一份
/// 方法字节任何机器任何时间算出同一结果。算法升级（加等价族、换遍历序）
/// 必须把 kSemanticFpVersion 加一，旧指纹靠版本位自然作废。
#pragma once

#include <array>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "jbc/core/class_file.hpp"

namespace mjbase::analysis {

/// @brief 算法版本字节（v1）。归一化规则/遍历序变更时必须加一。
inline constexpr ca::u8 kSemanticFpVersion = 0x01;

/// @brief 指纹哈希字节数（SHA-256 定长）。
inline constexpr ca::usize kSemanticFpHashBytes = 32;

/// @brief 指纹计算失败原因（结构畸形即拒绝，不做静默近似）。
enum class SemanticFpError : ca::u8 {
    Ok = 0,
    BadCodeAttr,      ///< Code 属性解码失败
    BadInsn,          ///< 指令流解码失败
    BadBranchTarget,  ///< 分支/switch 目标越过 code 边界或未落在指令边界
    BadExcEntry,      ///< 异常表处理器 pc 未落在存活指令边界
    BadCpOperand,     ///< CP 操作数索引无效或 tag 与指令形状不符
    BadBootstrap,     ///< BootstrapMethods 属性缺失/解码失败/bsm 序号越界
    NestedDynamic,    ///< bootstrap 静态参数再嵌 condy/indy（v1 不递归，报错）
};

/// @brief 错误码的稳定文本（诊断输出用）。
const char* to_string(SemanticFpError e) noexcept;

/// @brief 指纹值：32 字节哈希 + 64 位小写 hex 文本（ASCII）。
struct SemanticFp final
{
    std::array<ca::u8, kSemanticFpHashBytes> bytes{};
    ca::str::Utf8String hex;

    /// @brief 拷贝面：Utf8String 不可拷贝，hex 走 clone（字节同则文本同）。
    SemanticFp() = default;
    SemanticFp(const SemanticFp& other)
        : bytes(other.bytes)
        , hex(other.hex.clone())
    {}
    SemanticFp& operator=(const SemanticFp& other)
    {
        if (this != &other) {
            bytes = other.bytes;
            hex   = other.hex.clone();
        }
        return *this;
    }
    SemanticFp(SemanticFp&& other) noexcept            = default;
    SemanticFp& operator=(SemanticFp&& other) noexcept = default;

    /// @brief 字节相等判定（hex 是字节的小写文本形态，随字节相等）。
    friend bool operator==(const SemanticFp& a, const SemanticFp& b) noexcept
    {
        return a.bytes == b.bytes;
    }
    friend bool operator!=(const SemanticFp& a, const SemanticFp& b) noexcept
    {
        return !(a == b);
    }
    /// @brief 字节序全序（去重统计等容器用途）。
    friend bool operator<(const SemanticFp& a, const SemanticFp& b) noexcept
    {
        return a.bytes < b.bytes;
    }
};

/// @brief 计算方法语义指纹（v1）。
///
/// @p cf 提供 CP 与 BootstrapMethods 上下文，@p m 必须属于 @p cf。参数取
/// 可变引用只是懒 DOM 的解码驻留语义（decodeCode/decodeAttr 首次访问驻留
/// 缓存），算法本身只读。native/abstract（无 Code）只盖方法头，指纹恒定。
/// 结构畸形（目标越界、处理器不在指令边界、BSM 参数嵌套动态常量等）返回
/// Err，不做静默近似——指纹该变不变会拿旧结果配新代码。
ca::core::Result<SemanticFp, SemanticFpError>
semanticFp(jbc::core::ClassFile& cf, jbc::core::MemberInfo& m);

}   // namespace mjbase::analysis
