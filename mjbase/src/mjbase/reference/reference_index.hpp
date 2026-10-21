/// @defgroup reference_index 引用索引模块
///
/// 面向全程序 optimize/recover pass 的基础分析。回答一个窄问题：哪些扫描过的 class
/// 和方法体直接引用给定的 method、field 或 type？它刻意比调用图更小——只记录字节码
/// 中的编码事实，后续分析再结合继承图、方法覆盖、字段覆盖、副作用模型或 classpath
/// 数据。
///
/// 查询面同时提供"谁引用了此 method/field"的精确成员视图和"此类引用/owner 了什么"
/// 的类级/owner 级视图。
///
/// ## 扫描范围
///
/// 索引扫描 ClassResource 方法体并记录：
///
/// - 声明的 class（含直接超类和接口名）
/// - 声明的 method 和 field
/// - invoke 家族指令的方法调用
/// - 字段访问指令
/// - 显式类型指令（new、checkcast、instanceof、anewarray）
/// - 嵌入方法描述符、字段访问描述符和字段声明描述符中的对象类型
/// - `LDC` class 常量（object 和 array 类型）
/// - `multianewarray` 描述符
/// - method/field owner 作为类型引用
/// - 注解和签名中的元数据类型引用（类/字段/方法级 Runtime*Annotations + Signature；
///   方法级 AnnotationDefault 与 ParameterAnnotations；Record 组件内嵌注解。
///   *TypeAnnotation 家族 libjbc 未建模且无用例承载，本批明确放弃）
/// - 结构 class 元数据（EnclosingMethod、nest host/members、permitted subclasses、
///   inner classes、record component 描述符）
/// - class-string 反射使用、常量反射成员查找、method handle、lambda bootstrap 目标
/// - 通过操作数栈和局部变量别名的常量 `Class`/`Method`/`Constructor`/`Field`
///   对象的方法局部传播（经 reflection_interpreter 的域引擎快照）
/// - 确认的 `Method.invoke`、`Class.newInstance`、`Constructor.newInstance` 和
///   `Field.get/set` 执行/访问点（含基本类型字段访问器变体）
///
/// Owner-as-type 条目对 class 可达性很重要。即使在考虑虚分派或常量池引用之前，
/// 对 `B.callee()` 的调用或对 `B.value` 的读取就能使 class `B` 保持被引用状态。
///
/// Class-header、结构元数据和字段声明类型条目使用无源方法和指令索引的类级引用点，
/// 出现在 `typeReferencesFromClass()` 而非 `typeReferencesFrom()`。
///
/// ## 反射事实语义
///
/// 常量成员查找和实际反射执行是分离的事实：
///
/// - `Reflection` 类型表示成员对象被查找过，即使对象从未被执行或访问。
/// - `ReflectionExecution` 仅在方法局部数据流帧证明 `Method.invoke`、
///   `Class.newInstance` 或 `Constructor.newInstance` 收到已知反射对象时发出。
/// - `ReflectionAccess` 仅在证明的 `Field.get/set` 接收者时发出（含 getInt/setLong
///   等基本类型变体）。
/// - 相等反射值在控制流合并时存活；冲突值合并为 Unknown，索引不从瞬时路径猜测目标。
///
/// CallGraph 使用带解析描述符的 `ReflectionExecution` 事实，不将仅查找事实转为调用边。
///
/// ## 非目标
///
/// 索引不解析：
///
/// - 虚/接口分派
/// - 跨方法传递的反射对象
/// - 存储在字段或通用数组/集合中的值
/// - 非常量反射数据流
///
/// 反射 method/constructor 描述符在常量 `Class[]` 参数数组匹配扫描声明或方法声明
/// 包含唯一 owner/name 匹配时填充。支持的数组形状包括内联 `DUP`/`AASTORE` 构造和
/// 使用 `ANEWARRAY` 创建、`ASTORE` 存储、常量 `ALOAD`/`AASTORE` 序列填充的直线
/// 局部数组。对象、数组和包装器 `TYPE` 基本类型参数被识别。缺失或重复索引、局部
/// 覆盖和进入构造区域的控制流仍不解析。数组别名、分支合并、字段、跨过程传播和
/// 通用容器属于后续分析。索引也不判定引用是否存活——那些决策属于后续分析。
///
/// 格式错误的 class 被跳过（libjbc 建会话失败即不产出事实），使基础分析在容错
/// CLI 工作流中可用。
///
/// ## 键契约（MemberKey / alias 文本互通）
///
/// 方法/字段身份一律是 @ref mjbase::MemberKey（三段驻留 id），经本索引自有
/// KeyPool 建键（`pool()` 暴露，键随索引存活——与 asm/classpath CallGraph 同款
/// 「图自有池」纪律）；查询面同时收 MemberKey 与 (owner,name,desc) 文本三段，
/// 文本入参 intern 进同一池。与 mjbase::analysis::alias_analysis 的互通走**文本
/// 契约**：alias 侧键经 aliasMemberKey 建在 alias 进程级驻留池，两侧不做跨池
/// id 比较，一切成员比较以驻留文本内容等值为准（aliasInterned：内容相同 ⇔
/// 视图相同）。alias 查询（summaryOf / reflectionReceiverOf / fieldValueOf）
/// 直接传文本，由 alias 侧按同一内容寻址落键。
///
/// ## 分析集成
///
/// `ReferenceIndexAnalysis` 为 GlobalAnalysisManager 包装扫描。全局 pass 可
/// 作为常规 analysis 结果请求它，而不是自行重新扫描 container。
///
/// 预期分层：
///
/// 1. `ReferenceIndex`：直接编码引用
/// 2. `CallGraph`：方法声明节点和直接调用边
/// 3. `ClassHierarchy` / `MethodHierarchy` / `FieldHierarchy`：继承和覆盖关系
/// 4. Pass 特定策略：未使用成员删除、内联资格、访问收紧、重命名安全性、报告
///
/// ## 验证覆盖
///
/// `ReferenceIndexTest` 构建双 class fixture 并验证：
///
/// - `B.callee()` 的调用者
/// - `B.value` 的使用者
/// - 通过显式类型指令和 member owner 对 B 的类型引用
/// - 声明 class 查找（含直接超类和接口元数据）
/// - 声明 method/field 查找
/// - method/field/type 引用的源方法视图
/// - 批量全局 pass 查询的源 class 和目标 owner 视图
/// - 描述符、字段声明、class-header、LDC class 常量类型引用
/// - 注解/签名/结构元数据、反射字符串、反射成员查找、method handle、lambda bootstrap 目标
/// - 通过局部别名的 method/constructor/field 反射执行、控制流合并的 fail-closed 行为
/// - `GlobalAnalysisManager` 通过 `ReferenceIndexAnalysis` 集成
#pragma once

