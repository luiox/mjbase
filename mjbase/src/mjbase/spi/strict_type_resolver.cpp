#include "mjbase/spi/strict_type_resolver.hpp"

namespace mjbase::spi {

ca::core::Result<jbc::analysis::VType, jbc::analysis::TypeMergeError>
    StrictTypeResolverAdapter::merge(jbc::analysis::VType a, jbc::analysis::VType b)
{
    namespace ja = jbc::analysis;
    using ca::core::Err;
    using ca::core::Ok;

    // 结构性归并（两格同规，§10.2.3）：Null 作万能引用底；同类精确引用自并。
    // 计算域理论上只把具体引用对送进来，这里按防御式口径兜住缺名形态。
    if (a.k == ja::VKind::Null && b.k == ja::VKind::Ref) return Ok(b);
    if (b.k == ja::VKind::Null && a.k == ja::VKind::Ref) return Ok(a);
    if (a.k != ja::VKind::Ref || b.k != ja::VKind::Ref) {
        return Err(ja::TypeMergeError::Unresolvable);
    }
    if (a.name.empty() || b.name.empty()) {
        return Err(ja::TypeMergeError::Unresolvable);
    }
    if (a.name == b.name) return Ok(a);

    const std::string_view super = resolver_->getCommonSuperClass(a.name, b.name);
    if (super.empty()) {
        return Err(ja::TypeMergeError::NoCommonParent);
    }
    return Ok(ja::VType::ref(super));
}

}   // namespace mjbase::spi
