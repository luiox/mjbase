/// @file        def_use.hpp
/// @brief       方法内 slot 级 def-use：入参/指令定义标号、load/store 边、
///              栈使用边与字段访问事实。
///
/// 与 origin_domain 的分工：来源追踪回答「值最初由谁产生」，多来源汇合后退化
/// 为未知；本模块保留全部到达定义（有界集合），因此能回答「这个槽上的哪些
/// 写会到达这次读」——死存储、写后未读、分支合并后的多定义都以此为据。
///
/// 定义语义：
/// - 局部槽的定义是写入它的指令（store 家族与 iinc），入参由 MethodSig 播种为
///   Parameter 定义。store 覆盖旧值（不保留来源链——来源问题归 origin_domain）。
/// - 栈值的定义是产生它的指令；load 为恒等搬运，把局部槽的到达定义带进栈。
/// - dup 族与 swap 只复制/重排槽，不算使用；pop 弹值丢弃，算使用。
/// - 异常边采用引擎精确档：handler 入口 = 各抛出点 in-state locals 的合并，
///   死 try 范围的 handler 不可达。
///
/// 集合上界 kMaxDefs：溢出置 truncated，边照常交付但不完整。负向结论
/// （「没有使用」⇒ 可删）必须同时确认 truncated 为假；消费者用 ok() /
/// truncated() 把关。
///
/// 结果自持有（含字段文本的 KeyPool），构造后 CodeUnit、域与引擎均可释放；
/// InsnId 句柄在结果存活期内保持原方法的身份语义。
#pragma once

#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/keys.hpp"

namespace mjbase::asm_analyzer {

/// @brief 一个到达定义的稳定标号。
///
/// Instruction 的 `id` 是 InsnId::slot（同一 CodeUnit 内唯一；完整句柄经
/// MethodDefUse::instructionOf 解析）；Parameter 的 `id` 是入参局部槽位
/// （实例方法 this 为 0，与 MethodSig 播种一致）。
struct DefId
{
    enum class Kind : ca::u8 {
        None = 0,   ///< 无效标号（DefSet 不存此值）
        Parameter,
        Instruction,
    };

    Kind     kind = Kind::None;
    ca::u32  id   = 0;

    static DefId parameter(ca::u32 local) noexcept { return {Kind::Parameter, local}; }
    static DefId instruction(jbc::ir::InsnId insn) noexcept
    {
        return {Kind::Instruction, insn.slot};
    }

    bool valid() const noexcept { return kind != Kind::None; }
    bool operator==(const DefId& o) const noexcept { return kind == o.kind && id == o.id; }
    bool operator!=(const DefId& o) const noexcept { return !(*this == o); }
    bool operator<(const DefId& o) const noexcept
    {
        return kind != o.kind ? kind < o.kind : id < o.id;
    }
};

/// @brief 有界到达定义集合（有序、去重）。
///
/// 排序让 merge/equal 与遍历结果确定；上限防止大方法/环路上的集合无界增长。
class DefSet
{
public:
    static constexpr ca::u32 kMaxDefs = 16;

    ca::u32 size() const noexcept { return size_; }
    bool    empty() const noexcept { return size_ == 0; }
    DefId   at(ca::u32 i) const noexcept { return i < size_ ? defs_[i] : DefId{}; }
    bool    contains(DefId d) const noexcept;
    /// 是否因超过 kMaxDefs 丢弃了定义；true 时集合不完整。
    bool    truncated() const noexcept { return truncated_; }

    /// 插入一个定义；已存在返回 false。溢出时置 truncated。
    bool add(DefId d) noexcept;
    /// 标记集合不完整（合并 truncated 来源时用）。
    void markTruncated() noexcept;

    bool operator==(const DefSet& o) const noexcept;
    bool operator!=(const DefSet& o) const noexcept { return !(*this == o); }

private:
    DefId   defs_[kMaxDefs]{};
    ca::u32 size_      = 0;
    bool    truncated_ = false;
};

/// @brief def-use 域值：到达定义集合 + 槽宽。
struct DefUseValue
{
    DefSet defs;
    ca::u8 slots = 1;

    static DefUseValue unknownOf(ca::u8 width = 1) noexcept { return DefUseValue{DefSet{}, width}; }
    static DefUseValue ofParam(ca::u32 local, ca::u8 width = 1) noexcept
    {
        DefUseValue v;
        v.defs.add(DefId::parameter(local));
        v.slots = width;
        return v;
    }
    static DefUseValue ofInsn(jbc::ir::InsnId insn, ca::u8 width = 1) noexcept
    {
        DefUseValue v;
        v.defs.add(DefId::instruction(insn));
        v.slots = width;
        return v;
    }

    bool isUnknown() const noexcept { return defs.empty(); }
};

/// @brief def-use 域（IValueDomain<DefUseValue>）。
///
/// 局部写把值重定义为该 store 指令（宽度按 store opcode 归一——cat2 的
/// 槽布局不能依赖栈上旧值的宽度）；load 走默认恒等，把定义的到达集合带进
/// 栈。未写过的局部 load 由引擎回退 newValue，本域识别 load 族并返回空集合
/// （没有可追踪的定义），而不是把 load 自身当定义。
class DefUseDomain final : public jbc::analysis::IValueDomain<DefUseValue>
{
public:
    DefUseDomain() noexcept = default;

    DefUseValue top() override { return DefUseValue{}; }

    /// 并集（truncated 取或）；宽度取宽者——合法汇合下宽度一致。
    DefUseValue merge(const DefUseValue& a, const DefUseValue& b) override;

    DefUseValue newValue(const jbc::ir::Insn& producer, jbc::ir::InsnId at,
                         jbc::analysis::VKind shape) override;

