#pragma once

#include "mjbase/workspace/class_mutation_service.hpp"
#include "mjbase/workspace/workspace_types.hpp"

namespace mjbase::workspace {

/// @file i_workspace.hpp
/// @brief Workspace 抽象接口：业务契约方法集。
///
/// 各项目面向本接口编程；mjbase 提供 WorkspaceBase 基础实现（无业务语义的
/// jar 打开/资源缓存/搜索与变更骨架）。接口签名类型全部来自 workspace_types.hpp。
///
/// 事务契约：原始资源与工作副本分别保存。修改始终先发生
/// 在候选副本；只有候选完成严格验证（spi::sanitizeClassFile 引用语义预检 + Jasm
/// 验证）后才替换工作副本并增加 revision。调用方用 expectedRevision 可拒绝过期写入。
///
/// remap / rename / pipeline / workflow 方法族：
/// - 候选副本语义贯穿：analyze* 与 validate* 不触碰工作副本，apply* 与
///   runWorkflow 验证通过才原子提交并递增 revision；
/// - pipeline 组成以 mjbase 自有 pass 集为准（spi::PassRegistry 内置表），
///   "default-optimize" 等同名管线的组成以本注册表为准，消费方经
///   availablePasses/availablePipelines 枚举；
/// - PipelineRequest.jdkPath 非空时扫描 JDK 归档建层次图，经
///   spi::StrictTypeResolverAdapter 接入帧重算。
class IWorkspace
{
public:
    /// @brief 接口析构；实现类必须可安全析构。
    IWorkspace() = default;
    virtual ~IWorkspace() = default;

    IWorkspace(const IWorkspace&)            = delete;
    IWorkspace& operator=(const IWorkspace&) = delete;
    IWorkspace(IWorkspace&&)                 = delete;
    IWorkspace& operator=(IWorkspace&&)      = delete;

    /// @brief 打开主 JAR 并建立不可变原始快照与可修改工作副本。
    virtual WorkspaceResult<void> openJar(const std::string& path) = 0;

    /// @brief 打开单个 classfile，并以其 JVM internal name 建立稳定资源 URI。
    virtual WorkspaceResult<void> openClass(const std::string& path) = 0;

    /// @brief 添加只读 supporting JAR，用于后续层次和调用解析。
    /// @return 返回当前 Workspace 内稳定的 support ID 和内容 fingerprint；重复路径或内容被拒绝。
    virtual WorkspaceResult<SupportingResourceInfo> addSupportingJar(const std::string& path) = 0;

    /// @brief 按 support ID 移除只读 supporting JAR，并失效依赖 classpath 的分析缓存。
    /// @return 返回被移除的 supporting resource 描述；ID 不存在时返回 NotFound。
    virtual WorkspaceResult<SupportingResourceInfo> removeSupportingJar(const std::string& id) = 0;

    /// @brief 工作区是否已加载主 class 或 JAR。
    virtual bool isLoaded() const noexcept = 0;

    /// @brief 当前主制品种类；未加载时返回 ArtifactKind::None。
    virtual ArtifactKind artifactKind() const noexcept = 0;

    /// @brief 当前主资源的来源路径。
    virtual const std::string& sourcePath() const noexcept = 0;

    /// @brief 单 class 工作区的稳定资源 URI；其他 kind 返回空字符串。
    virtual const std::string& primaryClassUri() const noexcept = 0;

    /// @brief 当前工作区 revision；只在已提交变更、undo、redo 或 restore 后递增。
    virtual ca::u64 revision() const noexcept = 0;

    /// @brief 当前工作副本是否和原始快照不同。
    virtual bool isDirty() const = 0;

    /// @brief 当前工作副本是否满足导出条件。
    virtual bool canExport() const noexcept = 0;

    /// @brief 返回主资源的稳定排序摘要。
    virtual WorkspaceResult<std::vector<ResourceSummary>> listResources(bool includeClasses = true) const = 0;

    /// @brief 读取一个主资源的原始字节。
    virtual WorkspaceResult<ResourceContent> readResource(const std::string& uri) const = 0;

