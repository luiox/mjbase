/// @file        region_select.hpp
/// @brief       RegionSelect：区域挑选策略（保守档）。
///
/// 定位：RegionView 只交候选 + 事实（判据 4/5 以旗标交付、拒绝决策归
/// 消费方），「挑哪个区域」归策略层——消费方手写谓词形态（merge_plan_test
/// 的 outlineWhere、outline_plan_test 的 runJvmGateWhere）由 selectRegion
/// 统一承担。设施层既有行为（planOutline/merge 判据与落地）不因策略改变：
/// 策略定位在候选枚举与识别之间——
///   RegionView.candidates() → selectRegion → planOutline → apply。
///
/// 接口形状（对齐 plan 处方模式：纯数据 + 纯函数，值语义）：
///   输入 = RegionCandidate 集 + 事实面（RegionView / FrameView /
///   LivenessView，全部指针可缺省——缺哪面跳过哪层预检，策略在纯候选
///   数据上仍可决策）；输出 = RegionDecision（挑选决策 + 优先序）。
///   优先序供批量驱动复用（方法级逐区域驱动 outline）。
///
/// v1 默认策略（保守口径，规则详见 selectRegion 注）：
///   1. 事实面预检（与 planOutline v1 判据同向的廉价代理）：方法级 jsr
///      （判据 5）、候选级直线旗标（判据 1，v1 只开直线档）、handler
///      集为空（判据 4，try 覆盖不开档）、末条指令 goto（v1 出口形态）、
///      UninitThis 存活（判据 6，FrameView 面）、写回集为空（判据 3，
///      LivenessView 面：区域内被写 ∩ 出口锚后活跃 = 空）。
///   2. 规模次之：通过预检的候选按成员数升序（最小切口保守——嵌套候选
///      取最内层，批量驱动下先切内层不使外层候选过早失效），平局以首
///      成员块号（程序序）破平，决策确定性的。
///   3. 冲突保守拒绝：通过预检的候选中存在等尺寸部分重叠对（成员集相
///      交且互异——既非嵌套亦非重复，任意挑选均无结构依据）时全拒。
///      嵌套（⊆）与重复（=）不构成冲突；真实 RegionView 枚举的两候选
///      恒为不相交或嵌套（入口支配集 × 出口锚后支配的形状性质），本条
///      为防御档与手构候选面兜底。
///   4. 准入探针（可缺省）：消费方把 planOutline 识别包成探针注入，策略
///      按优先序逐个探测、首个通过即选中（outlineWhere 形态的策略化）；
///      缺省时直接选中优先序首位。
///
/// 可插拔（v2 起按消费方加策略不加核心）：RegionSelectPolicy 为纯数据
/// 档位（预检开关与排序取向），RegionSelectFn 为策略函数形状——新策略
/// = 同形状新纯函数，消费方（批量驱动）持函数指针换档，不改核心。
///
/// 红线：只读、值语义、无编辑状态；请求中的视图 /
/// 指针为借用（快照纪律——方法体结构变更后重建，句柄解析需原 CodeUnit
/// 存活）；策略拒绝不产生任何会话改动，判据级拒绝细节仍归 planOutline。
#pragma once

#include <functional>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/units.hpp"

#include "mjbase/asm/sem/frame_view.hpp"
#include "mjbase/asm/sem/liveness_view.hpp"
#include "mjbase/asm/sem/region_view.hpp"

