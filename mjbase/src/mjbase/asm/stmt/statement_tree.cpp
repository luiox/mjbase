/// @file        statement_tree.cpp
/// @brief       保守结构化语句层的构建、查询与叶子写回。
#include "mjbase/asm/stmt/statement_tree.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <utility>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"

#include "mjbase/asm/insn/insn_op.hpp"

namespace mjbase::asm_stmt {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

bool isJsr(jbc::core::Op op) noexcept
{
    return op == jbc::core::Op::JSR || op == jbc::core::Op::JSR_W;
}

const std::vector<StmtId>&         emptyStmtIds() noexcept
{
    static const std::vector<StmtId> kEmpty;
    return kEmpty;
}

const std::vector<jbc::ir::InsnId>& emptyInsns() noexcept
{
    static const std::vector<jbc::ir::InsnId> kEmpty;
    return kEmpty;
}

const asm_cfg::ControlFlowGraph& emptyCfg() noexcept
{
    static const asm_cfg::ControlFlowGraph kEmpty;
    return kEmpty;
}

}   // namespace

const char* to_string(StmtKind kind) noexcept
{
    switch (kind) {
    case StmtKind::Invalid: return "Invalid";
    case StmtKind::Sequence: return "Sequence";
    case StmtKind::Block: return "Block";
    case StmtKind::Opaque: return "Opaque";
    case StmtKind::If: return "If";
    case StmtKind::While: return "While";
    case StmtKind::For: return "For";
    case StmtKind::DoWhile: return "DoWhile";
    }
    return "Invalid";
}

const char* to_string(StatementError error) noexcept
{
    switch (error) {
    case StatementError::Ok: return "Ok";
    case StatementError::CfgBuild: return "CfgBuild";
    }
    return "CfgBuild";
}

const char* to_string(StmtEditError error) noexcept
{
    switch (error) {
    case StmtEditError::Ok: return "Ok";
    case StmtEditError::InvalidStmt: return "InvalidStmt";
    case StmtEditError::NotLeaf: return "NotLeaf";
    case StmtEditError::StaleWindow: return "StaleWindow";
    case StmtEditError::ExceptionBoundary: return "ExceptionBoundary";
    case StmtEditError::ExternallyReferenced: return "ExternallyReferenced";
    case StmtEditError::LabelAttached: return "LabelAttached";
    case StmtEditError::EditFailed: return "EditFailed";
    }
    return "EditFailed";
}

struct StatementTree::Impl
{
    struct Node
    {
        StmtKind                       kind = StmtKind::Invalid;
        StmtId                         parent;
        std::vector<StmtId>            children;
        std::vector<jbc::ir::InsnId>   insns;
        asm_cfg::BlockId               block;
        bool                           hasIf = false;
        IfInfo                         ifInfo;
        bool                           hasLoop = false;
        LoopInfo                       loopInfo;
    };

    asm_cfg::ControlFlowGraph cfg;
    std::vector<Node>         nodes;
    StmtId                    root;
    std::unordered_map<jbc::ir::InsnId, StmtId, asm_cfg::InsnIdHash> insnOwner;
    std::vector<StmtId>       blockOwner;
};

/// @brief 构建器：按程序序消费块，能匹配结构模板就建结构节点，否则建叶子。
///
/// 匹配是「先验证后建节点」：所有形状检查通过才创建子节点并标记消费，
/// 失败不留半成品节点。
struct StatementBuilder
{
    jbc::ir::CodeUnit& unit;
    StatementTree::Impl impl;
    std::vector<char>   consumed;
    std::vector<StmtId> topLevel;

    StatementBuilder(jbc::ir::CodeUnit& u, asm_cfg::ControlFlowGraph&& cfg)
        : unit(u)
    {
        impl.cfg = std::move(cfg);
        impl.blockOwner.assign(impl.cfg.blockCount(), kNoStmt);
        consumed.assign(impl.cfg.blockCount(), 0);
    }

    ca::u32 blockCount() const noexcept
    {
        return static_cast<ca::u32>(impl.cfg.blockCount());
    }

    const asm_cfg::BasicBlock& block(ca::u32 index) const
    {
        return impl.cfg.blocks()[index];
    }

