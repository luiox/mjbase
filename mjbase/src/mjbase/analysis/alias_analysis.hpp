/// @file        alias_analysis.hpp
/// @brief       全程序别名分析：栈/局部别名事实 + 方法摘要 + 反射接收者身份
///              + Andersen 包含约束索引。
///
/// 旧栈对应物：mbase/analysis/alias_analysis（AliasAnalysis + AliasFlowInterpreter
/// + AndersenConstraintInterpreter，骑 jbytecode DataflowAnalyzer）。迁移纪律是
/// 语义照旧、实现重写：
///
/// - 值代数（AliasValue / mergeAliasValues / AliasFrame）逐语义直迁；身份串
///   一律经 aliasInterned 进程级驻留（内容相同 ⇔ 视图相同），事实形态
///   （"param:N"/"this:Owner"/"new:..."/"array:..."/"field:..."/"class:X"/
///   "string:S"）是 remapper/reference 消费契约，逐字保留——尤其 LDC 类常量
///   的 "class:X" 反射接收者身份。
/// - 方法内传播双轨（与旧实现同构）：无分支方法走线性走查（自带栈/局部
///   状态，操作数全程可见）；有分支方法以 libjbc DomainEngine 跑别名事实域
///   （AliasValue 即域载荷，join 即 mergeAliasValues），操作数依赖的产点
///   （GETFIELD/CHECKCAST/invoke 结果）经「探测帧注入」补齐——引擎产点钩子
///   看不到操作数栈，这是 libjbc DomainEngine 的能力缺口（挂账项），注入法
///   在操作数链不穿过 invoke/GETFIELD 结果的常规形状下与旧解释器逐事实等值。
/// - 跨类固定点：Jacobi 全量重算（旧 P1 memoized 版的轮序列与全量版逐方法
///   等值，memoization 是性能优化不迁）；摘要持调用点绑定 + 惰性展开
///   （旧 P2 语义照迁，测试钉死）。
/// - Andersen 字节码约束：约束图求解为纯算法件（andersen_points_to 直迁）；
///   节点身份 = 方法内临时（"method\ninsn:idx"）/形参/this/return/静态槽/
///   合并节点，与旧实现逐串一致。
///
/// 反射接收者追踪不依赖 mbase ReflectionInterpreter（依赖方向相反：后者是
/// alias 的消费方）；"class:X" 事实由 LDC 类常量产点直接维护。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/analysis/andersen_points_to.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"

namespace mjbase::analysis {

/// @brief 保守 points-to 值类别（旧栈 AliasValueKind 语义照旧）。
///
/// Unknown 是一等值：消费方只有在「每条前驱（含异常前驱）上已知且稳定」
/// 时才允许把别名落成具体目标。
enum class AliasValueKind
{
    Unknown,
    Null,
    Object,
    Array,
    Container,
    Primitive
};

/// @brief 进程级别名身份驻留（内容相同 ⇔ 返回视图相同）。
///
/// 旧栈 aliasInterned 的对应物：别名引擎内 identity/pointsTo 一律存驻留视图，
/// 拷贝廉价、等值比较退化为内容比较。表只增不减（进程期存活）。
std::string_view aliasInterned(std::string_view text);

/// @brief 方法/字段/绑定键的统一构造（别名分析身份池驻留）。
///
/// MemberKey 取代旧栈 "owner\nname\ndesc" 拼接串键；测试与消费方构造摘要
/// 时必须经本入口建键，才能与分析产出的摘要键对上（同一驻留池、内容寻址）。
MemberKey aliasMemberKey(std::string_view owner, std::string_view name, std::string_view desc);

/// @brief 别名格值：类别 + 身份 + 诊断 points-to 集合（+ cat2 槽宽）。
struct AliasValue
{
    AliasValueKind                kind = AliasValueKind::Unknown;
    /// 驻留视图；Unknown/Null 语义为空。默认即驻留空串（旧栈 identity="" 语义），
    /// 消费方解引用安全。
    std::string_view              identity{};
    std::vector<std::string_view> pointsTo;   ///< 驻留视图，按字典序去重
    ca::u8                        slots = 1;  ///< 槽宽（引擎路径；long/double 载荷为 2）

