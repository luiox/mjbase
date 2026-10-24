/// @file        call_graph.hpp
/// @brief       直接方法调用图：调用点提取、正反向索引、可达闭包。
///
/// 底座换成 libjbc：
/// 类字节经 makeSessionFromBytes 建会话，逐方法 collectCodeUnits 升格后
/// 遍历 CodeDom 程序顺序，invoke 家族指令用 ClassSession::memberOf 把
/// CP 操作数解析为 MemberSem{owner,name,desc}，再驻留成 MemberKey 边。
///
/// 边模型（与旧版一致：只记精确编码的调用事实，分派策略留在图外）：
///   - invokespecial / invokestatic / invokedynamic 记直边；
///   - invokevirtual / invokeinterface 记边但标记为虚分派（本层 unresolved，
///     层次扩展批做目标重写；见 CallSite::isVirtualDispatch）；
///   - invokedynamic 的 CP 操作数没有 owner（memberOf 返回空视图），
///     callee 键的 owner 段驻留空串，消费者按 kind == Dynamic 分流；
///   - BSM 可解析时（见 indy.hpp），同一指令额外产出展开边：callee 为
///     MethodHandle 目标成员，kind 按 reference_kind 折算，indyExpanded 置真。
///     未知 BSM 家族只保留 Dynamic 保守边，不猜目标。
///
/// 节点以 MemberKey（KeyPool 驻留）为键；图自有池，键随图存活。
/// 扫描类集合中声明的方法标记 declared 并保留 access；只被引用未声明的
/// 方法（JDK/库/缺失类）默认保留为外部节点（CallGraphOptions 可关）。
///
/// 调用点带指令级定位：InsnId 在图保活的会话 CodeDom 内解析
/// （CallGraph::sessionOf）。图持有全部喂入会话的 shared_ptr 保活副本，
/// 消费者在图存活期内不得对会话重复 promote/lower——那会重建 CodeDom、
/// 使已发出的 InsnId 失效。
///
/// 视图层：`enumerateChains` 提供惰性调用链枚举，`buildCallTree` 提供带
/// 去重号与环标记的调用树；两者都只读图，不建全图快照。反向枚举沿用
/// `callersOf` / `incomingSitesOf`。
#pragma once

#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/classpath/indy.hpp"
#include "mjbase/keys.hpp"

namespace mjbase::asm_classpath {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief invoke 指令家族的分派语义（按 JVMS 6.5）。
enum class InvokeKind : ca::u8 {
    Virtual = 0,   ///< invokevirtual：虚分派边，本层 unresolved
    Special,       ///< invokespecial：直边（super/私有/构造器语义）
    Static,        ///< invokestatic：直边
    Interface,     ///< invokeinterface：虚分派边，本层 unresolved
    Dynamic,       ///< invokedynamic：保守边（owner 空；可解析时另发 indyExpanded 边）
};

/// @brief opcode → 分派语义；非 invoke 家族返回 false。
bool invokeKindOf(jbc::core::Op op, InvokeKind& out) noexcept;

/// @brief 一个调用点 = 一条精确编码的调用边 + 指令级定位。
struct CallSite
{
    MemberKey caller;                    ///< 调用方方法（也是指令的类/方法上下文）
    MemberKey callee;                    ///< 被调方法（indy 的 owner 段为空串驻留）
    jbc::ir::InsnId insn;                ///< invoke 指令句柄（sessionOf(site) 的 CodeDom 内解析）
    InvokeKind kind = InvokeKind::Static;
    ca::u32 session = 0;                 ///< 图内会话表下标（sessionOf 消费，勿手改）

    /// @brief 由 indy BSM 展开的补充边。
    ///
    /// callee 是展开目标（lambda implMethod / ConstantBootstraps.invoke 等），
    /// kind 按 MethodHandle reference_kind 折算；insn 仍指向原 INVOKEDYNAMIC
    /// 指令。原始 indy 边该位为 false。
    bool indyExpanded = false;
    /// @brief indy 的 BSM 家族。
    ///
    /// 仅 kind == Dynamic（原始边）或 indyExpanded（展开边）有意义；
    /// Dynamic + Unknown = 未能展开的保守边，消费者应按未知效果处理。
    BootstrapFamily indyFamily = BootstrapFamily::Unknown;

