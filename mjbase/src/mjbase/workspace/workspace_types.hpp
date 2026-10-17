#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"

namespace mjbase::workspace {

/// @file workspace_types.hpp
/// @brief Workspace 接口层共享的数据传输类型（DTO）。
///
/// 无业务语义的纯数据结构 + 枚举 + 常量：IWorkspace 接口签名与 WorkspaceBase
/// 实现共同使用。移植自 mbase/workspace/workspace_types（语义照旧），类型面按
/// mjbase 约定重定型：
/// - 错误通道：ca::Status/StatusOr → WorkspaceError + WorkspaceResult<T>
///   （mjbase 错误走 Result 值语义的总约定）；
/// - 诊断：jasm::Diagnostic 即 mjbase::jasm::Diagnostic（同形迁移）。
/// 依赖旧栈 remapper/renamer/pass-registry 的 DTO（MemberRemapPlan/RemapPolicy、
/// RenameRequest 族、PipelineRequest 族、WorkflowRequest 族）不随迁——对应设施
/// 未在 mjbase 落地，接口面见 i_workspace.hpp 的范围说明。

/// @brief workspace 模块错误码。与旧 ca::StatusCode 用点的映射：
///        InvalidArgument/NotFound/AlreadyExists/FailedPrecondition/
///        PermissionDenied/Aborted/DataLoss/Internal/Unimplemented 一一对应。
enum class WorkspaceError : ca::u8
{
    Ok = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    FailedPrecondition,
    PermissionDenied,
    Aborted,
    DataLoss,
    Internal,
    Unimplemented,
};

/// @brief 错误码 → 短名（断言输出/日志用）。
const char* to_string(WorkspaceError error) noexcept;

/// @brief workspace 模块错误值：码 + 消息文本（结构对齐 spi::PipelineError 的
///        模块边界错误形态——Result 错误通道要求可拷贝，诊断文本落在 std::string）。
///        message 沿用旧 ca::Status 的 "operation: detail" 拼接口径。
struct WorkspaceErrorInfo
{
    WorkspaceError code = WorkspaceError::Ok;
    std::string    message;

    bool is_ok() const noexcept { return code == WorkspaceError::Ok; }
    bool is_err() const noexcept { return code != WorkspaceError::Ok; }
};

/// @brief workspace 模块 Result 别名；Ok 恒为 code == Ok 的错误值。
template <typename T>
using WorkspaceResult = ca::core::Result<T, WorkspaceErrorInfo>;

/// @brief 工作区当前承载的主制品种类。
enum class ArtifactKind : ca::u8
{
    None,
    Class,
    Jar,
};

/// @brief 未提供 expectedRevision 时使用的显式哨兵值。
constexpr ca::u64 kAnyRevision = static_cast<ca::u64>(-1);

/// @brief 单次原始 class bytes 替换允许的服务端硬上限。
constexpr ca::usize kMaximumClassReplacementSize = 16U * 1024U * 1024U;

// ── JVM access flags（JVMS 表 4.1/4.4/4.5/4.6；数值即位掩码）──
inline constexpr ca::u32 kAccPublic       = 0x0001;
inline constexpr ca::u32 kAccPrivate      = 0x0002;
inline constexpr ca::u32 kAccProtected    = 0x0004;
inline constexpr ca::u32 kAccStatic       = 0x0008;
inline constexpr ca::u32 kAccFinal        = 0x0010;
inline constexpr ca::u32 kAccSuper        = 0x0020;
inline constexpr ca::u32 kAccSynchronized = 0x0020;
inline constexpr ca::u32 kAccVolatile     = 0x0040;
inline constexpr ca::u32 kAccBridge       = 0x0040;
inline constexpr ca::u32 kAccTransient    = 0x0080;
inline constexpr ca::u32 kAccVarargs      = 0x0080;
inline constexpr ca::u32 kAccNative       = 0x0100;
inline constexpr ca::u32 kAccInterface    = 0x0200;
inline constexpr ca::u32 kAccAbstract     = 0x0400;
inline constexpr ca::u32 kAccStrict       = 0x0800;
inline constexpr ca::u32 kAccSynthetic    = 0x1000;
inline constexpr ca::u32 kAccAnnotation   = 0x2000;
inline constexpr ca::u32 kAccEnum         = 0x4000;
inline constexpr ca::u32 kAccModule       = 0x8000;
inline constexpr ca::u32 kAccRecord       = 0x10000;
inline constexpr ca::u32 kAccSealed       = 0x10000;
inline constexpr ca::u32 kAccDeprecated   = 0x20000;

/// @brief 读写操作使用的乐观并发控制选项。
struct MutationRequest final
{
    ca::u64     expectedRevision = kAnyRevision;
    bool        dryRun           = false;
    std::string label;
};

/// @brief 一个变更字节区间，长度分别描述变更前后内容。
struct ByteRange final
{
    ca::usize offset       = 0;
    ca::usize beforeLength = 0;
    ca::usize afterLength  = 0;
};

/// @brief 单个资源的紧凑字节级 diff 摘要。
struct ByteDiff final
{
    std::string            uri;
    ca::usize              beforeSize = 0;
    ca::usize              afterSize  = 0;
    std::vector<ByteRange> ranges;
};

/// @brief 修改 class 后重新索引到的直接引用事实。
struct ReferenceImpact final
{
    std::string kind;
    std::string target;
};

/// @brief 所有修改操作共用的稳定结果协议。
///
/// status 为 Ok 时 committed/exportable/revision 才有意义；失败路径仍回填
/// revision 与 exportable（与旧 ca::Status 语义一致）。
struct MutationResult final
{
    WorkspaceErrorInfo                 status;
    ca::u64                            revision = 0;
    bool                               committed = false;
    bool                               exportable = false;
    std::vector<jasm::Diagnostic>      diagnostics;
    std::vector<ByteDiff>              byteDiffs;
    std::vector<std::string>           affectedClasses;
    std::vector<ReferenceImpact>       affectedReferences;

