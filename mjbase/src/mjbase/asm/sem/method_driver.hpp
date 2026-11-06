/// @file        method_driver.hpp
/// @brief       MethodDriver：方法级挑选与跨方法编排——多方法连续 outline
///              的调度层。
///
/// driveOutlines 是单方法闭环（选区 → 识别 → 落地 → 重建 → 再选），本层
/// 把它接成方法级编排——
///   方法挑选（selectMethods）→ 逐方法 driveOutlines（档位
///   透传）→ 聚合报告。分层纪律：策略归编排层、复用原语——编排层只消费
///   既有设施，outline_driver / region_select /
///   planOutline 行为不因编排改变。
///
/// 接口形状（对齐 RegionSelectPolicy 模式：纯数据 + 纯函数、值语义）：
///   输入 = MethodDriveRequest{session + resolver 借用 + 方法候选池 +
///   方法挑选档位（policy + fn 插拔点）+ drive 档位（逐字段透传给
///   OutlineDriveRequest）}；循环 = 按确定性顺序遍历方法 → 便宜预检
///   （RegionView 枚举候选数 0 直接跳过，不跑驱动）→ driveOutlines →
///   聚合。输出 = MethodDriveReport{逐方法 OutlineDriveReport + 总计
///   （应用层数 / 跳过数 / 全拒数 / 失败数）}。
///
/// 方法挑选策略 v1（保守口径，不做调用图感知——调用图感知需调用图设施，
/// 属 v2 方向）：确定性内部名序（方法名升序 → 描述符升序 → 池序稳定破平，
/// 重载与同名歧义全部消解）+ 无候选快跳（快跳在编排循环的便宜预检，
/// 不在策略——策略只管顺序，预检要建视图，职责分层）。策略接口留插
/// 拔形状：MethodSelectPolicy 纯数据档位 + MethodSelectFn 策略函数形状，
/// 新策略 = 同形状新纯函数（对齐 RegionSelectPolicy/RegionSelectFn 口径）。
///
/// 保守门（fail-closed 逐方法化）：
///   - 单方法驱动失败不阻断其他方法：逐方法独立驱动，失败原因记录在
///     该方法的 OutlineDriveReport（note + stop 便签字段），编排继续
///     （driveOutlines 的「单轮失败即停」语义在方法内不变，本层
///     把停的范围收在单方法内）；
///   - 编排层禁复用过期视图：便宜预检的 RegionView 只用于候选计数，
///     绝不传入驱动——驱动器每方法每轮从 session 重取方法体对象新鲜
///     装配（失效戳语义，同 outline_driver.hpp 文件头注）；预检视图
///     即用即弃；
///   - 候选池在编排开始前快照：池来自调用方显式列表或缺省全量枚举
///     （已升格方法，类文件序——顺序由策略归一），驱动新产出的合成
///     方法（outline$ 前缀）不进池，v1 不做递归编排。
///
/// 会话前置（同既有消费方口径）：编排消费已升格方法（MethodUnit::code
/// 非空，promoteMethod 产物）——调用方先走 jbc::ir::collectCodeUnits
/// （SemFixture / 既有门同款纪律）；未升格方法不进缺省池，显式点名则
/// 由驱动器定性 ParentUnusable（不静默）。
///
/// 快跳桶语义（便宜预检的保守面）：预检只数候选，不预判策略——方法级
/// 拒绝事实（如 jsr/ret）若使候选枚举为空，方法落快跳桶（skippedCount）
/// 而非全拒桶：保守结局等价（零落地、会话零改动），仅诊断便签不产出
/// （策略便签归驱动，预检不代跑）。候选非空而策略全拒的方法（如判据 6
/// 全滤）照常进驱动，落全拒桶（rejectedCount）。
///
/// 红线：请求/报告纯数据值语义；session / resolver
/// 为借用（编排全程存活）；预检视图为方法内快照，用完即弃。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/units.hpp"

#include "mjbase/asm/sem/outline_driver.hpp"
#include "mjbase/asm/sem/region_select.hpp"