    /// @brief 虚分派边（virtual/interface）：目标待层次批解析，本层 unresolved。
    bool isVirtualDispatch() const noexcept
    {
        return kind == InvokeKind::Virtual || kind == InvokeKind::Interface;
    }
    /// @brief 直边（special/static/dynamic）：callee 即精确目标。
    bool isDirect() const noexcept { return !isVirtualDispatch(); }

    bool operator==(const CallSite& o) const noexcept
    {
        return caller == o.caller && callee == o.callee && insn == o.insn &&
               kind == o.kind && session == o.session &&
               indyExpanded == o.indyExpanded && indyFamily == o.indyFamily;
    }
    bool operator!=(const CallSite& o) const noexcept { return !(*this == o); }
};

/// @brief build 失败记录：哪个方法、哪个阶段失败。
///
/// 文本视图指向 builder 持有会话的 SymbolPool（builder 存活期内有效）；
/// 失败时部分构建的图不交付，其池随之消亡，错误不得引用图内文本。
struct BuildError
{
    enum class Stage : ca::u8 {
        Promote,   ///< 方法体升格失败（error 有效）
        Operand,   ///< invoke 指令的 CP 操作数解析失败（insnSlot 有效）
    };

    Stage stage = Stage::Promote;
    jbc::ir::PromoteError error = jbc::ir::PromoteError::Ok;   ///< stage == Promote
    ca::u32 insnSlot = 0;   ///< stage == Operand：invoke 指令的 InsnId::slot
    std::string_view className;
    std::string_view methodName;
    std::string_view methodDesc;
};

/// @brief 构建策略。
struct CallGraphOptions
{
    /// 保留被引用但未在扫描集合中声明的目标节点（JDK/库/缺失类）。
    /// false 时指向外部目标的调用点整条丢弃（与旧版语义一致）。
    bool includeExternalTargets = true;
};

/// @brief 一条调用链：根到叶的简单方法序列。
///
/// 重复调用点合并为同一条边（`calleesOf` 去重语义），因此链是方法序列而
/// 不是调用点序列；需要逐调用点事实时用 `outgoingSitesOf`。
struct CallChain
{
    std::vector<MemberKey> methods;   ///< methods[0] 为根；空链不会产出
};

/// @brief 调用链枚举策略。
struct CallChainOptions
{
    /// 最大边数（根为 0 层）。0 = 不限制；达到上限的节点按叶处理。
    ca::u32 maxDepth = 0;
};

/// @brief 根节点没有父节点时的哨兵值。
inline constexpr ca::u32 kNoCallTreeParent = static_cast<ca::u32>(-1);

/// @brief 调用树节点：一次出现，不是去重后的方法。
///
/// 图里的重入（同一方法出现在多个分支）会产生多个节点；`methodId` 用于
/// 前端把同一方法折叠展示，`cycle` 标记回到当前路径祖先的边。节点下标即
/// 先序分配号，父节点下标由 `parent` 给出（根为 `kNoCallTreeParent`）。
struct CallTreeNode
{
    MemberKey            method;
    ca::u32              parent   = kNoCallTreeParent;
    ca::u32              depth    = 0;   ///< 根为 0，按边数递增
    ca::u32              methodId = 0;   ///< 去重号：同一 MemberKey 首次出现时分配
    bool                 cycle    = false;   ///< 回到当前路径祖先：保留节点、不展开
    std::vector<ca::u32> children;
};

/// @brief 调用树展开策略。
struct CallTreeOptions
{
    /// 最大边数（根为 0 层）。0 = 不限制。
    ca::u32 maxDepth = 0;
    /// 节点总数上限（含根）。0 = 不限制；到达上限后停止展开并置 truncated。
    ca::u32 maxNodes = 4096;
};

/// @brief 调用树展开结果。
struct CallTree
{
    std::vector<CallTreeNode> nodes;   ///< 先序；有根时 nodes[0] 恒为根，根缺失时为空
    bool                      truncated = false;   ///< 是否因 maxNodes 截断
};

class CallGraph;

/// @brief 惰性调用链游标：每次 next() 产出一条根到叶的简单路径。
///
/// 不建全量快照：游标只持有当前 DFS 路径与各层未消费的 callee 列表；环在
/// 路径内首次重复处截断（不产出重复方法）。游标借用 CallGraph，图销毁后
/// 不得继续使用。`next()` 返回 false 时枚举结束且 `out` 不变。
class CallChainCursor
{
public:
    CallChainCursor() = default;
    CallChainCursor(const CallChainCursor&)            = delete;
    CallChainCursor& operator=(const CallChainCursor&) = delete;
    CallChainCursor(CallChainCursor&&)                 = default;
    CallChainCursor& operator=(CallChainCursor&&)      = default;

