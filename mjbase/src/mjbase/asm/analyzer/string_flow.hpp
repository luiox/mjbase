/// @file        string_flow.hpp
/// @brief       字符串/常量流分析：来源追踪、闭环解码管线识别与逃逸事实。
///
/// 下游消费方：解密内联类 pass（拿 Base64/XOR/char 偏移的闭环结果）、
/// folding 类 pass（只进不逃逸的常量）。分析在 DomainEngine 上跑一遍字符串
/// 流格：格值只记「值的来源令牌」（字面量 ldc 点、builder/数组分配点、
/// 已识别调用的产出点），控制流合并与槽位迁移交给引擎；令牌求值在分析后
/// 按到达帧回溯，不重新实现栈机。
///
/// 识别范围（全部 fail-closed，求不出就 complete=false，不猜）：
/// - 字符串来源：ldc String、StringBuilder/StringBuffer（含跨局部变量语句）、
///   String.valueOf/copyValueOf/concat、new String(char[]/byte[])；
/// - char[]/byte[] 闭环：常量填充 + 全数组 XOR/加减常量变换（循环体形态）、
///   String.toCharArray 与 String.getBytes 的数组来源；
/// - 编码管线：Base64 解码（标准/URL-safe，经 libca_crypto）→ new String(...)，
///   以及 char 数组异或/偏移的闭环结果。
///
/// 明确不做：跨方法/跨字段的字符串流、依赖运行时 charset 的字节解码
/// （new String(byte[]) 无 charset）、非常量下标的数组写入、依赖数组内容
/// 多次迭代的循环（同一 store 指令对每个元素只应用一次）、反射/资源读取。
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/frame.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::asm_analyzer {

/// @brief 字符串值/来源类别。
enum class StringOrigin : ca::u8 {
    Unknown = 0,     ///< 可求值但不在下列类别（如 new String(String) 拷贝）
    Literal,         ///< ldc String
    Concat,          ///< StringBuilder/StringBuffer 管线
    ValueOf,         ///< String.valueOf / copyValueOf
    CharArray,       ///< char[] 闭环构造
    ByteArray,       ///< byte[] 闭环构造（非 Base64 路径）
    Base64Decode,    ///< Base64 解码闭环
    Xor,             ///< char/byte 数组逐元素异或常量
    CharShift,       ///< char/byte 数组逐元素加/减常量
};

/// @brief 一条字符串来源事实。
///
/// `value` 是 classfile 的 modified UTF-8 字节（与 CP Utf8 载荷同形，可直接
/// 交给 ConstantPool::addUtf8），借用结果内部驻留池，随结果存活。
struct StringFlowSource
{
    jbc::ir::InsnId site;        ///< 来源指令：ldc / new String 的构造调用 /
                                 ///< builder.toString / valueOf 调用
    StringOrigin origin = StringOrigin::Unknown;
    std::string_view value;      ///< 闭环求值结果；不可求值或空串时为空视图
    bool complete = false;       ///< 全部输入可求值（value 有意义）
    bool escaped = false;        ///< 值到达未识别的消费点（返回值/字段/数组存储/
                                 ///< 未知调用等）；false 表示只进不逃逸
};

/// @brief 一条闭环编码管线事实。
///
/// 只在求值完成时产出。`input` 是编码/变换的输入：Base64 为编码串本身；
/// char/byte 数组变换为变换前的完整内容（仅全数组循环变换记录，逐元素
/// 常量写入不记）。`output` 与来源事实的 value 相同（modified UTF-8）。
struct StringFlowPipeline
{
    jbc::ir::InsnId site;        ///< 管线产出点（new String / toString / valueOf）
    StringOrigin kind = StringOrigin::Unknown;   ///< Base64Decode / Xor / CharShift
    std::string_view input;      ///< 可求值时记录；字节输入或逐元素变换为空
    std::string_view output;     ///< 解码/变换结果
};

/// @brief 字符串流分析结果（自持，move-only）。
///
/// 来源与管线视图借用内部 KeyPool；结果移动后视图仍有效（池地址不变）。
/// 数据流失败（畸形方法体、无不动点）时 summary().ok=false，事实列表为空。
class StringFlowResult
{
public:
    StringFlowResult();
    ~StringFlowResult();

    StringFlowResult(StringFlowResult&& other) noexcept;
    StringFlowResult& operator=(StringFlowResult&& other) noexcept;
    StringFlowResult(const StringFlowResult&) = delete;
    StringFlowResult& operator=(const StringFlowResult&) = delete;

    const jbc::analysis::FrameSummary& summary() const noexcept;
    bool ok() const noexcept;

    /// @brief 全部来源事实（按指令顺序）。
    const std::vector<StringFlowSource>& sources() const noexcept;
    /// @brief 全部闭环编码管线事实（按指令顺序）。
    const std::vector<StringFlowPipeline>& pipelines() const noexcept;

    /// @brief 按来源点查询；未命中返回 nullptr。
    const StringFlowSource* sourceAt(jbc::ir::InsnId site) const noexcept;
    /// @brief 按产出点查询管线；未命中返回 nullptr。
    const StringFlowPipeline* pipelineAt(jbc::ir::InsnId site) const noexcept;

private:
    struct Impl;

    explicit StringFlowResult(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend StringFlowResult analyzeStringFlow(const jbc::core::ConstantPool& cp,
                                              const jbc::ir::CodeUnit& unit);
};

/// @brief 对一个方法体做字符串/常量流分析。
///
/// @param cp   方法所在类的常量池（借用；须活过本调用）。
/// @param unit 方法体（借用；须活过本调用）。
StringFlowResult analyzeStringFlow(const jbc::core::ConstantPool& cp,
                                   const jbc::ir::CodeUnit& unit);

}   // namespace mjbase::asm_analyzer