    /// @brief 按资源 URI 或 internal name 读取 class 原始字节和内容 fingerprint。
    /// @note 精确 URI 指向损坏 class 时仍返回 bytes，此时 internalName 为空。
    virtual WorkspaceResult<ClassBytesInfo> readClassBytes(const std::string& uriOrInternalName) const = 0;

    /// @brief 返回所有主 class 的轻量结构摘要。
    virtual WorkspaceResult<std::vector<ClassSummary>> listClassSummaries() const = 0;

    /// @brief 返回所有主 class 的完整结构化元数据。
    virtual WorkspaceResult<std::vector<ClassInfo>> listClasses() const = 0;

    /// @brief 通过资源 URI 或 JVM internal name 查询 class 详情。
    virtual WorkspaceResult<ClassInfo> classInfo(const std::string& uriOrInternalName) const = 0;

    /// @brief 枚举 class 常量池条目。
    /// @param uriOrInternalName 资源 URI 或 internal name。
    /// @param resolve true 时解析引用类条目（FieldRef/MethodRef/InterfaceMethodRef/
    /// NameAndType/InvokeDynamic/Dynamic）的 owner.name:desc 文本填入 text；false 时
    /// 引用类条目的 text 为空。非引用类条目（Utf8/Integer 等）总是填充 text。
    /// @note Long/Double 的第二槽保留空占位条目（malformed=true）；损坏 class 经
    ///       libjbc 解析失败时返回 DataLoss。
    virtual WorkspaceResult<std::vector<ConstantPoolEntryInfo>> constantPool(
        const std::string& uriOrInternalName, bool resolve) const = 0;

    /// @brief 以 owner/name/descriptor 精确查询方法详情。
    virtual WorkspaceResult<MethodInfo> methodInfo(const std::string& owner, const std::string& name,
                                                   const std::string& descriptor) const = 0;

    /// @brief 以 owner/name/descriptor 精确查询字段详情。
    virtual WorkspaceResult<FieldInfo> fieldInfo(const std::string& owner, const std::string& name,
                                                 const std::string& descriptor) const = 0;

    /// @brief 返回精确选择方法的逐指令摘要及行号元数据。
    virtual WorkspaceResult<MethodBytecode> methodBytecode(const std::string& owner,
                                                           const std::string& name,
                                                           const std::string& descriptor) const = 0;

    /// @brief 返回精确选择方法的完整指令渲染（保留 CP 索引与字节 PC）。
    /// @param resolve true 时填充 operandComment（解析 CP 引用为 owner.name:desc 等可读文本）；
    /// false 时 operandComment 为空。
    virtual WorkspaceResult<MethodInstructions> methodInstructions(const std::string& owner,
                                                                   const std::string& name,
                                                                   const std::string& descriptor,
                                                                   bool               resolve) const = 0;

    /// @brief 返回精确选择方法的规范化 Jasm 声明。
    virtual jasm::Result<jasm::JvmJasmMethodDisassembly> getMethodJasm(
        const std::string& owner, const std::string& name, const std::string& descriptor) const = 0;

    /// @brief 返回主资源 MANIFEST 的稳定排序主属性。
    virtual WorkspaceResult<std::vector<ManifestAttribute>> manifestAttributes() const = 0;

    /// @brief 提取主资源所有非 class 条目中的最短长度为 @p minimumLength 的可打印字符串。
    virtual WorkspaceResult<std::vector<ResourceStringInfo>> resourceStrings(
        ca::usize minimumLength = 4) const = 0;

    /// @brief 按 class 内部名或资源 URI 查询 class；损坏 class 会被跳过。
    virtual WorkspaceResult<std::vector<SearchHit>> searchClasses(
        const std::string& query, const SearchOptions& options = {}) const = 0;

    /// @brief 按 owner/name/descriptor/signature 查询字段和方法；损坏 class 会被跳过。
    virtual WorkspaceResult<std::vector<SearchHit>> searchMembers(
        const std::string& query, const SearchOptions& options = {}) const = 0;

    /// @brief 查询 class 常量池字符串和非 class 可打印资源字符串；损坏 class 会被跳过。
    virtual WorkspaceResult<std::vector<SearchHit>> searchStrings(
        const std::string& query, const SearchOptions& options = {}) const = 0;

