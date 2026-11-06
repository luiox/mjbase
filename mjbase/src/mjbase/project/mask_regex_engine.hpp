/// @file        mask_regex_engine.hpp
/// @brief       mask/suspect 正则引擎适配面：std::regex 的单点封装。
///
/// 引擎选型：先用 std::regex，性能出问题再换线性时间引擎（RE2 类）。
/// 本选型的前提是三条纪律，全部收口在本文件（换引擎只动这里，调用方零改动）：
///
///   1. **引擎类型不出适配面**：std::regex 及其相关类型只出现在本头实现
///      （pimpl）与 mask_regex_engine.cpp 内；接口只进 std::string_view、
///      出 bool，消费方（mask_suspect / mask_evaluator）不感知引擎。
///   2. **模式一律字面量、一次性编译**：本面只接受完整模式串并在
///      compile() 时编译，禁止运行期拼接模式；mask_rules 与 suspect 内建
///      模式的书写纪律是限定匹配长度上界、避免嵌套量词与重叠量词，控制
///      回溯面（长度门的兜底见下条）。
///   3. **输入长度门**：超过 kMaskMaxEvaluatedBytes 的文本不得送进本面
///      （调用方负责检查）——对抗性超长输入直接走 default_action，不进
///      正则，灾难回溯（ReDoS）防线的最后一环在各调用点的门检查。
#pragma once

#include <memory>
#include <string_view>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::project {

/// @brief 进正则引擎的文本字节长度上限；超过则调用方必须走免正则路径
///        （default_action 兜底）。256 字节覆盖正常字符串常量与注入形态
///        短语，超长文本本身即是 suspect 信号（见 mask_suspect）。
inline constexpr ca::usize kMaskMaxEvaluatedBytes = 256;

/// @brief 单条正则模式的字节长度上限（compile 拒收更长模式）。模式是人写
///        的静态字面量，1024 字节已远超合理形态规则；上限同时防住超大
///        模式串的编译期消耗。
inline constexpr ca::usize kMaximumMaskPatternBytes = 1024;

/// @brief 正则编译诊断（模式上下文由调用方附带，这里只带引擎原因）。
struct MaskRegexError
{
    ca::str::Utf8String message;
};

/// @brief 已编译正则（move-only；引擎句柄驻留 pimpl）。
class MaskRegex final
{
public:
    /// @brief 编译一条模式；语法错误或超长 → `Err`。ECMAScript 方言；
    ///        大小写不敏感由调用方显式传 icase（本面不加隐式旗标），模式
    ///        一律字面量。
    static ca::Result<MaskRegex, MaskRegexError> compile(std::string_view pattern,
                                                         bool             icase = false);

    MaskRegex(MaskRegex&&) noexcept;
    MaskRegex& operator=(MaskRegex&&) noexcept;
    MaskRegex(const MaskRegex&)            = delete;
    MaskRegex& operator=(const MaskRegex&) = delete;
    ~MaskRegex();

    /// @brief 子串搜索语义（等价 regex_search）：文本任一位置命中即真。
    ///        空文本按引擎原生语义求值（模式可声明为可匹配空串）。
    bool matches(std::string_view text) const;

private:
    struct Impl;
    explicit MaskRegex(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}   // namespace mjbase::project