    static AliasValue unknown(ca::u8 width = 1)
    {
        AliasValue value;
        value.slots = width;
        return value;
    }
    static AliasValue nullValue()
    {
        AliasValue value;
        value.kind = AliasValueKind::Null;
        return value;
    }
    static AliasValue object(std::string_view id)
    {
        AliasValue value;
        value.kind     = AliasValueKind::Object;
        value.identity = aliasInterned(id);
        value.pointsTo.push_back(value.identity);
        return value;
    }
    static AliasValue array(std::string_view id)
    {
        AliasValue value;
        value.kind     = AliasValueKind::Array;
        value.identity = aliasInterned(id);
        value.pointsTo.push_back(value.identity);
        return value;
    }
    static AliasValue container(std::string_view id)
    {
        AliasValue value;
        value.kind     = AliasValueKind::Container;
        value.identity = aliasInterned(id);
        value.pointsTo.push_back(value.identity);
        return value;
    }
    static AliasValue primitive(std::string_view id)
    {
        AliasValue value;
        value.kind     = AliasValueKind::Primitive;
        value.identity = aliasInterned(id);
        return value;
    }

    bool isKnown() const { return kind != AliasValueKind::Unknown; }
    bool operator==(const AliasValue& other) const
    {
        return kind == other.kind && identity == other.identity;
    }
    bool operator!=(const AliasValue& other) const { return !(*this == other); }
};

/// @brief 合并两个前驱值（旧栈 mergeAliasValues 语义照旧）。
///
/// 相等事实存活；任何分歧（含 Unknown）→ Unknown，但保留诊断 points-to
/// 并集。交换、幂等。
AliasValue mergeAliasValues(const AliasValue& lhs, const AliasValue& rhs);

/// @brief 方法内别名帧（按 JVM local 槽位键控；旧栈 AliasFrame 照迁）。
struct AliasFrame
{
    std::unordered_map<ca::i32, AliasValue> locals;
    bool                                    hasExceptionalPredecessor = false;

    AliasValue local(ca::i32 index) const
    {
        auto it = locals.find(index);
        return it == locals.end() ? AliasValue::unknown() : it->second;
    }
};

/// @brief 正常与异常前驱帧的保守合并（缺槽位按 Unknown 失败闭合，交换）。
AliasFrame mergeAliasFrames(const AliasFrame& lhs, const AliasFrame& rhs);

/// @brief 跨方法稳定摘要（反射/remap 消费契约；旧栈 AliasMethodSummary 照迁，
///        方法/字段键换 MemberKey 驻留键）。
struct AliasMethodSummary
{
    struct FieldWrite
    {
        MemberKey  field;
        AliasValue receiver;
        AliasValue value;
        bool       isStatic = false;
    };

    struct ContainerWrite
    {
        AliasValue container;
        AliasValue key;
        AliasValue value;
    };

    struct ArrayWrite
    {
        AliasValue array;
        AliasValue index;
        AliasValue value;
    };

    struct ReflectionReceiver
    {
        ca::i32    instructionIndex = -1;   ///< order() 程序序位置（旧栈为指令表下标）
        AliasValue receiver;
    };

    /// @brief 调用点绑定：callee 键 + 调用点实参 σ（惰性展开的引用化摘要）。
    struct CallBinding
    {
        MemberKey               callee;
        ca::i32                 instructionIndex = -1;
        AliasValue              receiver;
        std::vector<AliasValue> arguments;
        AliasValue              fwReceiver;   ///< fieldWrites 组的实例化 σ（CFG 帧值）
        std::vector<AliasValue> fwArguments;
        bool                    fieldWritesActive     = true;
        bool                    arrayWritesActive     = true;
        bool                    containerWritesActive = true;
    };

