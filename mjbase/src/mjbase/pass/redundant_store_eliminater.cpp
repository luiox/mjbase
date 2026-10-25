#include "mjbase/pass/redundant_store_eliminater.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::DefId;

}   // namespace

mjbase::spi::PreservedAnalyses RedundantStoreEliminater::run(mjbase::spi::MethodScope&       scope,
                                                             mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit& cu = scope.code;
    if (cu.dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();

    // 零使用是负向结论：数据流失败或到达集合溢出时整件跳过。
    const jbc::analysis::MethodSig sig{
        (scope.access & mjbase::hierarchy::kAccStatic) != 0, scope.desc};
    const mjbase::asm_analyzer::MethodDefUse du =
        mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, cu, sig);
    if (!du.ok() || du.truncated()) return mjbase::spi::PreservedAnalyses::all();

    // store 定义零使用边（栈/局部分量都没有）→ 死写，原位降级。
    bool modified = false;
    for (const InsnId id : cu.dom.order()) {
        const jbc::ir::Insn* view = cu.dom.peek(id);
        if (view == nullptr) continue;
        ca::u32 base  = 0;
        ca::u32 width = 0;
        if (!method_rewrite_util::storeLocalSpan(*view, base, width)) continue;
        if (!du.usesOf(DefId::instruction(id)).empty()) continue;
        auto got = cu.dom.get(id);
        if (got.is_err()) continue;
        jbc::ir::Insn* in = std::move(got).unwrap();
        in->op   = width == 2 ? Op::POP2 : Op::POP;
        in->a    = 0;
        in->b    = 0;
        in->wide = false;
        modified = true;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