#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"

namespace jbc::ir {
class ClassSession;
struct MethodUnit;
struct CodeUnit;
class ChangeSet;
}   // namespace jbc::ir

namespace mjbase::analysis {
class AliasAnalysis;
}   // namespace mjbase::analysis

namespace mjbase::reference {

/// @brief Stable identifier for a JVM method reference.
///
/// 三段为索引驻留池的借用视图（build/restore 时 intern），索引存活期内有效。
struct MethodRefKey
{
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

/// @brief Stable identifier for a JVM field reference.
struct FieldRefKey
{
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

/// @brief Location of a reference inside one method body.
///
/// source* 为驻留视图；类级事实（class-header/结构元数据/字段声明）的
/// sourceMethod/sourceDesc 为空视图且 insnIndex == -1。方法体内的
/// insnIndex 是 jbc::ir::CodeDom::order() 程序序下标（CodeDom 无
/// label/line 占位节点）。
struct ReferenceSite
{
    std::string_view sourceClass;
    std::string_view sourceMethod;
    std::string_view sourceDesc;
    ca::i32          opcode    = -1;
    ca::i32          insnIndex = -1;
};

/// @brief Why a method reference was recorded.
///
/// 枚举值即 .ridx 文件里的 kind 字节（kMethodKindMax = ReflectionExecution），
/// 顺序逐值冻结，不得重排。
enum class MethodReferenceKind
{
    Direct,
    MethodHandle,
    Lambda,
    /// @brief Constant reflection lookup diagnostic. target.desc may be empty
    /// when the descriptor was not resolved from the reflection argument array.
    /// Lookup facts do not represent execution and are excluded from CallGraph.
    Reflection,
    /// @brief Confirmed execution through Method.invoke or Constructor.newInstance.
    ReflectionExecution
};

/// @brief Why a field reference was recorded.
enum class FieldReferenceKind
{
    Direct,
    MethodHandle,
    /// @brief Constant reflection lookup diagnostic. target.desc may be empty
    /// when the field descriptor was not resolved from declarations or metadata.
    Reflection,
    /// @brief Confirmed access through Field.get/set or a primitive variant.
    ReflectionAccess
};

/// @brief Why a class/type name was recorded as referenced.
enum class TypeReferenceKind
{
    ClassSuper,
    ClassInterface,
    MethodOwner,
    FieldOwner,
    MethodDescriptor,
    FieldDescriptor,
    FieldDeclarationDescriptor,
    TypeInstruction,
    MultiANewArray,
    ClassConstant,
    Annotation,
    Signature,
    StructuralMetadata,
    ReflectionString,
    MethodHandleOwner
};

/// @brief Method reference discovered from bytecode or a supported indirect mechanism.
///
/// This is not a resolved call graph edge: virtual/interface dispatch is left to
/// later MethodHierarchy/ClassHierarchy-aware analyses. The target is exactly
/// the owner/name/descriptor encoded in the bytecode instruction.
struct MethodReference
{
    ReferenceSite       site;
    MethodRefKey        target;
    /// @brief Parsed argument descriptor for constant reflection lookups.
    ///
    /// This is intentionally separate from target.desc. Reflection facts expose
    /// a full JVM method descriptor only after build() can match the argument
    /// list against scanned declarations.
    std::string_view    reflectionArgDesc;
    bool                isInterface = false;
    MethodReferenceKind kind        = MethodReferenceKind::Direct;
};

/// @brief Field reference discovered from bytecode or a supported indirect mechanism.
struct FieldReference
{
    ReferenceSite      site;
    FieldRefKey        target;
    FieldReferenceKind kind = FieldReferenceKind::Direct;
};

/// @brief Direct type reference discovered from bytecode instructions.
///
/// Method/field owners are also recorded as type references because class-level
/// passes such as unused-class removal need to know that `Owner.member` keeps
/// `Owner` reachable even before a full call graph exists.
struct TypeReference
{
    ReferenceSite     site;
    std::string_view  targetType;   ///< 驻留视图；multianewarray 处为数组描述符原样
    TypeReferenceKind kind = TypeReferenceKind::TypeInstruction;
};

/// @brief Class declared by a scanned class resource.
///
/// ReferenceIndex records only the direct class identity, access flags, direct
/// superclass, and directly implemented interfaces. Transitive hierarchy
/// closure belongs to hierarchy analyses layered above this direct-reference
/// index. superName 空视图 = 无父类条目（java/lang/Object 自身 / module-info）。
struct ClassDeclaration
{
    std::string_view              name;
    std::string_view              superName;
    std::vector<std::string_view> interfaces;
    ca::u32                       access = 0;
};

/// @brief Method declared by a scanned class resource.
///
/// Declaration records let call-graph and reachability analyses distinguish
/// project-local methods from external targets that only appear in bytecode call
/// instructions.
struct MethodDeclaration
{
    MethodRefKey method;
    ca::u32      access = 0;
};

/// @brief Field declared by a scanned class resource.
struct FieldDeclaration
{
    FieldRefKey field;
    ca::u32     access = 0;
};

/// @brief Minimal direct-reference index for P3 whole-program analyses.
///
/// ReferenceIndex scans ClassResource bytecode and records direct references
/// from method bodies to methods, fields, and types. It also records conservative
/// metadata facts such as annotations, signatures, structural class metadata,
/// class-string reflection uses, method handles, lambdas, constant
/// Class.getDeclared* / Class.get* member lookups, and confirmed
/// reflective execution through Method/Constructor/Field objects propagated within a
/// method. Reflection member descriptors are filled when a constant
/// `Class[]` argument list matches a scanned method declaration, or when the
/// scanned declarations contain a unique owner/name match. It deliberately
/// does not resolve inheritance, virtual dispatch, interprocedural reflection
/// aliases, or conflicting control-flow values. Those are higher-level analyses
/// layered on top of this direct index.
/// Build policy: malformed classes are skipped
/// conservatively so a single bad resource does not make the whole analysis unavailable.
class ReferenceIndex
{
public:
    /// @brief Rebuild the index from all class resources in a container.
    ///
    /// Existing results are discarded. Every exposed query result borrows the
    /// index-owned KeyPool's interned text and stays valid for the index's
    /// lifetime. Alias consumption follows the header's text contract: the
    /// shared alias analysis is queried by (owner,name,desc) text and its
    /// "class:X" receiver identities are consumed as interned views.
    ///
    /// @param container Resource set to scan. Non-class resources are ignored.
    void build(const mjbase::model::ResourceContainer& container);