    /// @brief 精确查询常量池及即时数值字面量；数值文本支持等价的标准数值写法。
    virtual WorkspaceResult<std::vector<SearchHit>> searchNumbers(
        const std::string& query, const SearchOptions& options = {}) const = 0;

    /// @brief 按 mnemonic、opcode、节点类型和指令操作数查询方法指令；损坏 class 会被跳过。
    virtual WorkspaceResult<std::vector<SearchHit>> searchInstructions(
        const std::string& query, const SearchOptions& options = {}) const = 0;

    /// @brief 精确返回所有编码目标为 owner/name/descriptor 的方法引用。
    virtual WorkspaceResult<std::vector<ReferenceInfo>> xrefsToMethod(const std::string& owner,
                                                                      const std::string& name,
                                                                      const std::string& descriptor) const = 0;

    /// @brief 精确返回所有编码目标为 owner/name/descriptor 的字段引用。
    virtual WorkspaceResult<std::vector<ReferenceInfo>> xrefsToField(const std::string& owner,
                                                                     const std::string& name,
                                                                     const std::string& descriptor) const = 0;

    /// @brief 返回所有直接或结构性引用 internalName 的位置。
    virtual WorkspaceResult<std::vector<ReferenceInfo>> xrefsToClass(const std::string& internalName) const = 0;

    /// @brief 返回方法的直接调用目标；resolved 为 true 时扩展虚调用和接口调用。
    virtual WorkspaceResult<std::vector<ReferenceInfo>> calleesOfMethod(const std::string& owner,
                                                                        const std::string& name,
                                                                        const std::string& descriptor,
                                                                        bool resolved = false) const = 0;

    /// @brief 返回目标 class 的超类、接口、直接/全部子类和实现者。
    virtual WorkspaceResult<ClassRelations> classRelations(const std::string& internalName) const = 0;

    /// @brief 返回精确方法所在的继承/实现覆盖组件。
    virtual WorkspaceResult<std::vector<MethodOverrideInfo>> overridesOfMethod(
        const std::string& owner, const std::string& name, const std::string& descriptor) const = 0;

    /// @brief 返回可供 MCP/CLI 直接传输的完整 class JASM。
    virtual jasm::Result<jasm::JvmJasmDisassembly> getJasm(const std::string& uri) const = 0;

    /// @brief 在不提交的候选副本中汇编并验证完整 class JASM。
    virtual MutationResult validateJasm(const std::string& uri, ca::str::Utf16StringRef sourceName,
                                        ca::str::Utf16StringRef source,
                                        const MutationRequest& request = {}) = 0;

    /// @brief 原子应用完整 class JASM，成功后产生一个新的 revision。
    virtual MutationResult applyJasm(const std::string& uri, ca::str::Utf16StringRef sourceName,
                                     ca::str::Utf16StringRef source,
                                     const MutationRequest& request = {}) = 0;

    /// @brief 验证由专用 classfile adapter 生成的完整 class 候选字节，不提交工作区。
    virtual MutationResult validateClassBytes(const std::string& uri,
                                              const std::vector<ca::u8>& classBytes,
                                              const MutationRequest& request = {}) = 0;

    /// @brief 原子应用已由专用 classfile adapter 生成的完整 class 候选字节。
    virtual MutationResult applyClassBytes(const std::string& uri,
                                           const std::vector<ca::u8>& classBytes,
                                           const MutationRequest& request = {}) = 0;

    /// @brief 在不提交的候选副本中应用并验证一项结构化 class 修改。
    virtual MutationResult validateClassMutation(const ClassMutation& mutation,
                                                 const MutationRequest& request = {}) = 0;

    /// @brief 原子应用一项结构化 class 修改，成功后产生一个新的 revision。
    virtual MutationResult applyClassMutation(const ClassMutation& mutation,
                                              const MutationRequest& request = {}) = 0;

    /// @brief 原子替换一个 class 的原始字节；dry-run 时只返回完整验证和 diff。
    virtual MutationResult replaceClassBytes(const std::string& uriOrInternalName,
                                             const std::vector<ca::u8>& bytes,
                                             const MutationRequest& request = {}) = 0;

