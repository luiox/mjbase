#include "mjbase/spi/class_file_sanitize_gate.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_file_sanitize.hpp"

namespace mjbase::spi {

PreservedAnalyses ClassFileSanitizeGate::run(mjbase::model::ResourceContainer& container,
                                             GlobalAnalysisManager&            am)
{
    (void)am;
    auto&     resources = container.getResources();
    ca::usize rejected  = 0;

    for (auto it = resources.begin(); it != resources.end();) {
        if (!it->second || !ca::core::tag_cast::isa<mjbase::model::ClassResource>(it->second.get())) {
            ++it;
            continue;
        }
        auto*       cls   = ca::core::tag_cast::cast<mjbase::model::ClassResource>(it->second.get());
        const auto& bytes = cls->get();
        // 空 bytes 资源沿既有契约放行（GlobalToClassPassAdaptor 对其原样跳过）。
        if (bytes.empty()) {
            ++it;
            continue;
        }

        auto issues = sanitizeClassFile(bytes.data(), static_cast<ca::i32>(bytes.size()));
        if (issues.empty()) {
            ++it;
            continue;
        }

        const std::string detail = joinSanitizeIssues(issues).to_std_string();
        if (!options_.perClassFaultIsolation) {
            // 默认档位 fail-fast：任一阶段失败即上抛，作业级原子。
            throw std::runtime_error(std::string("ClassFileSanitizeGate: failed at '") +
                                     std::string(it->first) + "': " + detail);
        }
        if (options_.faultReport) {
            options_.faultReport->add({/*scope*/ ca::str::Utf8String::from_cstr("class"),
                                       ca::str::Utf8String::from_data_unchecked(
                                           reinterpret_cast<const ca::u8*>(it->first.data()),
                                           it->first.size()),
                                       /*reason*/ ca::str::Utf8String::from_cstr("sanitize_error"),
                                       ca::str::Utf8String::from_data_unchecked(
                                           reinterpret_cast<const ca::u8*>(detail.data()),
                                           detail.size())});
        }
        it = resources.erase(it);   // 剔除：下游 pass 与导出均不再见该类。
        ++rejected;
    }

    return rejected == 0 ? PreservedAnalyses::all() : PreservedAnalyses::none();
}

}   // namespace mjbase::spi
