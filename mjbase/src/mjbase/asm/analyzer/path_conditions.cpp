/// @file        path_conditions.cpp
/// @brief       路径条件收集实现：符号值域 + 边约束 must 分析 + 判定与修剪。
#include "path_conditions.hpp"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "jbc/analysis/domain.hpp"
#include "jbc/core/opcode.hpp"

#include "frame_query.hpp"

#include "mjbase/asm/cfg/control_flow_graph.hpp"
#include "mjbase/expr/expr_eval.hpp"
#include "mjbase/expr/expr_print.hpp"
#include "mjbase/expr/expr_simplify.hpp"
#include "mjbase/expr/expr_util.hpp"

namespace mjbase::asm_analyzer {

namespace {

using jbc::analysis::VKind;
using jbc::core::CpTag;
using jbc::core::Op;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::expr::BitWidth;
using mjbase::expr::ConstNode;
using mjbase::expr::ExprRef;
using mjbase::expr::NodeKind;
using mjbase::expr::VarNode;

/// 表达式规模上限：超过即退化为未知，避免约束键与判定成本失控。
constexpr ca::u32 kMaxExprNodes = 512;
/// 化简轮数上限（与 expr::SolveConfig 默认量级一致，只取够用的轮数）。
constexpr ca::u32 kMaxSimplifyRounds = 6;
/// 求值器深度上限（expr::eval 的显式栈保护）。
constexpr ca::u32 kMaxEvalDepth = 256;

expr::ExprRef simplifyWith(const std::vector<std::unique_ptr<expr::SimplifyPass>>& passes,
                           ExprRef e)
{
    if (!e) {
        return nullptr;
    }
    ExprRef current = std::move(e);
    for (ca::u32 round = 0; round < kMaxSimplifyRounds; ++round) {
        const ExprRef before = current;
        for (const auto& pass : passes) {
            current = pass->apply(current);
        }
        if (current.get() == before.get()) {
            break;
        }
        if (expr::nodeCount(*current) > kMaxExprNodes) {
            return nullptr;
        }
    }
    if (expr::nodeCount(*current) > kMaxExprNodes) {
        return nullptr;
    }
    return current;
}

bool isConstExpr(const ExprRef& e) noexcept
{
    return e && e->kind() == NodeKind::Const;
}

bool constOf(const ExprRef& e, ca::i64& out) noexcept
{
    if (!isConstExpr(e)) {
        return false;
    }
    out = static_cast<const ConstNode&>(*e).truncatedValue();
    return true;
}

bool sameExpr(const ExprRef& a, const ExprRef& b) noexcept
{
    if (a.get() == b.get()) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return a->structuralEqual(*b);
}

bool compareValues(ca::i64 a, ca::i64 b, PathCmp cmp) noexcept
{
    switch (cmp) {
    case PathCmp::Eq: return a == b;
    case PathCmp::Ne: return a != b;
    case PathCmp::Lt: return a < b;
    case PathCmp::Le: return a <= b;
    case PathCmp::Gt: return a > b;
    case PathCmp::Ge: return a >= b;
    }
    return false;
}

/// 比较关系换边：x < y ⇔ y > x。
PathCmp flip(PathCmp cmp) noexcept
{
    switch (cmp) {
    case PathCmp::Eq: return PathCmp::Eq;
    case PathCmp::Ne: return PathCmp::Ne;
    case PathCmp::Lt: return PathCmp::Gt;
    case PathCmp::Le: return PathCmp::Ge;
    case PathCmp::Gt: return PathCmp::Lt;
    case PathCmp::Ge: return PathCmp::Le;
    }
    return cmp;
}

/// @brief 一点符号值：空表达式 = 未知，否则是产值点的表达式身份。
struct SymValue
{
    ExprRef expr;
    ca::u8 slots = 1;
};

/// @brief 符号值域：常量折进表达式，未知产值点按句柄命名，汇合取结构相等。
///
/// 身份纪律：参数按 local 槽命名（p<idx>），其余产值点按指令句柄命名
/// （v<slot>）。同一产值点在方法内只有一个符号——回边处不同迭代的同名值
/// 由 must 分析的入口边稀释处理（见 path-conditions.md 的可靠性一节）。
class SymDomain final : public jbc::analysis::IValueDomain<SymValue>
{
public:
    explicit SymDomain(const jbc::core::ConstantPool& cp)
        : cp_(&cp)
        , passes_(expr::defaultPasses())
    {}

