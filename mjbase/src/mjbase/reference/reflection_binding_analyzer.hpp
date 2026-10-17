/// @file        reflection_binding_analyzer.hpp
/// @brief       反射 sink 调用绑定分析器：方法内值流落解 + 容器内存在性验证。
///
/// 面向反射 sink 调用点的**方法内**值流落解器。回答一个窄问题：每个反射 sink
/// （Class.forName、Class.getMethod、Method.invoke、Field.get/set 等）的调用点
/// 上，能否从字节码静态推出类名/成员名/参数类型，并对容器内真实声明做存在性
/// 验证，产出可审计的 ReflectionBinding。
///
/// ## 设计基调（fail-closed）
///
/// 宁 Pending 不误报：Resolved 状态必须经过成员存在性验证（类在容器内 + 成员
/// 名命中声明 + 参数类型可验证：Class[] 元素全知时参数列表精确匹配，元素不全时
/// 回退参数个数匹配）。任何一步无法静态确定都降级为 Pending 并携带原因分类；
/// MethodHandles.Lookup 族在目录中登记但首版不解析（Unsupported，catalog-only）。
/// 不从瞬时控制流路径猜测目标——冲突值在合并点折损为 Unknown。
///
/// ## 方法内值流覆盖面
///
/// - `ldc` 字符串常量、`ldc` class 常量（`Foo.class`）
/// - StringBuilder/StringBuffer 链（append/toString 折叠）
/// - invokedynamic `makeConcatWithConstants`（解析 recipe 常量段 + 动态参数位，
///   动态参数递归追踪）与 `makeConcat`
/// - `static final String` 字段（字段常量池 ConstValue）
/// - `static final String[]` 名表（扫描 `<clinit>` 中 newarray→putstatic 的全量
///   初始化序列）
/// - 数组元素 `aaload`：下标为常量时精确取值；非常量下标使该 sink 整体降级
///   Pending(ambiguous_array)
///
/// ## 迁移形态（jbytecode → libjbc）
///
/// 旧栈骑 BasicInterpreter/Analyzer 基类体系；本件换 libjbc 域插件：
/// BindingDomain 实现 IValueDomain<BindingValue>（纯产值点在域钩子内：int 常量、
/// ldc、GETSTATIC 常量/名表回填、CHECKCAST/ANEWARRAY/AALOAD/IADD/ISUB），
/// 操作数依赖产点（invoke 家族、indy 拼接、SB 产出）经「迭代探测-注入」后处理
/// 补齐；SB/AASTORE 的共享变异（引用语义）单列变异 pass——值轮收敛后按程序序
/// 一次性施加，再以注入表回灌值轮（与 reflection_interpreter 同族两跑范式，
/// 共享对象经 shared_ptr 携带，写时复制纪律只在纯值路径生效）。
///
/// ## 非目标（第一刀不做）
///
/// - 跨方法过程间摘要（#980 常量传播线）
/// - 字段表驱动的外部映射注入（解密字符串以解密后 LDC 形态进入，本分析器只认 LDC）
/// - callgraph 回灌（#1021 缺口 1 落地后的小集成步）
/// - helper 桩改名补丁
///
/// ## 层次
///
/// 与 ReferenceIndex（编码事实索引）互补：ReferenceIndex 记录"某处发生了反射
/// 查找/执行"，本模块回答"查的是谁、执行的是谁，且目标真实存在"。Binding 的
/// ReferenceSite 与 ReferenceIndex 同构，供后续 callgraph 消费。
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/reference/reference_index.hpp"

namespace jbc::ir {
class ClassSession;
}   // namespace jbc::ir