    jbc::core::Op lastOp(ca::u32 index) const
    {
        const auto& insns = block(index).insns;
        if (insns.empty()) return jbc::core::Op::NOP;
        const jbc::ir::Insn* insn = unit.dom.peek(insns.back());
        return insn == nullptr ? jbc::core::Op::NOP : insn->op;
    }

    /// @brief 直线块：末尾不是条件/switch/jsr（GOTO、return、athrow、顺序落入都算）。
    bool isPlain(ca::u32 index) const
    {
        const jbc::core::Op op = lastOp(index);
        return !asm_insn::isConditionalBranch(op) && !asm_insn::isSwitch(op) && !isJsr(op);
    }

    void normalTargets(ca::u32 index, std::vector<ca::u32>& out) const
    {
        for (const auto& edge : impl.cfg.succEdgesOf(asm_cfg::BlockId{index})) {
            if (edge.kind == asm_cfg::EdgeKind::Exception) continue;
            out.push_back(edge.target.index);
        }
    }

    /// @brief 无普通后继（终止块），或所有普通后继都指向 target。
    bool onlySuccessors(ca::u32 index, ca::u32 target) const
    {
        for (const auto& edge : impl.cfg.succEdgesOf(asm_cfg::BlockId{index})) {
            if (edge.kind == asm_cfg::EdgeKind::Exception) continue;
            if (edge.target.index != target) return false;
        }
        return true;
    }

    /// @brief 恰有一条普通后继且指向 target。
    bool singleSuccessor(ca::u32 index, ca::u32 target) const
    {
        bool any = false;
        for (const auto& edge : impl.cfg.succEdgesOf(asm_cfg::BlockId{index})) {
            if (edge.kind == asm_cfg::EdgeKind::Exception) continue;
            if (edge.target.index != target) return false;
            any = true;
        }
        return any;
    }

    struct CondEdges
    {
        ca::u32 taken = asm_cfg::BlockId::kInvalid;
        ca::u32 fall  = asm_cfg::BlockId::kInvalid;
        bool    ok    = false;
    };

    /// @brief 条件块的两条普通边；fall-through 必须物理落在下一块。
    CondEdges condEdges(ca::u32 index) const
    {
        CondEdges out;
        bool      hasTaken = false;
        bool      hasFall  = false;
        for (const auto& edge : impl.cfg.succEdgesOf(asm_cfg::BlockId{index})) {
            switch (edge.kind) {
            case asm_cfg::EdgeKind::Exception:
                break;
            case asm_cfg::EdgeKind::Condition:
                if (hasTaken) return {};
                out.taken = edge.target.index;
                hasTaken  = true;
                break;
            case asm_cfg::EdgeKind::FallThrough:
                if (hasFall) return {};
                out.fall = edge.target.index;
                hasFall  = true;
                break;
            default:
                return {};
            }
        }
        out.ok = hasTaken && hasFall && out.fall == index + 1 && out.taken != out.fall;
        return out;
    }

    StmtId pushNode(StatementTree::Impl::Node node)
    {
        impl.nodes.push_back(std::move(node));
        return StmtId{static_cast<ca::u32>(impl.nodes.size() - 1)};
    }

    StmtId makeLeaf(ca::u32 index, StmtKind kind)
    {
        StatementTree::Impl::Node node;
        node.kind  = kind;
        node.block = asm_cfg::BlockId{index};
        node.insns = block(index).insns;

        const StmtId id = pushNode(std::move(node));
        for (const auto& insn : impl.nodes[id.index].insns) {
            impl.insnOwner.emplace(insn, id);
        }
        impl.blockOwner[index] = id;
        return id;
    }

    void attach(const StmtId parent, const std::vector<StmtId>& children)
    {
        auto& node = impl.nodes[parent.index];
        node.children = children;
        for (const StmtId child : children) {
            impl.nodes[child.index].parent = parent;
        }
    }

    void ownBlock(ca::u32 index, StmtId owner)
    {
        impl.blockOwner[index] = owner;
        for (const auto& insn : block(index).insns) {
            impl.insnOwner.emplace(insn, owner);
        }
    }