    const std::vector<std::unique_ptr<expr::SimplifyPass>>& passes() const noexcept
    {
        return passes_;
    }

    SymValue top() override { return SymValue{nullptr, 1}; }

    SymValue merge(const SymValue& a, const SymValue& b) override
    {
        const ca::u8 slots = a.slots > b.slots ? a.slots : b.slots;
        if (!a.expr || !b.expr) {
            return SymValue{nullptr, slots};
        }
        if (a.slots == b.slots && sameExpr(a.expr, b.expr)) {
            return a;
        }
        return SymValue{nullptr, slots};
    }

    SymValue newValue(const Insn& producer, InsnId at, VKind shape) override
    {
        const ca::u8 slots =
            (shape == VKind::Long || shape == VKind::Double) ? 2 : 1;
        const BitWidth width = slots == 2 ? BitWidth::B64 : BitWidth::B32;
        switch (producer.op) {
        case Op::ICONST_M1: return SymValue{expr::exprConst(-1, BitWidth::B32), 1};
        case Op::ICONST_0: return SymValue{expr::exprConst(0, BitWidth::B32), 1};
        case Op::ICONST_1: return SymValue{expr::exprConst(1, BitWidth::B32), 1};
        case Op::ICONST_2: return SymValue{expr::exprConst(2, BitWidth::B32), 1};
        case Op::ICONST_3: return SymValue{expr::exprConst(3, BitWidth::B32), 1};
        case Op::ICONST_4: return SymValue{expr::exprConst(4, BitWidth::B32), 1};
        case Op::ICONST_5: return SymValue{expr::exprConst(5, BitWidth::B32), 1};
        case Op::LCONST_0: return SymValue{expr::exprConst(0, BitWidth::B64), 2};
        case Op::LCONST_1: return SymValue{expr::exprConst(1, BitWidth::B64), 2};
        case Op::BIPUSH:
        case Op::SIPUSH:
            return SymValue{expr::exprConst(producer.a, BitWidth::B32), 1};
        case Op::LDC:
        case Op::LDC_W:
        case Op::LDC2_W: {
            if (cp_->valid(producer.cp)) {
                switch (cp_->tagOf(producer.cp)) {
                case CpTag::Integer:
                    return SymValue{expr::exprConst(cp_->at(producer.cp).ival, BitWidth::B32), 1};
                case CpTag::Long:
                    return SymValue{expr::exprConst(cp_->at(producer.cp).lval, BitWidth::B64), 2};
                default: break;
                }
            }
            break;
        }
        default: break;
        }
        return SymValue{expr::exprVar(siteName(at), width), slots};
    }

    SymValue newParameter(ca::u32 localIdx, VKind shape) override
    {
        const ca::u8 slots =
            (shape == VKind::Long || shape == VKind::Double) ? 2 : 1;
        return SymValue{
            expr::exprVar("p" + std::to_string(localIdx),
                          slots == 2 ? BitWidth::B64 : BitWidth::B32),
            slots};
    }

    SymValue newException(const jbc::ir::CodeUnit::Exc&) override
    {
        return SymValue{nullptr, 1};
    }

    SymValue binary(Op op, const SymValue& a, const SymValue& b, VKind shape,
                    const Insn&, InsnId) override
    {
        const ca::u8 slots =
            (shape == VKind::Long || shape == VKind::Double) ? 2 : 1;
        const auto kind = binKindOf(op);
        if (!kind || !a.expr || !b.expr) {
            return SymValue{nullptr, slots};
        }
        return SymValue{simplifyWith(passes_, expr::exprBinary(*kind, a.expr, b.expr)), slots};
    }

