#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::project {

/// @brief 分析工程文件 schema 基线版本（v1 契约，luiox/morpher#1092）。
///
/// v1 顶层形状：
/// ```
/// {
///   "version": 1,
///   "kind": "mjt-analysis-project",
///   "inputs": [
///     {"path": "samples/app.jar", "fingerprint": "sha256:...", "role": "primary"}
///   ],
///   "shadow_sidecar": {"path": "samples/app.jar.mjt-shadow", "fingerprint": "sha256:..."},
///   "next_annotation_id": 4,
///   "annotations": [
///     {"id": 1,
///      "target": {"kind": "method", "owner": "com/x/A", "name": "run", "desc": "(I)V"},
///      "text": "...", "author": "...", "created_at": "2026-10-02T00:00:00Z"}
///   ]
/// }
/// ```
inline constexpr ca::u32 kAnalysisProjectFileVersionBase = 1;

/// @brief 分析工程文件 schema 当前版本（v2；载入接受 1..2，写出按内容懒升级）。
///
/// v2 相对 v1 的增量（定稿依据 doc_ai/analysis-project-schema-v2.md）：
/// - inputs 条目 role 枚举扩展 library|runtime（支撑挂载树的角色化解析栈）；
/// - inputs 条目新增可选 parent 字段（引用同文件内另一 input 的 path，构成
///   挂载树；空 = 顶层挂载）；
/// - 容器 digest 口径裁决为 "sha256:" + 64 hex（schema 层不做内容校验，口径
///   由调用方遵守；fnv1a64 遗留前缀兼容读，上层提示重算）。
///
/// 版本策略（懒升级）：写出 version 2 当且仅当实体携带 v2 特性（任一 role ∈
/// {library, runtime} 或任一 parent 非空），否则恒写 1——未用 v2 特性的工程
/// 对 #1096 冻结 v1 读者保持逐字节可读（零破坏边界最长化）。v1 读者遇 v2 文
/// 件严格拒载（版本门原样拒绝，不提供读 v1 子集的降级；裁决理由见设计文档
/// §1.2）。字段集合严格（未知字段/重复字段即整文件拒载），与 Shadow-Alias
/// 同一纪律；version 1 文件中出现 v2 值（library/runtime role 或非空
/// parent）按 Schema 拒载——版本标 1 就必须真是 v1。
///
/// 实体口径：
/// - **inputs**：被分析对象登记。`fingerprint` 必填（口径由调用方定，库不做
///   内容校验）；`role` 语义：primary=主分析对象、library=依赖库、
///   runtime=运行时镜像（JDK）、support=遗留辅助参考（v1 遗留值，写出保真
///   不静默归一）；`parent` 非空时必须引用已登记 input 的 path 且链条无环。
/// - **shadow_sidecar**：对 shadow sidecar 文件的**引用**（path + 可选
///   fingerprint），sidecar 本体仍是显示别名的唯一事实源——工程文件绝不内嵌
///   别名表，避免双写分叉（#1072 产物引用而非复制）。
/// - **annotations**：人工/工具批注。target 四类：class（仅 owner）、
///   method/field（owner+name+desc）、instruction（owner+name+desc+index，
///   index 为方法体内指令序号）。id 单调不复用（next_annotation_id 持久化，
///   加载时校验 ≥ max(id)+1）。
///
/// 确定性：写出按 inputs 路径字典序、annotations 按 id 升序稳定排列；
/// parse(write(parse(x))) 关键字段恒等（v1 与 v2 各自成立）。
inline constexpr ca::u32 kAnalysisProjectFileVersion = 2;

/// @brief 分析工程文件 kind 标记（与 shadow sidecar JSON 自描述区分）。
inline constexpr const char* kAnalysisProjectKind = "mjt-analysis-project";

/// @brief 输入 jar 在工程中的角色。
enum class AnalysisInputRole : ca::u8
{
    Primary,  ///< 主分析对象
    Support,  ///< 辅助参考（v1 遗留值：遗留文件写出保真；新登记用 Library）
    Library,  ///< 依赖库（v2；角色化解析序在 primary 之后）
    Runtime,  ///< 运行时镜像即 JDK（v2；角色化解析序最后）
};

