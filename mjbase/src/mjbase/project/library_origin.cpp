#include "mjbase/project/library_origin.hpp"

namespace mjbase::project {
namespace {

/// @brief 段边界前缀命中：字节前缀全等即命中。表条目已由编译期校验保证
///        以 '/' 收尾——'/' 落在段边界上，首段超长的名字（javaxx/Y 对
///        java/）在该字节处失配，等长更短的名字（jav/X）越界失配。
constexpr bool hasSegmentPrefix(std::string_view name, std::string_view root) noexcept
{
    return name.size() >= root.size()
        && name.compare(0, root.size(), root) == 0;
}

}   // namespace

LibraryOrigin classifyLibraryOrigin(const ca::str::Utf8StringRef& internalName) noexcept
{
    // 零拷贝互操作：内部名是字节透明的二进制名，按字节比较不做编码假设。
    const std::string_view name(internalName);

    for (const std::string_view root : kOfficialPackageRoots) {
        if (hasSegmentPrefix(name, root)) return LibraryOrigin::Jdk;
    }
    for (const std::string_view root : kThirdPartyLibraryRoots) {
        if (hasSegmentPrefix(name, root)) return LibraryOrigin::Known;
    }
    return LibraryOrigin::Unknown;
}

std::string_view libraryOriginName(LibraryOrigin origin) noexcept
{
    switch (origin) {
    case LibraryOrigin::Jdk: return "jdk";
    case LibraryOrigin::Known: return "known";
    case LibraryOrigin::Unknown: return "unknown";
    }
    return "unknown";
}

}   // namespace mjbase::project
