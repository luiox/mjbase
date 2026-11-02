/// @file        method_driver.cpp
/// @brief       MethodDriver 实现：方法挑选定序 + 逐方法驱动编排（A9）。
#include "mjbase/asm/sem/method_driver.hpp"

#include <algorithm>
#include <string_view>

namespace mjbase::asm_sem {

std::vector<jbc::ir::MethodId> selectMethods(const MethodSelectPolicy& policy,
                                             const MethodSelectRequest& request)
{
    std::vector<jbc::ir::MethodId> order = request.candidates;
    if (!policy.internalNameOrder) {
        return order;   /// 保池序（调用方自担确定性）
    }

    /// 确定性内部名序：方法名升序 → 描述符升序 → 池序稳定破平（重载与
    /// 同名歧义全部消解）。无效句柄无名字面，沉底保池内相对序。
    const jbc::ir::ClassUnit&    cu      = request.session.unit();
    const jbc::ir::SymbolPool&   symbols = cu.symbols();
    std::stable_sort(order.begin(), order.end(),
                     [&cu, &symbols](jbc::ir::MethodId a, jbc::ir::MethodId b) {
                         const jbc::ir::MethodUnit* ma = cu.method(a);
                         const jbc::ir::MethodUnit* mb = cu.method(b);
                         if (ma == nullptr || mb == nullptr) {
                             return mb != nullptr;   /// 无效句柄沉底
                         }
                         const std::string_view na = symbols.str(ma->name);
                         const std::string_view nb = symbols.str(mb->name);
                         if (na != nb) {
                             return na < nb;
                         }
                         return symbols.str(ma->desc) < symbols.str(mb->desc);
                     });
    return order;
}

MethodDriveReport driveMethods(const MethodDriveRequest& req)
{
    MethodDriveReport report;

    /// 候选池快照（编排开始前）：缺省 = 全量已升格方法（code 非空，升格
    /// 前置 collectCodeUnits 由调用方负责）按类文件序入池；驱动新产出的
    /// 合成方法不进池（v1 不递归编排）。
    std::vector<jbc::ir::MethodId> pool = req.methods;
    if (pool.empty()) {
        const jbc::ir::ClassUnit& cu = req.session.unit();
        pool.reserve(cu.methods.size());
        for (const jbc::ir::MethodUnit& mu : cu.methods) {
            if (mu.code != nullptr) {
                pool.push_back(mu.id);
            }
        }
    }

    const std::vector<jbc::ir::MethodId> order =
        req.select(req.selectPolicy, MethodSelectRequest{req.session, pool});

    const jbc::ir::ClassUnit& cu = req.session.unit();
    for (const jbc::ir::MethodId id : order) {
        /// 便宜预检：RegionView 枚举候选数 0 直接跳过（不跑驱动）。预检
        /// 视图只用于计数、即用即弃——绝不传入驱动（编排层禁复用过期
        /// 视图，失效戳语义见文件头注）。构建失败不跳过：交驱动器
        /// fail-closed 定性，不静默吞。
        const jbc::ir::MethodUnit* mu = cu.method(id);
        if (mu != nullptr && mu->code != nullptr) {
            auto builtView = RegionView::build(*mu->code);
            if (builtView.is_ok()
                && std::move(builtView).unwrap().candidates().empty()) {
                ++report.skippedCount;
                continue;
            }
        }

        /// 方法签名现场装配（帧播种面）：access/name/desc/thisClass 从
        /// 会话取，每方法新鲜构造。无体/句柄失效时 sig 留缺省——驱动器
        /// 请求侧前置（ParentUnusable）先于帧消费，失败记录在案。
        jbc::analysis::MethodSig sig;
        if (mu != nullptr) {
            sig.isStatic  = (mu->access & 0x0008u) != 0;
            sig.desc      = cu.symbols().str(mu->desc);
            sig.name      = cu.symbols().str(mu->name);
            sig.thisClass = cu.symbols().str(cu.thisClass);
        }

        /// 逐方法驱动（A7/A8 档位透传）；单方法失败不阻断编排——失败
        /// 原因记录在该方法的驱动报告里。
        OutlineDriveRequest driveReq{req.session, id, sig, req.resolver};
        driveReq.policy          = req.drivePolicy;
        driveReq.select          = req.driveSelect;
        driveReq.maxRounds       = req.maxRounds;
        driveReq.sequenceBase    = req.sequenceBase;
        driveReq.allowLvtCrossCut = req.allowLvtCrossCut;
        MethodDriveRecord rec;
        rec.method = id;
        rec.drive  = driveOutlines(driveReq);

        switch (rec.drive.note) {
        case OutlineDriveNote::SelectRejected:
            ++report.rejectedCount;
            break;
        case OutlineDriveNote::PlanFailed:
        case OutlineDriveNote::ApplyFailed:
        case OutlineDriveNote::ViewRebuildFailed:
        case OutlineDriveNote::ParentUnusable:
            ++report.failedCount;
            break;
        default:   /// CandidatesExhausted / IterationCap = 正常收敛
            break;
        }
        report.methods.push_back(std::move(rec));
    }
    return report;
}

}   // namespace mjbase::asm_sem
