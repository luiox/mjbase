#include "mjbase/project/mask_regex_engine.hpp"

#include <cstring>
#include <regex>
#include <string>

namespace mjbase::project {

struct MaskRegex::Impl
{
    std::regex pattern;
};

ca::Result<MaskRegex, MaskRegexError> MaskRegex::compile(std::string_view pattern, bool icase)
{
    if (pattern.size() > kMaximumMaskPatternBytes) {
        MaskRegexError error;
        error.message = ca::str::Utf8String::from_data_unchecked(
            reinterpret_cast<const ca::u8*>("pattern exceeds maximum regex length"),
            sizeof("pattern exceeds maximum regex length") - 1);
        return ca::Err(std::move(error));
    }
    auto impl = std::make_unique<Impl>();
    try {
        // ECMAScript 方言与 mjt MCP 查询面的既有 std::regex 口径一致；
        // icase 显式传参，其余旗标不隐式添加（optimize 等留给引擎默认）。
        const auto flags = icase ? (std::regex::ECMAScript | std::regex::icase)
                                 : std::regex::ECMAScript;
        impl->pattern = std::regex(pattern.data(), pattern.data() + pattern.size(), flags);
    }
    catch (const std::regex_error& error) {
        MaskRegexError failure;
        failure.message = ca::str::Utf8String::from_data_unchecked(
            reinterpret_cast<const ca::u8*>(error.what()), std::strlen(error.what()));
        return ca::Err(std::move(failure));
    }
    return ca::Ok(MaskRegex(std::move(impl)));
}

MaskRegex::MaskRegex(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

MaskRegex::MaskRegex(MaskRegex&&) noexcept = default;

MaskRegex& MaskRegex::operator=(MaskRegex&&) noexcept = default;

MaskRegex::~MaskRegex() = default;

bool MaskRegex::matches(std::string_view text) const
{
    if (impl_ == nullptr) return false;
    // 空视图的 data() 可能为空指针：regex 迭代器要求合法 range，落回静态
    // 空串保证语义精确（模式声明可匹配空串时按原生结果返回）。
    static const std::string kEmpty;
    const char* begin = text.data() != nullptr ? text.data() : kEmpty.data();
    try {
        return std::regex_search(begin, begin + text.size(), impl_->pattern);
    }
    catch (const std::regex_error&) {
        // 求值期异常（如 stack resource exhaustion 触发的实现定义错误）按
        // 不命中处理：mask 侧 fail-open 由 default_action 与 suspect 字节
        // 扫描兜底，不会因引擎异常把整条出口打成失败。
        return false;
    }
}

}   // namespace mjbase::project
