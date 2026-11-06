/// @file        outline_driver.hpp
/// @brief       OutlineDriver：区域批量驱动——策略选区到连续 outline 的
///              闭环。
///
/// selectRegion 是单轮决策（候选集 → 挑选/全拒），本驱动器把它接成闭环——
///   selectRegion → planOutline → apply → 重建 FrameView/RegionView/候选
///   → 再选（apply 之间必须重枚举：视图是构建时快照，失效戳语义，禁复用
///   旧视图；驱动器每轮从 session 重取方法体对象再重建，apply 换对象亦
///   不受影响）。
///
/// 真实 RegionView 枚举候选恒不相交或嵌套（形状性质见 region_select.hpp
/// 注），批量驱动
/// 按「内层优先」切完一层再重枚举，无需处理部分重叠——排序取 smallest
/// 档（policy.preferSmallest，v1 默认）即自然先切最内层。
///
/// 接口形状（延续 plan/策略的纯数据 + 纯函数、值语义口径）：输入 =
/// 方法（session + MethodId + 签名）+ 策略档位（RegionSelectPolicy）+
/// 迭代上限（maxRounds，显式参数，缺省保守）；输出 = 已 apply 轮次序列
/// （每轮决策与处方产物）+ 终止原因。驱动器只消费既有设施、不改动它们：
/// planOutline / merge / selectRegion 行为不因驱动改变——驱动器把
/// planOutline 识别包成准入探针注入 selectRegion（region_select.hpp 预
/// 留的消费形态），选中的候选恒可交付识别。
///
/// 保守门（fail-closed）：
///   - 单轮失败即停（识别失败 / 落地失败 / 视图重建失败），已 apply 的
///     轮次不回滚——逐轮独立合法（同 OutlinePlan::apply 不回滚口径）；
///   - 迭代上限显式参数，缺省保守（防组合爆炸）。注意 v1 策略面存在
///     「接力候选」形态：apply 后调用点块（装载 + invokestatic + 转发
///     goto）自身仍满足预检与出口形（出口 = 入口前缀 + 恰 1 新栈顶），
///     干净臂语料上每轮都能再切一层——v1 档上限是唯一护栏，耗尽前每轮
///     均产出一个独立合法的合成方法（行为保持由 JVM 门对拍钉死）；
///   - 收敛档：req.policy.excludeOutlineRelay = true 时排除档跳过
///     接力候选（判定事实与 planInline 的 NotOutlineSynthetic 同源，规则
///     见 region_select.hpp selectRegion 注）→ 候选耗尽 → CandidatesEx-
///     hausted 正常终止（AllExcluded 便签映射；v1 档永不产生该便签，既有
///     行为零改动）。开关经既有 policy 档位面透传，驱动器不另立旗标。
///
/// 红线：请求/报告纯数据值语义；session / resolver
/// 为借用（驱动全程存活）；逐轮视图为轮内快照，轮结束即弃。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/sem/outline_plan.hpp"
#include "mjbase/asm/sem/region_select.hpp"

