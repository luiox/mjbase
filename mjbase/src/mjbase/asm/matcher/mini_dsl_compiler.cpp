/// @file        mini_dsl_compiler.cpp
/// @brief       mini DSL 编译：AST → MatchStep 序列 + 求值替换策略。
///
/// 语义对照旧 
///   - ConstLoad 命名绑定按步骤下标从 matchedInsns 取值（ldc 载荷在纯
///     DOM 层不可读，读不到则不绑定，后续 require 报 UnboundVariable）；
///   - 求值改为 I/J 走 i64、F/D 走 double 的分域实现（旧实现全程 double，
///     超 2^53 的 long 失真；结果域内的取值一致）；
///   - Method 模式/替换（owner/name/desc）需要常量池语义，编译期
///     Err(DslSemantic)，不再静默降级为 opcode 匹配；
///   - VarLoad/VarStore 照旧编译为恒真步骤。
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mini_dsl_compiler.hpp"

#include "mini_dsl_binding.hpp"
#include "mini_dsl_parser.hpp"
#include "step_util.hpp"

namespace mjbase::asm_matcher::dsl {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

/// 助记符 → Op 的反查表（jbc::core::mnemonicOf 的 0x00..0xC9 全量反转，
/// 大小写不敏感）。
const std::unordered_map<std::string, jbc::core::Op>& mnemonicIndex()
{
    static const std::unordered_map<std::string, jbc::core::Op> index = [] {
        std::unordered_map<std::string, jbc::core::Op> m;
        m.reserve(0xca);
        for (ca::u32 v = 0; v <= 0xC9; ++v) {
            const auto op = static_cast<jbc::core::Op>(v);
            m.emplace(jbc::core::mnemonicOf(op), op);
        }
        return m;
    }();
    return index;
}

Result<jbc::core::Op, MatcherError> resolveOpcode(std::string_view name)
{
    std::string lower(name);
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto it = mnemonicIndex().find(lower);
    if (it == mnemonicIndex().end()) {
        return Err(MatcherError{MatchError::UnknownOpcode, 0});
    }
    return Ok(it->second);
}

/// 类型标签归一：大写、'L'→'J'；非法/空标签 Err。
Result<char, MatcherError> normalizeType(std::string_view t)
{
    if (t.size() != 1) return Err(MatcherError{MatchError::UnsupportedExpr, 0});
    char c = t.front();
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'L') c = 'J';
    if (c == 'I' || c == 'J' || c == 'F' || c == 'D') return Ok(c);
    return Err(MatcherError{MatchError::UnsupportedExpr, 0});
}

/// 命名绑定记录：值取自 matchedInsns[stepIndex]。
struct BindCapture
{
    ca::usize stepIndex = 0;
    std::string name;
};

/// 编译后的替换动作（旧 ReplaceStep 的可执行形态；Nothing 不产生动作）。
struct EmitOp
{
    jbc::core::Op op;
};
struct EmitOrigin
{
    int start;
    int endExclusive;
};
struct EmitLoad
{
    char typeTag;   // 已归一
    ExprPtr expr;
};
using EmitAction = std::variant<EmitOp, EmitOrigin, EmitLoad>;

/// i64 域截断到 i32（补码回绕，对齐 JVM int 运算）。
ca::i32 wrapI32(ca::i64 v) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(static_cast<ca::u64>(v)));
}

/// 取负（避免 i64 最小值取负的 UB）。
ca::i64 negI64(ca::i64 v) noexcept
{
    return static_cast<ca::i64>(0 - static_cast<ca::u64>(v));
}

/// i64 除/模：除零得 0；MIN/-1 按补码回绕（对齐 JVM idiv/irem）。
ca::i64 divI64(ca::i64 a, ca::i64 b) noexcept
{
    if (b == 0) return 0;
    if (b == -1) return negI64(a);
    return a / b;
}

ca::i64 remI64(ca::i64 a, ca::i64 b) noexcept
{
    if (b == 0) return 0;
    if (b == -1) return 0;   // x rem -1 == 0
    return a % b;
}

/// double → i64 的定义行为截断（越界饱和，NaN 得 0）。
ca::i64 truncToI64(double v) noexcept
{
    if (std::isnan(v)) return 0;
    // 2^63 的 double 表示恰好可表示；严格大于等于它即越上界。
    if (v >= 9223372036854775808.0) return std::numeric_limits<ca::i64>::max();
    if (v <= -9223372036854775808.0) return std::numeric_limits<ca::i64>::min();
    return static_cast<ca::i64>(v);
}

Result<double, MatcherError> evalReal(const ExprNode& e, DslBinding& b);

