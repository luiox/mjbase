/// @file        region_view.hpp
/// @brief       RegionView：基本块 / SESE 候选区域的只读视图（asm/sem 设施层首批）。
///
/// 设计依据 luiox/mjbase #115 §2.2/§3.1：在 asm/cfg 的支配树 + 逆图后支配
/// 上，按入口边枚举最大 SESE 候选区域——
///   R = { x : 入口边目标支配 x ∧ 出口锚后支配 x }（出口锚自身除外），
/// 出口锚在入口边的支配集成员的普通后继中探测（收敛检查钉定唯一越出目标），
/// 再以判据 1（#115 §1.1）过滤：单入口（成员的全部入边——含异常入边——
/// 只允许来自成员内部或该入口边）与单出口（成员的普通出边越出目标收敛
/// 于唯一块）。判据 4（异常上下文同质）与判据 5（jsr/ret）按设施层复用
/// 标准交付为事实与旗标，不静默吞候选——拒绝决策归消费方（A2 validate、
/// stmt/消解器）。判据 6（UninitThis 存活期）需帧信息，归 FrameView 探针
/// 与 A2 validate 组合。
///
/// 后支配只在普通边域（非异常边）上计算：SESE 出口分析看普通控制流；
/// try 覆盖内的成员经 handler 离场不构成区域出口（判据 4 的同质语义）
/// ——异常入边仍参与单入口检查（外部异常跳入 = 控制流进入区域，保守
/// 拒绝）。无出口候选（如整方法、以 return 收尾的尾部区域）不交付：
/// outline 调用点需要续延边，v1 口径（#115 §1.2）。
///
/// 红线（#115 §2.1）：只读、值语义。视图是构建时快照（CFG/支配树/块级
/// handler 事实均为自持有值），方法体结构变更后重建；块句柄的解析仍需
/// 原 CodeUnit 存活（同 asm/cfg 契约）。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/ir/units.hpp"

#include "mjbase/asm/cfg/cfg_analysis.hpp"
#include "mjbase/asm/cfg/control_flow_graph.hpp"

namespace mjbase::asm_sem {

/// @brief handler 事实：handler 块 + catch 类型 CP 索引（0 = any）。
struct HandlerRef
{
    mjbase::asm_cfg::BlockId handler{};
    ca::u32                 catchCp = 0;

    bool operator==(const HandlerRef& o) const noexcept
    {
        return handler == o.handler && catchCp == o.catchCp;
    }
    bool operator!=(const HandlerRef& o) const noexcept { return !(*this == o); }
    bool operator<(const HandlerRef& o) const noexcept
    {
        return handler != o.handler ? handler < o.handler : catchCp < o.catchCp;
    }
};

/// @brief 一个 SESE 候选区域（按入口边枚举的最大候选）。
struct RegionCandidate
{
    mjbase::asm_cfg::BlockId entryFrom;    ///< 入口边源块（区域外）
    mjbase::asm_cfg::BlockId entryBlock;   ///< 入口边目标块 = 区域头
    mjbase::asm_cfg::BlockId exitTarget;   ///< 唯一普通越出目标
    std::vector<mjbase::asm_cfg::BlockId> members;  ///< 升序

    /// 判据 4：成员 handler 集恒等（true 时 handlers 为共享集）。
    bool exceptionHomogeneous = false;
    /// 判据 1 的直线形态：普通控制流成链（无内部分支/汇合）；A2 v1
    /// 只开这一档（#115 §1.2）。
    bool straightLine = false;
    /// 同质时的共享 handler 集（升序去重；非同质为空）。
    std::vector<HandlerRef> handlers;
};

/// @brief SESE 候选区域视图（值语义、自持有）。
class RegionView
{
public:
    /// @brief 建图 + 支配/后支配 + 块级 handler 事实（不修改 unit）。
    ///
    /// CFG 构建失败（无效分支目标/异常表边界）原样透传 asm_cfg::CfgError，
    /// 不做静默跳过。
    static ca::core::Result<RegionView, mjbase::asm_cfg::CfgError>
    build(jbc::ir::CodeUnit& unit);

    /// 块级控制流图（程序序块表 + 边表）。
    const mjbase::asm_cfg::ControlFlowGraph& cfg() const noexcept { return cfg_; }

    /// 入口侧支配树（asm_cfg 语义：后继集含异常边）。
    const mjbase::asm_cfg::DominatorTree& dominators() const noexcept { return dominators_; }

    /// 判据 5 的语料级事实：方法体含 jsr/jsr_w/ret（语形扫描）。
    bool hasSubroutine() const noexcept { return hasSubroutine_; }

    /// @brief 块的 handler 集：覆盖该块的异常表条目（handler 块, catchCp），
    ///        升序去重。无效块返回空表。
    std::vector<HandlerRef> handlerSetOf(mjbase::asm_cfg::BlockId id) const;

    /// @brief 成员块的 handler 集是否恒等（判据 4；空集/单块平凡成立）。
    bool exceptionHomogeneous(const std::vector<mjbase::asm_cfg::BlockId>& members) const;

    /// @brief 判据 1 的直线形态：成员普通控制流成链——除末成员外每个
    ///        成员的普通出边恰为唯一下一成员，末成员普通出边至多一个
    ///        越出目标；异常边不参与（归判据 4）。单块恒真。
    bool straightLine(const std::vector<mjbase::asm_cfg::BlockId>& members) const;

    /// @brief 全量枚举 SESE 候选（按入口边；每边至多一个最大候选）。
    ///
    /// 过滤口径：判据 1（单入口单出口）在此生效；判据 4/5 以旗标/事实
    /// 交付（见类注）。成员为空或不可达块不参与。
    std::vector<RegionCandidate> candidates() const;

private:
    mjbase::asm_cfg::ControlFlowGraph cfg_;
    mjbase::asm_cfg::DominatorTree    dominators_;
    std::vector<std::vector<HandlerRef>> handlers_;   ///< 按块下标
    bool hasSubroutine_ = false;
};

}   // namespace mjbase::asm_sem