    /// @brief Restore the index from already-materialized primary tables.
    ///
    /// Used by the persistent cache loader to skip bytecode parsing. The six
    /// primary tables are moved in verbatim (their views are re-interned into
    /// this index's pool), then all derived lookup maps are rebuilt to match
    /// the post-build state. The caller must guarantee the tables originate
    /// from a trusted writer (validated magic/version/etc); restore does not
    /// re-validate table invariants beyond what the derived map rebuild requires.
    ///
    /// Type references whose `site.sourceMethod` is empty are treated as
    /// class-scoped (the same convention as `makeClassSite`), so they populate
    /// `typeRefsBySourceClass_` but not `typeRefsBySource_` — matching the
    /// original `appendTypeReference(ref, methodScoped=false)` behavior.
    void restore(std::vector<ClassDeclaration>  classDecls,
                 std::vector<MethodDeclaration> methodDecls,
                 std::vector<FieldDeclaration>  fieldDecls,
                 std::vector<MethodReference>   methodRefs,
                 std::vector<FieldReference>    fieldRefs,
                 std::vector<TypeReference>     typeRefs);

    /// @brief Remove all collected references and target lookup tables.
    void clear();

    /// @brief E2 局部重建：按 ChangeSet 对单个类做成员级补丁（design.md §35.4）。
    ///
    /// 与全量 build 的等价性是 E2 硬验收：同一批变换后，本入口补丁出的六张
    /// 主表与「变换后容器全量重扫」逐项一致（相等性由单元与管线测试钉死）。
    /// 补丁只重扫变更粒度命中的成员/事实面，未变更类的字节码不重解析；主表
    /// 打补丁后派生查询图统一重建。
    ///
    /// - @p className 是索引中该类既有事实的键（变换前的类内部名；未改名场景
    ///   即会话当前名）。类级变更（ClassCreated/ClassMetadata）走整类重扫：
    ///   先按本键摘除全部旧事实，再按会话当前状态重扫（改名后新名落键）。
    /// - 字段粒度变更重扫类的类级事实面（类声明 + 类作用域类型引用 + 全部
    ///   字段声明/元数据）——字段元数据产出的类型事实不含字段坐标，无法按键
    ///   单独摘除，整体重扫是该面的精确上限；方法体事实不受影响。
    /// - 方法粒度变更按键精确替换：摘除该成员声明与以其为 source 的引用事实，
    ///   再从会话重扫该成员（声明 + 元数据 + 方法体）。
    /// - 前置条件：@p session 处于变换后状态且存活（ChangeSet 身份视图的
    ///   寿命 = 会话）；别名分析面在补丁路径不参与（与 restore 同口径）。
    void applyChangeSet(std::string_view              className,
                        jbc::ir::ClassSession&        session,
                        const jbc::ir::ChangeSet&     changes);