namespace mjbase::reference {

/// @brief 反射 sink 的 API 分类。
enum class ReflectionApiKind
{
    ClassForName,          ///< Class.forName(String) 及三参重载
    ClassLoaderLoadClass,  ///< ClassLoader.loadClass(String)
    MemberLookup,          ///< Class.get*Method(s)/get*Constructor(s)/get*Field(s)
    MethodInvoke,          ///< Method.invoke
    ConstructorNewInstance,///< Constructor.newInstance
    ClassNewInstance,      ///< Class.newInstance
    FieldAccess,           ///< Field.get/set
    LookupMethodHandle,    ///< MethodHandles.Lookup find*/unreflect*（catalog-only）
};

/// @brief API 分类的人类可读短标签（"Class.forName" 形态）。
const char* reflectionApiKindLabel(ReflectionApiKind kind);

/// @brief 绑定三态。
enum class ReflectionBindingState
{
    Resolved,    ///< 目标已在容器内验证存在
    Pending,     ///< 无法静态确定或验证失败（见 reason）
    Unsupported, ///< 目录已登记但首版不解析（catalog-only）
};

/// @brief Pending 原因分类。
enum class ReflectionPendingReason
{
    None,              ///< 非 Pending（占位）
    UnknownArgs,       ///< 实参无法静态确定
    AmbiguousArray,    ///< 名表/Class[] 下标非常量
    UnknownReceiver,   ///< 接收者（Class/Method/Field 对象）未知
    NoSuchClass,       ///< 目标类不在容器内（fail-closed，不猜外部类）
    NoSuchMember,      ///< 类可判定但成员不存在或参数类型（个数/描述符）对不上
    MemberEnumeration, ///< 复数枚举形态（getMethods 等）无单一目标
};

/// @brief Pending 原因的稳定 snake_case 标签（输出/JSON 用）。
const char* reflectionPendingReasonLabel(ReflectionPendingReason reason);

/// @brief 绑定状态的稳定小写标签（输出/JSON 用）。
const char* reflectionBindingStateLabel(ReflectionBindingState state);

/// @brief Resolved 状态下经验证的目标成员引用。
struct ReflectionBindingTarget
{
    std::string_view owner;   ///< internal name（池驻留视图）
    std::string_view name;    ///< 成员名（类级 sink 无成员名，为空）
    std::string_view desc;    ///< 命中声明的成员描述符
};

/// @brief 一个反射 sink 调用点的绑定结论。
struct ReflectionBinding
{
    /// @brief 调用点位置（与 ReferenceIndex::ReferenceSite 同构；insnIndex 为
    /// CodeDom order() 下标，池驻留视图）。
    ReferenceSite           site;
    ReflectionApiKind       api       = ReflectionApiKind::ClassForName;
    ReflectionBindingState  state     = ReflectionBindingState::Pending;
    ReflectionPendingReason reason    = ReflectionPendingReason::None;
    /// @brief 解析出的目标类名（internal name；未知为空）。
    std::string_view className;
    /// @brief 解析出的成员名（未知为空）。
    std::string_view memberName;
    /// @brief 参数类型个数（Class[] 槽可判定时 >= 0，否则 -1）。
    ca::i32          paramTypeCount = -1;
    /// @brief Resolved 时的目标成员（Pending/Unsupported 为空）。
    ReflectionBindingTarget target;
};

/// @brief 汇总计量：sink 总数、三态计数、按 API kind 分布与落解率。
struct ReflectionBindingSummary
{
    ca::usize total       = 0;
    ca::usize resolved    = 0;
    ca::usize pending     = 0;
    ca::usize unsupported = 0;
    /// @brief 按 ReflectionApiKind 枚举序分布（下标即枚举值）。
    std::vector<ca::usize> byKind;

