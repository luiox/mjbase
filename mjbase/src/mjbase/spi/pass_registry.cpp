#include "mjbase/spi/pass_registry.hpp"

#include <algorithm>
#include <utility>

#include "mjbase/pass/char_arr_to_str_simplifier.hpp"
#include "mjbase/pass/class_member_sorter.hpp"
#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/junk_code_inserter.hpp"
#include "mjbase/pass/meta_info_remover.hpp"
#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/pass/strength_reducer.hpp"
#include "mjbase/pass/unused_check_cleaner.hpp"
#include "mjbase/pass/unused_member_remover.hpp"

namespace mjbase::spi {

const char* to_string(PassStability stability) noexcept
{
    switch (stability) {
    case PassStability::Default: return "Default";
    case PassStability::OptIn: return "OptIn";
    case PassStability::Experimental: return "Experimental";
    case PassStability::Stub: return "Stub";
    }
    return "?";
}

// ── PassRegistry ─────────────────────────────────────────────

PassRegistry& PassRegistry::instance()
{
    static PassRegistry registry;
    return registry;
}

PassRegistry& PassRegistry::registerPipeline(const std::string& name,
                                             const std::string& description,
                                             PipelineFactory    factory, PassStability stability)
{
    Entry entry;
    entry.meta   = PassPipelineInfo{name, description, stability};
    entry.factory = std::move(factory);
    entries_[name] = std::move(entry);
    return *this;
}

bool PassRegistry::hasPipeline(const std::string& name) const
{
    return entries_.find(name) != entries_.end();
}

PipelineBuilder PassRegistry::getPipeline(const std::string& name) const
{
    const auto it = entries_.find(name);
    if (it == entries_.end()) return PipelineBuilder{};
    return it->second.factory();
}

const PassPipelineInfo* PassRegistry::pipelineInfo(const std::string& name) const
{
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second.meta;
}

std::vector<PassPipelineInfo> PassRegistry::listPipelines() const
{
    std::vector<PassPipelineInfo> out;
    out.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) out.push_back(entry.meta);
    std::sort(out.begin(), out.end(),
              [](const PassPipelineInfo& lhs, const PassPipelineInfo& rhs) {
                  return lhs.name < rhs.name;
              });
    return out;
}

// ── 内置流水线（mjbase 自有 pass 集）─────────────────────────

namespace {

/// 默认链：确定性方法体清理（Nop 消除 + 冗余装载消除 + 冗余空检查清除）。
/// 只收行为等价的清理件：ConstantFolder/StrengthReducer 按 pass 域标签是
/// opt-in 挂点，JunkCodeInserter 是加噪音件——都不进默认链。
PipelineBuilder buildDefaultOptimizePipeline()
{
    PipelineBuilder builder;
    builder.addMethodPass([] { return std::make_unique<mjbase::pass::NopRemover>(); },
                          "NopRemover")
        .addMethodPass([] { return std::make_unique<mjbase::pass::RedundantLoadValEliminator>(); },
                       "RedundantLoadValEliminator")
        .addMethodPass([] { return std::make_unique<mjbase::pass::UnusedCheckCleaner>(); },
                       "UnusedCheckCleaner");
    return builder;
}

}   // namespace

ca::usize registerBuiltinPipelines()
{
    auto& reg = PassRegistry::instance();
    if (reg.hasPipeline("default-optimize")) return 0;   // 幂等，避免重复注册

    reg.registerPipeline("default-optimize",
                         "Deterministic method-body cleanup (nop removal, redundant "
                         "load/store elimination, redundant check removal)",
                         [] { return buildDefaultOptimizePipeline(); }, PassStability::Default);

    reg.registerPipeline("phase.constant-fold",
                         "Fold constant operand-stack chains (opt-in)",
                         [] {
                             PipelineBuilder b;
                             b.addMethodPass(
                                 [] { return std::make_unique<mjbase::pass::ConstantFolder>(); },
                                 "ConstantFolder");
                             return b;
                         },
                         PassStability::OptIn);

    reg.registerPipeline("phase.strength-reduce",
                         "Rewrite powers-of-two multiply/divide into shifts (opt-in)",
                         [] {
                             PipelineBuilder b;
                             b.addMethodPass(
                                 [] { return std::make_unique<mjbase::pass::StrengthReducer>(); },
                                 "StrengthReducer");
                             return b;
                         },
                         PassStability::OptIn);

    reg.registerPipeline("phase.chararr-to-string",
                         "Rewrite char-array String construction into constants (opt-in)",
                         [] {
                             PipelineBuilder b;
                             b.addMethodPass(
                                 [] {
                                     return std::make_unique<mjbase::pass::CharArrToStrSimplifier>();
                                 },
                                 "CharArrToStrSimplifier");
                             return b;
                         },
                         PassStability::OptIn);

    reg.registerPipeline("phase.meta-info-remove",
                         "Remove debug/framework metadata (source, signatures, annotations)",
                         [] {
                             PipelineBuilder b;
                             b.addClassPass(
                                 [] { return std::make_unique<mjbase::pass::MetaInfoRemover>(); },
                                 "MetaInfoRemover");
                             return b;
                         },
                         PassStability::OptIn);

    reg.registerPipeline("phase.class-member-sort",
                         "Sort class members into a deterministic order (opt-in)",
                         [] {
                             PipelineBuilder b;
                             b.addClassPass(
                                 [] { return std::make_unique<mjbase::pass::ClassMemberSorter>(); },
                                 "ClassMemberSorter");
                             return b;
                         },
                         PassStability::OptIn);

    reg.registerPipeline("phase.junk-code-insert",
                         "Insert deterministic dead branch blocks (noise injection, opt-in)",
                         [] {
                             PipelineBuilder b;
                             b.addMethodPass(
                                 [] { return std::make_unique<mjbase::pass::JunkCodeInserter>(); },
                                 "JunkCodeInserter");
                             return b;
                         },
                         PassStability::OptIn);

    reg.registerPipeline("phase.unused-member-remove",
                         "Remove unreachable classes and unused private members (container-wide)",
                         [] {
                             PipelineBuilder b;
                             b.addGlobalPass(
                                 [] { return std::make_unique<mjbase::pass::EmptyClassRemover>(); },
                                 "EmptyClassRemover")
                                 .addGlobalPass(
                                     [] {
                                         return std::make_unique<mjbase::pass::UnusedClassRemover>();
                                     },
                                     "UnusedClassRemover")
                                 .addGlobalPass(
                                     [] {
                                         return std::make_unique<mjbase::pass::UnusedFieldRemover>();
                                     },
                                     "UnusedFieldRemover")
                                 .addGlobalPass(
                                     [] {
                                         return std::make_unique<mjbase::pass::GlobalUnusedMethodRemover>();
                                     },
                                     "GlobalUnusedMethodRemover");
                             return b;
                         },
                         PassStability::OptIn);

    // 返回实际注册的流水线数，避免硬编码常量随新增 pipeline 漂移（mbase 纪律照迁）。
    return reg.listPipelines().size();
}

}   // namespace mjbase::spi