    /// @brief Index-owned intern pool: fact text views resolve and stay valid here.
    const KeyPool& pool() const noexcept { return pool_; }

    /// @brief Return all direct call instructions targeting a method key.
    ///
    /// The key is matched exactly against the owner/name/descriptor encoded in
    /// the invoke instruction. No inheritance, bridge, interface, or virtual
    /// dispatch expansion is applied here.
    const std::vector<MethodReference>& methodReferencesTo(MemberKey key) const;
    const std::vector<MethodReference>& methodReferencesTo(std::string_view owner,
                                                           std::string_view name,
                                                           std::string_view desc) const;

    /// @brief Return all direct field access instructions targeting a field key.
    ///
    /// Field hierarchy and shadowing policy are intentionally left to later
    /// analyses built on top of the direct index.
    const std::vector<FieldReference>& fieldReferencesTo(MemberKey key) const;
    const std::vector<FieldReference>& fieldReferencesTo(std::string_view owner,
                                                         std::string_view name,
                                                         std::string_view desc) const;

    /// @brief Return all direct type references matching an internal JVM name.
    ///
    /// Results include explicit type instructions plus method/field owner
    /// entries. Array descriptors are preserved as encoded for multianewarray
    /// instructions because descriptor normalization belongs to higher layers.
    const std::vector<TypeReference>& typeReferencesTo(std::string_view targetType) const;

