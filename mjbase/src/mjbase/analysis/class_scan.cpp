#include "mjbase/analysis/class_scan.hpp"

#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"

namespace mjbase::analysis {

std::vector<ScannedClass> scanClassResources(const mjbase::model::ResourceContainer& container)
{
    std::vector<ScannedClass> classes;
    container.visitResources([&classes](std::string_view, const mjbase::model::IResource& res) {
        if (res.getType() != mjbase::model::ResourceType::Class) return;
        const auto* cls = ca::core::tag_cast::cast<const mjbase::model::ClassResource>(&res);
        if (cls == nullptr || cls->get().empty()) return;
        auto session = jbc::ir::makeSessionFromBytes(
            jbc::core::ByteView{cls->get().data(), cls->get().size()});
        if (session.is_err()) return;   // 畸形类不产出事实（fail-open）
        ScannedClass scanned;
        scanned.session = session.unwrap();
        scanned.name    = scanned.session->unit().symbols().str(scanned.session->unit().thisClass);
        classes.push_back(std::move(scanned));
    });
    return classes;
}

}   // namespace mjbase::analysis
