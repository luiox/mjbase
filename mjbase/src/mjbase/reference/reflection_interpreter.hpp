/// @file        reflection_interpreter.hpp
/// @brief       反射对象常量数据流解释器（独立成件）：反射接收者/成员对象追踪。
///
/// - 值域只追踪反射所需常量闭包：ldc 字符串、ldc class 常量（"class:X"）、
///   Class.forName(String) 产物、Class.get*Method/get*Constructor/get*Field
///   产物（Method/Constructor/Field，带 Class[] 实参描述符）、GETSTATIC 经
///   alias 字段槽回填的 string:/class: 值。其余一切 Unknown（fail-closed）。
/// - 基类体系换 libjbc 域插件：ReflectionDomain 实现 IValueDomain<ReflectionValue>，
///   栈/局部/cat2/控制流全部由 DomainEngine 承载；invoke 产点（引擎产点钩子
///   看不到操作数栈的能力缺口）经「迭代探测-注入」补齐——每轮 FrameQuery 后处理
///   重算 invoke 产点写入注入表，值单调增强至稳定（与 alias_analysis 两跑范式
///   同族，链式反射 forName→getMethod→invoke 需要多轮）。
/// - 合并 fail-closed：冲突值折损 Unknown（经 mergeAliasValues 的别名格判定，
///   相等反射值在控制流合并点存活）。
/// - 消费 alias 契约：形参/this/字段返回传播经 mjbase::asm_analyzer::AliasAnalysis
///   查询（param:/this:/field:/class:/string: 身份，文本内容寻址）。
///
/// 消费方：reference_index（反射执行事实的接收者快照）与
/// reflection_binding_analyzer（共享 normalizeClassName/classLiteralTrackingName/
/// intConstantOf/primitiveDescriptorFromTypeField 等纯函数与 LDC 常量产出语义）。
/// 与 mjbase::analysis::alias_analysis 内嵌反射接收者子集（buildReflectionClosure
/// 的 reflectionReceivers）有重叠面，本件不改 alias_analysis 实现。
#pragma once

#include <optional>
#include <string_view>
#include <unordered_map>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::asm_analyzer {
class AliasAnalysis;
}   // namespace mjbase::analysis

namespace mjbase::reference {

/// @brief 类名归一化：`(Lfoo/Bar;)V` 描述符形态与点分名统一为 internal name。
///
/// 口径：先剥 L...; 外层，再把 '.' 换成 '/'。数组/原始类型形态不是合法的
/// Class.forName 实参，原样保留交由存在性验证失败。
std::string_view normalizeClassName(std::string_view value);

/// @brief CP Class 条目名 → 反射追踪名：OBJECT 取内部名（归一化）；数组取
///        描述符原样（`[I`/`[Ljava/lang/String;`）；原始类型/void 关键字与
///        空名返回空（不成追踪事实）。
std::string_view classLiteralTrackingName(std::string_view cpClassName);

/// @brief 类常量追踪名 → 参数描述符形态：OBJECT → `Lname;`；数组 → 描述符原样。
std::string classLiteralDescriptor(std::string_view trackingName);

/// @brief 读取 int 常量（iconst_0..5 / bipush / sipush / iconst_m1 可选）。
///
/// allowNegative=false 时 bipush/sipush 负值按不可判定处理；=true 时
/// iconst_m1 也识别。
bool intConstantOf(const jbc::ir::Insn& insn, ca::i32& out, bool allowNegative);

/// @brief 原始类型 TYPE 常量（Integer.TYPE 等 GETSTATIC）→ 单字符描述符；
///        非 TYPE 形态返回空。
std::string_view primitiveDescriptorFromTypeField(std::string_view owner, std::string_view name,
                                                  std::string_view desc);

/// @brief 反射值：方法局部数据流里的反射对象抽象（basic
///        形状载荷由引擎承担不再携带；文本一律 aliasInterned 驻留视图）。
struct ReflectionValue
{
    enum class Kind
    {
        Unknown,
        String,
        Class,
        Method,
        Constructor,
        Field
    };