namespace mjbase::asm_sem {

/// @brief 方法挑选档位（纯数据；缺省值 = v1 保守口径）。
struct MethodSelectPolicy
{
    /// v1 确定性内部名序：方法名升序 → 描述符升序 → 池序稳定破平
    /// （重载消解；确定性编排序）。false = 保池序（调用方自担确定性）。
    bool internalNameOrder = true;
};

/// @brief 方法挑选请求（值语义）：候选池快照 + 会话取名面（指针借用，
///        请求全程存活——策略只读，不改会话）。
struct MethodSelectRequest
{
    jbc::ir::ClassSession&                session;    ///< 方法名/描述符解析面
    const std::vector<jbc::ir::MethodId>& candidates; ///< 候选池（编排前快照）
};

/// @brief 策略函数形状（值语义插拔点，同 RegionSelectFn 换档口径）：
///        v2 起按消费方加策略不加核心（调用图感知挑选 = v2 方向）。
using MethodSelectFn = std::vector<jbc::ir::MethodId> (*)(const MethodSelectPolicy&,
                                                          const MethodSelectRequest&);

/// @brief v1 默认策略：确定性内部名序（见 MethodSelectPolicy 注）。
///
/// 纯排序：无效句柄沉底保持池内相对序（stable），不触碰会话。
std::vector<jbc::ir::MethodId> selectMethods(const MethodSelectPolicy& policy,
                                             const MethodSelectRequest& request);

/// @brief 编排请求（值语义 + 借用面）：session / resolver 编排全程存活
///        借用；方法视图逐方法自建，请求不携带（失效戳语义见文件头注）。
struct MethodDriveRequest
{
    jbc::ir::ClassSession&        session;   ///< 父类会话（apply 走它）
    jbc::analysis::ITypeResolver& resolver;  ///< Strict 引用合并策略（透传）

    /// 方法候选池（编排前快照；空 = 全量已升格方法（code 非空）按类文
    /// 件序入池——顺序仍由策略归一；未升格会话先走 collectCodeUnits，
    /// 见文件头注）。驱动新产出的合成方法不进池（v1 不递归编排）。
    std::vector<jbc::ir::MethodId> methods{};

    /// 方法挑选档位 + 策略函数（插拔点；缺省 v1 内部名序）。
    MethodSelectPolicy selectPolicy{};
    MethodSelectFn     select = &selectMethods;

    // ---- drive 档位（逐字段透传给每个方法的 OutlineDriveRequest）----

    /// 区域策略档位（收敛开关 excludeOutlineRelay 经此透传）。
    RegionSelectPolicy drivePolicy{};
    /// 区域策略函数（缺省 v1 默认策略，同 OutlineDriveRequest 口径）。
    RegionSelectFn driveSelect = &selectRegion;
    /// 每方法迭代上限（显式参数，缺省保守——防组合爆炸，同
    /// OutlineDriveRequest.maxRounds 口径）。
    ca::u32 maxRounds = 16;
    /// 命名序号起点（逐方法生效：合成方法名 `outline$<名>$<序>` 按父方法
    /// 名分域，不同方法间不冲突，序号各自从起点起）。
    ca::u32 sequenceBase = 1;
    /// LVT/LVTT 越切点行处置开关透传（同 OutlineRequest 口径）。
    bool allowLvtCrossCut = false;
};

/// @brief 逐方法记录（已驱动方法；值语义）。
struct MethodDriveRecord
{
    jbc::ir::MethodId  method{};   ///< 驱动的方法身份
    OutlineDriveReport drive{};    ///< 驱动报告（逐轮 + 终止原因 +
                                   ///  停轮便签——失败原因就地可查）
};

/// @brief 编排报告（值语义）：逐方法驱动报告 + 总计。
///
/// methods 只含已驱动方法（按编排序）；便宜预检零候选跳过的方法不落
/// methods（无驱动报告可记），计入 skippedCount。
struct MethodDriveReport
{
    std::vector<MethodDriveRecord> methods;   ///< 逐方法记录（按编排序）
    ca::usize skippedCount = 0;   ///< 便宜预检零候选跳过数（不跑驱动）
    ca::usize rejectedCount = 0;  ///< 驱动全拒方法数（SelectRejected：方法
                                  ///  级 jsr / 判据全滤等，策略面拒绝）
    ca::usize failedCount = 0;    ///< 驱动失败方法数（PlanFailed / ApplyFailed
                                  ///  / ViewRebuildFailed / ParentUnusable——
                                  ///  失败不阻断其他方法，原因见逐方法报告）

    /// 应用层总数：各方法已 apply 轮次之和（OutlineDriveReport 口径聚合）。
    ca::usize totalApplied() const noexcept
    {
        ca::usize total = 0;
        for (const MethodDriveRecord& rec : methods) {
            total += rec.drive.appliedCount();
        }
        return total;
    }
};

/// @brief 方法级编排闭环：候选池快照 → 策略定序 → 逐方法便宜预检（候选
///        数 0 跳过）→ driveOutlines（档位透传）→ 聚合。
///
/// 逐方法口径：
///   - 便宜预检只做 RegionView 枚举计数：候选数 0 直接跳过（不跑驱动、
///     不落 methods，计 skippedCount）；视图构建失败不跳过——交驱动器
///     fail-closed 定性（ViewRebuildFailed 记录在案，不静默吞）；
///   - 方法签名（帧播种）由编排层从会话现场装配（access/name/desc/
///     thisClass），每方法新鲜构造，不复用任何前方法遗留；
///   - 无体/句柄失效方法不预跳：交驱动器定性（ParentUnusable），计入
///     failedCount（请求面显式点名的方法给出处方可查的失败记录，不做
///     静默跳过）。
MethodDriveReport driveMethods(const MethodDriveRequest& req);

}   // namespace mjbase::asm_sem
