/// @file        rename_engine.hpp
/// @brief       统一 renamer 编排引擎：建议流 → 合法性过滤 → 裁决 → override
///              闭包 → 包展开 → 后缀消解 → 计划 + 审计记录。
///
/// 流水线与裁决语义逐阶段照旧：
///
///   收集建议流 → 合法性过滤（复用 IdentifierUtil，`<init>`/`<clinit>` 豁免，
///   内部名分段）→ 同 target confidence 裁决（同 confidence 先到先得）→ 同
///   override 组件按规范根去重 → method override 闭包整组同改 → package 前缀
///   展开（all-or-reject）→ 残余撞名 `_N` 后缀消解（方法组统一后缀，0..64 无
///   一致后缀整组拒）→ 构造 RenamePlan（类映射先行，成员规则 newOwner/newDesc
///   预映像）+ NameChangeRecord → plan-only。
///
/// fail-closed：schema/建议错误逐条拒并记录不中断。
///
/// 应用面（step 8）：planOnly=false 且计划非空时，引擎计划按登记序搬运成
/// asm/remapper 域的 MemberRemapPlan，经 applyResourceRemapPlan+RemapPolicy
/// 就地应用到容器；应用零变化（逐类失败被
/// remapper 面按类跳过后的聚合信号）为 ApplyFailed + ok=false；
/// planOnly=false 且计划为空保持旧栈 zero-applied 成功口径。容器入参因此收
/// 非 const&（应用路径就地写回）。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/mapping/mapping_format_registry.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/renamer/name_proposal.hpp"
#include "mjbase/renamer/name_provider.hpp"
#include "mjbase/renamer/rename_plan.hpp"

namespace mjbase::renamer {

/// @brief 引擎运行选项（plan-only 门 + 应用面门控）。
struct RenamerOptions
{
    /// true（默认）：只构造 RenamePlan 与审计记录，不改容器——最安全口径。
    /// false：计划非空时经 asm/remapper 域 applyResourceRemapPlan 就地应用。
    bool planOnly = true;
    /// 应用面门控（bytecode/reflectionStrings/annotations/signatures/resources），
    /// planOnly=false 时生效。默认 RemapPolicy{} 只改字节码面；类资源 URI 随
    /// 类改名移动需显式开 resources——remapper 批双门口径（bytecode+resources
    /// 才换 URI），与旧栈 URI 单随 bytecode 门的行为差异为 remapper 批已锁定
    /// 的语义面差异，renamer 应用面继承。
    mjbase::asm_remapper::RemapPolicy policy{};
};

/// @brief 一次 run 的计数摘要。
struct RenameRunStats
{
    ca::usize proposed       = 0;   ///< 收到的建议总数（各 provider 流合计）
    ca::usize accepted       = 0;   ///< 通过裁决的 proposal 数（不含闭包展开）
    ca::usize rejected       = 0;   ///< 被拒 proposal 数（含 provider 级失败）
    ca::usize renamedMembers = 0;   ///< 展开后落 plan 的改名条目数（类+字段+方法闭包成员）
    ca::usize suffixResolved = 0;   ///< `_N` 后缀消解次数
};

/// @brief 一条已接受改名的溯源条目（驻留视图；寿命 = 进程别名池）。
struct AcceptedChange
{
    ProposalKind     kind = ProposalKind::Class;
    std::string_view owner{};
    std::string_view oldName{};
    std::string_view desc{};
    std::string_view newName{};
    std::string_view source{};
    double           confidence  = 0.0;
    bool             fromClosure = false;   ///< override 闭包整组展开出来的成员（非主建议）
    std::string_view rationale{};
    std::string_view evidence{};
};

/// @brief 一条被拒建议的流水条目（完整建议原文 + 原因 + 细节）。
struct RejectedProposal
{
    NameProposal     proposal;
    RejectionReason  reason = RejectionReason::None;
    std::string      detail;
};

/// @brief 改名审计记录：IntermediateMapping 底座 + source/confidence 溯源 +
///        被拒流水。
///
/// 映射底座可经 MappingFormatRegistry 导出（Tiny-V2 往返恒等为验收基准）；
/// 溯源与被拒流水随记录对象在内存中供审计。别名规则（继承字段/闭世界继承
/// 方法）只入 plan，不入本记录——与旧栈口径一致。
class NameChangeRecord
{
public:
    NameChangeRecord() = default;
    NameChangeRecord(NameChangeRecord&&)                 = default;
    NameChangeRecord& operator=(NameChangeRecord&&)      = default;
    NameChangeRecord(const NameChangeRecord&)            = delete;
    NameChangeRecord& operator=(const NameChangeRecord&) = delete;

    /// @brief 映射底座（接受条目按 kind 落 package/class/field/method 四类映射）。
    const mjbase::mapping::IntermediateMapping& mapping() const { return mapping_; }

    const std::vector<AcceptedChange>&   accepted() const { return accepted_; }
    const std::vector<RejectedProposal>& rejected() const { return rejected_; }

    /// @brief 经注册格式导出映射文本（如 "Tiny-V2"）；未注册格式 Err。
    ca::core::Result<ca::str::Utf8String, std::string> exportMapping(
        std::string_view formatName, const mjbase::mapping::MappingFormatRegistry& registry) const;

    /// @brief 静态导出通道：任意 IntermediateMapping 底座复用同一导出路径
    ///        （外部记录经此导出；语义与实例方法完全一致）。
    static ca::core::Result<ca::str::Utf8String, std::string> exportMapping(
        const mjbase::mapping::IntermediateMapping& mapping, std::string_view formatName,
        const mjbase::mapping::MappingFormatRegistry& registry);

private:
    // 以下 mutator 仅 RenamerEngine 填充。
    friend class RenamerEngine;

    mjbase::mapping::IntermediateMapping& mappingMut() { return mapping_; }
    void                                  addAccepted(AcceptedChange change);
    void addRejected(NameProposal proposal, RejectionReason reason, std::string detail);

    mjbase::mapping::IntermediateMapping mapping_;
    std::vector<AcceptedChange>          accepted_;
    std::vector<RejectedProposal>        rejected_;
};

/// @brief 一次 run 的产物：成败 + 错误消息 + 计划 + 审计记录 + 计数。
///
/// ok=false 表示 run 失败（context 构建失败或应用面失败）；plan/record 仍随
/// 结果交付供排查。旧栈 ca::Status 通道换 ok+error 值对（mjbase Result 值语义
/// 纪律，不引 Status）。
struct RenamerRunResult
{
    bool            ok     = false;
    std::string     error;          ///< ok=false 时的失败原因
    bool            applied = false;   ///< 应用面成功落地（含 zero-applied）
    RenameRunStats  stats;
    RenamePlan      plan;
    NameChangeRecord record;
};

/// @brief 统一 renamer 编排引擎（管线见文件头）。
class RenamerEngine
{
public:
    /// @brief 对 container 跑一轮建议收集、计划构造与（可选）应用。
    ///
    /// @param container 目标容器；plan-only 时只读，应用模式下就地写回。
    /// @param providers 建议来源列表，按序收集（到达序参与 tie-break）。
    RenamerRunResult run(mjbase::model::ResourceContainer& container,
                         const std::vector<NameProvider*>& providers,
                         const RenamerOptions&             options);
};

}   // namespace mjbase::renamer