    static void appendInsns(std::vector<jbc::ir::InsnId>&       out,
                            const std::vector<jbc::ir::InsnId>& part)
    {
        out.insert(out.end(), part.begin(), part.end());
    }

    std::optional<StmtId> tryFor(ca::u32 header, ca::u32& next)
    {
        const CondEdges ce = condEdges(header);
        if (!ce.ok) return std::nullopt;

        const ca::u32 body   = header + 1;
        const ca::u32 update = header + 2;
        const ca::u32 exit   = header + 3;
        if (exit >= blockCount()) return std::nullopt;
        if (!isPlain(body) || !isPlain(update)) return std::nullopt;
        if (!singleSuccessor(body, update) || !singleSuccessor(update, header)) return std::nullopt;
        if (!((ce.taken == body && ce.fall == exit) || (ce.taken == exit && ce.fall == body))) {
            return std::nullopt;
        }

        const StmtId bodyLeaf   = makeLeaf(body, StmtKind::Block);
        const StmtId updateLeaf = makeLeaf(update, StmtKind::Block);

        StatementTree::Impl::Node node;
        node.kind     = StmtKind::For;
        node.hasLoop  = true;
        node.loopInfo = LoopInfo{block(header).insns.back(), ce.taken == body};
        node.insns    = block(header).insns;
        appendInsns(node.insns, impl.nodes[bodyLeaf.index].insns);
        appendInsns(node.insns, impl.nodes[updateLeaf.index].insns);

        const StmtId id = pushNode(std::move(node));
        attach(id, {bodyLeaf, updateLeaf});
        ownBlock(header, id);
        consumed[header] = consumed[body] = consumed[update] = 1;
        next = exit;
        return id;
    }

    std::optional<StmtId> tryWhile(ca::u32 header, ca::u32& next)
    {
        const CondEdges ce = condEdges(header);
        if (!ce.ok) return std::nullopt;

        const ca::u32 body = header + 1;
        const ca::u32 exit = header + 2;
        if (exit >= blockCount()) return std::nullopt;
        if (!isPlain(body) || !singleSuccessor(body, header)) return std::nullopt;
        if (!((ce.taken == body && ce.fall == exit) || (ce.taken == exit && ce.fall == body))) {
            return std::nullopt;
        }

        const StmtId bodyLeaf = makeLeaf(body, StmtKind::Block);

        StatementTree::Impl::Node node;
        node.kind     = StmtKind::While;
        node.hasLoop  = true;
        node.loopInfo = LoopInfo{block(header).insns.back(), ce.taken == body};
        node.insns    = block(header).insns;
        appendInsns(node.insns, impl.nodes[bodyLeaf.index].insns);

        const StmtId id = pushNode(std::move(node));
        attach(id, {bodyLeaf});
        ownBlock(header, id);
        consumed[header] = consumed[body] = 1;
        next = exit;
        return id;
    }

    std::optional<StmtId> tryDoWhile(ca::u32 body, ca::u32& next)
    {
        const ca::u32 latch = body + 1;
        const ca::u32 exit  = body + 2;
        if (exit >= blockCount()) return std::nullopt;
        if (!isPlain(body) || !singleSuccessor(body, latch)) return std::nullopt;

        const CondEdges ce = condEdges(latch);
        if (!ce.ok) return std::nullopt;
        if (ce.taken != body || ce.fall != exit) return std::nullopt;

        const StmtId bodyLeaf = makeLeaf(body, StmtKind::Block);

        StatementTree::Impl::Node node;
        node.kind     = StmtKind::DoWhile;
        node.hasLoop  = true;
        node.loopInfo = LoopInfo{block(latch).insns.back(), true};
        node.insns    = impl.nodes[bodyLeaf.index].insns;
        appendInsns(node.insns, block(latch).insns);

        const StmtId id = pushNode(std::move(node));
        attach(id, {bodyLeaf});
        ownBlock(latch, id);
        consumed[body] = consumed[latch] = 1;
        next = exit;
        return id;
    }

