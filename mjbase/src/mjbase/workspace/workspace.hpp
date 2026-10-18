#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/workspace/i_workspace.hpp"
#include "mjbase/workspace/workspace_types.hpp"

namespace mjbase::workspace {

/// @file workspace.hpp
/// @brief WorkspaceBase：可回滚的、单线程持有的 class/JAR 分析与编辑工作区（基础实现）。
///
/// 移植自 mbase/workspace/workspace（语义照旧），底座按 libjbc 重定型：
/// - class 解码/检索/搜索走 jbc::core::readClass 骨架 + jbc::ir 会话（按需 promote）；
/// - 候选验证接 mjbase::spi 协作面：spi::sanitizeClassFile 引用语义预检（闸门语义
///   在候选入口前置）+ mjbase::jasm 验证；
/// - 结构化修改经 ClassMutationService（libjbc 会话改写 + spi::ClassResourcePendingCommit
///   两阶段写回）；
/// - 错误通道为 WorkspaceError + WorkspaceResult（旧 ca::Status/StatusOr 的映射见
///   workspace_types.hpp）；
/// - remap/rename/pipeline/workflow 方法族落在 spi 协作面上：管线组装经
///   spi::PipelineBuilder/applyTo（pass-contract §9 装配层），重映射经
///   asm_remapper::applyResourceRemapPlan，帧重算经
///   spi::CommonSuperClassResolver + StrictTypeResolverAdapter（jdkPath 层次
///   图由本类扫描 JDK 归档构建）。
class WorkspaceBase : public IWorkspace
{
public:
    WorkspaceBase();
    ~WorkspaceBase() override;

    WorkspaceBase(const WorkspaceBase&)            = delete;
    WorkspaceBase& operator=(const WorkspaceBase&) = delete;
    WorkspaceBase(WorkspaceBase&&)                 = delete;
    WorkspaceBase& operator=(WorkspaceBase&&)      = delete;

    /// @brief 已注册 pass 摘要（静态查询：无工作区状态，不进接口）。
    static std::vector<PassDescription> availablePasses();

    /// @brief 已注册 pipeline 摘要（静态查询：无工作区状态，不进接口）。
    static std::vector<PipelineDescription> availablePipelines();

    /// @brief 打开主 JAR 并建立不可变原始快照与可修改工作副本。
    WorkspaceResult<void> openJar(const std::string& path) override;

    /// @brief 打开主 JAR（#1110 族：可注入条目名宽容解码回调；缺省空 = 原始
    /// 字节直通零漂移）。非虚重载：decoder 只贯通本实现的主 JAR 导入面
    /// （importFromJar），接口层 IWorkspace::openJar 维持单形参不动。
    /// @param decoder 条目名宽容解码回调（同 mjbase::jar::IJarReader::setEntryNameDecoder；
    /// 与 listJarEntries/资源 URI 消费面保持同一解码口径）。
    WorkspaceResult<void> openJar(const std::string& path,
                                  const mjbase::jar::IJarReader::EntryNameDecoder& decoder);

    /// @brief 打开单个 classfile，并以其 JVM internal name 建立稳定资源 URI。
    WorkspaceResult<void> openClass(const std::string& path) override;

    /// @brief 添加只读 supporting JAR，用于后续层次和调用解析。
    /// @return 返回当前 Workspace 内稳定的 support ID 和内容 fingerprint；重复路径或内容被拒绝。
    WorkspaceResult<SupportingResourceInfo> addSupportingJar(const std::string& path) override;

    /// @brief 按 support ID 移除只读 supporting JAR，并失效依赖 classpath 的分析缓存。
    /// @return 返回被移除的 supporting resource 描述；ID 不存在时返回 NotFound。
    WorkspaceResult<SupportingResourceInfo> removeSupportingJar(const std::string& id) override;

    /// @brief 工作区是否已加载主 class 或 JAR。
    bool isLoaded() const noexcept override;

    /// @brief 当前主制品种类；未加载时返回 ArtifactKind::None。
    ArtifactKind artifactKind() const noexcept override;

    /// @brief 当前主资源的来源路径。
    const std::string& sourcePath() const noexcept override;

    /// @brief 单 class 工作区的稳定资源 URI；其他 kind 返回空字符串。
    const std::string& primaryClassUri() const noexcept override;

    /// @brief 当前工作区 revision；只在已提交变更、undo、redo 或 restore 后递增。
    ca::u64 revision() const noexcept override;

    /// @brief 当前工作副本是否和原始快照不同。
    bool isDirty() const override;

    /// @brief 当前工作副本是否满足导出条件。
    bool canExport() const noexcept override;

    /// @brief 返回主资源的稳定排序摘要。
    WorkspaceResult<std::vector<ResourceSummary>> listResources(bool includeClasses = true) const override;

