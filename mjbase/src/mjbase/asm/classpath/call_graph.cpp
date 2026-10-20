/// @file        call_graph.cpp
/// @brief       直接调用图实现：构建两遍扫描 + 正反向索引 + 可达闭包。
#include "call_graph.hpp"

#include <algorithm>
#include <unordered_set>

#include "mjbase/util.hpp"

namespace mjbase::asm_classpath {

namespace {

/// MethodHandle → 调用点分派语义；字段句柄与域外 kind 不产调用边。
bool invokeKindOfHandle(const HandleTarget& target, InvokeKind& out) noexcept
{
    switch (target.dispatch()) {
    case HandleDispatch::Virtual: out = InvokeKind::Virtual; return true;
    case HandleDispatch::Static: out = InvokeKind::Static; return true;
    case HandleDispatch::Special:
    case HandleDispatch::Constructor: out = InvokeKind::Special; return true;
    case HandleDispatch::Interface: out = InvokeKind::Interface; return true;
    default: return false;
    }
}

}   // namespace

bool invokeKindOf(jbc::core::Op op, InvokeKind& out) noexcept
{
    using Op = jbc::core::Op;
    switch (op) {
    case Op::INVOKEVIRTUAL: out = InvokeKind::Virtual; return true;
    case Op::INVOKESPECIAL: out = InvokeKind::Special; return true;
    case Op::INVOKESTATIC: out = InvokeKind::Static; return true;
    case Op::INVOKEINTERFACE: out = InvokeKind::Interface; return true;
    case Op::INVOKEDYNAMIC: out = InvokeKind::Dynamic; return true;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// CallGraph 查询面
// ---------------------------------------------------------------------------

const CallGraph::Node* CallGraph::nodeOf(MemberKey m) const noexcept
{
    const auto it = nodeIndexOf_.find(m);
    return it == nodeIndexOf_.end() ? nullptr : &nodes_[it->second];
}

ca::u32 CallGraph::internNode(MemberKey m)
{
    const auto it = nodeIndexOf_.find(m);
    if (it != nodeIndexOf_.end()) {
        return it->second;
    }
    const ca::u32 index = static_cast<ca::u32>(nodes_.size());
    nodes_.push_back(Node{m, 0, false, {}, {}});
    nodeIndexOf_.emplace(m, index);
    return index;
}

bool CallGraph::contains(MemberKey m) const noexcept
{
    return nodeIndexOf_.find(m) != nodeIndexOf_.end();
}

bool CallGraph::isDeclared(MemberKey m) const noexcept
{
    const Node* n = nodeOf(m);
    return n != nullptr && n->declared;
}

ca::u32 CallGraph::accessOf(MemberKey m) const noexcept
{
    const Node* n = nodeOf(m);
    return n != nullptr ? n->access : 0;
}

std::vector<CallSite> CallGraph::outgoingSitesOf(MemberKey caller) const
{
    std::vector<CallSite> out;
    const Node* n = nodeOf(caller);
    if (n == nullptr) {
        return out;
    }
    out.reserve(n->outSites.size());
    for (const ca::u32 si : n->outSites) {
        out.push_back(sites_[si]);
    }
    return out;
}

std::vector<CallSite> CallGraph::incomingSitesOf(MemberKey callee) const
{
    std::vector<CallSite> out;
    const Node* n = nodeOf(callee);
    if (n == nullptr) {
        return out;
    }
    out.reserve(n->inSites.size());
    for (const ca::u32 si : n->inSites) {
        out.push_back(sites_[si]);
    }
    return out;
}

ca::usize CallGraph::callSiteCountOf(MemberKey m) const noexcept
{
    const Node* n = nodeOf(m);
    return n != nullptr ? n->inSites.size() : 0;
}

std::vector<MemberKey> CallGraph::calleesOf(MemberKey caller) const
{
    std::vector<MemberKey> out;
    const Node* n = nodeOf(caller);
    if (n == nullptr) {
        return out;
    }
    std::unordered_set<MemberKey, MemberKeyHash> seen;
    for (const ca::u32 si : n->outSites) {
        if (seen.insert(sites_[si].callee).second) {
            out.push_back(sites_[si].callee);
        }
    }
    return out;
}

std::vector<MemberKey> CallGraph::callersOf(MemberKey callee) const
{
    std::vector<MemberKey> out;
    const Node* n = nodeOf(callee);
    if (n == nullptr) {
        return out;
    }
    std::unordered_set<MemberKey, MemberKeyHash> seen;
    for (const ca::u32 si : n->inSites) {
        if (seen.insert(sites_[si].caller).second) {
            out.push_back(sites_[si].caller);
        }
    }
    return out;
}

std::shared_ptr<const jbc::ir::ClassSession> CallGraph::sessionOf(const CallSite& site) const
{
    if (site.session >= sessions_.size()) {
        return nullptr;
    }
    return sessions_[site.session];
}

std::shared_ptr<const jbc::ir::ClassSession> CallGraph::sessionOfClass(ClassKey cls) const
{
    const auto it = classToSession_.find(cls.id);
    if (it == classToSession_.end() || it->second >= sessions_.size()) {
        return nullptr;
    }
    return sessions_[it->second];
}

std::vector<MemberKey> CallGraph::reachableClosure(const std::vector<MemberKey>& entries,
                                                   bool skipLibraryClasses) const
{
    std::vector<MemberKey> result;
    if (entries.empty()) {
        return result;
    }

    // 入口先入结果（图中存在者），再沿出边传播；seen 同时充当栈去重。
    std::vector<ca::u8> seen(nodes_.size(), 0);
    std::vector<ca::u32> stack;
    for (const MemberKey e : entries) {
        const auto it = nodeIndexOf_.find(e);
        if (it == nodeIndexOf_.end()) {
            continue;
        }
        if (seen[it->second] == 0) {
            seen[it->second] = 1;
            result.push_back(nodes_[it->second].key);
            stack.push_back(it->second);
        }
    }

    while (!stack.empty()) {
        const Node& n = nodes_[stack.back()];
        stack.pop_back();
        for (const ca::u32 si : n.outSites) {
            const MemberKey target = sites_[si].callee;
            if (skipLibraryClasses && util::isLibraryClass(pool_.str(target.cls))) {
                continue;
            }
            const auto it = nodeIndexOf_.find(target);
            if (it == nodeIndexOf_.end()) {
                continue;   // 建边时目标必建节点，防御式兜底
            }
            if (seen[it->second] == 0) {
                seen[it->second] = 1;
                result.push_back(target);
                stack.push_back(it->second);
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// 调用链 / 调用树视图
// ---------------------------------------------------------------------------

CallChainCursor::CallChainCursor(const CallGraph& graph, MemberKey root,
                                 CallChainOptions options)
    : graph_(&graph), maxDepth_(options.maxDepth)
{
    if (!graph.contains(root)) {
        return;
    }
    stack_.push_back(Frame{root, graph.calleesOf(root), 0});
    onPath_.insert(root);
    exhausted_ = false;
}

bool CallChainCursor::next(CallChain& out)
{
    while (!exhausted_) {
        Frame& top = stack_.back();
        const ca::u32 depth = static_cast<ca::u32>(stack_.size() - 1);
        const bool depthLimited = maxDepth_ != 0 && depth >= maxDepth_;

        // 入帧判定只做一次：没有可展开的 callee（空/全部在路径上）或已到
        // 深度上限时，该帧就是链尾。返回父帧后路径恢复原状，不能重复判叶。
        if (!top.entered) {
            top.entered = true;
            bool expandable = !depthLimited;
            if (expandable) {
                expandable = false;
                for (const MemberKey callee : top.callees) {
                    if (onPath_.find(callee) == onPath_.end()) {
                        expandable = true;
                        break;
                    }
                }
            }
            if (!expandable) {
                out.methods.clear();
                out.methods.reserve(stack_.size());
                for (const Frame& f : stack_) {
                    out.methods.push_back(f.method);
                }
                onPath_.erase(top.method);
                stack_.pop_back();
                if (stack_.empty()) {
                    exhausted_ = true;
                }
                return true;
            }
        }

        if (top.next < top.callees.size()) {
            const MemberKey callee = top.callees[top.next++];
            if (onPath_.find(callee) != onPath_.end()) {
                continue;   // 环：跳过该边，继续同层其它 callee
            }
            onPath_.insert(callee);
            stack_.push_back(Frame{callee, graph_->calleesOf(callee), 0, false});
            continue;
        }

        // 子树全部展开完毕：回退，不产出前缀路径。
        onPath_.erase(top.method);
        stack_.pop_back();
        if (stack_.empty()) {
            exhausted_ = true;
        }
    }
    return false;
}

CallChainCursor CallGraph::enumerateChains(MemberKey root, CallChainOptions options) const
{
    return CallChainCursor(*this, root, options);
}

CallTree CallGraph::buildCallTree(MemberKey root, CallTreeOptions options) const
{
    CallTree tree;
    if (!contains(root)) {
        return tree;
    }

    std::unordered_map<MemberKey, ca::u32, MemberKeyHash> methodIds;
    std::unordered_set<MemberKey, MemberKeyHash> onPath;

    struct Frame
    {
        ca::u32               node = 0;
        std::vector<MemberKey> callees;
        ca::usize             next = 0;
    };

    const auto appendNode = [&](MemberKey method, ca::u32 parent, ca::u32 depth,
                                bool cycle) {
        auto ids = methodIds.find(method);
        if (ids == methodIds.end()) {
            ids = methodIds.emplace(method, static_cast<ca::u32>(methodIds.size())).first;
        }
        const ca::u32 index = static_cast<ca::u32>(tree.nodes.size());
        CallTreeNode node;
        node.method   = method;
        node.parent   = parent;
        node.depth    = depth;
        node.methodId = ids->second;
        node.cycle    = cycle;
        tree.nodes.push_back(std::move(node));
        if (parent != kNoCallTreeParent) {
            tree.nodes[parent].children.push_back(index);
        }
        return index;
    };

    if (options.maxNodes != 0) {
        tree.nodes.reserve(options.maxNodes);
    }
    appendNode(root, kNoCallTreeParent, 0, false);

    std::vector<Frame> stack;
    stack.push_back(Frame{0, calleesOf(root), 0});
    onPath.insert(root);

    while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next >= top.callees.size()) {
            onPath.erase(tree.nodes[top.node].method);
            stack.pop_back();
            continue;
        }

        const MemberKey callee = top.callees[top.next++];
        const ca::u32 depth = tree.nodes[top.node].depth + 1;
        if (options.maxDepth != 0 && depth > options.maxDepth) {
            continue;   // 深度上限：该节点按叶处理，不再展开
        }
        if (options.maxNodes != 0 && tree.nodes.size() >= options.maxNodes) {
            tree.truncated = true;
            break;
        }

        const bool cycle = onPath.find(callee) != onPath.end();
        const ca::u32 childIdx = appendNode(callee, top.node, depth, cycle);
        if (cycle) {
            continue;   // 回到当前路径祖先：保留节点、不展开
        }
        onPath.insert(callee);
        stack.push_back(Frame{childIdx, calleesOf(callee), 0});
    }

    return tree;
}

// ---------------------------------------------------------------------------
// CallGraphBuilder
// ---------------------------------------------------------------------------

Result<void, jbc::ir::SessionError> CallGraphBuilder::addClassBytes(jbc::core::ByteView bytes)
{
    auto r = jbc::ir::makeSessionFromBytes(bytes);
    if (r.is_err()) {
        return Err(r.unwrap_err());
    }
    sessions_.push_back(std::move(r).unwrap());
    return Ok();
}

void CallGraphBuilder::addSession(std::shared_ptr<jbc::ir::ClassSession> session)
{
    if (session == nullptr) {
        return;
    }
    for (const auto& existing : sessions_) {
        if (existing.get() == session.get()) {
            return;   // 同一会话重复喂入幂等
        }
    }
    sessions_.push_back(std::move(session));
}

Result<CallGraph, BuildError> CallGraphBuilder::build()
{
    CallGraph g;
    // 图持有全部会话的保活副本：调用点的 InsnId 依赖会话 CodeDom 存活。
    g.sessions_ = sessions_;

    // ---- 第一遍：扫描集合中声明的方法建节点（含无代码方法） ----
    for (ca::usize si = 0; si < sessions_.size(); ++si) {
        const jbc::ir::ClassUnit& u = sessions_[si]->unit();
        const jbc::ir::SymbolPool& syms = u.symbols();
        const ClassKey owner = classKeyOf(g.pool(), syms.str(u.thisClass));
        g.classToSession_.emplace(owner.id, static_cast<ca::u32>(si));
        for (const jbc::ir::MethodUnit& m : u.methods) {
            // 下标寻址：后续 push_back 扩容不失效。
            CallGraph::Node& node = g.nodes_[g.internNode(
                memberKeyOf(g.pool(), owner, syms.str(m.name), syms.str(m.desc)))];
            node.declared = true;
            node.access   = m.access;
        }
    }

    // ---- 第二遍：逐方法升格，遍历 CodeDom 提取 invoke 调用点 ----
    for (ca::usize si = 0; si < sessions_.size(); ++si) {
        jbc::ir::ClassSession& s = *sessions_[si];
        const jbc::ir::ClassUnit& u = s.unit();
        const jbc::ir::SymbolPool& syms = u.symbols();
        const std::string_view className = syms.str(u.thisClass);
        const ClassKey owner = classKeyOf(g.pool(), className);

        const std::vector<jbc::ir::CodeVisit> visits = jbc::ir::collectCodeUnits(s);
        for (const jbc::ir::CodeVisit& v : visits) {
            const jbc::ir::MethodUnit& mu = u.methods[v.methodIdx];
            const std::string_view methodName = syms.str(mu.name);
            const std::string_view methodDesc = syms.str(mu.desc);

            if (v.kind == jbc::ir::CodeVisit::Kind::Failed) {
                return Err(BuildError{BuildError::Stage::Promote, v.error, 0,
                                      className, methodName, methodDesc});
            }
            if (v.kind != jbc::ir::CodeVisit::Kind::Ok) {
                continue;   // abstract/native：第一遍已建节点，无调用边
            }

            const MemberKey callerKey =
                memberKeyOf(g.pool(), owner, methodName, methodDesc);
            const ca::u32 callerIdx = g.internNode(callerKey);

            const jbc::ir::CodeDom& dom = v.unit->dom;
            for (const jbc::ir::InsnId id : dom.order()) {
                const jbc::ir::Insn* in = dom.peek(id);
                if (in == nullptr) {
                    continue;
                }
                InvokeKind kind;
                if (!invokeKindOf(in->op, kind)) {
                    continue;
                }
                jbc::ir::ClassSession::MemberSem sem;
                if (!s.memberOf(*in, sem)) {
                    return Err(BuildError{BuildError::Stage::Operand,
                                          jbc::ir::PromoteError::Ok, id.slot,
                                          className, methodName, methodDesc});
                }

                // indy 先取 BSM 解析结果（fail-closed：未知家族仍走保守边）。
                IndyResolution indy;
                const bool isIndy =
                    kind == InvokeKind::Dynamic && resolveIndyInsn(s, *in, indy);

                // indy 的 CP 操作数无 owner（memberOf 置空视图）→ 驻留空串。
                const MemberKey calleeKey =
                    sem.kind == jbc::ir::ClassSession::MemberSem::Kind::InvokeDynamic
                        ? MemberKey{g.pool().intern(std::string_view{"", 0}),
                                    g.pool().intern(sem.name),
                                    g.pool().intern(sem.desc)}
                        : memberKeyOf(g.pool(), classKeyOf(g.pool(), sem.owner),
                                      sem.name, sem.desc);

                // 外部目标丢弃：未声明的 callee 不建节点、调用点不落
                //（与旧版 includeExternalTargets=false 语义一致：节点不存在）。
                if (g.contains(calleeKey) || options_.includeExternalTargets) {
                    const ca::u32 calleeIdx = g.internNode(calleeKey);
                    const ca::u32 siteIdx = static_cast<ca::u32>(g.sites_.size());
                    CallSite site{callerKey, calleeKey, id, kind,
                                  static_cast<ca::u32>(si)};
                    if (isIndy) {
                        site.indyFamily = indy.family;
                    }
                    g.sites_.push_back(site);
                    g.nodes_[callerIdx].outSites.push_back(siteIdx);
                    g.nodes_[calleeIdx].inSites.push_back(siteIdx);
                }

                // BSM 可解析时补真实边：同一 insn 追加展开目标，kind 按
                // MethodHandle reference_kind 折算。字段句柄（record getter）
                // 不产调用边；未知家族只有上面的保守 Dynamic 边。
                if (isIndy && indy.known()) {
                    std::vector<MemberKey> emitted;
                    for (const HandleTarget& target : indy.targets) {
                        InvokeKind targetKind;
                        if (!invokeKindOfHandle(target, targetKind)) {
                            continue;
                        }
                        const MemberKey targetKey =
                            memberKeyOf(g.pool(), classKeyOf(g.pool(), target.owner),
                                        target.name, target.desc);
                        if (std::find(emitted.begin(), emitted.end(), targetKey) !=
                            emitted.end()) {
                            continue;
                        }
                        emitted.push_back(targetKey);
                        if (!g.contains(targetKey) && !options_.includeExternalTargets) {
                            continue;
                        }
                        const ca::u32 targetIdx = g.internNode(targetKey);
                        const ca::u32 siteIdx = static_cast<ca::u32>(g.sites_.size());
                        CallSite site{callerKey, targetKey, id, targetKind,
                                      static_cast<ca::u32>(si)};
                        site.indyExpanded = true;
                        site.indyFamily   = indy.family;
                        g.sites_.push_back(site);
                        g.nodes_[callerIdx].outSites.push_back(siteIdx);
                        g.nodes_[targetIdx].inSites.push_back(siteIdx);
                    }
                }
            }
        }
    }

    return Ok(std::move(g));
}

}   // namespace mjbase::asm_classpath
