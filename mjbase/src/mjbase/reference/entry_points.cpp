#include "mjbase/reference/entry_points.hpp"

#include "mjbase/hierarchy/summary.hpp"

namespace mjbase::reference {

bool isJavaMainMethod(const MethodDeclaration& decl) noexcept
{
    using mjbase::hierarchy::kAccPublic;
    using mjbase::hierarchy::kAccStatic;

    constexpr ca::u32 kRequiredAccess = kAccPublic | kAccStatic;
    return decl.method.name == "main" && decl.method.desc == "([Ljava/lang/String;)V" &&
           (decl.access & kRequiredAccess) == kRequiredAccess;
}

}   // namespace mjbase::reference
