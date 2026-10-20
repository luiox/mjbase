/// @file        const_domain.hpp
/// @brief       常量传播域：ConstLattice 格 + ConstDomain 钩子 + 查询入口。
///
/// 旧栈对应物：asm/analyzer 的 constant_interpreter（ConstantValue +
/// ConstantInterpreter）。迁移纪律是语义照旧、实现重写：格、JVM 折叠语义与
/// 合并规则按旧实现的公开语义移植；控制流 worklist、槽迁移、入参播种调度
/// 全部交给 jbc::analysis::DomainEngine，本域只供给值。
///
/// 与旧实现的范围差（代表性子集，见迁移报告）：
/// - 载荷收窄为 i64（int 符号扩展 / long）与引用常量（null / String CP 条目），
///   float/double 常量不折（旧实现经 ca::core::Any 载荷折全数值族）。
/// - 旧实现的数组建模（newarray/aaload/astore 常量数组）与 reference handler
///   注入（getfield/invoke 返回值）不迁：前者依赖旧栈 Any 载荷，后者属
///   classpath/引用索引层能力，待后续模块接入。
/// - 旧实现的入参值播种（args 向量）以常量种子形态保留：propagateConstants
///   的 seeds 重载按描述符把 ConstLattice 种子落到入参槽。旧栈的
///   short/char/bool 归一（normalizeIntValue）在新格层天然成立——int 宽度
///   只有一类 Int 载荷，种子直接以 ofInt 给出；未给出种子的参数仍为 Top，
///   形状/槽宽正确。
#pragma once

#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "frame_query.hpp"

namespace mjbase::asm_analyzer {

/// @brief 常量传播格值：Bottom ⊑ Const ⊑ Top，merge 为 join。
///
/// - Top：未知/不可折（吸收元）。未知产点（getfield、invoke 返回值等）与
///   冲突合并（两个不同常量汇合）都落到 Top；`slots` 同时承载宽度信息，
///   cat2 的未知值（long/double 入参）保持 slots=2，槽布局不塌。
/// - Const：已知常量。数值载荷为 i64（Int=符号扩展的 int，Long=long），
///   引用载荷为 null 或 CP String 条目索引。
/// - Bottom：不可达（单位元，merge(Bottom, x) = x）。引擎侧的死代码以
///   reachable=false 表达，不产生 Bottom；保留它是为了让格代数完整、调用
///   方可显式表达「该来源无约束」。
struct ConstLattice
{
    enum class K : ca::u8 {
        Bottom = 0,   ///< 不可达（join 单位元）
        Const  = 1,   ///< 已知常量
        Top    = 2,   ///< 未知/非常量（join 吸收元）
    };
    /// Const 载荷类别；Top 时仅 slots 有意义。
    enum class T : ca::u8 {
        None = 0,   ///< 无数值载荷（Top/Bottom，或引用载荷未细分时不用）
        Int,        ///< i：符号扩展的 int 常量（1 槽）
        Long,       ///< i：long 常量（2 槽）
        Null,       ///< aconst_null（1 槽）
        Str,        ///< cp：CP String 条目索引（1 槽）
    };

    K k = K::Top;
    T t = T::None;
    ca::u8 slots = 1;   ///< 槽宽（long/double 载荷为 2；所有层级保留）
    ca::i64 i = 0;      ///< Int/Long 载荷
    ca::u32 cp = 0;     ///< Str 的 CP String 条目索引

    static ConstLattice topOf(ca::u8 width = 1) noexcept { return ConstLattice{K::Top, T::None, width, 0, 0}; }
    static ConstLattice bottomOf(ca::u8 width = 1) noexcept
    {
        return ConstLattice{K::Bottom, T::None, width, 0, 0};
    }
    static ConstLattice ofInt(ca::i32 v) noexcept { return ConstLattice{K::Const, T::Int, 1, v, 0}; }
    static ConstLattice ofLong(ca::i64 v) noexcept { return ConstLattice{K::Const, T::Long, 2, v, 0}; }
    static ConstLattice ofNull() noexcept { return ConstLattice{K::Const, T::Null, 1, 0, 0}; }
    static ConstLattice ofStr(ca::u32 cpIdx) noexcept { return ConstLattice{K::Const, T::Str, 1, 0, cpIdx}; }

    /// 按引擎给出的结果形状构造未知值（cat2 保持双槽宽）。
    static ConstLattice unknownOf(jbc::analysis::VKind shape) noexcept
    {
        return topOf(shape == jbc::analysis::VKind::Long || shape == jbc::analysis::VKind::Double
                         ? 2
                         : 1);
    }

    bool isTop() const noexcept { return k == K::Top; }
    bool isBottom() const noexcept { return k == K::Bottom; }
    bool isConst() const noexcept { return k == K::Const; }
    bool isInt() const noexcept { return k == K::Const && t == T::Int; }
    bool isLong() const noexcept { return k == K::Const && t == T::Long; }
    bool isNull() const noexcept { return k == K::Const && t == T::Null; }
    bool isStr() const noexcept { return k == K::Const && t == T::Str; }

    /// 数值载荷（Int=符号扩展值 / Long）。仅 isInt()/isLong() 时有意义。
    ca::i64 asI64() const noexcept { return i; }
};

/// @brief 常量传播域（IValueDomain<ConstLattice>）。
///
/// 折叠语义照旧实现：整数/长整数加减乘与位运算遵循 JVM 补码环绕；MIN/-1
/// 的除法与余数遵循 JVM 特例；移位距离按低 5/6 位屏蔽；除零不折（运行时
/// 抛 ArithmeticException，数据流层面退回 Top）。loadLocal/storeLocal 用
/// 引擎默认恒等——常量穿透 store→load。
///
/// 持有常量池引用（ldc 族读 Integer/Long/String 载荷）；cp 须活过全部
/// analyze 调用（域与引擎均只借用，不拥有）。
class ConstDomain final : public jbc::analysis::IValueDomain<ConstLattice>
{
public:
    explicit ConstDomain(const jbc::core::ConstantPool& cp) noexcept
        : cp_(&cp)
    {}