    /// @brief Return direct method calls emitted by one source method.
    ///
    /// This is the forward view paired with methodReferencesTo(). It is useful
    /// for call graph construction and local "what does this method touch?"
    /// reports without scanning the global reference list.
    const std::vector<MethodReference>& methodReferencesFrom(MemberKey source) const;
    const std::vector<MethodReference>& methodReferencesFrom(std::string_view owner,
                                                             std::string_view name,
                                                             std::string_view desc) const;

    /// @brief Return direct field accesses emitted by one source method.
    const std::vector<FieldReference>& fieldReferencesFrom(MemberKey source) const;
    const std::vector<FieldReference>& fieldReferencesFrom(std::string_view owner,
                                                           std::string_view name,
                                                           std::string_view desc) const;

    /// @brief Return direct type references emitted by one source method.
    const std::vector<TypeReference>& typeReferencesFrom(MemberKey source) const;
    const std::vector<TypeReference>& typeReferencesFrom(std::string_view owner,
                                                         std::string_view name,
                                                         std::string_view desc) const;

    /// @brief Return direct method calls emitted by any method in a source class.
    ///
    /// This class-level forward view is intended for global passes that need to
    /// invalidate or inspect all outgoing references from one class without
    /// iterating over every method-level source key.
    const std::vector<MethodReference>& methodReferencesFromClass(std::string_view owner) const;

    /// @brief Return direct field accesses emitted by any method in a source class.
    const std::vector<FieldReference>& fieldReferencesFromClass(std::string_view owner) const;

    /// @brief Return direct type references associated with a source class.
    ///
    /// Results include method-body references plus class declaration facts such
    /// as superclass, interfaces, and field declaration descriptor types.
    const std::vector<TypeReference>& typeReferencesFromClass(std::string_view owner) const;

    /// @brief Return all direct call instructions whose encoded target owner matches owner.
    const std::vector<MethodReference>& methodReferencesToOwner(std::string_view owner) const;

    /// @brief Return all direct field access instructions whose encoded target owner matches owner.
    const std::vector<FieldReference>& fieldReferencesToOwner(std::string_view owner) const;