    DefUseValue newParameter(ca::u32 localIdx, jbc::analysis::VKind shape) override;

    /// throwable 不是指令/入参定义的；入口栈值无到达定义。
    DefUseValue newException(const jbc::ir::CodeUnit::Exc& entry) override;

    /// 局部写：定义 = 本 store（IINC 同路径），宽度按 opcode 归一。
    DefUseValue storeLocal(const jbc::ir::Insn& store, jbc::ir::InsnId id,
                           const DefUseValue& v, ca::u32 idx) override;

    ca::u8 width(const DefUseValue& v) override { return v.slots; }

    bool equal(const DefUseValue& a, const DefUseValue& b) override;

    /// handler 由 try 范围抛出点的出口边惰性并入（保守合并各抛出点 locals）。
    bool preciseExceptionEdges() const override { return true; }
};

/// @brief 使用位置：操作数栈值或局部槽。
enum class DefUseSlot : ca::u8
{
    Stack,
    Local,
};

/// @brief 一条 def→use 边。
struct DefUseEdge
{
    DefId             def;
    jbc::ir::InsnId   use;
    DefUseSlot        slot  = DefUseSlot::Stack;
    /// Stack：从栈顶起第几个**值**（0 = 栈顶值，cat2 底槽不计）；
    /// Local：局部槽号。
    ca::u32           index = 0;
    /// 该使用点的到达集合溢出：边存在但可能缺定义。
    bool              truncated = false;
};

/// @brief 局部槽访问记录（读/写各一条；iinc 两条）。
struct LocalAccess
{
    jbc::ir::InsnId  insn;
    ca::u32          local = 0;
    enum class Kind : ca::u8
    {
        Read,
        Write,
    } kind = Kind::Read;
    /// Read：到达定义集合；Write：{本指令}。
    DefSet defs;
};

/// @brief 字段访问事实（get/put 家族）。
///
/// owner/name/desc 是结果自有池的驻留视图，随 MethodDefUse 存活。
struct FieldAccess
{
    jbc::ir::InsnId  insn;
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
    bool             write    = false;   ///< false = 读
    bool             isStatic = false;
    /// getfield/putfield 的接收者到达定义。
    DefSet objectDefs;
    /// put* 的存入值到达定义。
    DefSet valueDefs;

    bool truncated() const noexcept
    {
        return objectDefs.truncated() || valueDefs.truncated();
    }
};

/// @brief 单方法 def-use 结果（帧数据流 + 边表 + 字段访问事实）。
///
/// 只在数据流成功（ok()）且可达的指令上产事实；不可达指令与失败方法不产
/// 部分边（字段访问同样不产——不可达字段访问由 ReferenceIndex 的全量扫描面
/// 覆盖）。
class MethodDefUse
{
public:
    MethodDefUse() = default;

    const jbc::analysis::FrameSummary& summary() const noexcept { return summary_; }
    /// 数据流是否成功；false 时 edges/localAccesses/fieldAccesses 为空。
    bool ok() const noexcept { return summary_.ok && analyzed_; }

    const std::vector<DefUseEdge>& edges() const noexcept { return edges_; }
    const std::vector<LocalAccess>& localAccesses() const noexcept { return localAccesses_; }
    const std::vector<FieldAccess>& fieldAccesses() const noexcept { return fieldAccesses_; }

    /// 指向 def 的全部使用边（结果内存中的视图，随本对象存活）。
    std::vector<const DefUseEdge*> usesOf(DefId def) const;
    /// 指向该指令的全部使用边。
    std::vector<const DefUseEdge*> usesAt(jbc::ir::InsnId use) const;
    /// 局部槽的全部读访问。
    std::vector<const LocalAccess*> readsOfLocal(ca::u32 local) const;

    /// 定义指令的完整句柄；Parameter/未知返回 nullptr。
    const jbc::ir::InsnId* instructionOf(DefId def) const;

    /// 字段访问文本的驻留池。
    const KeyPool& pool() const noexcept { return pool_; }

    /// 任一使用点的到达集合溢出过（负向结论需谨慎）。
    bool truncated() const noexcept { return truncated_; }
    /// 有字段引用无法解析（CP 畸形）；字段事实不完整。
    bool hasUnresolvedFieldRefs() const noexcept { return unresolvedFieldRefs_; }

    ca::usize edgeCount() const noexcept { return edges_.size(); }

private:
    friend MethodDefUse analyzeDefUse(const jbc::core::ConstantPool& cp,
                                      const jbc::ir::CodeUnit& u);
    friend MethodDefUse analyzeDefUse(const jbc::core::ConstantPool& cp,
                                      const jbc::ir::CodeUnit& u,
                                      const jbc::analysis::MethodSig& sig);
    MethodDefUse(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                 const jbc::analysis::MethodSig* sig);

    jbc::analysis::FrameSummary summary_;
    bool                      analyzed_ = false;
    std::vector<DefUseEdge>   edges_;
    std::vector<LocalAccess>  localAccesses_;
    std::vector<FieldAccess>  fieldAccesses_;
    std::unordered_map<ca::u64, std::vector<ca::u32>> edgesByDef_;
    std::unordered_map<ca::u32, std::vector<ca::u32>> edgesByUse_;
    std::unordered_map<ca::u32, jbc::ir::InsnId>      insnBySlot_;
    KeyPool                   pool_;
    bool                      truncated_ = false;
    bool                      unresolvedFieldRefs_ = false;
};

/// @brief def-use 分析（不播种入参：入参局部槽视为无定义）。
MethodDefUse analyzeDefUse(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u);

/// @brief def-use 分析（按签名播种入参，long/double 占双槽）。
MethodDefUse analyzeDefUse(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                           const jbc::analysis::MethodSig& sig);

}   // namespace mjbase::asm_analyzer