    bool is_ok() const noexcept { return status.is_ok(); }
};

/// @brief JAR 内资源的稳定摘要。
struct ResourceSummary final
{
    std::string uri;
    std::string type;
    ca::usize   size = 0;
};

/// @brief 资源的原始字节读取结果。
struct ResourceContent final
{
    ResourceSummary      summary;
    std::vector<ca::u8> bytes;
};

/// @brief 一个 class 资源的原始字节和稳定内容标识。
///
/// internalName 在 class header 损坏时为空；调用方仍可通过精确资源 URI 读取原始字节用于诊断。
struct ClassBytesInfo final
{
    std::string         resourceUri;
    std::string         internalName;
    std::string         fingerprint;
    std::vector<ca::u8> bytes;
};

/// @brief 当前工作副本中一个相对原始 JAR 已变化的 class 摘要。
struct ModifiedClassInfo final
{
    std::string resourceUri;
    std::string originalInternalName;
    std::string currentInternalName;
    std::string originalFingerprint;
    std::string currentFingerprint;
    ca::usize   originalSize = 0;
    ca::usize   currentSize = 0;
};

/// @brief 注解元素的完整值树。
struct AnnotationElement final
{
    std::string                    name;
    char                           tag = 's';
    std::string                    value;
    std::string                    annotationDescriptor;
    std::vector<AnnotationElement> children;
};

/// @brief 一个运行时可见或不可见注解。
struct AnnotationInfo final
{
    std::string                    descriptor;
    bool                           visible = false;
    std::vector<AnnotationElement> elements;
};

/// @brief 局部变量调试表条目。
struct LocalVariableInfo final
{
    std::string name;
    std::string descriptor;
    std::string signature;
    ca::i32     index = 0;
};

/// @brief 方法字节码中的源码行号映射。
///
/// instructionIndex 为方法体内代码指令序号（不含 label/line 节点——mjbase
/// 底座 CodeDom 中标签是句柄不是指令）；pc 无对应指令时为 -1。
struct LineNumberInfo final
{
    ca::i32 instructionIndex = -1;
    ca::i32 line             = -1;
};

/// @brief 一条方法字节码指令的稳定摘要。
struct InstructionInfo final
{
    ca::i32      index  = -1;
    ca::i32      opcode = -1;
    std::string  mnemonic;
    std::string  nodeType;
};

/// @brief switch 指令的一个跳转分支（key 与绝对目标 PC）。
struct SwitchCaseInfo final
{
    ca::i32 key      = 0;
    ca::i32 targetPc = 0;
};

/// @brief 一条字节码指令的完整渲染信息（保留 CP 索引与字节 PC）。
/// @details 供 info/disasm 消费。pc 为该指令在方法 code 数组中的字节偏移；
/// cpIndex 为该指令引用的常量池条目索引（无引用为 -1）；operandText 为助记符后
/// 的操作数文本（如 "#5"/"3, 1"/"#5 dims=2"）；operandComment 为 resolve 后的
/// 人类可读注释（owner.name:desc / "str" / 字面值，已过 IdentifierUtil 可读化）；
/// switchDefaultPc>=0 时该指令是 switch，switchCases 列出各分支。
struct InstructionDetailInfo final
{
    ca::i32                          pc             = -1;
    ca::i32                          opcode         = -1;
    std::string                      mnemonic;
    ca::i32                          cpIndex        = -1;
    std::string                      operandText;
    std::string                      operandComment;
    bool                             isSwitch       = false;
    ca::i32                          switchDefaultPc = -1;
    std::vector<SwitchCaseInfo>      switchCases;
};

/// @brief 一个方法的完整指令渲染。
struct MethodInstructions final
{
    ca::i32                              maxStack      = 0;
    ca::i32                              maxLocals     = 0;
    std::vector<InstructionDetailInfo>   instructions;
};

/// @brief 常量池条目的稳定摘要。
/// @details tag 为标签名（"Utf8"/"Integer"/"MethodRef" 等，与 JVM 规范常量名对齐）；
/// text 为条目值文本：非引用类条目总是填充；引用类条目仅在 resolve=true 时填充
/// 解析后的 owner.name:desc 文本，否则为空。malformed 标记 Long/Double 的第二槽
/// 占位条目（tag/text 为空，保留索引占位使条目数与常量池槽数一致）。
struct ConstantPoolEntryInfo final
{
    ca::usize   index = 0;
    std::string tag;
    std::string text;
    bool        malformed = false;
};

/// @brief 常量值摘要。text 为中性渲染文本（无类型后缀、字符串不加引号）。
struct ConstantValueInfo final
{
    /// @brief 常量值类型，决定 text 的量纲与调用方应补的后缀。
    enum class Kind
    {
        Integer,
        Long,
        Float,
        Double,
        String
    };
    Kind        kind = Kind::Integer;
    std::string text;
};

/// @brief 字段的结构化元数据。
struct FieldInfo final
{
    std::string                          name;
    std::string                          descriptor;
    std::string                          signature;
    ca::i32                              access = 0;
    std::vector<AnnotationInfo>          annotations;
    /// ConstantValue 属性；无该属性则为 nullopt。
    std::optional<ConstantValueInfo>     constantValue;
};

/// @brief 方法的结构化元数据。
struct MethodInfo final
{
    std::string                    name;
    std::string                    descriptor;
    std::string                    signature;
    ca::i32                        access = 0;
    ca::i32                        maxStack = 0;
    ca::i32                        maxLocals = 0;
    ca::usize                      instructionCount = 0;
    ca::usize                      tryCatchCount = 0;
    std::vector<std::string>       exceptions;
    std::vector<LocalVariableInfo> localVariables;
    std::vector<LineNumberInfo>    lineNumbers;
    std::vector<AnnotationInfo>    annotations;
};

/// @brief 精确选择的方法及其字节码指令摘要。
struct MethodBytecode final
{
    MethodInfo                   method;
    std::vector<InstructionInfo> instructions;
};

/// @brief class 列表使用的轻量结构摘要。
/// @details 不保留 signature、annotation、成员详情或方法体；详细查询使用 ClassInfo。
struct ClassSummary final
{
    std::string resourceUri;
    std::string internalName;
    std::string superName;
    ca::i32     access         = 0;
    ca::i32     version        = 0;
    ca::usize   interfaceCount = 0;
    ca::usize   fieldCount     = 0;
    ca::usize   methodCount    = 0;
};

/// @brief class 的结构化元数据。
struct ClassInfo final
{
    std::string                 resourceUri;
    std::string                 internalName;
    std::string                 superName;
    std::string                 signature;
    ca::i32                     access = 0;
    ca::i32                     version = 0;
    std::vector<std::string>    interfaces;
    std::vector<FieldInfo>      fields;
    std::vector<MethodInfo>     methods;
    std::vector<std::string>    innerClasses;
    std::vector<AnnotationInfo> annotations;
};

/// @brief MANIFEST 主属性的稳定键值条目。
struct ManifestAttribute final
{
    std::string key;
    std::string value;
};

/// @brief 非 class 资源中提取出的可打印字符串。
struct ResourceStringInfo final
{
    std::string uri;
    ca::usize   offset = 0;
    std::string value;
};

/// @brief 一个可分页传输的结构化搜索命中。
///
/// kind 表示命中来源，例如 class、field、method、constant_pool_string、resource_string、
/// constant_pool_number、instruction 或 instruction_number。instructionIndex 只在方法指令
/// 命中时有效（代码指令序号）；其余命中为 -1。
struct SearchHit final
{
    std::string resourceUri;
    std::string kind;
    std::string owner;
    std::string name;
    std::string descriptor;
    ca::i32     instructionIndex = -1;
    std::string value;
};

/// @brief 文本搜索的匹配模式。
enum class SearchMatchMode : ca::u8
{
    Contains,
    Exact,
    Regex,
};

/// @brief Workspace 五类搜索共用的过滤条件。
///
/// owner 和 packagePrefix 使用 JVM internal name；memberKind 为空或 field/method；opcode 为 -1 时
/// 不过滤。并非每类搜索都使用全部字段，例如 class 搜索忽略 memberKind/opcode。
struct SearchOptions final
{
    SearchMatchMode matchMode = SearchMatchMode::Contains;
    bool            caseSensitive = false;
    std::string     owner;
    std::string     packagePrefix;
    std::string     memberKind;
    ca::i32         opcode = -1;
};

/// @brief 方法覆盖组件中的一个声明。
struct MethodOverrideInfo final
{
    std::string owner;
    std::string name;
    std::string descriptor;
    bool        primary = false;
    bool        supporting = false;
    std::string supportId;
};

/// @brief 支持库的只读描述。
struct SupportingResourceInfo final
{
    std::string id;
    std::string path;
    std::string fingerprint;
    ca::usize   resourceCount = 0;
    ca::usize   classCount = 0;
};

/// @brief 精确 xref 查询返回的引用位置和目标。
///
/// instructionIndex 为方法体内代码指令序号；class 级引用（头/结构元数据）为 -1。
struct ReferenceInfo final
{
    std::string sourceClass;
    std::string sourceMethod;
    std::string sourceDescriptor;
    ca::i32     instructionIndex = -1;
    std::string target;
    std::string kind;
    bool        supporting = false;
    std::string supportId;
};

/// @brief 继承关系结果中一个 class 的来源。
struct ClassRelationSource final
{
    std::string internalName;
    bool        supporting = false;
    std::string supportId;
};

/// @brief 一个 class 的继承、接口和后代闭包。
struct ClassRelations final
{
    std::vector<std::string> superclasses;
    std::vector<std::string> interfaces;
    std::vector<std::string> directSubclasses;
    std::vector<std::string> allSubclasses;
    std::vector<std::string> implementors;
    std::vector<ClassRelationSource> sources;
};

/// @brief 一项可由 JAR 修复事务处理的资源问题。
struct JarRepairIssue final
{
    std::string uri;
    std::string kind;
    std::string message;
    ca::usize   size = 0;
    bool        selectedForRemoval = false;
};

/// @brief JAR 修复的显式、可审阅删除策略。
struct JarRepairRequest final
{
    bool removeInvalidClassEntries = false;
    bool removeCorruptOutputMarkers = false;
};

/// @brief JAR 修复扫描结果及候选事务预览或提交结果。
struct JarRepairResult final
{
    MutationResult              mutation;
    ca::usize                   resourceCount = 0;
    ca::usize                   classCount = 0;
    std::vector<JarRepairIssue> issues;
};

/// @brief 一个工作区与外部 JAR 的全资源字节比较结果。
struct JarDiffResult final
{
    WorkspaceErrorInfo      status;
    std::string             referencePath;
    ca::usize               workspaceOnly = 0;
    ca::usize               referenceOnly = 0;
    ca::usize               modified = 0;
    std::vector<ByteDiff>   byteDiffs;
};

/// @brief 已完成工作流候选 JAR 的内部提交请求。
///
/// candidateJarPath 必须由上层事务私有持有，不能直接暴露给不可信客户端。exportPath 非空时会在
/// commit 前从同一候选原子导出；commit 失败会删除该导出。dryRun 只验证候选和导出目标。
///
/// 旧栈 WorkflowRequest（remapPlan/pipeline/referenceQueries 组合事务）依赖未迁的
/// remapper/pass-registry 设施，不随迁（见 i_workspace.hpp 范围说明）；候选 JAR 的
/// 导入-验证-导出-提交契约由本类型与 commitWorkflowCandidateJar 承载。
struct WorkflowCandidateCommitRequest final
{
    std::string candidateJarPath;
    std::string exportPath;
};

/// @brief 工作流候选 JAR 导入、导出和提交的统一结果。
struct WorkflowCandidateCommitResult final
{
    MutationResult mutation;
    std::string    exportPath;
    bool           exported = false;
};

}   // namespace mjbase::workspace
