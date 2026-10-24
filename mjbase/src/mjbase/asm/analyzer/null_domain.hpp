/// @file        null_domain.hpp
/// @brief       可空性域（nullness 子集）：NullValue 格 + NullDomain + 查询入口。
///
/// 本域是 SimpleVerifier 形态的完整类型格机的 nullness 事实子集：
///
/// - 格为 Nullability 四态（NOT_NULL/NULL_/NULLABLE/UNKNOWN），合并
///   代数为 NullabilityUtil::merge 的 join 语义（NotNull+Null→Nullable，
///   Nullable 对事实吸收）。一处显式重排：旧格把 UNKNOWN 当 join 单位元
///   （乐观合并，依赖分支细化兜底），引擎约定 top 必须是吸收元——本格把
///   Unknown 升为顶（未知产点与任何事实合并为未知），数据流层面可靠。
/// - 产点事实：ACONST_NULL → Null；LDC 字符串 → NotNull；
///   其余（含 NEW——不给事实）→ Unknown。
///
/// 范围说明：类类型载荷与类层级合并
/// （isAssignableFrom/getSuperClass）不在本域——libjbc 分析层是形状级，类层级
/// 属 mjbase hierarchy 模块；IFNULL/IFNONNULL 分支细化不在本域——
/// DomainEngine 无分支条件钩子；aaload 数组元素 nullness（getElementValue）
/// 属堆建模，同不在本域。
#pragma once

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "frame_query.hpp"

namespace mjbase::asm_analyzer {

/// @brief nullness 格层级：Unknown 为顶（join 吸收元），Nullable 次之，
///        NotNull/Null 为互异事实。
enum class NullState : ca::u8 {
    NotNull = 0,   ///< 必非空（本子集内仅 ldc 字符串产生）
    Null    = 1,   ///< 必空（aconst_null）
    Nullable = 2,  ///< 两路矛盾（NotNull 与 Null 汇合）；对事实吸收
    Unknown = 3,   ///< 未知（顶）：未知产点，或与未知汇合
};

/// @brief nullness 格值（槽宽并入，cat2 布局在合法汇合下不塌）。
struct NullValue
{
    NullState s = NullState::Unknown;
    ca::u8 slots = 1;

    static NullValue of(NullState st, ca::u8 width = 1) noexcept { return NullValue{st, width}; }
    /// 按引擎结果形状构造未知值（long/double 双槽）。
    static NullValue unknownOfShape(jbc::analysis::VKind shape) noexcept
    {
        return of(NullState::Unknown,
                  shape == jbc::analysis::VKind::Long || shape == jbc::analysis::VKind::Double
                      ? 2
                      : 1);
    }
};

/// @brief nullness 域（IValueDomain<NullValue>）。
///
/// 不追类类型，只答空非空三态问题；loadLocal/storeLocal 用引擎默认恒等
/// （事实穿透 store→load）。持有常量池引用（LDC 判 String 载荷），cp 须活过
/// 全部 analyze 调用。
class NullDomain final : public jbc::analysis::IValueDomain<NullValue>
{
public:
    explicit NullDomain(const jbc::core::ConstantPool& cp) noexcept
        : cp_(&cp)
    {}

    NullValue top() override { return NullValue::of(NullState::Unknown); }

    /// join：同事实保持；NotNull 与 Null → Nullable；Nullable 吸收事实；
    /// Unknown 吸收一切（顶）。
    NullValue merge(const NullValue& a, const NullValue& b) override;

    /// 产值点：ACONST_NULL → Null；LDC/LDC_W 字符串 → NotNull；其余未知。
    NullValue newValue(const jbc::ir::Insn& producer, jbc::ir::InsnId at,
                       jbc::analysis::VKind shape) override;

    /// 方法入参：未知。
    NullValue newParameter(ca::u32 localIdx, jbc::analysis::VKind shape) override;

    /// 异常处理器入口 throwable：未知。
    NullValue newException(const jbc::ir::CodeUnit::Exc& entry) override;

    ca::u8 width(const NullValue& v) override { return v.slots; }

    bool equal(const NullValue& a, const NullValue& b) override;

private:
    const jbc::core::ConstantPool* cp_;
};

/// @brief nullness 分析结果：FrameQuery 查询面（载荷自持，无外部借用）。
class NullResult
{
public:
    NullResult(FrameQuery<NullValue> q)
        : q_(std::move(q))
    {}

    const jbc::analysis::FrameSummary& summary() const noexcept { return q_.summary(); }
    bool ok() const noexcept { return q_.ok(); }
    bool reachable(jbc::ir::InsnId id) const noexcept { return q_.reachable(id); }
    /// @copydoc FrameQuery::localAt
    const NullValue* localAt(jbc::ir::InsnId id, ca::u32 idx) const noexcept
    {
        return q_.localAt(id, idx);
    }
    /// @copydoc FrameQuery::stackFromTop
    const NullValue* stackFromTop(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept
    {
        return q_.stackFromTop(id, fromTop);
    }

private:
    FrameQuery<NullValue> q_;
};

/// @brief nullness 分析（不播种入参）。
NullResult analyzeNullness(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u);

/// @brief nullness 分析（按签名播种入参：未知，long/double 占双槽）。
NullResult analyzeNullness(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                           const jbc::analysis::MethodSig& sig);

}   // namespace mjbase::asm_analyzer