    std::optional<StmtId> tryIf(ca::u32 header, ca::u32& next)
    {
        const CondEdges ce = condEdges(header);
        if (!ce.ok) return std::nullopt;

        const ca::u32 first = header + 1;
        const ca::u32 other = header + 2;
        if (other >= blockCount()) return std::nullopt;
        if (ce.taken != other) return std::nullopt;
        if (!isPlain(first) || !isPlain(other)) return std::nullopt;

        // 单臂：fall-through 臂顺序落入 taken 目标（汇合块）。taken 路径直接
        // 跳过该臂，因此该臂可达汇合块是必要条件。
        if (singleSuccessor(first, other)) {
            const StmtId arm = makeLeaf(first, StmtKind::Block);

            StatementTree::Impl::Node node;
            node.kind    = StmtKind::If;
            node.hasIf   = true;
            node.ifInfo  = IfInfo{block(header).insns.back(), false, false};
            node.insns   = block(header).insns;
            appendInsns(node.insns, impl.nodes[arm.index].insns);

            const StmtId id = pushNode(std::move(node));
            attach(id, {arm});
            ownBlock(header, id);
            consumed[header] = consumed[first] = 1;
            next = other;
            return id;
        }

        // 双臂：head、arm A、arm B、汇合块依次相邻；两臂只能落到汇合块或终止。
        const ca::u32 join = header + 3;
        const bool   joinExists = join < blockCount();
        if (joinExists) {
            if (!onlySuccessors(first, join) || !onlySuccessors(other, join)) return std::nullopt;
        }
        else {
            std::vector<ca::u32> firstTargets;
            std::vector<ca::u32> otherTargets;
            normalTargets(first, firstTargets);
            normalTargets(other, otherTargets);
            if (!firstTargets.empty() || !otherTargets.empty()) return std::nullopt;
        }

        const StmtId firstLeaf = makeLeaf(first, StmtKind::Block);
        const StmtId otherLeaf = makeLeaf(other, StmtKind::Block);

        StatementTree::Impl::Node node;
        node.kind    = StmtKind::If;
        node.hasIf   = true;
        node.ifInfo  = IfInfo{block(header).insns.back(), ce.taken == first, true};
        node.insns   = block(header).insns;
        appendInsns(node.insns, impl.nodes[firstLeaf.index].insns);
        appendInsns(node.insns, impl.nodes[otherLeaf.index].insns);

        const StmtId id = pushNode(std::move(node));
        attach(id, {firstLeaf, otherLeaf});
        ownBlock(header, id);
        consumed[header] = consumed[first] = consumed[other] = 1;
        next = joinExists ? join : blockCount();
        return id;
    }

    Result<StatementTree, StatementError> build()
    {
        const ca::u32 count = blockCount();
        ca::u32       index = 0;
        while (index < count) {
            if (consumed[index]) {
                ++index;
                continue;
            }

            ca::u32               next = index + 1;
            std::optional<StmtId> node;

            const bool plain = isPlain(index);
            if (plain) {
                node = tryDoWhile(index, next);
            }
            else if (asm_insn::isConditionalBranch(lastOp(index))) {
                node = tryFor(index, next);
                if (!node) node = tryWhile(index, next);
                if (!node) node = tryIf(index, next);
            }

            if (!node) {
                node = makeLeaf(index, plain ? StmtKind::Block : StmtKind::Opaque);
                next = index + 1;
            }

            topLevel.push_back(*node);
            index = next > index ? next : index + 1;
        }

        StatementTree::Impl::Node root;
        root.kind     = StmtKind::Sequence;
        root.children = topLevel;
        for (const StmtId child : root.children) {
            appendInsns(root.insns, impl.nodes[child.index].insns);
        }

        const StmtId rootId = pushNode(std::move(root));
        for (const StmtId child : impl.nodes[rootId.index].children) {
            impl.nodes[child.index].parent = rootId;
        }
        impl.root = rootId;

        return Ok(StatementTree(std::make_unique<StatementTree::Impl>(std::move(impl))));
    }
};

StatementTree::StatementTree()
    : impl_(std::make_unique<Impl>())
{}

StatementTree::~StatementTree() = default;