    Kind            kind = Kind::Unknown;
    std::string_view owner;               ///< Class/Method/Constructor/Field 目标类
    std::string_view name;                ///< String 内容或成员名
    std::string_view desc;                ///< 已解析的成员描述符（查找族为空）
    std::string_view reflectionArgDesc;   ///< Class[] 实参描述符

    bool operator==(const ReflectionValue& other) const noexcept
    {
        return kind == other.kind && owner == other.owner && name == other.name &&
               desc == other.desc && reflectionArgDesc == other.reflectionArgDesc;
    }
    bool operator!=(const ReflectionValue& other) const noexcept { return !(*this == other); }
};

/// @brief 确认的反射执行目标（文本驻留视图）。
struct ReflectionExecutionTarget
{
    enum class Kind
    {
        Method,
        Constructor,
        Field
    };

    Kind             kind = Kind::Method;
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
    std::string_view reflectionArgDesc;
};

/// @brief 常量 `Class[]` 实参描述符解析。
///
/// 从 lookup 指令（order 下标 lookupPos）向前走：ALOAD 中转的局部数组形态
/// （ANEWARRAY → ASTORE → (ALOAD; 常量下标; 类常量/TYPE GETSTATIC; AASTORE)*
/// → ALOAD）与内联 DUP 形态（ANEWARRAY; (DUP; 常量下标; 类常量; AASTORE)*）。
/// 区域内存在汇入跳转、下标缺失/重复/非常量时返回空——不猜。
std::string_view reflectionArgumentDescriptor(const jbc::ir::CodeUnit&       code,
                                              const jbc::core::ConstantPool& cp,
                                              ca::usize                      lookupPos);

/// @brief [firstPos, lastPos] 区域是否存在汇入跳转（CodeDom 形态的直线守卫）。
///
/// 区域内的标签
/// 边界被方法内任意跳转/switch 命中即 true。标签解析位置 p ∈ (firstPos, lastPos]
/// 计入（紧邻区域首指令之前的边界不计）。反射实参数组与 binding 名表扫描的
/// 直线序列守卫共用件。
bool regionHasIncomingBranches(const jbc::ir::CodeUnit& code, ca::usize firstPos,
                               ca::usize lastPos);

/// @brief 反射执行判定。
///
/// Method.invoke / Class.newInstance / Constructor.newInstance / Field.get(set
/// 及基本类型变体)：接收者为已知反射对象时产出目标；否则 false（fail-closed）。
bool resolveReflectionExecution(jbc::core::Op op, std::string_view owner, std::string_view name,
                                std::string_view desc, const ReflectionValue& receiver,
                                ReflectionExecutionTarget& target);

/// @brief 反射数据流快照。
struct ReflectionSnapshot
{
    bool ok = false;   ///< false = 引擎拒斥（JSR/RET、畸形控制流）→ fail-open，
                       ///< 消费方保留 direct/lookup 事实、不追加数据流事实。
    /// invoke 指令（CodeDom order() 下标）→ 调用点接收者值。仅含可达调用点；
    /// 栈深不足的调用点缺席。已含 alias "class:X" 接收者覆盖。
    std::unordered_map<ca::u32, ReflectionValue> receivers;
};

/// @brief 反射查询上下文（一个方法的静态视图）。
struct ReflectionQueryContext
{
    const jbc::ir::CodeUnit*              code    = nullptr;
    const jbc::core::ConstantPool*        cp      = nullptr;
    std::string_view                      owner;      ///< 所属类（alias 查询键）
    std::string_view                      name;       ///< 所属方法（alias 查询键）
    std::string_view                      desc;       ///< 所属方法描述符
    bool                                  isStatic = false;
    const mjbase::asm_analyzer::AliasAnalysis* aliases  = nullptr;
};

/// @brief 计算方法内反射数据流快照（域引擎 + 迭代探测-注入 + FrameQuery 收口）。
ReflectionSnapshot snapshotReflectionFacts(const ReflectionQueryContext& ctx);

}   // namespace mjbase::reference