/// @brief role 串 → 枚举（解析共用；tag 集合见常量注释）。
///
/// 本体在 .cpp（roleTag/parseRole 对）；此处只暴露语义判定：v2 特有角色。
inline bool isV2InputRole(AnalysisInputRole role) noexcept
{
    return role == AnalysisInputRole::Library || role == AnalysisInputRole::Runtime;
}

/// @brief 批注锚定目标的类别。
enum class AnnotationTargetKind : ca::u8
{
    Class,        ///< 类级：owner
    Method,       ///< 方法级：owner + name + desc
    Field,        ///< 字段级：owner + name + desc
    Instruction,  ///< 指令级：owner + name + desc + index（方法体内序号）
};

/// @brief 工程内登记的一个输入。
struct AnalysisInput
{
    ca::str::Utf8String path;         ///< jar/工程路径（非空，工程内唯一）
    ca::str::Utf8String fingerprint;  ///< 内容指纹（非空；v2 口径 "sha256:"+64hex）
    AnalysisInputRole   role = AnalysisInputRole::Primary;
    ca::str::Utf8String parent;       ///< 挂载父容器的登记 path（v2；空 = 顶层挂载）
};

/// @brief 批注锚定目标。
struct AnnotationTarget
{
    AnnotationTargetKind kind = AnnotationTargetKind::Class;
    ca::str::Utf8String  owner;   ///< 类内部名（恒必填）
    ca::str::Utf8String  name;    ///< method/field/instruction 必填；class 必须为空
    ca::str::Utf8String  desc;    ///< method/field/instruction 必填；class 必须为空
    ca::i64              index = -1;  ///< 仅 instruction ≥ 0；其余恒 -1
};

/// @brief 一条批注。
struct AnalysisAnnotation
{
    ca::u64         id = 0;  ///< 工程内唯一，单调分配不复用
    AnnotationTarget target;
    ca::str::Utf8String text;       ///< 批注正文（非空）
    ca::str::Utf8String author;     ///< 作者/工具名（非空）
    ca::str::Utf8String createdAt;  ///< ISO-8601 字符串（非空；格式库不校验）
};

/// @brief 对 shadow sidecar 文件的引用（非内嵌；fingerprint 可空）。
struct ShadowSidecarRef
{
    ca::str::Utf8String path;         ///< sidecar 文件路径（非空）
    ca::str::Utf8String fingerprint;  ///< 内容指纹（可空；口径后续统一）
};

/// @brief 分析工程解析/读写错误类别。
enum class AnalysisProjectErrorKind : ca::u8
{
    JsonRead,  ///< JSON 语法错误 / 文件打不开
    Schema,    ///< schema 违约：字段缺失/类型/未知字段/id 冲突/版本不识别
    Io,        ///< 写出失败
};

/// @brief 分析工程诊断（人读消息；文件级 API 再补路径前缀）。
struct AnalysisProjectError
{
    AnalysisProjectErrorKind kind = AnalysisProjectErrorKind::Schema;
    ca::str::Utf8String      message;
};

/// @brief 分析工程最小库：实体登记 + 严格 JSON v1/v2 读写（luiox/morpher#1092）。
///
/// 纯数据层，不感知 CLI/MCP/执行接口（P2 起）与工作区会话；值语义存储
/// （人工规模实体量，无热路径，不引入 arena 视图生命周期）。返回指针/引用
/// 的查询在任一增删后失效——表式 API 常规纪律。
class AnalysisProject
{
public:
    /// @brief 空工程（version/kind 由写出层补齐）。
    static AnalysisProject createEmpty();

    AnalysisProject() = default;

    //-- inputs ------------------------------------------------------------

    /// @brief 登记输入；path 重复、parent 非空但未登记或构成环时返回 false（不改动）。
    bool addInput(AnalysisInput input);

    /// @brief 按 path 摘除输入；返回摘除条数（0 或 1）。
    ///
    /// 目标被其他条目的 parent 引用（v2 挂载树）时拒绝摘除返回 0——不允许
    /// 在内存里构造悬垂挂载引用（解析期同款纪律）。
    ca::usize removeInput(const ca::str::Utf8StringRef& path);