    /// @brief 带入参常量种子的构造（移植旧栈 args 向量语义）。
    ///
    /// argSeeds 按描述符参数序给出（long/double 参数各占一项，种子宽度须与
    /// 参数类别一致——J/D 配 ofLong，其余配 1 槽种子）；种子按描述符落到
    /// local 槽（实例方法 this 占 0 号，cat2 参数占双槽），个数超出参数个数
    /// 的多余项忽略。种子经 newParameter 供给，槽宽由种子自报。
    ConstDomain(const jbc::core::ConstantPool& cp, const jbc::analysis::MethodSig& sig,
                std::vector<ConstLattice> argSeeds);

    ConstLattice top() override { return ConstLattice::topOf(); }

    /// join：Bottom 单位元；Top 吸收元；同载荷常量保持；不同常量 → Top
    /// （宽度尽量保留，cat2 槽布局在合法汇合下不塌）。
    ConstLattice merge(const ConstLattice& a, const ConstLattice& b) override;

    /// 一切产值点：常量族直接成格值；ldc 读常量池；其余产点未知。
    ConstLattice newValue(const jbc::ir::Insn& producer, jbc::ir::InsnId at,
                          jbc::analysis::VKind shape) override;

    /// 入参：有种子（构造时按描述符落槽）取种子，否则 Top（宽度按形状）。
    ConstLattice newParameter(ca::u32 localIdx, jbc::analysis::VKind shape) override;

    /// 异常处理器入口 throwable：未知引用。
    ConstLattice newException(const jbc::ir::CodeUnit::Exc& entry) override;

    /// 二元折叠：int/long 算术、位、移位、比较（LCMP）按 JVM 语义；
    /// 任一操作数非对应常量、或除零 → Top。
    ConstLattice binary(jbc::core::Op op, const ConstLattice& a, const ConstLattice& b,
                        jbc::analysis::VKind result, const jbc::ir::Insn& at,
                        jbc::ir::InsnId id) override;

    /// 一元折叠：取负与 int/long/窄化转换族；float/double 参与的转换不折。
    ConstLattice unary(jbc::core::Op op, const ConstLattice& a, jbc::analysis::VKind result,
                       const jbc::ir::Insn& at, jbc::ir::InsnId id) override;

    ca::u8 width(const ConstLattice& v) override { return v.slots; }

    /// fixpoint 相等：层级、宽度、载荷（Str 按字符串内容比较，容忍 CP 里
    /// 同值异条目）。
    bool equal(const ConstLattice& a, const ConstLattice& b) override;

    /// Str 常量的字符串内容（借用常量池；非 Str 返回空视图）。
    std::string_view strOf(const ConstLattice& v) const noexcept;

private:
    /// ldc/ldc_w/ldc2_w 的常量载荷（Integer/Long/String 成格值，其余未知）。
    ConstLattice ldcValue(const jbc::ir::Insn& in, jbc::analysis::VKind shape) const noexcept;

    /// 两个 CP String 条目是否同一字符串（先按索引短路，再按内容）。
    bool sameStrPayload(ca::u32 cpA, ca::u32 cpB) const noexcept;

    const jbc::core::ConstantPool* cp_;
    /// 入参常量种子：local 槽位 → 值（带入参构造时填充）。
    std::unordered_map<ca::u32, ConstLattice> seeds_;
};

/// @brief 常量传播结果：FrameQuery 查询面 + Str 内容解析。
///
/// Str 内容借用分析时的常量池：cp 须活过本结果的使用期。
class ConstResult
{
public:
    ConstResult(const jbc::core::ConstantPool& cp, FrameQuery<ConstLattice> q)
        : q_(std::move(q))
        , cp_(&cp)
    {}

    const jbc::analysis::FrameSummary& summary() const noexcept { return q_.summary(); }
    bool ok() const noexcept { return q_.ok(); }
    bool reachable(jbc::ir::InsnId id) const noexcept { return q_.reachable(id); }
    /// @copydoc FrameQuery::localAt
    const ConstLattice* localAt(jbc::ir::InsnId id, ca::u32 idx) const noexcept
    {
        return q_.localAt(id, idx);
    }
    /// @copydoc FrameQuery::stackFromTop
    const ConstLattice* stackFromTop(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept
    {
        return q_.stackFromTop(id, fromTop);
    }
    /// @brief Str 格值的字符串内容（借用常量池；非 Str 返回空视图）。
    std::string_view strOf(const ConstLattice& v) const noexcept;

private:
    FrameQuery<ConstLattice> q_;
    const jbc::core::ConstantPool* cp_;
};

/// @brief 常量传播（不播种入参；入参槽为空，load 退回 newValue）。
ConstResult propagateConstants(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u);

/// @brief 常量传播（按签名播种入参：值为 Top，形状/槽宽正确，long/double
///        占双槽——与 DomainEngine 的 MethodSig 播种协议一致）。
ConstResult propagateConstants(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                               const jbc::analysis::MethodSig& sig);

/// @brief 常量传播（按签名播种入参并给常量种子：argSeeds 按描述符参数序
///        落到入参槽，缺项/多余项的处理见 ConstDomain 带参构造）。
ConstResult propagateConstants(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                               const jbc::analysis::MethodSig& sig,
                               std::vector<ConstLattice> argSeeds);

}   // namespace mjbase::asm_analyzer