    /// @brief Return a declared method by exact owner/name/descriptor.
    const MethodDeclaration* declaredMethod(MemberKey key) const;
    const MethodDeclaration* declaredMethod(std::string_view owner, std::string_view name,
                                            std::string_view desc) const;

    /// @brief Return a declared field by exact owner/name/descriptor.
    const FieldDeclaration* declaredField(MemberKey key) const;
    const FieldDeclaration* declaredField(std::string_view owner, std::string_view name,
                                          std::string_view desc) const;

    /// @brief Return all methods declared by a scanned class.
    const std::vector<MethodDeclaration>& declaredMethodsIn(std::string_view owner) const;

    /// @brief Return all fields declared by a scanned class.
    const std::vector<FieldDeclaration>& declaredFieldsIn(std::string_view owner) const;

    /// @brief Return a declared class by exact internal JVM name.
    const ClassDeclaration* declaredClass(std::string_view owner) const;

    /// @brief Return every declared class in scan order.
    const std::vector<ClassDeclaration>& classDeclarations() const { return classDecls_; }

    /// @brief Return every direct method reference in scan order.
    const std::vector<MethodReference>& methodReferences() const { return methodRefs_; }

    /// @brief Return every direct field reference in scan order.
    const std::vector<FieldReference>& fieldReferences() const { return fieldRefs_; }

    /// @brief Return every direct type reference in scan order.
    const std::vector<TypeReference>& typeReferences() const { return typeRefs_; }

    /// @brief Return every declared method in scan order.
    const std::vector<MethodDeclaration>& methodDeclarations() const { return methodDecls_; }

    /// @brief Return every declared field in scan order.
    const std::vector<FieldDeclaration>& fieldDeclarations() const { return fieldDecls_; }

    /// @brief Number of direct method reference instructions collected.
    ca::usize methodReferenceCount() const { return methodRefs_.size(); }

    /// @brief Number of direct field reference instructions collected.
    ca::usize fieldReferenceCount() const { return fieldRefs_.size(); }

    /// @brief Number of direct type reference records collected.
    ca::usize typeReferenceCount() const { return typeRefs_.size(); }

    /// @brief Number of declared methods collected.
    ca::usize methodDeclarationCount() const { return methodDecls_.size(); }

    /// @brief Number of declared fields collected.
    ca::usize fieldDeclarationCount() const { return fieldDecls_.size(); }