    /// @brief 落解率（百分比，0-100；无 sink 时为 0）。
    double resolvedRatePercent() const
    {
        return total == 0 ? 0.0 : static_cast<double>(resolved) * 100.0 / static_cast<double>(total);
    }
};

/// @brief 反射 sink 目录（数据驱动）。
///
/// 每项建模：API kind + 参数位（类名/成员名/参数类型列表）。Lookup 族登记为
/// catalog-only（首版不解析）。
struct ReflectionSinkEntry
{
    const char*       owner = nullptr;
    const char*       name  = nullptr;
    const char*       desc  = nullptr;
    ReflectionApiKind kind  = ReflectionApiKind::ClassForName;
    /// @brief 实参槽位（0 = 第一个显式实参，不含接收者）。
    int  classNameArg      = -1; ///< String 类名实参槽
    int  memberNameArg     = -1; ///< String 成员名实参槽
    int  paramTypesArg     = -1; ///< Class[] 参数类型列表实参槽
    bool requiresClassReceiver = false; ///< 接收者必须是已知 Class 对象（成员查找族）
    bool producesMemberObject  = false; ///< 产出 Method/Constructor/Field 对象（供执行 sink 追踪）
    bool catalogOnly           = false; ///< 仅登记不解析（Lookup 族）
    bool memberEnumeration     = false; ///< 复数枚举形态（getMethods 等）
};

/// @brief 全量 sink 目录（首次调用时构建，进程内共享只读表）。
const std::vector<ReflectionSinkEntry>& reflectionSinkCatalog();

/// @brief 反射调用绑定分析器。
///
/// 两趟扫描：第一趟收集容器内全部类/方法/字段声明（存在性验证底表）与
/// `static final String` 常量、`static final String[]` 名表（`<clinit>` 全量
/// 初始化序列扫描）；第二趟对每个方法跑抽象数据流，在 sink 调用点上落解并
/// 验证。格式错误的 class 被跳过，单类损坏不影响整体分析。
///
/// 文本契约：全部输出字段为分析器自有 KeyPool 的驻留视图（build/restore 生命
/// 期有效）；与 ReferenceIndex 的互通按内容等值（驻留文本内容寻址）。
class ReflectionBindingAnalyzer
{
public:
    /// @brief 从容器全量构建。重复调用先清空上次结果。
    void build(const mjbase::model::ResourceContainer& container);

    /// @brief 清空全部结果。
    void clear();

    /// @brief 全部绑定（按扫描顺序）。
    const std::vector<ReflectionBinding>& bindings() const { return bindings_; }

    /// @brief static final String 常量表（key = "owner.name" 驻留视图）。方法内
    /// 值流解释器查询用；只读。
    const std::unordered_map<std::string_view, std::string_view>& stringConstants() const
    {
        return stringConstants_;
    }

    /// @brief static final String[] 名表（key = "owner.name" 驻留视图）。方法内
    /// 值流解释器查询用；只读。
    const std::unordered_map<std::string_view, std::vector<std::string>>& nameTables() const
    {
        return nameTables_;
    }

    /// @brief 汇总计量。
    ReflectionBindingSummary summarize() const;

    /// @brief 按来源类过滤（internal name 精确匹配，内容寻址）。
    std::vector<const ReflectionBinding*> bindingsOfClass(std::string_view sourceClass) const;

private:
    /// @brief 存在性验证底表：单类的成员声明（名字 + 描述符，声明序；池驻留视图）。
    struct ClassInfo
    {
        std::vector<std::pair<std::string_view, std::string_view>> methods;   ///< name + desc
        std::vector<std::pair<std::string_view, std::string_view>> fields;    ///< name + desc
    };

    void indexSession(jbc::ir::ClassSession& session);
    void analyzeSession(jbc::ir::ClassSession& session);
    void scanStaticNameTables(jbc::ir::ClassSession& session, std::string_view className);

    mjbase::KeyPool pool_;

    std::unordered_map<std::string_view, ClassInfo>              classes_;
    /// @brief static final String 常量表：key = "owner.name" → 值。
    std::unordered_map<std::string_view, std::string_view>       stringConstants_;
    /// @brief static final String[] 名表：key = "owner.name" → 全量元素。
    std::unordered_map<std::string_view, std::vector<std::string>> nameTables_;
    std::vector<ReflectionBinding>                               bindings_;
};

}   // namespace mjbase::reference
