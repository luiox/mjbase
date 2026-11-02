/// @file        outline_driver.cpp
/// @brief       OutlineDriver 实现：逐轮选区/识别/落地/重建的驱动闭环（A7）。
#include "mjbase/asm/sem/outline_driver.hpp"

namespace mjbase::asm_sem {

const char* to_string(OutlineDriveNote note) noexcept
{
    switch (note) {
    case OutlineDriveNote::CandidatesExhausted: return "candidates-exhausted";
    case OutlineDriveNote::IterationCap:        return "iteration-cap";
    case OutlineDriveNote::SelectRejected:      return "select-rejected";
    case OutlineDriveNote::PlanFailed:          return "plan-failed";
    case OutlineDriveNote::ApplyFailed:         return "apply-failed";
    case OutlineDriveNote::ViewRebuildFailed:   return "view-rebuild-failed";
    case OutlineDriveNote::ParentUnusable:      return "parent-unusable";
    }
    return "unknown";
}

OutlineDriveReport driveOutlines(const OutlineDriveRequest& req)
{
    OutlineDriveReport report;
    ca::u32 seq = req.sequenceBase;
    for (ca::u32 round = 0; round < req.maxRounds; ++round) {
        /// 请求侧前置：方法体对象每轮从 session 重取（apply 可能换对象，
        /// 旧句柄不留用——失效戳语义）。
        const jbc::ir::MethodUnit* mu = req.session.unit().method(req.parent);
        if (mu == nullptr || mu->code == nullptr) {
            report.note = OutlineDriveNote::ParentUnusable;
            report.stopOutlineError = OutlineError::ParentNoCode;
            return report;
        }
        jbc::ir::CodeUnit& unit = *mu->code;

        /// 视图重建（apply 之间必须重枚举——RegionView/LivenessView 是
        /// 构建时快照；CfgError 面失败 fail-closed 即停）。帧面宽容装配：
        /// Strict 引擎不交付帧时缺面跳过判据 6 预检（策略更宽但绝不比
        /// 事实面更乐观，与 region_select 装配口径同向）。
        auto builtView = RegionView::build(unit);
        if (builtView.is_err()) {
            report.note = OutlineDriveNote::ViewRebuildFailed;
            return report;
        }
        RegionView view = std::move(builtView).unwrap();
        auto builtLive = LivenessView::build(unit);
        if (builtLive.is_err()) {
            report.note = OutlineDriveNote::ViewRebuildFailed;
            return report;
        }
        LivenessView live = std::move(builtLive).unwrap();
        FrameView frames{};
        bool hasFrames = false;
        if (auto builtFrames =
                FrameView::build(req.session.backing().cp, unit, req.sig, req.resolver);
            builtFrames.is_ok()) {
            frames    = std::move(builtFrames).unwrap();
            hasFrames = true;
        }

        /// 策略选区：准入探针 = planOutline 识别（判据级精确准入——选中
        /// 候选恒可交付识别；探针闭包借用本轮视图）。
        const std::vector<RegionCandidate> candidates = view.candidates();
        OutlineDriveRound rec;
        rec.sequence       = seq;
        rec.candidateCount = candidates.size();
        RegionSelectRequest selReq{candidates, &view, &unit};
        selReq.frame    = hasFrames ? &frames : nullptr;
        selReq.liveness = &live;
        selReq.session  = &req.session;   /// v2 排除档身份事实面（A8）
        selReq.admit    = [&req, &view, seq](const RegionCandidate& c) {
            return planOutline(
                       OutlineRequest{req.session, req.parent, req.sig, req.resolver,
                                      view, c, seq, req.allowLvtCrossCut})
                .is_ok();
        };
        const RegionDecision d = req.select(req.policy, selReq);
        rec.selectNote = d.note;
        if (!d.accepted) {
            /// 排除档耗尽（AllExcluded，A8）与零候选同为正常收敛：接力候选
            /// 被跳过后候选池抽干；其余全拒仍为 SelectRejected（v1 口径）。
            report.note = (d.note == RegionSelectNote::NoCandidates
                           || d.note == RegionSelectNote::AllExcluded)
                              ? OutlineDriveNote::CandidatesExhausted
                              : OutlineDriveNote::SelectRejected;
            report.stopSelectNote = d.note;
            return report;
        }
        rec.chosen           = d.chosen;
        rec.chosenMemberCount = candidates[d.chosen].members.size();

        /// 识别 + 落地。识别失败为防御档（探针已滤，确定性识别下不可达）；
        /// 落地失败 fail-closed 即停——已 apply 的轮次不回滚（逐轮独立合法，
        /// 同 OutlinePlan::apply 不回滚口径）。
        auto planned = planOutline(
            OutlineRequest{req.session, req.parent, req.sig, req.resolver, view,
                           candidates[d.chosen], seq, req.allowLvtCrossCut});
        if (planned.is_err()) {
            report.note = OutlineDriveNote::PlanFailed;
            report.stopOutlineError = planned.unwrap_err();
            return report;
        }
        OutlinePlan plan = std::move(planned).unwrap();
        auto applied = plan.apply(req.session, req.resolver);
        if (applied.is_err()) {
            report.note = OutlineDriveNote::ApplyFailed;
            report.stopOutlineError = applied.unwrap_err();
            return report;
        }
        rec.outcome = std::move(applied).unwrap();
        ++seq;
        report.rounds.push_back(std::move(rec));
    }
    /// 上限轮全部 apply 成功仍未耗尽：迭代上限命中（保守门）。
    report.note = OutlineDriveNote::IterationCap;
    return report;
}

}   // namespace mjbase::asm_sem