    /// @brief Number of declared classes collected.
    ca::usize classDeclarationCount() const { return classDecls_.size(); }

private:
    void indexSession(jbc::ir::ClassSession& session,
                      const mjbase::analysis::AliasAnalysis* aliases);
    /// @brief 类级事实面：类声明 + 超/接口 + 类级元数据类型引用（§35.4 补丁面）。
    void indexClassHeader(jbc::ir::ClassSession&  session,
                          std::string_view        className,
                          const ReferenceSite&    classSite);
    /// @brief 单字段事实面：字段声明 + 描述符/Signature/注解类型引用。字段
    ///        元数据经 backingIndex 定位（成员增删后逻辑下标 ≠ 物理位置）。
    void indexFieldUnit(jbc::ir::ClassSession& session,
                        ca::usize              fieldIdx,
                        std::string_view       className,
                        const ReferenceSite&   classSite);
    /// @brief 方法声明入表（first-seen wins，与 build/restore 同口径）。
    void appendMethodDeclaration(jbc::ir::ClassSession&     session,
                                 const jbc::ir::MethodUnit& method,
                                 std::string_view           className);
    /// @brief 方法级元数据类型引用：描述符/Signature/注解族/参数注解/缺省值。
    void indexMethodMetadata(jbc::ir::ClassSession&         session,
                             const jbc::ir::MethodUnit&     method,
                             const ReferenceSite&           declarationSite);
    /// @brief 单方法补丁扫：声明 + 元数据 + 方法体（applyChangeSet 方法粒度）。
    void indexOneMethod(jbc::ir::ClassSession&                 session,
                        std::string_view                       methodName,
                        std::string_view                       methodDesc,
                        std::string_view                       className,
                        const mjbase::analysis::AliasAnalysis* aliases);
    void indexMethodBody(jbc::ir::ClassSession&           session,
                         const jbc::ir::CodeUnit&         code,
                         std::string_view                 className,
                         const jbc::ir::MethodUnit&       method,
                         const mjbase::analysis::AliasAnalysis* aliases);
    /// @brief 摘除一个方法成员的全部事实：声明 + 以 (类, 成员) 为 source 的引用。
    void dropMethodFacts(std::string_view className, std::string_view name,
                         std::string_view desc);
    /// @brief 摘除类级事实面：类声明 + 全部字段声明 + 类作用域类型引用。
    void dropClassScopedFacts(std::string_view className);
    /// @brief 摘除该类在索引中的全部事实（整类重扫的前置）。
    void dropAllClassFacts(std::string_view className);
    /// @brief 声明派生查询图重建（first-seen wins，与 build/restore 同构）。
    void rebuildDeclarationLookups();
    /// @brief 补丁收尾：声明图 + 引用图重建 + 反射描述符补全（幂等）。
    void finalizeChangePatch();
    void appendMethodReference(const MethodReference& ref);
    void appendFieldReference(const FieldReference& ref);
    void appendTypeReference(const TypeReference& ref, bool methodScoped);
    void resolveReflectionMemberDescriptors();
    void rebuildReferenceLookups();

    /// 文本三段查询入参 intern 进本池（内容寻址、幂等），故 pool 为 mutable。
    mutable KeyPool pool_;

    std::vector<ClassDeclaration>                                   classDecls_;
    std::vector<MethodDeclaration>                                  methodDecls_;
    std::vector<FieldDeclaration>                                   fieldDecls_;
    std::vector<MethodReference>                                    methodRefs_;
    std::vector<FieldReference>                                     fieldRefs_;
    std::vector<TypeReference>                                      typeRefs_;
    std::unordered_map<std::string_view, std::size_t>               classDeclsByKey_;
    std::unordered_map<MemberKey, std::size_t, MemberKeyHash>       methodDeclsByKey_;
    std::unordered_map<MemberKey, std::size_t, MemberKeyHash>       fieldDeclsByKey_;
    std::unordered_map<std::string_view, std::vector<MethodDeclaration>> methodDeclsByOwner_;
    std::unordered_map<std::string_view, std::vector<FieldDeclaration>>  fieldDeclsByOwner_;
    std::unordered_map<MemberKey, std::vector<MethodReference>, MemberKeyHash> methodRefsByTarget_;
    std::unordered_map<MemberKey, std::vector<FieldReference>, MemberKeyHash>  fieldRefsByTarget_;
    std::unordered_map<std::string_view, std::vector<TypeReference>>    typeRefsByTarget_;
    std::unordered_map<std::string_view, std::vector<MethodReference>>  methodRefsByTargetOwner_;
    std::unordered_map<std::string_view, std::vector<FieldReference>>   fieldRefsByTargetOwner_;
    std::unordered_map<MemberKey, std::vector<MethodReference>, MemberKeyHash> methodRefsBySource_;
    std::unordered_map<MemberKey, std::vector<FieldReference>, MemberKeyHash>  fieldRefsBySource_;
    std::unordered_map<MemberKey, std::vector<TypeReference>, MemberKeyHash>   typeRefsBySource_;
    std::unordered_map<std::string_view, std::vector<MethodReference>>  methodRefsBySourceClass_;
    std::unordered_map<std::string_view, std::vector<FieldReference>>   fieldRefsBySourceClass_;
    std::unordered_map<std::string_view, std::vector<TypeReference>>    typeRefsBySourceClass_;
};

}   // namespace mjbase::reference
