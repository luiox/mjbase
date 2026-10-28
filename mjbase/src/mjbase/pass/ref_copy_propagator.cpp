#include "mjbase/pass/ref_copy_propagator.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/origin_domain.hpp"
#include "mjbase/hierarchy/summary.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;

/// aload 族取槽位：泛形用 a 域，_0..3 快捷形由操作码隐含。非 aload 返回
/// false。
bool aloadSlotOf(const jbc::ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ALOAD) {
        slot = static_cast<ca::u32>(in.a);
        return true;
    }
    if (in.op >= Op::ALOAD_0 && in.op <= Op::ALOAD_3) {
        slot = static_cast<ca::u32>(static_cast<ca::u8>(in.op)
                                    - static_cast<ca::u8>(Op::ALOAD_0));
        return true;
    }
    return false;
}

/// 生产者白名单改写形态：把 aload 读点原位改写为来源指令的等值形态。
/// 返回 false = 白名单外（保留读点）。同一常量池条目引用恒等的 ldc
/// （String/Class/MethodType）与值不可变的 aload/aconst_null 才放行。
bool rewriteToProducer(jbc::ir::Insn& load, const jbc::ir::Insn& producer,
                       jbc::core::CpTag producerLdcTag)
{
    ca::u32 producerSlot = 0;
    if (aloadSlotOf(producer, producerSlot)) {
        ca::u32 loadSlot = 0;
        aloadSlotOf(load, loadSlot);
        // 同槽自赋值改写无意义，跳过防抖动。
        if (producerSlot == loadSlot) return false;
        load.op   = Op::ALOAD;
        load.a    = static_cast<ca::u32>(producerSlot);
        load.b    = 0;
        load.cp   = 0;
        load.wide = false;
        return true;
    }
    if (producer.op == Op::ACONST_NULL) {
        load.op   = Op::ACONST_NULL;
        load.a    = 0;
        load.b    = 0;
        load.cp   = 0;
        load.wide = false;
        return true;
    }
    if (producer.op == Op::LDC) {
        if (producerLdcTag != jbc::core::CpTag::String
            && producerLdcTag != jbc::core::CpTag::Class
            && producerLdcTag != jbc::core::CpTag::MethodType) {
            return false;
        }
        load.op   = Op::LDC;
        load.a    = 0;
        load.b    = 0;
        load.cp   = producer.cp;
        load.wide = false;
        return true;
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses RefCopyPropagator::run(mjbase::spi::MethodScope&       scope,
                                                      mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit& cu = scope.code;
    if (cu.dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();

    // 单来源事实是改写前提：数据流失败整件跳过。
    const jbc::analysis::MethodSig sig{
        (scope.access & mjbase::hierarchy::kAccStatic) != 0, scope.desc};
    const mjbase::asm_analyzer::OriginResult origins =
        mjbase::asm_analyzer::traceOrigins(scope.session.backing().cp, cu, sig);
    if (!origins.ok()) return mjbase::spi::PreservedAnalyses::all();

    bool modified = false;
    for (const jbc::ir::InsnId id : cu.dom.order()) {
        const jbc::ir::Insn* view = cu.dom.peek(id);
        if (view == nullptr || !origins.reachable(id)) continue;
        ca::u32 loadSlot = 0;
        if (!aloadSlotOf(*view, loadSlot)) continue;

        const mjbase::asm_analyzer::OriginValue* origin =
            origins.localAt(id, loadSlot);
        if (origin == nullptr || !origin->isInsn()) continue;
        const jbc::ir::Insn* producer = cu.dom.peek(origin->insn);
        if (producer == nullptr) continue;
        const jbc::core::CpTag producerLdcTag =
            producer->op == Op::LDC && scope.session.backing().cp.valid(producer->cp)
                ? scope.session.backing().cp.tagOf(producer->cp)
                : jbc::core::CpTag::Utf8;

        auto got = cu.dom.get(id);
        if (got.is_err()) continue;
        if (!rewriteToProducer(*std::move(got).unwrap(), *producer, producerLdcTag)) {
            continue;
        }
        modified = true;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