/// 整型域求值（tag 'I'：每步回绕 i32；tag 'J'：全程 i64）。
Result<ca::i64, MatcherError> evalInt(const ExprNode& e, DslBinding& b, char tag)
{
    if (const auto* lit = std::get_if<ExprNode::IntLit>(&e.data)) {
        return Ok(lit->value);
    }
    if (const auto* var = std::get_if<ExprNode::VarRef>(&e.data)) {
        auto v = b.require(var->name);
        if (v.is_err()) return Err(v.unwrap_err());
        const DslValue& dv = v.unwrap();
        if (dv.kind == DslValue::Kind::Int || dv.kind == DslValue::Kind::Long) {
            return Ok(dv.num);
        }
        return Ok(truncToI64(dv.real));   // 浮绑定进整域：截断
    }
    if (const auto* una = std::get_if<ExprNode::Unary>(&e.data)) {
        if (una->op != "-") return Err(MatcherError{MatchError::UnsupportedExpr, 0});
        auto v = evalInt(*una->operand, b, tag);
        if (v.is_err()) return v;
        return Ok(negI64(v.unwrap()));
    }
    if (const auto* bin = std::get_if<ExprNode::Binary>(&e.data)) {
        auto l = evalInt(*bin->left, b, tag);
        if (l.is_err()) return l;
        auto r = evalInt(*bin->right, b, tag);
        if (r.is_err()) return r;
        const ca::i64 a = l.unwrap();
        const ca::i64 c = r.unwrap();
        const ca::u64 ua = static_cast<ca::u64>(a);
        const bool isI = tag == 'I';
        const auto done = [&](ca::i64 v) {
            return Ok(isI ? static_cast<ca::i64>(wrapI32(v)) : v);
        };
        const auto shiftMask = [&]() {
            return static_cast<ca::i64>(static_cast<ca::u64>(c) & (isI ? 0x1F : 0x3F));
        };
        const std::string& op = bin->op;
        if (op == "+") return done(a + c);
        if (op == "-") return done(a - c);
        if (op == "*") return done(a * c);
        if (op == "/") return done(divI64(a, c));
        if (op == "%") return done(remI64(a, c));
        if (op == "&") return done(a & c);
        if (op == "|") return done(a | c);
        if (op == "^") return done(a ^ c);
        if (op == "<<") return done(a << shiftMask());
        if (op == ">>") return done(a >> shiftMask());
        if (op == ">>>") return done(static_cast<ca::i64>(ua >> shiftMask()));
        return Err(MatcherError{MatchError::UnsupportedExpr, 0});
    }
    if (const auto* cast = std::get_if<ExprNode::Cast>(&e.data)) {
        auto target = normalizeType(cast->targetType);
        if (target.is_err()) return Err(target.unwrap_err());
        switch (target.unwrap()) {
        case 'I': {
            auto v = evalInt(*cast->inner, b, 'I');
            if (v.is_err()) return v;
            return Ok(static_cast<ca::i64>(wrapI32(v.unwrap())));
        }
        case 'J': return evalInt(*cast->inner, b, 'J');
        case 'F':
        case 'D': {
            auto v = evalReal(*cast->inner, b);
            if (v.is_err()) return Err(v.unwrap_err());
            return Ok(truncToI64(v.unwrap()));
        }
        }
    }
    return Err(MatcherError{MatchError::UnsupportedExpr, 0});
}

/// 浮点域求值（F/D 同路：中间量保持 double，F 的舍入只发生在 cast/发射）。
Result<double, MatcherError> evalReal(const ExprNode& e, DslBinding& b)
{
    if (const auto* lit = std::get_if<ExprNode::IntLit>(&e.data)) {
        return Ok(static_cast<double>(lit->value));
    }
    if (const auto* var = std::get_if<ExprNode::VarRef>(&e.data)) {
        auto v = b.require(var->name);
        if (v.is_err()) return Err(v.unwrap_err());
        const DslValue& dv = v.unwrap();
        if (dv.kind == DslValue::Kind::Int || dv.kind == DslValue::Kind::Long) {
            return Ok(static_cast<double>(dv.num));
        }
        return Ok(dv.real);
    }
    if (const auto* una = std::get_if<ExprNode::Unary>(&e.data)) {
        if (una->op != "-") return Err(MatcherError{MatchError::UnsupportedExpr, 0});
        auto v = evalReal(*una->operand, b);
        if (v.is_err()) return v;
        return Ok(-v.unwrap());
    }
    if (const auto* bin = std::get_if<ExprNode::Binary>(&e.data)) {
        auto l = evalReal(*bin->left, b);
        if (l.is_err()) return l;
        auto r = evalReal(*bin->right, b);
        if (r.is_err()) return r;
        const double a = l.unwrap();
        const double c = r.unwrap();
        const std::string& op = bin->op;
        if (op == "+") return Ok(a + c);
        if (op == "-") return Ok(a - c);
        if (op == "*") return Ok(a * c);
        if (op == "/") return c != 0.0 ? Ok(a / c) : Ok(0.0);
        if (op == "%") return c != 0.0 ? Ok(std::fmod(a, c)) : Ok(0.0);
        return Err(MatcherError{MatchError::UnsupportedExpr, 0});
    }
    if (const auto* cast = std::get_if<ExprNode::Cast>(&e.data)) {
        auto target = normalizeType(cast->targetType);
        if (target.is_err()) return Err(target.unwrap_err());
        switch (target.unwrap()) {
        case 'I':
        case 'J': {
            auto v = evalInt(*cast->inner, b, target.unwrap());
            if (v.is_err()) return Err(v.unwrap_err());
            return Ok(static_cast<double>(v.unwrap()));
        }
        case 'F': {
            auto v = evalReal(*cast->inner, b);
            if (v.is_err()) return v;
            return Ok(static_cast<double>(static_cast<float>(v.unwrap())));
        }
        case 'D': return evalReal(*cast->inner, b);
        }
    }
    return Err(MatcherError{MatchError::UnsupportedExpr, 0});
}

