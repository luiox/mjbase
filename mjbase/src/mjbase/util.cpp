/// @file        util.cpp
/// @brief       内部名/描述符工具实现。
#include "mjbase/util.hpp"

namespace mjbase::util {

std::string_view packageNameOf(std::string_view internalName) noexcept
{
    const auto pos = internalName.rfind('/');
    return pos == std::string_view::npos ? std::string_view{}
                                         : internalName.substr(0, pos);
}

std::string_view simpleNameOf(std::string_view internalName) noexcept
{
    const auto pos = internalName.rfind('/');
    return pos == std::string_view::npos ? internalName
                                         : internalName.substr(pos + 1);
}

std::string_view enclosingClassOf(std::string_view internalName) noexcept
{
    const auto slash = internalName.rfind('/');
    const auto dollar = internalName.rfind('$');
    if (dollar == std::string_view::npos ||
        (slash != std::string_view::npos && dollar < slash)) {
        return {};   // 顶层类，或 '$' 落在包名里（畸形输入按无外围类处理）
    }
    return internalName.substr(0, dollar);
}

std::string_view descriptorToInternalName(std::string_view descriptor) noexcept
{
    if (descriptor.size() < 3 || descriptor.front() != 'L' ||
        descriptor.back() != ';') {
        return {};
    }
    return descriptor.substr(1, descriptor.size() - 2);
}

bool isLibraryClass(std::string_view internalName) noexcept
{
    const auto starts = [&](std::string_view p) {
        return internalName.size() >= p.size() &&
               internalName.substr(0, p.size()) == p;
    };
    return starts("java/") || starts("javax/") || starts("jakarta/") ||
           starts("sun/") || starts("com/sun/") || starts("jdk/") ||
           starts("org/w3c/") || starts("org/xml/");
}

}   // namespace mjbase::util