namespace mjbase::asm_sem {

/// @brief 驱动终止原因（诊断面；逐轮细节在 rounds / stop 便签字段）。
enum class OutlineDriveNote : ca::u8 {
    CandidatesExhausted = 0, ///< 候选耗尽：重建后候选集为空（正常收敛）
    IterationCap,            ///< 迭代上限命中（maxRounds 轮已 apply，保守门）
    SelectRejected,          ///< 策略全拒（stopSelectNote 给因）
    PlanFailed,              ///< 识别失败（防御档：探针档下不可达，
                             ///  stopOutlineError 给因）
    ApplyFailed,             ///< 落地失败（fail-closed 即停，已 apply 轮次
                             ///  不回滚；stopOutlineError 给因）
    ViewRebuildFailed,       ///< 重建 RegionView/LivenessView 失败（CfgError
                             ///  面，fail-closed）
    ParentUnusable,          ///< 父方法不可用（句柄无效/无方法体——请求侧
                             ///  前置，防御档）
};

const char* to_string(OutlineDriveNote note) noexcept;

/// @brief 单轮记录（已 apply 的轮次；值语义）。
struct OutlineDriveRound
{
    ca::u32   sequence = 0;          ///< 本轮命名序号（`outline$<名>$<序>`）
    ca::usize candidateCount = 0;    ///< 本轮重建后的候选数
    ca::usize chosen = 0;            ///< 选中候选下标（本轮候选集内）
    ca::usize chosenMemberCount = 0; ///< 选中候选成员块数（内层优先断言面）
    RegionSelectNote selectNote = RegionSelectNote::NoCandidates; ///< 策略便签
    OutlineOutcome   outcome{};     ///< apply 产物（合成方法身份 + 调用点）
};

/// @brief 驱动请求（值语义 + 借用面）：session / resolver 全程存活借用；
///        视图/候选逐轮自建，请求不携带（失效戳语义见文件头注）。
struct OutlineDriveRequest
{
    jbc::ir::ClassSession&        session;  ///< 父类会话（apply 走它）
    jbc::ir::MethodId             parent;   ///< 父方法身份
    jbc::analysis::MethodSig      sig;      ///< 父方法签名（帧播种）
    jbc::analysis::ITypeResolver& resolver; ///< Strict 引用合并策略
    RegionSelectPolicy policy{};            ///< 策略档位（v1 保守口径缺省）；
                                            ///  收敛开关 = excludeOutlineRelay
                                            ///  （缺省 false = 接力现状）
    /// 策略函数（值语义插拔点，同 RegionSelectFn 换档口径；缺省 v1 默认策略）。
    RegionSelectFn select = &selectRegion;
    /// 迭代上限（显式参数，缺省保守——防组合爆炸；v1 策略面存在接力候选
    /// 形态，上限是唯一护栏，见文件头注）。
    ca::u32 maxRounds = 16;
    /// 命名序号起点（合成方法名 `outline$<名>$<序>` 的首序）。
    ca::u32 sequenceBase = 1;
    /// LVT/LVTT 越切点行处置开关透传（同 OutlineRequest 口径：缺省拒绝，
    /// true 时 applier 裁剪记 lvtDrops）。
    bool allowLvtCrossCut = false;
};

/// @brief 驱动报告（值语义）：已 apply 轮次序列 + 终止原因 + 停轮便签。
///
/// rounds 只含成功 apply 的轮次（按序）；停轮那轮的决策信息在终止原因与
/// stop 便签字段（不落 rounds——已 apply 轮次与停轮轮次不混表）。
struct OutlineDriveReport
{
    std::vector<OutlineDriveRound> rounds;   ///< 已 apply 轮次（按驱动序）
    OutlineDriveNote note = OutlineDriveNote::CandidatesExhausted; ///< 终止原因
    /// SelectRejected 时的策略便签（其余终止原因下为 Ok）。
    RegionSelectNote stopSelectNote = RegionSelectNote::Ok;
    /// PlanFailed / ApplyFailed 时的识别/落地错误码（其余终止原因下为 Ok）。
    OutlineError stopOutlineError = OutlineError::Ok;

    ca::usize appliedCount() const noexcept { return rounds.size(); }
};

/// @brief 驱动闭环：逐轮 selectRegion → planOutline → apply → 重建视图/候选
///        → 再选，直至候选耗尽 / 策略全拒 / 迭代上限 / 单轮失败（fail-closed）。
///
/// 逐轮口径：
///   - 从 session 重取方法体对象（apply 可能换对象），重建 RegionView /
///     LivenessView / FrameView（帧面宽容装配——Strict 引擎不交付帧时缺面
///     跳过判据 6 预检，与 region_select_test 装配口径同向）；
///   - 准入探针 = planOutline 识别（判据级精确准入；探针失败码即识别码）；
///   - 全拒映射：NoCandidates / AllExcluded（排除档耗尽，正常收敛）→
///     CandidatesExhausted，其余策略便签 → SelectRejected（stopSelectNote
///     保真）。
OutlineDriveReport driveOutlines(const OutlineDriveRequest& req);

}   // namespace mjbase::asm_sem