    /// @brief 读取一个主资源的原始字节。
    WorkspaceResult<ResourceContent> readResource(const std::string& uri) const override;

    /// @brief 按资源 URI 或 internal name 读取 class 原始字节和内容 fingerprint。
    /// @note 精确 URI 指向损坏 class 时仍返回 bytes，此时 internalName 为空。
    WorkspaceResult<ClassBytesInfo> readClassBytes(const std::string& uriOrInternalName) const override;

    /// @brief 返回所有主 class 的轻量结构摘要。
    WorkspaceResult<std::vector<ClassSummary>> listClassSummaries() const override;

    /// @brief 返回所有主 class 的完整结构化元数据。
    WorkspaceResult<std::vector<ClassInfo>> listClasses() const override;

    /// @brief 通过资源 URI 或 JVM internal name 查询 class 详情。
    WorkspaceResult<ClassInfo> classInfo(const std::string& uriOrInternalName) const override;

    /// @brief 枚举 class 常量池条目。
    WorkspaceResult<std::vector<ConstantPoolEntryInfo>> constantPool(const std::string& uriOrInternalName,
                                                                     bool               resolve) const override;

    /// @brief 以 owner/name/descriptor 精确查询方法详情。
    WorkspaceResult<MethodInfo> methodInfo(const std::string& owner, const std::string& name,
                                           const std::string& descriptor) const override;

    /// @brief 以 owner/name/descriptor 精确查询字段详情。
    WorkspaceResult<FieldInfo> fieldInfo(const std::string& owner, const std::string& name,
                                         const std::string& descriptor) const override;

    /// @brief 返回精确选择方法的逐指令摘要及行号元数据。
    WorkspaceResult<MethodBytecode> methodBytecode(const std::string& owner,
                                                   const std::string& name,
                                                   const std::string& descriptor) const override;

    /// @brief 返回精确选择方法的完整指令渲染（保留 CP 索引与字节 PC）。
    WorkspaceResult<MethodInstructions> methodInstructions(const std::string& owner,
                                                           const std::string& name,
                                                           const std::string& descriptor,
                                                           bool               resolve) const override;

    /// @brief 返回精确选择方法的规范化 Jasm 声明。
    jasm::Result<jasm::JvmJasmMethodDisassembly> getMethodJasm(
        const std::string& owner, const std::string& name, const std::string& descriptor) const override;

    /// @brief 返回主资源 MANIFEST 的稳定排序主属性。
    WorkspaceResult<std::vector<ManifestAttribute>> manifestAttributes() const override;

    /// @brief 提取主资源所有非 class 条目中的最短长度为 @p minimumLength 的可打印字符串。
    WorkspaceResult<std::vector<ResourceStringInfo>> resourceStrings(ca::usize minimumLength = 4) const override;

    /// @brief 按 class 内部名或资源 URI 查询 class；损坏 class 会被跳过。
    WorkspaceResult<std::vector<SearchHit>> searchClasses(
        const std::string& query, const SearchOptions& options = {}) const override;

    /// @brief 按 owner/name/descriptor/signature 查询字段和方法；损坏 class 会被跳过。
    WorkspaceResult<std::vector<SearchHit>> searchMembers(
        const std::string& query, const SearchOptions& options = {}) const override;

    /// @brief 查询 class 常量池字符串和非 class 可打印资源字符串；损坏 class 会被跳过。
    WorkspaceResult<std::vector<SearchHit>> searchStrings(
        const std::string& query, const SearchOptions& options = {}) const override;

    /// @brief 精确查询常量池及即时数值字面量；数值文本支持等价的标准数值写法。
    WorkspaceResult<std::vector<SearchHit>> searchNumbers(
        const std::string& query, const SearchOptions& options = {}) const override;

    /// @brief 按 mnemonic、opcode、节点类型和指令操作数查询方法指令；损坏 class 会被跳过。
    WorkspaceResult<std::vector<SearchHit>> searchInstructions(
        const std::string& query, const SearchOptions& options = {}) const override;

    /// @brief 精确返回所有编码目标为 owner/name/descriptor 的方法引用。
    WorkspaceResult<std::vector<ReferenceInfo>> xrefsToMethod(const std::string& owner,
                                                              const std::string& name,
                                                              const std::string& descriptor) const override;

    /// @brief 精确返回所有编码目标为 owner/name/descriptor 的字段引用。
    WorkspaceResult<std::vector<ReferenceInfo>> xrefsToField(const std::string& owner,
                                                             const std::string& name,
                                                             const std::string& descriptor) const override;

    /// @brief 返回所有直接或结构性引用 internalName 的位置。
    WorkspaceResult<std::vector<ReferenceInfo>> xrefsToClass(const std::string& internalName) const override;