    SymValue unary(Op op, const SymValue& a, VKind shape, const Insn&, InsnId) override
    {
        const ca::u8 slots =
            (shape == VKind::Long || shape == VKind::Double) ? 2 : 1;
        if (!a.expr) {
            return SymValue{nullptr, slots};
        }
        if (op == Op::INEG || op == Op::LNEG) {
            return SymValue{simplifyWith(passes_, expr::exprNeg(a.expr)), slots};
        }
        // 转换族（I2C/I2B/I2S、I2L 等）改变值域而不改变表达式树，建模成
        // 同一表达式会给出错误结论——保守退化为未知。
        return SymValue{nullptr, slots};
    }

    ca::u8 width(const SymValue& v) override { return v.slots; }

    bool equal(const SymValue& a, const SymValue& b) override
    {
        return a.slots == b.slots && sameExpr(a.expr, b.expr);
    }

private:
    static std::string siteName(InsnId id) { return "v" + std::to_string(id.slot); }

    static std::optional<expr::BinOpKind> binKindOf(Op op) noexcept
    {
        switch (op) {
        case Op::IADD:
        case Op::LADD: return expr::BinOpKind::Add;
        case Op::ISUB:
        case Op::LSUB: return expr::BinOpKind::Sub;
        case Op::IMUL:
        case Op::LMUL: return expr::BinOpKind::Mul;
        case Op::IDIV:
        case Op::LDIV: return expr::BinOpKind::Div;
        case Op::IREM:
        case Op::LREM: return expr::BinOpKind::Rem;
        case Op::IAND:
        case Op::LAND: return expr::BinOpKind::And;
        case Op::IOR:
        case Op::LOR: return expr::BinOpKind::Or;
        case Op::IXOR:
        case Op::LXOR: return expr::BinOpKind::Xor;
        case Op::ISHL:
        case Op::LSHL: return expr::BinOpKind::Shl;
        case Op::ISHR:
        case Op::LSHR: return expr::BinOpKind::Shr;
        case Op::IUSHR:
        case Op::LUSHR: return expr::BinOpKind::UShr;
        default: return std::nullopt;
        }
    }