namespace mjbase::asm_sem {

/// @brief 全拒原因便签（策略层诊断面；判据级细节仍归设施层错误码）。
enum class RegionSelectNote : ca::u8 {
    Ok = 0,              ///< 已选中（accepted = true）
    NoCandidates,        ///< 候选集为空（RegionView 未交付候选）
    MethodSubroutine,    ///< 方法级：方法体含 jsr/ret（判据 5，先于候选级过滤）
    AllFiltered,         ///< 全部候选被事实面预检过滤（直线/异常/出口形/判据 3/6 面）
    ConflictAmbiguous,   ///< 冲突保守拒绝：存在等尺寸部分重叠候选对
    NoneAdmitted,        ///< 通过预检的候选全部被准入探针拒绝（判据级细节在探针侧）
    AllExcluded,         ///< 全部候选被排除档跳过（v2 排除档；至少一个命中
                         ///  接力形态——批量驱动据此判候选耗尽正常收敛）
};

const char* to_string(RegionSelectNote note) noexcept;

/// @brief 策略档位（纯数据；缺省值 = v1 保守口径全开）。
struct RegionSelectPolicy
{
    /// 判据 1 直线旗标预检（v1 只开直线档；放开交消费方自担）。
    bool requireStraightLine = true;
    /// 判据 4 预检：异常同质且 handler 集为空（try 覆盖不开档）。
    bool requireNoHandlers = true;
    /// v1 出口形态预检：末成员末条指令为 GOTO（出口栈形取其到达帧）。
    bool requireExitGoto = true;
    /// 排序取向：true = 成员数升序（规模次之，最小切口）；false = 枚举序。
    bool preferSmallest = true;
    /// 冲突保守拒绝：等尺寸部分重叠候选对 → 全拒（false = 程序序破平挑选）。
    bool rejectOverlapTie = true;
    /// v2 排除档：跳过「合成接力块」候选——apply 调用点形态（装载 +
    /// invokestatic + 转发 goto，判定事实与 planInline 的 NotOutlineSynthetic
    /// 同源，规则详见 selectRegion 注）。缺省 false = v1 不排除（保持
    /// 批量驱动的接力现状，迭代上限是唯一护栏）；true = 驱动收敛档，接力
    /// 候选被跳过后候选耗尽正常终止。误排除的方向是安全的：少切一个形似
    /// 接力块的真实区域（错过优化机会），不产生任何非法变换。
    bool excludeOutlineRelay = false;
};

/// @brief 策略请求（值语义）：候选集必填，事实面与探针可缺省——缺哪面
///        跳过哪层预检（见 selectRegion 规则注）。指针均为借用，快照
///        纪律同各视图（方法体结构变更后重建，原 CodeUnit 存活期内有效）。
struct RegionSelectRequest
{
    const std::vector<RegionCandidate>& candidates;   ///< 候选集（必填；通常为
                                                      ///  RegionView::candidates 产物）
    const RegionView* view = nullptr;       ///< 候选源视图（方法级 jsr 事实 +
                                            ///  块→指令解析；缺省跳过相关预检）
    jbc::ir::CodeUnit* unit = nullptr;      ///< 指令句柄解析面（出口形/写集
                                            ///  预检 peek 指令；缺省跳过）
    const FrameView* frame = nullptr;       ///< 帧事实面（判据 6 UninitThis
                                            ///  预检；缺省跳过）
    const LivenessView* liveness = nullptr; ///< 活跃事实面（判据 3 写回集
                                            ///  预检；缺省跳过）
    /// 会话借用面（v2 排除档专用）：invokestatic 目标的身份解析（同类 +
    /// outline$ 前缀 + synthetic 三元事实，planInline 同源）。缺省（空）=
    /// 排除档无从判定，一律不排除（保守回落 v1 枚举口径）。其余档位不用
    /// 本面；指针借用，请求全程存活。
    const jbc::ir::ClassSession* session = nullptr;
    /// 准入探针：判据级精确准入（消费方把 planOutline 识别包成探针，识别
    /// 上下文随闭包捕获；同 FrameView 的 FunctionTypeResolver 注入面）。
    /// 返回 true = 该候选可交付识别。缺省（空）= 纯事实面决策。
    std::function<bool(const RegionCandidate&)> admit;
};

/// @brief 挑选决策（值语义）：挑选决策 + 优先序。
struct RegionDecision
{
    bool accepted = false;   ///< false = 全部拒绝（note 给因；order 仍携带通过预检的序）
    /// 选中候选下标（request.candidates 内；accepted 时有效，探针档 =
    /// 优先序中首个通过探针者，纯事实档 = 优先序首位）。
    ca::usize chosen = 0;
    /// 优先序（通过预检的候选下标，最优在前；供批量驱动逐区域消费）。
    std::vector<ca::usize> order;
    /// 全拒原因（accepted 时为 Ok）。
    RegionSelectNote note = RegionSelectNote::NoCandidates;
};

/// @brief 策略函数形状（值语义插拔点）：v2 起按消费方加策略不加核心——
///        新策略 = 同形状新纯函数，消费方持有函数指针换档。
using RegionSelectFn = RegionDecision (*)(const RegionSelectPolicy&,
                                          const RegionSelectRequest&);

/// @brief 优先序（纯面）：通过预检的候选下标集 → 排序（规模升序 + 首成员
///        程序序破平；policy.preferSmallest = false 时保枚举序）。纯候选
///        数据上的纯函数，批量驱动可直接复用。
std::vector<ca::usize>
rankRegions(const RegionSelectPolicy& policy, const std::vector<RegionCandidate>& candidates,
            const std::vector<ca::usize>& passing);

/// @brief v1 默认策略：事实面预检 → 冲突保守拒绝 → 优先序 → 准入探针。
///
/// 规则（决策依据）：
///   - 方法级（先于候选级）：RegionView/FrameView 任一面报 jsr/ret →
///     全拒（判据 5 是方法体语形事实，逐候选过滤无意义）。
///   - 候选级预检（与 planOutline v1 判据同向的廉价代理；通过者进优先序）：
///     F2 直线旗标（判据 1）、F3 异常同质且 handler 集为空（判据 4；
///     非同质候选 handlers 为空但旗标为假，同拒）、F4 末成员末条
///     指令 GOTO（v1 出口形态；需 view + unit）、F5 区域成员到达帧无
///     UninitThis（判据 6；需 view + frame）、F6 写回集为空（判据 3：区
///     域内 store/iinc 写槽 ∩ 出口锚后活跃 = 空；需 view + unit + liveness）。
///     事实面缺省跳过对应预检——策略更宽但绝不比事实面更乐观（探针缺省
///     时选中者未必过 planOutline 全判据，判据 2/切面级结论归识别侧）。
///   - 冲突保守拒绝：等尺寸部分重叠对（相交且互异）→ 全拒；嵌套/重复
///     不冲突（形状性质见文件头注）。
///   - 准入探针：按优先序逐个探测，首个通过即选中；全不通过 → 全拒
///     （NoneAdmitted）。缺省探针 → 选中优先序首位。
///   - v2 排除档（policy.excludeOutlineRelay = true）：先于事实面预检
///     跳过「合成接力块」候选——apply 调用点块的形态（接力候选：每轮
///     apply 后调用点块自身满足全部预检与出口形，驱动器
///     借此连切不停）。判定（保守匹配发射形态，宁可漏判不可误判）：
///       a. 单成员块，末条指令为转发 goto（apply 恒以 GOTO 复用出口标签；
///          直落出口形态不在本档判定范围——requireExitGoto 档下不会出现）；
///       b. 块内恰一个 INVOKESTATIC，其余指令限装载族（局部读）+ DUP/DUP2
///          复位（apply preamble = [复位 DUP] + 局部入参装载，别无他物）；
///       c. 调用目标身份事实与 planInline 的 NotOutlineSynthetic 同源：同类
///          （owner = thisClass）+ outline$ 名前缀 + access 恒等
///          private|static|synthetic（悬空/身份不符 = 非本设施产物，不排除）。
///     事实面不足（view/unit/session 任一缺）→ 不排除（保守回落 v1）。
///     被排除候选不进优先序、不进探针；通过预检的候选为空且至少一个命中
///     排除时记 AllExcluded（批量驱动映射候选耗尽正常收敛），否则仍记
///     AllFiltered（v1 口径不变）。
RegionDecision selectRegion(const RegionSelectPolicy& policy,
                            const RegionSelectRequest& request);

}   // namespace mjbase::asm_sem
