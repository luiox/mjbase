#include "mjbase/pass/line_number_remover.hpp"

#include <cstddef>
#include <string_view>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"

namespace mjbase::pass {

namespace {

constexpr std::string_view kCode            = "Code";
constexpr std::string_view kLineNumberTable = "LineNumberTable";

}   // namespace

mjbase::spi::PreservedAnalyses LineNumberRemover::run(
    jbc::ir::ClassSession& session, mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    jbc::core::ClassFile& cf      = session.backing();
    bool                  changed = false;

    for (jbc::core::MemberInfo& m : cf.methods) {
        jbc::core::AttrSlot* codeSlot = nullptr;
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, kCode)) {
                codeSlot = &slot;
                break;
            }
        }
        if (codeSlot == nullptr) continue;

        auto decoded = cf.decodeCode(m);
        if (decoded.is_err()) continue;
        jbc::core::CodeAttr* code          = decoded.unwrap();
        bool                 methodChanged = false;
        for (std::size_t i = code->attrs.size(); i-- > 0;) {
            if (cf.cp.utf8Equals(code->attrs[i].nameCp, kLineNumberTable)) {
                code->attrs.erase(code->attrs.begin() + static_cast<std::ptrdiff_t>(i));
                methodChanged = true;
            }
        }
        if (methodChanged) {
            (void)codeSlot->mutate();
            changed = true;
        }
    }

    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