    /// @brief 返回当前工作副本中相对打开时原始快照已变化的 class。
    virtual WorkspaceResult<std::vector<ModifiedClassInfo>> modifiedClasses() const = 0;

    /// @brief 在不提交候选副本的情况下分析一个显式 MemberRemapPlan 的影响。
    virtual ImpactAnalysis analyzeRemap(const asm_remapper::MemberRemapPlan& plan,
                                        const asm_remapper::RemapPolicy& policy) const = 0;

    /// @brief 原子应用一个显式 MemberRemapPlan。
    virtual MutationResult applyRemap(const asm_remapper::MemberRemapPlan& plan,
                                      const asm_remapper::RemapPolicy&   policy,
                                      const MutationRequest& request = {}) = 0;

    /// @brief 生成重命名映射和风险报告，不修改工作副本。
    virtual RenameAnalysis analyzeRename(const RenameRequest& request) const = 0;

    /// @brief 原子应用一个已通过 override 风险检查的重命名请求。
    virtual MutationResult applyRename(const RenameRequest& request,
                                       const MutationRequest& requestOptions = {}) = 0;

    /// @brief 在不提交候选副本的情况下运行 pass/pipeline 并返回验证和 byte diff。
    virtual MutationResult validatePipeline(const PipelineRequest& pipeline,
                                            const MutationRequest& request = {}) = 0;

    /// @brief 原子执行 pass/pipeline；dryRun 时与 validatePipeline 返回同一类结果。
    virtual MutationResult applyPipeline(const PipelineRequest& pipeline,
                                         const MutationRequest& request = {}) = 0;

    /// @brief 在一个候选事务中执行 remap、可选 pipeline、验证和候选 xref。
    virtual WorkflowResult runWorkflow(const WorkflowRequest& workflow,
                                       const MutationRequest& request = {}) = 0;

    /// @brief 导入一个私有工作流候选 JAR，并按当前 revision 验证、导出和提交。
    /// @warning candidateJarPath 仅限受控后台事务生成的 JAR；提交失败会移除本次创建的 exportPath。
    virtual WorkflowCandidateCommitResult commitWorkflowCandidateJar(
        const WorkflowCandidateCommitRequest& candidate,
        const MutationRequest& request = {}) = 0;

    /// @brief 比较当前工作副本与一个外部 JAR 的全部可读取资源，不修改工作区。
    virtual JarDiffResult diffJar(const std::string& referencePath) const = 0;

    /// @brief 扫描并预览当前工作副本的 JAR 修复候选，不提交变更。
    virtual JarRepairResult analyzeJarRepair(const JarRepairRequest& repair,
                                             const MutationRequest& request = {}) const = 0;

    /// @brief 原子应用已显式选择的 JAR 修复；dryRun 时返回预览结果。
    virtual JarRepairResult applyJarRepair(const JarRepairRequest& repair,
                                           const MutationRequest& request = {}) = 0;

    /// @brief 将指定 class 恢复到打开工作区时的原始字节。
    virtual MutationResult revertClass(const std::string& uri, const MutationRequest& request = {}) = 0;

    /// @brief 保存当前工作副本的具名快照，不改变 revision。
    virtual WorkspaceResult<void> createSavepoint(const std::string& name) = 0;

    /// @brief 恢复具名快照，产生新 revision。
    virtual MutationResult restoreSavepoint(const std::string& name,
                                            const MutationRequest& request = {}) = 0;

    /// @brief 撤销上一次已提交的事务。
    virtual MutationResult undo(const MutationRequest& request = {}) = 0;

    /// @brief 重做上一次撤销的事务。
    virtual MutationResult redo(const MutationRequest& request = {}) = 0;

    /// @brief 将当前工作副本原子写入一个此前不存在的输出 JAR。
    virtual WorkspaceResult<void> exportJar(const std::string& outputPath) const = 0;

    /// @brief 将单 class 工作副本原子写入一个此前不存在的输出 classfile。
    virtual WorkspaceResult<void> exportClass(const std::string& outputPath) const = 0;

    /// @brief 返回已附加的 supporting JAR 摘要。
    virtual std::vector<SupportingResourceInfo> supportingResources() const = 0;
};

}   // namespace mjbase::workspace
