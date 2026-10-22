/// @file        name_proposal.hpp
/// @brief       改名建议契约值对象：ProposalKind / RejectionReason / NameProposal。
///
/// 全部字段是 aliasInterned（进程级内容寻址池）
/// 的借用视图——内容相同 ⇔ 视图相同，本类型可拷贝（视图在池存活期稳定）。
/// 构造入口负责驻留，消费方不得绕过构造函数私填视图。
#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"

namespace mjbase::renamer {

/// @brief 建议改名的目标类别。
enum class ProposalKind : ca::u8
{
    Class,    ///< 类改名：oldName = 内部类名（a/b/C），owner 与 desc 留空
    Package,  ///< 包改名：oldName = 旧包路径（a/b），owner 与 desc 留空；引擎展开为逐类映射
    Field,    ///< 字段改名：owner + oldName + desc 必填
    Method,   ///< 方法改名：owner + oldName + desc 必填；引擎做 override 闭包整组同改
};

/// @brief 建议被拒原因（fail-closed 流水列）。
enum class RejectionReason : ca::u8
{
    None = 0,
    Schema,             ///< 契约字段缺失/越界：desc 缺失、confidence 出 [0,1]、空名、no-op 等
    IllegalIdentifier,  ///< 新名不是合法 Java 标识符 / 内部名分段非法
    Keyword,            ///< 新名命中 Java 关键字表
    ReservedName,       ///< `<init>`/`<clinit>` 豁免不改，也不可作为新名
    NotFound,           ///< 改名目标（类/包前缀/成员 owner）在容器中不存在
    Conflict,           ///< 同 target 裁决败者 / 残余撞名整组失败
    ProviderError,      ///< provider propose() 整体硬失败（该 provider 建议流全弃）
    ApplyFailed,        ///< 应用阶段失败（全量不落盘口径）
};

/// @brief RejectionReason 的稳定字符串标签（审计/日志用）。
std::string_view rejectionReasonName(RejectionReason reason);

/// @brief 一条改名建议（驻留视图值语义）。
///
/// 所有字符串字段经 aliasInterned 驻留：构造时 intern，拷贝廉价、等值比较
/// 退化为内容比较。视图寿命 = 进程别名池存活期（与 alias/remapper 消费面
/// 同一契约）。
class NameProposal
{
public:
    NameProposal() = default;

    /// @brief 全字段构造（rationale/evidence 留空）：字符串一律驻留。
    NameProposal(ProposalKind kind,
                 std::string_view owner,
                 std::string_view oldName,
                 std::string_view desc,
                 std::string_view newName,
                 double           confidence,
                 std::string_view source);

    /// @brief 带 rationale/evidence 的全量构造（AI 建议契约审计透传）。
    NameProposal(ProposalKind kind,
                 std::string_view owner,
                 std::string_view oldName,
                 std::string_view desc,
                 std::string_view newName,
                 double           confidence,
                 std::string_view source,
                 std::string_view rationale,
                 std::string_view evidence);

    /// 目标类别。
    ProposalKind kind = ProposalKind::Class;
    /// 字段/方法的 owner 内部类名；class/package 建议必须留空（oldName 即路径）。
    std::string_view owner{};
    /// 改名前名字：class=内部类名 / package=包路径 / field/method=成员名。
    std::string_view oldName{};
    /// 字段/方法描述符；class/package 建议必须留空。field/method 建议必填（exact 规则）。
    std::string_view desc{};
    /// 改名后名字。
    std::string_view newName{};
    /// 建议置信度，契约范围 [0,1]；越界按 Schema 拒。
    double confidence = 0.0;
    /// 来源标签（provider 名 / "ai:xxx" / "restore" 等）；为空时引擎填 provider 名。
    std::string_view source{};
    /// 建议理由（AI 建议契约 rationale 字段透传；可空，引擎只留痕不改写）。
    std::string_view rationale{};
    /// 证据（AI 建议契约 evidence 字段透传；可空，留痕口径同 rationale）。
    std::string_view evidence{};
};

}   // namespace mjbase::renamer
