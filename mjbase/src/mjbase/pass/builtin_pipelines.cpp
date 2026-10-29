// 内置命名流水线装配（组合根）：注册面在 spi/pass_registry.hpp，具体 pass
// 的管线组成落在本文件（pass 域）——spi 的 .cpp 不再反向包含 pass 头。
// 消费方（workspace/CLI 入口）经 registerBuiltinPipelines() 显式 bootstrap。

#include "mjbase/spi/pass_registry.hpp"

#include <memory>
#include <utility>

#include "mjbase/pass/char_arr_to_str_simplifier.hpp"
#include "mjbase/pass/class_member_sorter.hpp"
#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/junk_code_inserter.hpp"
#include "mjbase/pass/meta_info_remover.hpp"
#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/pass/stack_operation.hpp"
#include "mjbase/pass/strength_reducer.hpp"
#include "mjbase/pass/unused_check_cleaner.hpp"
#include "mjbase/pass/unused_member_remover.hpp"

namespace mjbase::spi {

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

    reg.registerPipeline("phase.stack-op-simplify",
                         "Fold redundant stack-operation windows to a fixpoint (opt-in)",
                         [] {
                             PipelineBuilder b;
                             b.addMethodPass(
                                 [] {
                                     return std::make_unique<mjbase::pass::StackOperationSimplifier>();
                                 },
                                 "StackOperationSimplifier");
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

    // 返回实际注册的流水线数，避免硬编码常量随新增 pipeline 漂移。
    return reg.listPipelines().size();
}

}   // namespace mjbase::spi