    MemberKey                       method;
    std::vector<AliasValue>         parameters;
    AliasValue                      returnValue;
    std::vector<FieldWrite>         fieldWrites;
    std::vector<ArrayWrite>         arrayWrites;
    std::vector<ContainerWrite>     containerWrites;
    std::vector<ReflectionReceiver> reflectionReceivers;
    std::vector<CallBinding>        callBindings;
    bool                            hasExceptionalExit = false;
};

/// @brief 展开 callee 绑定后的等价事实流（诊断/测试专用）。
struct AliasExpandedFacts
{
    std::vector<AliasMethodSummary::FieldWrite>     fieldWrites;
    std::vector<AliasMethodSummary::ArrayWrite>     arrayWrites;
    std::vector<AliasMethodSummary::ContainerWrite> containerWrites;
};

/// @brief 实例字段槽键：接收者身份（驻留视图；空 = unknown 接收者/静态槽）
///        + 字段键。旧栈拼接串键的复合化（零串构造）。
struct SlotKey
{
    std::string_view identity;   ///< 接收者身份（池驻留，存活期覆盖本分析）
    MemberKey        field;

    bool operator==(const SlotKey& o) const noexcept
    {
        return identity == o.identity && field == o.field;
    }
};

/// @brief 槽键哈希（身份视图哈希 × 成员键哈希）。
struct SlotKeyHash
{
    std::size_t operator()(const SlotKey& k) const noexcept;
};

/// @brief 双视图键哈希（数组/容器槽：(容器身份, 元素键身份)；空视图 = 通配）。
struct PairViewHash
{
    std::size_t operator()(const std::pair<std::string_view, std::string_view>& k) const noexcept;
};


struct AliasFieldSummary
{
    MemberKey  field;
    AliasValue receiver;
    AliasValue value;
};

/// @brief 共享别名分析（旧栈 AliasAnalysis 契约照迁）。
///
/// 持有值/合并契约并把稳定摘要算到全程序不动点。分支方法经 DomainEngine
/// 别名域在真实 CFG join 处合并栈/局部值；分歧保持 Unknown 并保留诊断
/// points-to。上层分析必须使用这些值，不得私设反射专用别名格。
class AliasAnalysis
{
public:
    /// @brief 全罐构建（容器内全部类；畸形类/方法不产出事实）。
    void build(const mjbase::model::ResourceContainer& container);

    /// @brief 反射闭包 scoped 构建（rename --apply 的 alias 段专用）。
    ///
    /// R1 含 java/lang/Class 反射查找调用点的类；R2 R1∪R3∪R4 的容器内传递
    /// callee 闭包；R3 含 Class 形静态槽读写指令的类；R4 声明 Class 形静态
    /// 字段的类。作用域外的类不产出摘要（summaryOf 返回 nullptr）。收敛轮数
    /// 只反映本闭包固定点。
    void buildReflectionClosure(const mjbase::model::ResourceContainer& container);

    void clear();

    const AliasMethodSummary* summaryOf(std::string_view owner, std::string_view name,
                                        std::string_view desc) const;

    /// @brief 反射查找接收者的共享别名事实（按指令程序序位置）。
    const AliasValue* reflectionReceiverOf(std::string_view owner, std::string_view name,
                                           std::string_view desc, ca::i32 instructionIndex) const;

    const std::vector<AliasMethodSummary>& summaries() const { return summaries_; }

    /// @brief 静态槽（unknown 接收者）字段事实。
    const AliasValue* fieldValueOf(std::string_view owner, std::string_view name,
                                   std::string_view desc) const;

    /// @brief 具名接收者的字段事实；未知接收者返回 nullptr。
    const AliasValue* fieldValueOf(const AliasValue& receiver, std::string_view owner,
                                   std::string_view name, std::string_view desc) const;

    /// @brief 跨全部接收者分配目标解析字段读。
    ///
    /// Unknown 接收者可携带 CFG 合并留下的诊断 points-to；各目标独立查询，
    /// 冲突保持 Unknown 且保留 points-to——调用方不得当作唯一具体别名。
    std::optional<AliasValue> resolvedFieldValueOf(const AliasValue& receiver,
                                                   std::string_view owner, std::string_view name,
                                                   std::string_view desc) const;

    const AliasValue* containerValueOf(std::string_view identity) const;
    const AliasValue* containerValueOf(const AliasValue& container, const AliasValue& key) const;

