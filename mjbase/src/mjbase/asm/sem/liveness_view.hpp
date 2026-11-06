/// @file        liveness_view.hpp
/// @brief       LivenessView：局部槽后向活跃（may-liveness）只读视图
///              （asm/sem 设施层）。
///
/// 口径：「局部槽后向活跃集 + 出口活跃集」——回答「出口之后还有谁读这个
/// 槽」。outline 判据 3 的现档口径是「被写且出口后仍活跃才拒绝」：
/// copy-set 最小化的写回集 = 区域内被写 ∩ 出口后活跃，v1 要求为空。
///
/// 数据流口径：块级后向 may-liveness 标准方程——out[b] = ∪ in[succ]
/// （后继含异常 handler 边：handler 可读任意槽，保守并入）；块内按指令
/// 逆序迁移 in(i) = (out(i) − def(i)) ∪ use(i)。def/use 语义与
/// asm/analyzer/def_use 一致：load 族 = use、store 族 = def（cat2 store
/// 连带杀高半槽）、iinc = use + def、ret = use（jsr 方法 outline 侧拒绝，
/// 视图照常交付事实）。
///
/// 红线：只读、值语义、自持有；视图为构建时快照，方法体
/// 任何结构变更后须重建（与 asm/cfg / FrameView 同口径，不做增量维护）；
/// 过期句柄查询探测式返回 false，不抛错。活跃集是 may-近似（存在路径
/// 可读即活跃）——「不活跃 ⇒ 可剔除/可写不回传」的负向结论方向安全：
/// 高估活跃只会多拒，不会漏判。
#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/units.hpp"

#include "mjbase/asm/cfg/control_flow_graph.hpp"

namespace mjbase::asm_sem {

/// @brief 局部槽后向活跃视图（值语义、自持有）。
class LivenessView
{
public:
    /// @brief 分析方法体（只读消费 dom/exc；CFG 构建要求非 const 句柄，
    ///        与 RegionView 同口径）。
    /// @return Err：CFG 构建失败（分支/异常表边界无法解析），不交付部分集。
    static ca::core::Result<LivenessView, mjbase::asm_cfg::CfgError>
    build(jbc::ir::CodeUnit& unit);

    /// 活跃槽位上界（全部指令局部操作数 + cat2 高半槽的最大值 + 1）。
    ca::u32 slotCount() const noexcept { return slotCount_; }

    /// @brief 指令执行后（out 状态）槽是否活跃。无效句柄/越界 false。
    ///
    /// 出口活跃集查询入口：对区域末条 goto 逐槽查询即「出口后仍被读」
    /// （out = 后继块入状态的并，含异常边）。
    bool localLiveAfter(jbc::ir::InsnId id, ca::u32 slot) const noexcept;

    /// @brief 指令执行前（in 状态）槽是否活跃。无效句柄/越界 false。
    bool localLiveBefore(jbc::ir::InsnId id, ca::u32 slot) const noexcept;

    /// @brief 指令执行后仍活跃的槽位表（升序；出口活跃集按此取）。
    std::vector<ca::u32> liveAfterSlots(jbc::ir::InsnId id) const;

private:
    /// 单条指令的局部 def/use（def 先杀后用，块内逆序迁移用）。
    struct LocalFx
    {
        bool      isLoad = false;
        bool      isStore = false;
        bool      isInc = false;
        bool      isRet = false;
        ca::u32   slot = 0;
        bool      cat2 = false;   ///< long/double：store 连带高半槽
    };

    static LocalFx localFxOf(const jbc::ir::Insn& in) noexcept;

    mjbase::asm_cfg::ControlFlowGraph cfg_{};
    ca::u32 slotCount_ = 0;
    /// 与 dom.order() 对齐的 out 活跃集与 def/use 事实（in 集按 fx 推导）。
    std::vector<std::vector<bool>> liveOut_;
    std::vector<LocalFx> fx_;
    std::unordered_map<ca::u32, ca::usize> posOf_;   ///< InsnId.slot → 下标
};

}   // namespace mjbase::asm_sem