    /// @brief 取下一条链；结束返回 false。
    bool next(CallChain& out);

private:
    friend class CallGraph;

    /// @brief DFS 帧：当前方法 + 去重后的 callee 列表 + 下一个待展开下标。
    struct Frame
    {
        MemberKey              method;
        std::vector<MemberKey> callees;
        ca::usize              next = 0;
        bool                   entered = false;   ///< 入帧的叶/深度判定已做
    };

    CallChainCursor(const CallGraph& graph, MemberKey root, CallChainOptions options);

    const CallGraph* graph_ = nullptr;
    std::vector<Frame> stack_;
    std::unordered_set<MemberKey, MemberKeyHash> onPath_;
    ca::u32 maxDepth_ = 0;
    bool exhausted_ = true;
};

class CallGraphBuilder;

/// @brief 直接调用图（查询面）：节点表 + 调用点边表 + 正反向索引。
///
/// 值语义产物：由 CallGraphBuilder::build 产出，move-only；MemberKey 的
/// 驻留文本由图自有的 KeyPool 保活，图存活期内键与查询结果均有效。
class CallGraph
{
public:
    CallGraph() = default;

    CallGraph(const CallGraph&)            = delete;
    CallGraph& operator=(const CallGraph&) = delete;
    CallGraph(CallGraph&&)                 = default;
    CallGraph& operator=(CallGraph&&)      = default;

    /// @brief 图自有的驻留池：查询键构造（memberKeyOf）与文本展开共用。
    KeyPool& pool() noexcept { return pool_; }
    const KeyPool& pool() const noexcept { return pool_; }

    ca::usize methodCount() const noexcept { return nodes_.size(); }
    ca::usize siteCount() const noexcept { return sites_.size(); }

    /// @brief 方法是否为图节点（声明或被引用）。
    bool contains(MemberKey m) const noexcept;

    /// @brief 方法是否在扫描类集合中声明（false = 外部引用节点）。
    bool isDeclared(MemberKey m) const noexcept;

    /// @brief 声明方法的 access 标志；未知方法返回 0。
    ca::u32 accessOf(MemberKey m) const noexcept;

    /// @brief caller 的全部调用点（含虚分派未解析边、indy 展开边与重复调用点），
    ///        按扫描序。
    std::vector<CallSite> outgoingSitesOf(MemberKey caller) const;

    /// @brief 指向 callee 的全部调用点（反向索引），按扫描序。
    std::vector<CallSite> incomingSitesOf(MemberKey callee) const;

    /// @brief 指向 m 的调用点计数（同一调用方多处调用分别计数）。
    ///
    /// 与 callersOf（去重调用方集合）互补，供「单点调用才内联」类消费者使用。
    ca::usize callSiteCountOf(MemberKey m) const noexcept;

    /// @brief 去重后的直接被调方法键集合。
    std::vector<MemberKey> calleesOf(MemberKey caller) const;

    /// @brief 去重后的直接调用方键集合。
    std::vector<MemberKey> callersOf(MemberKey callee) const;

    /// @brief 调用点所属类的保活会话：site.insn 在其 CodeDom 内解析。
    ///
    /// 返回 const 会话：解析只读（peek/order/memberOf）；编辑请走消费者
    /// 自己的会话句柄，不要经图改。
    std::shared_ptr<const jbc::ir::ClassSession> sessionOf(const CallSite& site) const;

    /// @brief 声明某类的会话（先喂先得）；仅被引用的类返回 nullptr。
    std::shared_ptr<const jbc::ir::ClassSession> sessionOfClass(ClassKey cls) const;

