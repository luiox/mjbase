/// @file        origin_domain.hpp
/// @brief       来源追踪域：OriginValue（定义指令/入参/未知）+ OriginDomain + 查询入口。
///
/// 旧栈对应物：mbase/asm/analyzer 的 original_source_interpreter
/// （OriginalSourceValue + OriginalSourceInterpreter）。迁移纪律是语义照旧、
/// 实现重写：本域回答「这个槽里的值最初由谁定义」——定义指令句柄、入参
/// 身份、或未知；控制流/槽迁移/入参播种全部交给 jbc::analysis::DomainEngine。
///
/// 与旧实现的范围差（代表性子集，见迁移报告）：
/// - 旧值同时携带 producer 指令集（合并取并集）、copied-from 链与常量载荷；
///   本域收窄为单一 origin 事实：不同来源汇合即 Unknown（单来源域的正确
///   退化，与旧 merge「producer 并集」相对——多来源问题不再可答）。
/// - 常量事实不携带：需要常量时并行跑 ConstDomain（域引擎的换域即插即用
///   正是为此设计）。
/// - origin 穿透 store→load 与栈复制（dup/swap/load 本地复制）保留——这是
///   旧 copiedFrom 链回答的核心问题。
#pragma once

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "frame_query.hpp"

namespace mjbase::asm_analyzer {

/// @brief 来源格值：单一 origin 事实。
///
/// kind==Insn 时 `insn` 是值的定义指令句柄（引擎按产值点传入，含运算指令
/// ——运算结果的来源是本条指令，与旧实现 unary/binary 的 producer 语义一致）；
/// kind==Param 时 `local` 是入参所在的局部变量槽位（实例方法 this 占 0 号；
/// 旧实现的 parameterLocal 同语义——宽参数的「第几个参数」需调用方按描述符
/// 自行换算，槽位即入参的唯一稳定身份）。
struct OriginValue
{
    enum class Kind : ca::u8 {
        Unknown = 0,   ///< 未知（冲突合并的退化；top）
        Insn,          ///< 定义指令句柄
        Param,         ///< 方法入参（槽位身份）
    };

    Kind kind = Kind::Unknown;
    ca::u8 slots = 1;             ///< 槽宽（long/double 载荷为 2，所有层级保留）
    jbc::ir::InsnId insn;         ///< kind==Insn
    ca::u32 local = 0;            ///< kind==Param：入参局部槽位

    static OriginValue unknownOf(ca::u8 width = 1) noexcept
    {
        return OriginValue{Kind::Unknown, width, jbc::ir::kNoInsn, 0};
    }
    static OriginValue ofInsn(jbc::ir::InsnId id, ca::u8 width = 1) noexcept
    {
        return OriginValue{Kind::Insn, width, id, 0};
    }
    static OriginValue ofParam(ca::u32 localIdx, ca::u8 width = 1) noexcept
    {
        return OriginValue{Kind::Param, width, jbc::ir::kNoInsn, localIdx};
    }

    static OriginValue unknownOfShape(jbc::analysis::VKind shape) noexcept
    {
        return unknownOf(shape == jbc::analysis::VKind::Long || shape == jbc::analysis::VKind::Double
                             ? 2
                             : 1);
    }

    bool isUnknown() const noexcept { return kind == Kind::Unknown; }
    bool isInsn() const noexcept { return kind == Kind::Insn; }
    bool isParam() const noexcept { return kind == Kind::Param; }
};

/// @brief 来源追踪域（IValueDomain<OriginValue>）。
///
/// 一切产值点（含运算结果，经引擎默认 binary/unary 转发 newValue）的来源
/// 是该指令；loadLocal/storeLocal 用引擎默认恒等——来源穿透 store→load
/// 与栈复制。不读常量池（构造仍收 cp 仅供 DomainEngine 接线：ldc/get/put/
/// invoke 的形状效应查池，方法体含这些指令时须传真池）。
class OriginDomain final : public jbc::analysis::IValueDomain<OriginValue>
{
public:
    OriginDomain() noexcept = default;

    OriginValue top() override { return OriginValue::unknownOf(); }

    /// join：同一事实（同句柄/同入参槽位且同宽）保持；不同事实 → Unknown
    /// （宽度尽量保留，cat2 槽布局在合法汇合下不塌）。
    OriginValue merge(const OriginValue& a, const OriginValue& b) override;

    /// 一切产值点：来源 = 产生该值的指令。
    OriginValue newValue(const jbc::ir::Insn& producer, jbc::ir::InsnId at,
                         jbc::analysis::VKind shape) override;

    /// 方法入参：Param 身份（槽位号）。
    OriginValue newParameter(ca::u32 localIdx, jbc::analysis::VKind shape) override;

    /// 异常处理器入口 throwable：未知来源。
    OriginValue newException(const jbc::ir::CodeUnit::Exc& entry) override;

    ca::u8 width(const OriginValue& v) override { return v.slots; }

    bool equal(const OriginValue& a, const OriginValue& b) override;
};

/// @brief 来源追踪结果：FrameQuery 查询面（载荷自持，无外部借用）。
class OriginResult
{
public:
    OriginResult(FrameQuery<OriginValue> q)
        : q_(std::move(q))
    {}

    const jbc::analysis::FrameSummary& summary() const noexcept { return q_.summary(); }
    bool ok() const noexcept { return q_.ok(); }
    bool reachable(jbc::ir::InsnId id) const noexcept { return q_.reachable(id); }
    /// @copydoc FrameQuery::localAt
    const OriginValue* localAt(jbc::ir::InsnId id, ca::u32 idx) const noexcept
    {
        return q_.localAt(id, idx);
    }
    /// @copydoc FrameQuery::stackFromTop
    const OriginValue* stackFromTop(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept
    {
        return q_.stackFromTop(id, fromTop);
    }

private:
    FrameQuery<OriginValue> q_;
};

/// @brief 来源追踪（不播种入参）。
/// @param cp 仅供引擎接线（本域不读）；方法体含 ldc/get/put/invoke 时须传
///        对应常量池，纯局部/常量/分支方法体可传空池。
OriginResult traceOrigins(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u);

/// @brief 来源追踪（按签名播种入参：Param 身份，long/double 占双槽）。
OriginResult traceOrigins(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                          const jbc::analysis::MethodSig& sig);

}   // namespace mjbase::asm_analyzer