    /// @brief 按具体或通配下标解析数组元素。
    const AliasValue* arrayValueOf(const AliasValue& array, const AliasValue& index) const;

    /// @brief 跨诊断 points-to 目标解析数组元素。
    std::optional<AliasValue> resolvedArrayValueOf(const AliasValue& array,
                                                   const AliasValue& index) const;

    /// @brief 已知值代表的具体分配类型（"new:" 身份的尾段，排序去重）。
    std::vector<std::string> possibleTypesOf(const AliasValue& value) const;

    std::vector<std::string> possibleReturnTypesOf(std::string_view owner, std::string_view name,
                                                   std::string_view desc) const;

    /// @brief 到达方法返回槽的 Andersen 分配点集合（诊断出口）。
    std::vector<std::string> andersenReturnPointsToOf(std::string_view owner,
                                                      std::string_view name,
                                                      std::string_view desc) const;

    /// @brief 到达方法某形参的 Andersen 分配点集合（诊断出口）。
    std::vector<std::string> andersenParameterPointsToOf(std::string_view owner,
                                                         std::string_view name,
                                                         std::string_view desc,
                                                         ca::usize        parameter) const;

    /// @brief 展开调用绑定后的等价事实流（诊断/测试专用）。
    AliasExpandedFacts expandedFactsOf(std::string_view owner, std::string_view name,
                                       std::string_view desc) const;

    /// @brief 展开 method 摘要的指定事实组（惰性实例化核心；环上每方法至多
    ///        展开一次，重入只贡献 callee 自身事实）。
    void expandFieldWrites(const AliasMethodSummary&                    summary,
                           std::vector<ca::usize>&                      visiting,
                           std::vector<AliasMethodSummary::FieldWrite>& out) const;
    void expandArrayWrites(const AliasMethodSummary&                    summary,
                           std::vector<ca::usize>&                      visiting,
                           std::vector<AliasMethodSummary::ArrayWrite>& out) const;
    void expandContainerWrites(const AliasMethodSummary&                          summary,
                               std::vector<ca::usize>&                            visiting,
                               std::vector<AliasMethodSummary::ContainerWrite>&   out) const;

    /// @brief 最近一次固定点使用的整程序轮数。
    ca::usize convergenceRounds() const { return convergenceRounds_; }

    void addSummary(AliasMethodSummary summary);

    /// @brief 绑定键 → 摘要槽位（解析域 = expansionSource_ 优先，缺省自身）。
    const AliasMethodSummary* resolveBinding(const MemberKey& calleeKey,
                                             ca::usize*       slotIndex) const;

private:
    void buildFixedPoint(const std::vector<const struct AliasMethodContext*>& scope,
                         ca::usize                                            roundBound);
    void indexAndersenSummary(const AliasMethodSummary& summary);
    /// 槽状态（fields_/arrayValues_/containerValues_）与另一实例的深比（轮末对账）。
    bool slotStatesDiffer(const AliasAnalysis& other) const;

    /// 摘要键 → 槽位。
    std::unordered_map<MemberKey, ca::usize, MemberKeyHash>    byKey_;
    std::vector<AliasMethodSummary>                            summaries_;
    std::vector<AliasFieldSummary>                             fields_;
    /// 字段槽（接收者身份空视图 = unknown 接收者/静态槽；与旧栈拼接串键同构）。
    std::unordered_map<SlotKey, ca::usize, SlotKeyHash>        fieldsByKey_;
    std::unordered_map<std::pair<std::string_view, std::string_view>, AliasValue,
                       PairViewHash>                           arrayValues_;
    std::unordered_map<std::pair<std::string_view, std::string_view>, AliasValue,
                       PairViewHash>                           containerValues_;
    AndersenPointsToSolver                                     andersen_;
    ca::usize                                                  convergenceRounds_ = 0;

    /// 绑定键解析域：固定点期间指向上一轮快照（Jacobi 轮内只读上一轮）。
    const AliasAnalysis* expansionSource_ = nullptr;
    /// 固定点内悬置 andersen 摘要索引（收敛后由字节码索引一次性完成）。
    bool suspendAndersenIndexing_ = false;
};

}   // namespace mjbase::analysis