    /// @brief 返回方法的直接调用目标；resolved 为 true 时扩展虚调用和接口调用。
    WorkspaceResult<std::vector<ReferenceInfo>> calleesOfMethod(const std::string& owner,
                                                                const std::string& name,
                                                                const std::string& descriptor,
                                                                bool resolved = false) const override;

    /// @brief 返回目标 class 的超类、接口、直接/全部子类和实现者。
    WorkspaceResult<ClassRelations> classRelations(const std::string& internalName) const override;

    /// @brief 返回精确方法所在的继承/实现覆盖组件。
    WorkspaceResult<std::vector<MethodOverrideInfo>> overridesOfMethod(
        const std::string& owner, const std::string& name, const std::string& descriptor) const override;

    /// @brief 返回可供 MCP/CLI 直接传输的完整 class JASM。
    jasm::Result<jasm::JvmJasmDisassembly> getJasm(const std::string& uri) const override;

    /// @brief 在不提交的候选副本中汇编并验证完整 class JASM。
    MutationResult validateJasm(const std::string& uri, ca::str::Utf16StringRef sourceName,
                                ca::str::Utf16StringRef source,
                                const MutationRequest& request = {}) override;

    /// @brief 原子应用完整 class JASM，成功后产生一个新的 revision。
    MutationResult applyJasm(const std::string& uri, ca::str::Utf16StringRef sourceName,
                             ca::str::Utf16StringRef source,
                             const MutationRequest& request = {}) override;

    /// @brief 验证由专用 classfile adapter 生成的完整 class 候选字节，不提交工作区。
    MutationResult validateClassBytes(const std::string& uri,
                                      const std::vector<ca::u8>& classBytes,
                                      const MutationRequest& request = {}) override;

    /// @brief 原子应用已由专用 classfile adapter 生成的完整 class 候选字节。
    MutationResult applyClassBytes(const std::string& uri,
                                   const std::vector<ca::u8>& classBytes,
                                   const MutationRequest& request = {}) override;

    /// @brief 在不提交的候选副本中应用并验证一项结构化 class 修改。
    MutationResult validateClassMutation(const ClassMutation& mutation,
                                         const MutationRequest& request = {}) override;

    /// @brief 原子应用一项结构化 class 修改，成功后产生一个新的 revision。
    MutationResult applyClassMutation(const ClassMutation& mutation,
                                      const MutationRequest& request = {}) override;

    /// @brief 原子替换一个 class 的原始字节；dry-run 时只返回完整验证和 diff。
    MutationResult replaceClassBytes(const std::string& uriOrInternalName,
                                     const std::vector<ca::u8>& bytes,
                                     const MutationRequest& request = {}) override;

    /// @brief 返回当前工作副本中相对打开时原始快照已变化的 class。
    WorkspaceResult<std::vector<ModifiedClassInfo>> modifiedClasses() const override;

    /// @brief 在不提交候选副本的情况下分析一个显式 MemberRemapPlan 的影响。
    ImpactAnalysis analyzeRemap(const asm_remapper::MemberRemapPlan& plan,
                                const asm_remapper::RemapPolicy& policy) const override;

    /// @brief 原子应用一个显式 MemberRemapPlan。
    MutationResult applyRemap(const asm_remapper::MemberRemapPlan& plan,
                              const asm_remapper::RemapPolicy&   policy,
                              const MutationRequest& request = {}) override;

    /// @brief 生成重命名映射和风险报告，不修改工作副本。
    RenameAnalysis analyzeRename(const RenameRequest& request) const override;

    /// @brief 原子应用一个已通过 override 风险检查的重命名请求。
    MutationResult applyRename(const RenameRequest& request,
                               const MutationRequest& requestOptions = {}) override;

    /// @brief 在不提交候选副本的情况下运行 pass/pipeline 并返回验证和 byte diff。
    MutationResult validatePipeline(const PipelineRequest& pipeline,
                                    const MutationRequest& request = {}) override;

    /// @brief 原子执行 pass/pipeline；dryRun 时与 validatePipeline 返回同一类结果。
    MutationResult applyPipeline(const PipelineRequest& pipeline,
                                 const MutationRequest& request = {}) override;

    /// @brief 在一个候选事务中执行 remap、可选 pipeline、验证和候选 xref。
    WorkflowResult runWorkflow(const WorkflowRequest& workflow,
                               const MutationRequest& request = {}) override;

    /// @brief 导入一个私有工作流候选 JAR，并按当前 revision 验证、导出和提交。
    /// @warning candidateJarPath 仅限受控后台事务生成的 JAR；提交失败会移除本次创建的 exportPath。
    WorkflowCandidateCommitResult commitWorkflowCandidateJar(
        const WorkflowCandidateCommitRequest& candidate,
        const MutationRequest& request = {}) override;