    const jbc::core::ConstantPool* cp_;
    std::vector<std::unique_ptr<expr::SimplifyPass>> passes_;
};

/// 条件分支族（谓词层只覆盖整数比较；null 系由本层返回 Unknown）。
bool isConditionalBranch(Op op) noexcept
{
    switch (op) {
    case Op::IFEQ:
    case Op::IFNE:
    case Op::IFLT:
    case Op::IFGE:
    case Op::IFGT:
    case Op::IFLE:
    case Op::IF_ICMPEQ:
    case Op::IF_ICMPNE:
    case Op::IF_ICMPLT:
    case Op::IF_ICMPGE:
    case Op::IF_ICMPGT:
    case Op::IF_ICMPLE:
    case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE:
    case Op::IFNULL:
    case Op::IFNONNULL: return true;
    default: return false;
    }
}

std::optional<PathCmp> unaryCmpOf(Op op) noexcept
{
    switch (op) {
    case Op::IFEQ: return PathCmp::Eq;
    case Op::IFNE: return PathCmp::Ne;
    case Op::IFLT: return PathCmp::Lt;
    case Op::IFGE: return PathCmp::Ge;
    case Op::IFGT: return PathCmp::Gt;
    case Op::IFLE: return PathCmp::Le;
    default: return std::nullopt;
    }
}

std::optional<PathCmp> binaryCmpOf(Op op) noexcept
{
    switch (op) {
    case Op::IF_ICMPEQ: return PathCmp::Eq;
    case Op::IF_ICMPNE: return PathCmp::Ne;
    case Op::IF_ICMPLT: return PathCmp::Lt;
    case Op::IF_ICMPGE: return PathCmp::Ge;
    case Op::IF_ICMPGT: return PathCmp::Gt;
    case Op::IF_ICMPLE: return PathCmp::Le;
    default: return std::nullopt;
    }
}

/// 常量侧归一到右操作数；换边保持语义（x < 5 与 5 > x 同为 x<5）。
PathConstraint canonical(PathCmp cmp, ExprRef lhs, ExprRef rhs)
{
    if (isConstExpr(lhs) && !isConstExpr(rhs)) {
        std::swap(lhs, rhs);
        cmp = flip(cmp);
    }
    return PathConstraint{cmp, std::move(lhs), std::move(rhs)};
}

std::optional<PathConstraint> conditionOf(const Insn& in,
                                          const FrameQuery<SymValue>& frames,
                                          InsnId at)
{
    if (const auto cmp = unaryCmpOf(in.op)) {
        const SymValue* v = frames.stackFromTop(at, 0);
        if (v == nullptr || !v->expr) {
            return std::nullopt;
        }
        return canonical(*cmp, v->expr,
                         expr::exprConst(0, v->slots == 2 ? BitWidth::B64 : BitWidth::B32));
    }
    if (const auto cmp = binaryCmpOf(in.op)) {
        const SymValue* right = frames.stackFromTop(at, 0);
        const SymValue* left = frames.stackFromTop(at, 1);
        if (left == nullptr || right == nullptr || !left->expr || !right->expr) {
            return std::nullopt;
        }
        return canonical(*cmp, left->expr, right->expr);
    }
    return std::nullopt;
}

PathConstraint negated(const PathConstraint& c)
{
    return PathConstraint{negate(c.cmp), c.lhs, c.rhs};
}

std::string keyOf(const PathConstraint& c)
{
    std::string key = to_string(c.cmp);
    key.push_back('|');
    key += expr::toInfix(*c.lhs);
    key.push_back('|');
    key += expr::toInfix(*c.rhs);
    return key;
}

/// 事实集：键是有序规范化文本，值保留表达式（查询/判定用）。map 的键序
/// 同时给 constraintsAt 提供确定性输出。
using FactSet = std::map<std::string, PathConstraint>;

bool insertFact(FactSet& facts, const PathConstraint& c)
{
    const std::string key = keyOf(c);
    if (facts.count(key) != 0) {
        return true;
    }
    if (facts.count(keyOf(negated(c))) != 0) {
        return false;
    }
    facts.emplace(key, c);
    return true;
}

FactSet intersectFacts(const FactSet& a, const FactSet& b)
{
    FactSet out;
    for (const auto& entry : a) {
        if (b.count(entry.first) != 0) {
            out.emplace(entry.first, entry.second);
        }
    }
    return out;
}

using Bindings = std::unordered_map<std::string, expr::ExprValue>;

/// 事实集中的 var == const 等式转成变量绑定；同一变量多个不同常量时弃用
/// 该绑定（不一致事实集不参与求值，保守退回未知）。
Bindings bindingsOf(const FactSet& facts)
{
    Bindings out;
    std::unordered_set<std::string> conflicted;
    for (const auto& entry : facts) {
        const PathConstraint& atom = entry.second;
        if (atom.cmp != PathCmp::Eq) {
            continue;
        }
        const VarNode* var = nullptr;
        const ConstNode* con = nullptr;
        if (atom.lhs && atom.lhs->kind() == NodeKind::Var && isConstExpr(atom.rhs)) {
            var = static_cast<const VarNode*>(atom.lhs.get());
            con = static_cast<const ConstNode*>(atom.rhs.get());
        }
        else if (atom.rhs && atom.rhs->kind() == NodeKind::Var && isConstExpr(atom.lhs)) {
            var = static_cast<const VarNode*>(atom.rhs.get());
            con = static_cast<const ConstNode*>(atom.lhs.get());
        }
        if (var == nullptr || con == nullptr) {
            continue;
        }
        const auto inserted = out.emplace(var->name(), expr::ExprValue(con->value(), con->bitWidth()));
        if (!inserted.second && inserted.first->second.rawValue() != con->truncatedValue()) {
            conflicted.insert(var->name());
        }
    }
    for (const std::string& name : conflicted) {
        out.erase(name);
    }
    return out;
}

std::optional<ca::i64> evalConst(const ExprRef& e, const Bindings& bindings)
{
    if (!e) {
        return std::nullopt;
    }
    const expr::EvalResult result = expr::eval(*e, bindings, kMaxEvalDepth);
    if (!result.ok) {
        return std::nullopt;
    }
    return result.value.rawValue();
}

/// @brief 在事实集下判定谓词。
///
/// 判定顺序：常量/同式 → 事实原子（含否定）→ var==const 绑定求值 →
/// Eq/Ne 的差式常量折叠。顺序与成本匹配：前两步覆盖绝大多数边，绑定求值
/// 处理 x==5 推出 x+1==6，差式兜底处理可完全折叠的等式。
PathTruth decide(const PathConstraint& c, const FactSet& facts,
                 const std::vector<std::unique_ptr<expr::SimplifyPass>>& passes)
{
    if (!c.lhs || !c.rhs) {
        return PathTruth::Unknown;
    }
    if (sameExpr(c.lhs, c.rhs)) {
        const bool holds = c.cmp == PathCmp::Eq || c.cmp == PathCmp::Le || c.cmp == PathCmp::Ge;
        return holds ? PathTruth::AlwaysTrue : PathTruth::AlwaysFalse;
    }
    ca::i64 left = 0;
    ca::i64 right = 0;
    if (constOf(c.lhs, left) && constOf(c.rhs, right)) {
        return compareValues(left, right, c.cmp) ? PathTruth::AlwaysTrue : PathTruth::AlwaysFalse;
    }
    if (facts.count(keyOf(c)) != 0) {
        return PathTruth::AlwaysTrue;
    }
    if (facts.count(keyOf(negated(c))) != 0) {
        return PathTruth::AlwaysFalse;
    }
    const Bindings bindings = bindingsOf(facts);
    if (!bindings.empty()) {
        const auto lv = evalConst(c.lhs, bindings);
        const auto rv = evalConst(c.rhs, bindings);
        if (lv && rv) {
            return compareValues(*lv, *rv, c.cmp) ? PathTruth::AlwaysTrue : PathTruth::AlwaysFalse;
        }
    }
    if (c.cmp == PathCmp::Eq || c.cmp == PathCmp::Ne) {
        const ExprRef diff =
            simplifyWith(passes, expr::exprSub(c.lhs, c.rhs));
        ca::i64 value = 0;
        if (constOf(diff, value)) {
            const bool holds = c.cmp == PathCmp::Eq ? value == 0 : value != 0;
            return holds ? PathTruth::AlwaysTrue : PathTruth::AlwaysFalse;
        }
    }
    return PathTruth::Unknown;
}

/// 条件分支弹出的槽数（修剪时复现栈效应）；非条件分支返回 0。
ca::u8 stackPoppedOf(Op op) noexcept
{
    switch (op) {
    case Op::IFEQ:
    case Op::IFNE:
    case Op::IFLT:
    case Op::IFGE:
    case Op::IFGT:
    case Op::IFLE:
    case Op::IFNULL:
    case Op::IFNONNULL: return 1;
    case Op::IF_ICMPEQ:
    case Op::IF_ICMPNE:
    case Op::IF_ICMPLT:
    case Op::IF_ICMPGE:
    case Op::IF_ICMPGT:
    case Op::IF_ICMPLE:
    case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE: return 2;
    default: return 0;
    }
}

}   // namespace

// ---------------------------------------------------------------------------
// 公开值类型
// ---------------------------------------------------------------------------

const char* to_string(PathCmp cmp) noexcept
{
    switch (cmp) {
    case PathCmp::Eq: return "eq";
    case PathCmp::Ne: return "ne";
    case PathCmp::Lt: return "lt";
    case PathCmp::Le: return "le";
    case PathCmp::Gt: return "gt";
    case PathCmp::Ge: return "ge";
    }
    return "?";
}

PathCmp negate(PathCmp cmp) noexcept
{
    switch (cmp) {
    case PathCmp::Eq: return PathCmp::Ne;
    case PathCmp::Ne: return PathCmp::Eq;
    case PathCmp::Lt: return PathCmp::Ge;
    case PathCmp::Le: return PathCmp::Gt;
    case PathCmp::Gt: return PathCmp::Le;
    case PathCmp::Ge: return PathCmp::Lt;
    }
    return cmp;
}

bool PathConstraint::constantValue(bool& value) const noexcept
{
    ca::i64 left = 0;
    ca::i64 right = 0;
    if (!constOf(lhs, left) || !constOf(rhs, right)) {
        return false;
    }
    value = compareValues(left, right, cmp);
    return true;
}

bool operator==(const PathConstraint& a, const PathConstraint& b) noexcept
{
    return a.cmp == b.cmp && sameExpr(a.lhs, b.lhs) && sameExpr(a.rhs, b.rhs);
}

bool operator!=(const PathConstraint& a, const PathConstraint& b) noexcept
{
    return !(a == b);
}

const char* to_string(PathTruth truth) noexcept
{
    switch (truth) {
    case PathTruth::Unknown: return "unknown";
    case PathTruth::AlwaysTrue: return "always-true";
    case PathTruth::AlwaysFalse: return "always-false";
    }
    return "?";
}

const char* to_string(PathConditionError error) noexcept
{
    switch (error) {
    case PathConditionError::Ok: return "ok";
    case PathConditionError::CfgBuild: return "cfg-build";
    case PathConditionError::Analysis: return "analysis";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 结果对象
// ---------------------------------------------------------------------------

struct PathConditionResult::Impl
{
    jbc::analysis::FrameSummary summary;
    PathConditionError error = PathConditionError::Ok;
    asm_cfg::ControlFlowGraph cfg;
    std::vector<ca::u8> blockReached;
    std::vector<std::vector<PathConstraint>> constraints;
    std::unordered_map<InsnId, PathTruth, asm_cfg::InsnIdHash> truths;
};

PathConditionResult::PathConditionResult()
    : impl_(std::make_unique<Impl>())
{}

PathConditionResult::PathConditionResult(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{}

PathConditionResult::~PathConditionResult() = default;

PathConditionResult::PathConditionResult(PathConditionResult&& other) noexcept = default;

PathConditionResult& PathConditionResult::operator=(PathConditionResult&& other) noexcept = default;

const jbc::analysis::FrameSummary& PathConditionResult::summary() const noexcept
{
    static const jbc::analysis::FrameSummary kEmpty{};
    return impl_ ? impl_->summary : kEmpty;
}

bool PathConditionResult::ok() const noexcept
{
    return impl_ && impl_->error == PathConditionError::Ok && impl_->summary.ok;
}

PathConditionError PathConditionResult::error() const noexcept
{
    return impl_ ? impl_->error : PathConditionError::Analysis;
}

asm_cfg::BlockId PathConditionResult::blockOf(InsnId insn) const noexcept
{
    return impl_ ? impl_->cfg.blockOfInsn(insn) : asm_cfg::kNoBlock;
}

bool PathConditionResult::blockReachable(asm_cfg::BlockId block) const noexcept
{
    if (!ok() || !block.valid() || block.index >= impl_->blockReached.size()) {
        return false;
    }
    return impl_->blockReached[block.index] != 0;
}

const std::vector<PathConstraint>&
PathConditionResult::constraintsAt(asm_cfg::BlockId block) const noexcept
{
    static const std::vector<PathConstraint> kEmpty{};
    if (!ok() || !block.valid() || block.index >= impl_->constraints.size()) {
        return kEmpty;
    }
    return impl_->constraints[block.index];
}

PathTruth PathConditionResult::truthOf(InsnId branch) const noexcept
{
    if (!ok()) {
        return PathTruth::Unknown;
    }
    const auto it = impl_->truths.find(branch);
    return it == impl_->truths.end() ? PathTruth::Unknown : it->second;
}

// ---------------------------------------------------------------------------
// 收集
// ---------------------------------------------------------------------------

PathConditionResult collectPathConditions(const jbc::core::ConstantPool& cp,
                                          const jbc::ir::CodeUnit& unit,
                                          const jbc::analysis::MethodSig* sig)
{
    auto impl = std::make_unique<PathConditionResult::Impl>();

    // ControlFlowGraph::build 的契约是不修改 unit（只做切块与边构造），
    // 分析入口保持 const 借用，避免调用方为只读查询放宽方法体句柄。
    auto built = asm_cfg::ControlFlowGraph::build(const_cast<jbc::ir::CodeUnit&>(unit));
    if (built.is_err()) {
        impl->error = PathConditionError::CfgBuild;
        return PathConditionResult(std::move(impl));
    }
    impl->cfg = std::move(built).unwrap();

    SymDomain dom(cp);
    jbc::analysis::DomainEngine<SymValue> engine(cp, dom);
    jbc::analysis::DomainResult<SymValue> analyzed =
        sig != nullptr ? engine.analyze(unit, *sig) : engine.analyze(unit);
    impl->summary = analyzed.summary;
    if (!analyzed.summary.ok) {
        impl->error = PathConditionError::Analysis;
        return PathConditionResult(std::move(impl));
    }
    FrameQuery<SymValue> frames(unit, std::move(analyzed));

    // 每条条件分支的谓词（操作数未知时为 nullopt；null 系/引用比较不建模）。
    std::unordered_map<InsnId, std::optional<PathConstraint>, asm_cfg::InsnIdHash> predicates;
    for (const InsnId id : unit.dom.order()) {
        const Insn* in = unit.dom.peek(id);
        if (in == nullptr || !isConditionalBranch(in->op)) {
            continue;
        }
        predicates.emplace(id, conditionOf(*in, frames, id));
    }

    const auto& blocks = impl->cfg.blocks();
    const ca::usize blockCount = blocks.size();
    impl->blockReached.assign(blockCount, 0);
    impl->constraints.assign(blockCount, {});
    if (blockCount == 0) {
        return PathConditionResult(std::move(impl));
    }

    // 边约束：taken 边取谓词，fall-through 边取否定；其余边无附加约束。
    const auto constraintOfEdge =
        [&](const asm_cfg::BasicBlock& block,
            const asm_cfg::Edge& edge) -> std::optional<PathConstraint> {
        if (block.insns.empty()) {
            return std::nullopt;
        }
        const auto it = predicates.find(block.insns.back());
        if (it == predicates.end() || !it->second) {
            return std::nullopt;
        }
        if (edge.kind == asm_cfg::EdgeKind::Condition) {
            return it->second;
        }
        if (edge.kind == asm_cfg::EdgeKind::FallThrough) {
            return negated(*it->second);
        }
        return std::nullopt;
    };

    // must 分析：入口事实为空集；其余块取所有入边事实集的交集。事实集只
    // 收缩不增长，回边只可能稀释 header 事实（正确：首轮迭代不受后向约束）。
    std::vector<FactSet> states(blockCount);
    std::vector<ca::u8> hasState(blockCount, 0);
    const asm_cfg::BlockId entry = impl->cfg.entry();
    hasState[entry.index] = 1;
    impl->blockReached[entry.index] = 1;
    std::deque<ca::u32> work;
    work.push_back(entry.index);

    const ca::usize visitCap = blockCount * 64 + 4096;
    ca::usize visits = 0;
    while (!work.empty()) {
        if (++visits > visitCap) {
            // 事实收缩是单调的；触发上限只可能是畸形输入，fail-closed。
            impl->error = PathConditionError::Analysis;
            impl->blockReached.assign(blockCount, 0);
            impl->constraints.assign(blockCount, {});
            impl->truths.clear();
            return PathConditionResult(std::move(impl));
        }
        const ca::u32 index = work.front();
        work.pop_front();
        const asm_cfg::BasicBlock& block = blocks[index];
        for (const asm_cfg::Edge& edge : impl->cfg.succEdgesOf(asm_cfg::BlockId{index})) {
            const std::optional<PathConstraint> constraint = constraintOfEdge(block, edge);
            FactSet candidate = states[index];
            if (constraint) {
                const PathTruth truth = decide(*constraint, states[index], dom.passes());
                if (truth == PathTruth::AlwaysFalse) {
                    continue;   // 该边被路径事实排除
                }
                if (truth != PathTruth::AlwaysTrue && !insertFact(candidate, *constraint)) {
                    continue;   // 与源块事实矛盾：不可行边
                }
            }
            const ca::u32 target = edge.target.index;
            if (target >= blockCount) {
                continue;
            }
            if (!hasState[target]) {
                states[target] = std::move(candidate);
                hasState[target] = 1;
                impl->blockReached[target] = 1;
                work.push_back(target);
                continue;
            }
            FactSet merged = intersectFacts(states[target], candidate);
            if (merged != states[target]) {
                states[target] = std::move(merged);
                work.push_back(target);
            }
        }
    }

    for (ca::usize i = 0; i < blockCount; ++i) {
        if (!hasState[i]) {
            continue;
        }
        impl->constraints[i].reserve(states[i].size());
        for (const auto& entryFact : states[i]) {
            impl->constraints[i].push_back(entryFact.second);
        }
    }

    for (const auto& [id, predicate] : predicates) {
        if (!predicate) {
            continue;
        }
        const asm_cfg::BlockId block = impl->cfg.blockOfInsn(id);
        if (!block.valid() || block.index >= blockCount || !hasState[block.index]) {
            continue;
        }
        impl->truths.emplace(id, decide(*predicate, states[block.index], dom.passes()));
    }

    return PathConditionResult(std::move(impl));
}

std::vector<InsnId> pruneDecidedBranches(jbc::ir::CodeUnit& unit,
                                         const PathConditionResult& facts)
{
    std::vector<InsnId> rewritten;
    if (!facts.ok()) {
        return rewritten;
    }
    // 插入会改动顺序：按快照顺序处理，新增指令不参与本轮判定。
    const std::vector<InsnId> order = unit.dom.order();
    for (const InsnId id : order) {
        const Insn* in = unit.dom.peek(id);
        if (in == nullptr) {
            continue;
        }
        const ca::u8 popped = stackPoppedOf(in->op);
        if (popped == 0) {
            continue;
        }
        const PathTruth truth = facts.truthOf(id);
        if (truth == PathTruth::Unknown) {
            continue;
        }
        const Op replacement = popped == 1 ? Op::POP : Op::POP2;
        if (truth == PathTruth::AlwaysFalse) {
            auto insn = unit.dom.get(id);
            if (insn.is_err()) {
                continue;
            }
            Insn* rewrite = insn.unwrap();
            rewrite->op = replacement;
            rewrite->wide = false;
            rewrite->a = 0;
            rewrite->b = 0;
            rewrite->target = jbc::ir::kNoLabel;
            rewrite->targets.clear();
            rewrite->keys.clear();
            rewritten.push_back(id);
            continue;
        }
        // 恰真：先补操作数出栈，再改成无条件跳转（GOTO_W 避免插入后的
        // 16 位偏移溢出）。
        Insn pop;
        pop.op = replacement;
        const auto inserted = unit.dom.insertBefore(id, pop);
        if (inserted.is_err()) {
            continue;
        }
        auto insn = unit.dom.get(id);
        if (insn.is_err()) {
            continue;
        }
        Insn* rewrite = insn.unwrap();
        rewrite->op = Op::GOTO_W;
        rewrite->wide = false;
        rewrite->a = 0;
        rewrite->b = 0;
        rewritten.push_back(id);
    }
    return rewritten;
}

}   // namespace mjbase::asm_analyzer