StatementTree::StatementTree(StatementTree&& other) noexcept
    : impl_(std::move(other.impl_))
{}

StatementTree& StatementTree::operator=(StatementTree&& other) noexcept
{
    if (this != &other) impl_ = std::move(other.impl_);
    return *this;
}

StatementTree::StatementTree(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{}

Result<StatementTree, StatementError> StatementTree::build(jbc::ir::CodeUnit& unit)
{
    auto cfg = asm_cfg::ControlFlowGraph::build(unit);
    if (cfg.is_err()) return Err(StatementError::CfgBuild);

    StatementBuilder builder(unit, std::move(cfg).unwrap());
    return builder.build();
}

const asm_cfg::ControlFlowGraph& StatementTree::cfg() const noexcept
{
    return impl_ ? impl_->cfg : emptyCfg();
}

StmtId StatementTree::root() const noexcept
{
    return impl_ ? impl_->root : kNoStmt;
}

ca::usize StatementTree::size() const noexcept
{
    return impl_ ? impl_->nodes.size() : 0;
}

bool StatementTree::contains(StmtId id) const noexcept
{
    return impl_ && id.index < impl_->nodes.size();
}

StmtKind StatementTree::kindOf(StmtId id) const noexcept
{
    if (!contains(id)) return StmtKind::Invalid;
    return impl_->nodes[id.index].kind;
}

StmtId StatementTree::parentOf(StmtId id) const noexcept
{
    if (!contains(id)) return kNoStmt;
    return impl_->nodes[id.index].parent;
}

const std::vector<StmtId>& StatementTree::childrenOf(StmtId id) const noexcept
{
    if (!contains(id)) return emptyStmtIds();
    return impl_->nodes[id.index].children;
}

const std::vector<jbc::ir::InsnId>& StatementTree::insnsOf(StmtId id) const noexcept
{
    if (!contains(id)) return emptyInsns();
    return impl_->nodes[id.index].insns;
}

jbc::ir::InsnId StatementTree::firstInsnOf(StmtId id) const noexcept
{
    const auto& insns = insnsOf(id);
    return insns.empty() ? jbc::ir::kNoInsn : insns.front();
}

jbc::ir::InsnId StatementTree::lastInsnOf(StmtId id) const noexcept
{
    const auto& insns = insnsOf(id);
    return insns.empty() ? jbc::ir::kNoInsn : insns.back();
}

asm_cfg::BlockId StatementTree::blockOf(StmtId id) const noexcept
{
    if (!contains(id)) return asm_cfg::kNoBlock;
    const auto& node = impl_->nodes[id.index];
    if (node.kind != StmtKind::Block && node.kind != StmtKind::Opaque) {
        return asm_cfg::kNoBlock;
    }
    return node.block;
}

StmtId StatementTree::stmtOf(jbc::ir::InsnId insn) const noexcept
{
    if (!impl_) return kNoStmt;
    const auto it = impl_->insnOwner.find(insn);
    return it == impl_->insnOwner.end() ? kNoStmt : it->second;
}

StmtId StatementTree::stmtOfBlock(asm_cfg::BlockId block) const noexcept
{
    if (!impl_ || block.index >= impl_->blockOwner.size()) return kNoStmt;
    return impl_->blockOwner[block.index];
}

const IfInfo* StatementTree::ifInfoOf(StmtId id) const noexcept
{
    if (!contains(id)) return nullptr;
    const auto& node = impl_->nodes[id.index];
    return node.hasIf ? &node.ifInfo : nullptr;
}

const LoopInfo* StatementTree::loopInfoOf(StmtId id) const noexcept
{
    if (!contains(id)) return nullptr;
    const auto& node = impl_->nodes[id.index];
    return node.hasLoop ? &node.loopInfo : nullptr;
}

bool StatementTree::hasSecondArm(StmtId ifStmt) const noexcept
{
    const IfInfo* info = ifInfoOf(ifStmt);
    return info != nullptr && info->hasSecondArm;
}

StmtId StatementTree::takenArmOf(StmtId ifStmt) const noexcept
{
    const IfInfo* info = ifInfoOf(ifStmt);
    if (info == nullptr) return kNoStmt;

    const auto& children = childrenOf(ifStmt);
    if (children.empty()) return kNoStmt;
    if (info->hasSecondArm) {
        return children[info->firstArmOnTaken ? 0 : 1];
    }
    return info->firstArmOnTaken ? children[0] : kNoStmt;
}

StmtId StatementTree::fallThroughArmOf(StmtId ifStmt) const noexcept
{
    const IfInfo* info = ifInfoOf(ifStmt);
    if (info == nullptr) return kNoStmt;

    const auto& children = childrenOf(ifStmt);
    if (children.empty()) return kNoStmt;
    if (info->hasSecondArm) {
        return children[info->firstArmOnTaken ? 1 : 0];
    }
    return info->firstArmOnTaken ? kNoStmt : children[0];
}

StmtId StatementTree::loopBodyOf(StmtId loop) const noexcept
{
    const StmtKind kind = kindOf(loop);
    if (kind != StmtKind::While && kind != StmtKind::For && kind != StmtKind::DoWhile) {
        return kNoStmt;
    }
    const auto& children = childrenOf(loop);
    return children.empty() ? kNoStmt : children[0];
}

StmtId StatementTree::loopUpdateOf(StmtId loop) const noexcept
{
    if (kindOf(loop) != StmtKind::For) return kNoStmt;
    const auto& children = childrenOf(loop);
    return children.size() > 1 ? children[1] : kNoStmt;
}

Result<jbc::ir::InsnId, StmtEditError>
replaceLeaf(jbc::ir::CodeUnit& unit, const StatementTree& tree, StmtId leaf,
            std::vector<jbc::ir::Insn> replacement)
{
    if (!tree.contains(leaf)) return Err(StmtEditError::InvalidStmt);

    const StmtKind kind = tree.kindOf(leaf);
    if (kind != StmtKind::Block && kind != StmtKind::Opaque) {
        return Err(StmtEditError::NotLeaf);
    }

    const auto& window = tree.insnsOf(leaf);
    if (window.empty()) return Err(StmtEditError::InvalidStmt);

    auto& dom = unit.dom;
    for (const auto& insn : window) {
        const jbc::ir::Insn* live = dom.peek(insn);
        if (live == nullptr || insn.owner != dom.owner()) {
            return Err(StmtEditError::StaleWindow);
        }
    }

    const auto inWindow = [&window](jbc::ir::InsnId insn) {
        return std::find(window.begin(), window.end(), insn) != window.end();
    };

    for (const auto& entry : unit.exc) {
        const jbc::ir::LabelId labels[] = {entry.start, entry.end, entry.handler};
        for (const jbc::ir::LabelId label : labels) {
            auto target = dom.resolveLabel(label);
            if (target.is_err()) return Err(StmtEditError::EditFailed);
            const jbc::ir::LabelTarget resolved = std::move(target).unwrap();
            if (!resolved.isEnd() && inWindow(resolved.insn)) {
                return Err(StmtEditError::ExceptionBoundary);
            }
        }
    }

    for (const auto& insn : window) {
        if (!dom.usersOf(insn).empty()) return Err(StmtEditError::ExternallyReferenced);
    }
    for (const auto& insn : window) {
        if (!dom.labelsAt(insn).empty()) return Err(StmtEditError::LabelAttached);
    }

    jbc::ir::CodeEditor        editor(unit);
    std::vector<jbc::ir::InsnId> inserted;
    inserted.reserve(replacement.size());

    const auto rollback = [&editor, &inserted] {
        if (!inserted.empty()) {
            (void)editor.eraseBatch(inserted);
            inserted.clear();
        }
    };

    for (auto& insn : replacement) {
        auto result = editor.insertBefore(window.front(), std::move(insn));
        if (result.is_err()) {
            rollback();
            return Err(StmtEditError::EditFailed);
        }
        inserted.push_back(result.unwrap());
    }

    if (editor.eraseBatch(window).is_err()) {
        rollback();
        return Err(StmtEditError::EditFailed);
    }

    return Ok(inserted.empty() ? jbc::ir::kNoInsn : inserted.front());
}

}   // namespace mjbase::asm_stmt