    /// @brief 比较当前工作副本与一个外部 JAR 的全部可读取资源，不修改工作区。
    JarDiffResult diffJar(const std::string& referencePath) const override;

    /// @brief 扫描并预览当前工作副本的 JAR 修复候选，不提交变更。
    JarRepairResult analyzeJarRepair(const JarRepairRequest& repair,
                                     const MutationRequest& request = {}) const override;

    /// @brief 原子应用已显式选择的 JAR 修复；dryRun 时返回预览结果。
    JarRepairResult applyJarRepair(const JarRepairRequest& repair,
                                   const MutationRequest& request = {}) override;

    /// @brief 将指定 class 恢复到打开工作区时的原始字节。
    MutationResult revertClass(const std::string& uri, const MutationRequest& request = {}) override;

    /// @brief 保存当前工作副本的具名快照，不改变 revision。
    WorkspaceResult<void> createSavepoint(const std::string& name) override;

    /// @brief 恢复具名快照，产生新 revision。
    MutationResult restoreSavepoint(const std::string& name,
                                    const MutationRequest& request = {}) override;

    /// @brief 撤销上一次已提交的事务。
    MutationResult undo(const MutationRequest& request = {}) override;

    /// @brief 重做上一次撤销的事务。
    MutationResult redo(const MutationRequest& request = {}) override;

    /// @brief 将当前工作副本原子写入一个此前不存在的输出 JAR。
    WorkspaceResult<void> exportJar(const std::string& outputPath) const override;

    /// @brief 将单 class 工作副本原子写入一个此前不存在的输出 classfile。
    WorkspaceResult<void> exportClass(const std::string& outputPath) const override;

    /// @brief 返回已附加的 supporting JAR 摘要。
    std::vector<SupportingResourceInfo> supportingResources() const override;

private:
    struct Snapshot;
    struct SupportingResource;
    struct AnalysisGraph;

    WorkspaceErrorInfo ensureLoaded(const std::string& operation) const;
    bool           accepts(const MutationRequest& request) const;
    MutationResult revisionConflict() const;
    MutationResult stateFailure(WorkspaceErrorInfo status) const;
    MutationResult commit(std::unique_ptr<model::ResourceContainer> candidate,
                          std::vector<jasm::Diagnostic> diagnostics,
                          const MutationRequest& request);

    std::unique_ptr<model::ResourceContainer> cloneWorking() const;
    std::unique_ptr<model::ResourceContainer> cloneOriginal() const;
    /// @brief 工作副本 + supporting JAR 的合并只读容器（xref/callees 扫描面；
    ///        主容器 URI 优先，同 URI 的支撑资源跳过）。
    std::unique_ptr<model::ResourceContainer> analysisScanContainer() const;
    WorkspaceResult<AnalysisGraph>            analysisGraph() const;
    WorkspaceResult<std::string>       resolveClassUri(const model::ResourceContainer& container,
                                                       const std::string& uriOrInternalName) const;
    WorkspaceErrorInfo validateCandidate(const model::ResourceContainer& candidate,
                                         std::vector<jasm::Diagnostic>& diagnostics) const;
    /// @brief 在候选副本上组装并运行 pass/pipeline（runWorkflow 前置协议见
    ///        i_workspace.hpp：pipeline/passNames 互斥，均空取 default-optimize）。
    WorkspaceErrorInfo runPipeline(model::ResourceContainer& candidate,
                                   const PipelineRequest& request) const;
    /// @brief 把重命名请求展开为重映射计划 + 审阅映射表 + 风险清单
    ///        （analyzeRename/applyRename 共用的展开面）。
    WorkspaceErrorInfo buildRenamePlan(const RenameRequest& request,
                                       asm_remapper::MemberRemapPlan&     plan,
                                       std::vector<RenameMapping>&        mappings,
                                       std::vector<MethodOverrideInfo>&   overrides,
                                       std::vector<std::string>&          risks) const;
    /// @brief 引用结果来源装饰：sourceClass 落在 supporting JAR 时填 supporting/supportId。
    void annotateSupportingSources(std::vector<ReferenceInfo>& references) const;
    std::unordered_map<std::string, std::string> supportingClassIds() const;

    model::ResourceContainer* workingContainer() noexcept;
    const model::ResourceContainer* workingContainer() const noexcept;

    std::unique_ptr<model::ResourceContainer> original_;
    std::unique_ptr<model::ResourceContainer> working_;
    std::vector<SupportingResource> supporting_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    std::unordered_map<std::string, std::unique_ptr<model::ResourceContainer>> savepoints_;
    ArtifactKind artifactKind_ = ArtifactKind::None;
    std::string  sourcePath_;
    std::string  primaryClassUri_;
    ca::u64      revision_ = 0;
    ca::u64      nextSupportingId_ = 1;
};

}   // namespace mjbase::workspace