/// 从命中指令提取绑定值（ldc 族载荷不可读 → false，不绑定）。
bool readDslValue(const jbc::ir::Insn& in, DslValue& out)
{
    ca::i32 iv = 0;
    ca::i64 lv = 0;
    double rv = 0.0;
    if (Steps::getIntValue(in, iv)) {
        out = DslValue{DslValue::Kind::Int, iv, 0.0};
        return true;
    }
    if (Steps::getLongValue(in, lv)) {
        out = DslValue{DslValue::Kind::Long, lv, 0.0};
        return true;
    }
    if (Steps::getFloatValue(in, rv)) {
        out = DslValue{DslValue::Kind::Float, 0, rv};
        return true;
    }
    if (Steps::getDoubleValue(in, rv)) {
        out = DslValue{DslValue::Kind::Double, 0, rv};
        return true;
    }
    return false;
}

}   // namespace

Result<MatchRule, MatcherError> MiniDslCompiler::compile(const Rule& astRule)
{
    std::vector<MatchStep> steps;
    std::vector<BindCapture> captures;
    steps.reserve(astRule.matchSteps.size());

    for (ca::usize si = 0; si < astRule.matchSteps.size(); ++si) {
        const PatternStep& step = astRule.matchSteps[si];
        if (const auto* opc = std::get_if<PatternStep::Opcode>(&step.data)) {
            auto op = resolveOpcode(opc->name);
            if (op.is_err()) return Err(op.unwrap_err());
            steps.push_back(MatchStep::op(op.unwrap()));
        } else if (std::get_if<PatternStep::Method>(&step.data) != nullptr) {
            // owner/name/desc 匹配需要常量池语义；纯 CodeDom 层明确拒绝，
            // 不降级为按 opcode 匹配（静默放宽等于错配）。
            return Err(MatcherError{MatchError::DslSemantic, 0});
        } else if (const auto* cl = std::get_if<PatternStep::ConstLoad>(&step.data)) {
            auto tag = normalizeType(cl->typeTag);
            if (tag.is_err()) return Err(tag.unwrap_err());
            const char t = tag.unwrap();
            const ca::i64 want = cl->constVal;
            const bool hasConst = cl->hasConst;
            MatchStep s = MatchStep::anyIf([t, want, hasConst](const jbc::ir::Insn& in) {
                if (!Steps::isConstLoadOfType(t, in)) return false;
                if (!hasConst) return true;
                // 值约束：旧 AST 解析了 =v 但未接线；纯 DOM 可判形态在此接上。
                ca::i32 iv = 0;
                ca::i64 lv = 0;
                double rv = 0.0;
                switch (t) {
                case 'I':
                    return Steps::getIntValue(in, iv) && static_cast<ca::i64>(iv) == want;
                case 'J': return Steps::getLongValue(in, lv) && lv == want;
                case 'F':
                    return Steps::getFloatValue(in, rv) &&
                           static_cast<ca::i64>(rv) == want;
                case 'D':
                    return Steps::getDoubleValue(in, rv) &&
                           static_cast<ca::i64>(rv) == want;
                }
                return false;
            });
            if (!cl->bind.empty()) captures.push_back(BindCapture{si, cl->bind});
            steps.push_back(std::move(s));
        } else if (std::get_if<PatternStep::VarLoad>(&step.data) != nullptr ||
                   std::get_if<PatternStep::VarStore>(&step.data) != nullptr) {
            // 照旧：vload/vstore 编译为恒真步骤。
            steps.push_back(MatchStep::any());
        }
    }

    std::vector<EmitAction> plan;
    plan.reserve(astRule.replaceSteps.size());
    for (const ReplaceStep& rs : astRule.replaceSteps) {
        if (rs.isLoadExpr()) {
            auto tag = normalizeType(rs.loadExprType);
            if (tag.is_err()) return Err(tag.unwrap_err());
            plan.push_back(EmitLoad{tag.unwrap(), rs.loadExpr});
            continue;
        }
        if (std::get_if<ReplaceStep::Nothing>(&rs.data) != nullptr) {
            continue;   // 不发射
        }
        if (const auto* opc = std::get_if<ReplaceStep::Opcode>(&rs.data)) {
            auto op = resolveOpcode(opc->name);
            if (op.is_err()) return Err(op.unwrap_err());
            plan.push_back(EmitOp{op.unwrap()});
        } else if (const auto* org = std::get_if<ReplaceStep::OriginRange>(&rs.data)) {
            if (org->start < 0 || org->start >= org->endExclusive) {
                return Err(MatcherError{MatchError::DslSemantic, 0});
            }
            plan.push_back(EmitOrigin{org->start, org->endExclusive});
        } else if (std::get_if<ReplaceStep::Method>(&rs.data) != nullptr) {
            return Err(MatcherError{MatchError::DslSemantic, 0});   // 同模式侧：需 CP
        }
    }

    MatchRule rule(std::move(steps));
    rule.setStrategy([plan = std::move(plan), captures = std::move(captures)](
                         MatchContext& ctx) -> Result<void, MatcherError> {
        DslBinding binding;
        for (const BindCapture& c : captures) {
            if (c.stepIndex >= ctx.matchedInsns.size()) continue;   // 照旧：越界不绑定
            DslValue v;
            if (readDslValue(ctx.matchedInsns[c.stepIndex], v)) {
                if (!binding.bind(c.name, v)) {
                    return Err(MatcherError{MatchError::DslSemantic, 0});
                }
            }
        }
        for (const EmitAction& act : plan) {
            if (const auto* e = std::get_if<EmitOp>(&act)) {
                jbc::ir::Insn in;
                in.op = e->op;
                ctx.replacement.push_back(in);
            } else if (const auto* o = std::get_if<EmitOrigin>(&act)) {
                if (static_cast<ca::usize>(o->endExclusive) > ctx.matchedInsns.size()) {
                    return Err(MatcherError{MatchError::DslSemantic, 0});
                }
                for (int i = o->start; i < o->endExclusive; ++i) {
                    ctx.replacement.push_back(ctx.matchedInsns[static_cast<ca::usize>(i)]);
                }
            } else if (const auto* l = std::get_if<EmitLoad>(&act)) {
                if (l->typeTag == 'I' || l->typeTag == 'J') {
                    auto v = evalInt(*l->expr, binding, l->typeTag);
                    if (v.is_err()) return Err(v.unwrap_err());
                    auto insn = l->typeTag == 'I'
                                    ? Steps::makeIntInsn(wrapI32(v.unwrap()))
                                    : Steps::makeLongInsn(v.unwrap());
                    if (insn.is_err()) return Err(insn.unwrap_err());
                    ctx.replacement.push_back(insn.unwrap());
                } else {
                    auto v = evalReal(*l->expr, binding);
                    if (v.is_err()) return Err(v.unwrap_err());
                    auto insn = l->typeTag == 'F' ? Steps::makeFloatInsn(v.unwrap())
                                                  : Steps::makeDoubleInsn(v.unwrap());
                    if (insn.is_err()) return Err(insn.unwrap_err());
                    ctx.replacement.push_back(insn.unwrap());
                }
            }
        }
        return Ok();
    });
    return Ok(std::move(rule));
}

Result<MatchRule, MatcherError> MiniDslCompiler::compile(std::string_view dslRule)
{
    auto rule = MiniDslParser::parseRule(dslRule);
    if (rule.is_err()) return Err(rule.unwrap_err());
    return compile(rule.unwrap());
}

Result<void, MatcherError> MiniDslCompiler::compileAndAdd(PatternMatcher& matcher,
                                                          std::string_view dslRule)
{
    auto rule = compile(dslRule);
    if (rule.is_err()) return Err(rule.unwrap_err());
    matcher.addRule(std::move(rule).unwrap());
    return Ok();
}

Result<void, MatcherError> MiniDslCompiler::compileAllAndAdd(PatternMatcher& matcher,
                                                             std::string_view dslRules)
{
    auto rules = MiniDslParser::parseRules(dslRules);
    if (rules.is_err()) return Err(rules.unwrap_err());
    for (const Rule& r : rules.unwrap()) {
        auto compiled = compile(r);
        if (compiled.is_err()) return Err(compiled.unwrap_err());
        matcher.addRule(std::move(compiled).unwrap());
    }
    return Ok();
}

}   // namespace mjbase::asm_matcher::dsl
