/// @file        slot_provenance.hpp
/// @brief       SlotProvenanceView：栈槽 + 局部槽的到达定义只读视图（asm/sem 设施层首批）。
///
/// 设计依据 luiox/mjbase #115 §2.2：切面槽 provenance——逐指令逐槽的到达
/// 定义集（def → uses，栈槽与局部槽同域建模），供 outline 判据 2/3 的
/// 切面检查消费（A2 起）。
///
/// 与 asm/analyzer/def_use 的关系：def-use 的并集域（DefUseDomain）已把
/// 栈槽纳入建模，本视图直接复用其域与边语义（asm_analyzer 的 DefId /
/// DefSet / DefUseEdge / LocalAccess 类型原样借用），在其上补两件事：
///   1. 逐指令逐槽的到达定义查询面（MethodDefUse 只交付边表，不交付
///      任意槽位的到达集查询）；
///   2. DUP 族的补充使用边——#115 §2.2 口径「复制槽不产定义、产使用边」：
///      dup 族复制出的槽不产生新定义（域语义本就如此），但按使用边记账
///      （def_use 边语义里 dup 族不算使用，本视图按切面口径补齐；SWAP
///      只重排不复制，不补边）。
///
/// 「切面槽干净」（§3.4）：该槽 provenance 唯一——到达集恰一且不溢出。
/// 负向结论纪律沿用 slot_def_use 头注纪律：truncated() 为真时「唯一/无使用」类
/// 结论不可信，消费方必须先把关。
///
/// 结果值语义、自持有（同 MethodDefUse：InsnId 句柄在结果存活期内保持
/// 原方法身份语义）；方法体结构变更后重建。数据流失败时 ok()=false，
/// 全部查询退化为空集/false。
#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"

namespace mjbase::asm_sem {

/// @brief 切面槽 provenance 视图（值语义、自持有）。
class SlotProvenanceView
{
public:
    /// @brief 分析方法（按签名播种入参；不修改 unit）。
    ///
    /// 两次引擎遍历同源装配：MethodDefUse（边/访问事实，def_use 既有
    /// 语义零漂移）+ DomainEngine<DefUseValue> 保真帧（逐槽查询面）。
    /// 方法体规模下重建成本可接受（#115 §2.1 同 asm/cfg 口径）。
    static SlotProvenanceView build(const jbc::core::ConstantPool& cp,
                                    const jbc::ir::CodeUnit& u,
                                    const jbc::analysis::MethodSig& sig);

    /// 数据流是否成功；false 时 edges/localAccesses 为空、槽查询退化空集。
    bool ok() const noexcept { return analyzed_; }

    /// 任一到达集溢出过（def_use 边 + DUP 补充边合计）；负向结论须先把关。
    bool truncated() const noexcept { return truncated_; }

    /// 引擎 summary（失败码/熔断诊断）。
    const jbc::analysis::FrameSummary& summary() const noexcept { return summary_; }

    /// 指令是否可达（死代码无到达帧；无效句柄 false）。
    bool reachable(jbc::ir::InsnId id) const noexcept { return frameOf(id) != nullptr; }

    /// @brief 到达帧的栈上「值」个数（cat2 垫底行不计）。不可达返回 0。
    ca::u32 stackValueCount(jbc::ir::InsnId id) const noexcept;

    /// @brief 栈槽到达定义（fromTop 按值计：0 = 栈顶值，cat2 底槽不计）。
    /// @return 空集：不可达 / 越过栈深。
    mjbase::asm_analyzer::DefSet stackDefsAt(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept;

    /// @brief 局部槽到达定义。空集：不可达 / 越界 / cat2 垫底行。
    mjbase::asm_analyzer::DefSet localDefsAt(jbc::ir::InsnId id, ca::u32 local) const noexcept;

    /// @brief 切面槽「干净」：到达定义恰一且不溢出（#115 §3.4）。
    bool stackSlotClean(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept;
    bool localSlotClean(jbc::ir::InsnId id, ca::u32 local) const noexcept;

    /// def→use 边表（def_use 边 + DUP 族补充边；构建序，构建后索引）。
    const std::vector<mjbase::asm_analyzer::DefUseEdge>& edges() const noexcept { return edges_; }

    /// 指向 def 的全部使用边（结果内存中的视图，随本对象存活）。
    std::vector<const mjbase::asm_analyzer::DefUseEdge*> usesOf(mjbase::asm_analyzer::DefId def) const;

    /// 指向该指令的全部使用边。
    std::vector<const mjbase::asm_analyzer::DefUseEdge*> usesAt(jbc::ir::InsnId use) const;

    /// 局部槽访问记录（def_use 既有语义：读/写各一条，iinc 两条）。
    const std::vector<mjbase::asm_analyzer::LocalAccess>& localAccesses() const noexcept
    {
        return localAccesses_;
    }

    /// 定义指令的完整句柄；Parameter/未知返回 nullptr。
    const jbc::ir::InsnId* instructionOf(mjbase::asm_analyzer::DefId def) const;

private:
    const jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>* frameOf(jbc::ir::InsnId id) const noexcept;

    /// DUP 族（dup/dup_x1/dup_x2/dup2/dup2_x1/dup2_x2）的补充使用边：
    /// 被复制槽位（顶部 1/2 槽）承载的到达定义 → 本指令。
    void appendDupEdges(const jbc::ir::CodeUnit& u,
                        const std::vector<jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>>& frames);

    jbc::analysis::FrameSummary summary_{};
    bool analyzed_ = false;
    bool truncated_ = false;
    std::vector<jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>> frames_;   ///< 与 dom.order() 对齐
    std::unordered_map<ca::u32, ca::usize> posOf_;             ///< InsnId.slot → frames 下标
    std::vector<mjbase::asm_analyzer::DefUseEdge> edges_;
    std::vector<mjbase::asm_analyzer::LocalAccess> localAccesses_;
    std::unordered_map<ca::u64, std::vector<ca::u32>> edgesByDef_;
    std::unordered_map<ca::u32, std::vector<ca::u32>> edgesByUse_;
    std::unordered_map<ca::u32, jbc::ir::InsnId> insnBySlot_;
};

}   // namespace mjbase::asm_sem