    /// @brief 直接调用可达闭包：入口集自身（图中存在者）+ 沿出边可达的方法集。
    ///
    /// - 不在图中的入口被忽略；入口总是入结果（即使命中过滤条件）。
    /// - skipLibraryClasses 时，callee 拥有者匹配 util::isLibraryClass 的边
    ///   不再传播（边被跳过，已入结果的节点保留）。
    /// - 沿全部精确编码边走（含虚分派未解析边）；层次语义在图外。
    std::vector<MemberKey> reachableClosure(const std::vector<MemberKey>& entries,
                                            bool skipLibraryClasses = false) const;

    /// @brief 按根方法枚举调用链（惰性 DFS，简单路径）。
    ///
    /// 根不在图中时返回立即耗尽的游标。链按 DFS 先序产出；callee 顺序沿用
    /// 调用点扫描序（`calleesOf` 去重）。环在路径内首次重复处截断，因此
    /// 每条链都是不含重复方法的简单路径。反向枚举（方法 → 调用者）沿用
    /// `callersOf` / `incomingSitesOf`，本面不重复提供。
    CallChainCursor enumerateChains(MemberKey root,
                                    CallChainOptions options = {}) const;

    /// @brief 展开调用树（每次出现一个节点，先序分配下标）。
    ///
    /// 与链枚举的差别：重入方法在树里保留为独立节点（用 `methodId` 去重
    /// 展示），回到当前路径祖先的边保留为 `cycle` 叶节点，便于 UI 展示环。
    /// 根不在图中时返回空树。`maxNodes` 截断只停止继续展开，已生成节点
    /// 保持完整；截断事实由 `CallTree::truncated` 表达，不视为错误。
    CallTree buildCallTree(MemberKey root, CallTreeOptions options = {}) const;

private:
    friend class CallGraphBuilder;

    /// @brief 图节点：一个方法的键 + 声明信息 + 出/入调用点下标表。
    struct Node
    {
        MemberKey key;
        ca::u32 access = 0;
        bool declared  = false;
        std::vector<ca::u32> outSites;   ///< 本方法发出的调用点（下标进 sites_）
        std::vector<ca::u32> inSites;    ///< 指向本方法的调用点
    };

    const Node* nodeOf(MemberKey m) const noexcept;
    /// @brief 取或建节点，返回稳定下标（构建期 nodes_ 扩容不失效，构建代码一律下标寻址）。
    ca::u32 internNode(MemberKey m);

    KeyPool pool_;
    std::vector<Node> nodes_;
    std::unordered_map<MemberKey, ca::u32, MemberKeyHash> nodeIndexOf_;
    std::vector<CallSite> sites_;
    std::vector<std::shared_ptr<jbc::ir::ClassSession>> sessions_;
    std::unordered_map<ca::u32, ca::u32> classToSession_;   ///< classKey.id → 先喂会话下标
};

/// @brief 调用图构建器：喂类字节或已开会话 → 产出 CallGraph。
///
/// 逐类两遍：先为扫描集合中声明的方法建节点，再逐方法升格遍历 invoke
/// 提取调用点。build 可重复调用（每次全量重建图，喂入集合不变）。
class CallGraphBuilder
{
public:
    explicit CallGraphBuilder(CallGraphOptions options = CallGraphOptions{})
        : options_(options)
    {}

    /// @brief 喂入一份 class 字节：makeSessionFromBytes 建会话并保活。
    ///
    /// 字节在调用期间保持有效即可（会话复制源字节）；同一内容重复喂入
    /// 会得到两个会话、两组调用点（builder 不按类名去重）。
    Result<void, jbc::ir::SessionError> addClassBytes(jbc::core::ByteView bytes);

    /// @brief 喂入已开的会话（builder 持保活副本，遍历只读）。
    ///
    /// 已 promote 的方法直接复用现有 CodeDom（InsnId 不变）。同一会话
    /// shared_ptr 重复喂入幂等（指针判等跳过）。
    void addSession(std::shared_ptr<jbc::ir::ClassSession> session);

    /// @brief 在当前喂入集合上构建调用图。
    ///
    /// 失败即停（fail-fast）：方法升格失败或 invoke 操作数解析失败返回
    /// Err(BuildError)，部分构建的图不交付。abstract/native 等无代码方法
    /// 是预期形态，只建节点不建边。
    Result<CallGraph, BuildError> build();

private:
    CallGraphOptions options_;
    std::vector<std::shared_ptr<jbc::ir::ClassSession>> sessions_;
};

}   // namespace mjbase::asm_classpath