    /// @brief 按 path 查输入；未登记返回 nullptr。
    const AnalysisInput* findInput(const ca::str::Utf8StringRef& path) const;

    const std::vector<AnalysisInput>& inputs() const { return inputs_; }

    //-- shadow sidecar 引用（可选单槽） ------------------------------------

    void                 setShadowSidecar(ShadowSidecarRef ref);
    void                 clearShadowSidecar() { shadowSidecar_.reset(); }
    const ShadowSidecarRef* shadowSidecar() const { return shadowSidecar_ ? &*shadowSidecar_ : nullptr; }

    //-- annotations -------------------------------------------------------

    /// @brief 追加批注；target/正文任一非法返回 0（不分配 id）。
    ///
    /// id 单调分配（nextAnnotationId_，删除不复用）。
    ca::u64 addAnnotation(AnnotationTarget          target,
                          const ca::str::Utf8StringRef& text,
                          const ca::str::Utf8StringRef& author,
                          const ca::str::Utf8StringRef& createdAt);

    /// @brief 按 id 摘除批注；命中 true。
    bool removeAnnotation(ca::u64 id);

    /// @brief 按 id 查批注；未命中 nullptr。
    const AnalysisAnnotation* findAnnotation(ca::u64 id) const;

    /// @brief 某类下的全部批注（按 id 升序；指针在增删后失效）。
    std::vector<const AnalysisAnnotation*> annotationsOf(
        const ca::str::Utf8StringRef& owner) const;

    const std::vector<AnalysisAnnotation>& annotations() const { return annotations_; }

    /// @brief 下一个将分配的批注 id（加载后 ≥ 文件内 max(id)+1）。
    ca::u64 nextAnnotationId() const { return nextAnnotationId_; }

    //-- 整体 --------------------------------------------------------------

    bool    empty() const { return inputs_.empty() && annotations_.empty() && !shadowSidecar_; }
    void    clear();
    ca::usize inputCount() const { return inputs_.size(); }
    ca::usize annotationCount() const { return annotations_.size(); }

private:
    std::vector<AnalysisInput>            inputs_;
    std::unordered_map<std::string, ca::usize> inputIndex_;    ///< path → inputs_ 下标
    std::optional<ShadowSidecarRef>       shadowSidecar_;
    std::vector<AnalysisAnnotation>       annotations_;
    std::unordered_map<ca::u64, ca::usize>     annotationIndex_;  ///< id → annotations_ 下标
    std::unordered_map<std::string, std::vector<ca::usize>> ownerIndex_;  ///< owner → 下标集
    ca::u64                               nextAnnotationId_ = 1;

    // 加载绕过 addAnnotation 的单调 id 分配（文件 id 是事实），直塞内部表。
    friend ca::Result<AnalysisProject, AnalysisProjectError> parseAnalysisProjectText(
        const ca::str::Utf8StringRef& text);
};

/// @brief target 合法性（addAnnotation 与解析共用口径）：见 hpp 头部实体注释。
bool isValidAnnotationTarget(const AnnotationTarget& target);

/// @brief 严格解析工程 JSON 文本：任一 schema 错误即整文件拒载（零登记）。
ca::Result<AnalysisProject, AnalysisProjectError> parseAnalysisProjectText(
    const ca::str::Utf8StringRef& text);

/// @brief 从文件读取并严格解析工程；错误消息含路径。
ca::Result<AnalysisProject, AnalysisProjectError> readAnalysisProjectFile(
    const ca::str::Utf8StringRef& path);

/// @brief 序列化工程（pretty JSON；inputs 按路径字典序、annotations 按 id 稳定排列）。
///
/// 验收口径：parse(write(parse(x))) 关键字段恒等；同实体集任意插入序输出逐字节一致。
ca::Result<ca::str::Utf8String, AnalysisProjectError> writeAnalysisProjectText(
    const AnalysisProject& project);

/// @brief 序列化并写出到文件。
ca::Result<void, AnalysisProjectError> writeAnalysisProjectFile(
    const ca::str::Utf8StringRef& path, const AnalysisProject& project);

}   // namespace mjbase::project
