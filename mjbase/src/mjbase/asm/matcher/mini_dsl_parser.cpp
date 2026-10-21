/// @file        mini_dsl_parser.cpp
/// @brief       mini DSL 词法/语法解析。
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "mini_dsl_parser.hpp"

namespace mjbase::asm_matcher::dsl {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

constexpr const char* kMethodOpcodes[] = {
    "invokevirtual", "invokestatic", "invokespecial", "invokeinterface"};

bool isMethodOpcodeToken(std::string_view lower)
{
    for (const char* m : kMethodOpcodes) {
        if (lower == m) return true;
    }
    return false;

}

std::string lowerAscii(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string_view trim(std::string_view s)
{
    ca::usize b = 0;
    ca::usize e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

/// 数字解析（可选前导 '+'，十进制），溢出 Err；不含异常。
Result<ca::i64, MatcherError> parseI64(std::string_view digits, ca::usize pos)
{
    ca::usize i = 0;
    bool negative = false;
    if (i < digits.size() && digits[i] == '-') {
        negative = true;
        ++i;
    } else if (i < digits.size() && digits[i] == '+') {
        ++i;
    }
    if (i >= digits.size()) return Err(MatcherError{MatchError::DslSyntax, pos});

    constexpr ca::u64 kLimit = 0x7FFFFFFFFFFFFFFFull;   // INT64_MAX
    ca::u64 v = 0;
    for (; i < digits.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(digits[i]);
        if (std::isdigit(c) == 0) return Err(MatcherError{MatchError::DslSyntax, pos});
        const ca::u64 d = static_cast<ca::u64>(c - '0');
        if (v > (kLimit - d) / 10) return Err(MatcherError{MatchError::DslSyntax, pos});
        v = v * 10 + d;
    }
    if (negative) return Ok(-static_cast<ca::i64>(v));
    return Ok(static_cast<ca::i64>(v));
}

/// 按括号外的 '-' 切分步骤（表达式内的 '-' 属于 load(...) 的实参）。
Result<std::vector<std::string>, MatcherError> splitByDashOutsideParens(std::string_view text)
{
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : text) {
        if (c == '(') {
            ++depth;
            cur += c;
            continue;
        }
        if (c == ')') {
            if (--depth < 0) return Err(MatcherError{MatchError::DslSyntax, 0});
            cur += c;
            continue;
        }
        if (c == '-' && depth == 0) {
            const std::string_view t = trim(cur);
            if (!t.empty()) parts.emplace_back(t);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (depth != 0) return Err(MatcherError{MatchError::DslSyntax, 0});
    const std::string_view t = trim(cur);
    if (!t.empty()) parts.emplace_back(t);
    return Ok(std::move(parts));
}

/// 按行切分后逐行做括号外 '-' 切分。
Result<std::vector<std::string>, MatcherError> splitSteps(std::string_view side)
{
    std::vector<std::string> result;
    ca::usize begin = 0;
    while (begin <= side.size()) {
        ca::usize end = side.find('\n', begin);
        const bool last = end == std::string_view::npos;
        if (last) end = side.size();
        const std::string_view t = trim(side.substr(begin, end - begin));
        if (!t.empty()) {
            auto parts = splitByDashOutsideParens(t);
            if (parts.is_err()) return parts;
            for (std::string& p : parts.unwrap()) result.push_back(std::move(p));
        }
        if (last) break;
        begin = end + 1;
    }
    return Ok(std::move(result));
}

/// Pratt 表达式解析（优先级与）。
class ExprParser
{
public:
    explicit ExprParser(std::string_view text)
        : text_(text)
    {}

    Result<ExprPtr, MatcherError> parse()
    {
        auto e = parseOr();
        if (e.is_err()) return e;
        skipSpace();
        if (pos_ < text_.size()) {
            return Err(MatcherError{MatchError::DslSyntax, pos_});
        }
        return e;
    }

private:
    std::string_view text_;
    ca::usize pos_ = 0;

    void skipSpace()
    {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool match(char c)
    {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool matchString(std::string_view s)
    {
        skipSpace();
        if (pos_ + s.size() <= text_.size() && text_.substr(pos_, s.size()) == s) {
            pos_ += s.size();
            return true;
        }
        return false;
    }

    std::string_view readIdentifier()
    {
        skipSpace();
        if (pos_ >= text_.size()) return {};
        const unsigned char first = static_cast<unsigned char>(text_[pos_]);
        if (std::isalpha(first) == 0 && text_[pos_] != '_') return {};
        const ca::usize start = pos_;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (std::isalnum(c) == 0 && text_[pos_] != '_') break;
            ++pos_;
        }
        return text_.substr(start, pos_ - start);
    }

    std::string_view readNumber()
    {
        skipSpace();
        const ca::usize start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '+') ++pos_;
        while (pos_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
        const ca::usize digits = pos_ - start - ((start < text_.size() && text_[start] == '+') ? 1 : 0);
        if (digits == 0) {
            pos_ = start;
            return {};
        }
        return text_.substr(start, pos_ - start);
    }

    Result<ExprPtr, MatcherError> parseOr()
    {
        auto l = parseXor();
        if (l.is_err()) return l;
        ExprPtr left = l.unwrap();
        while (match('|')) {
            auto r = parseXor();
            if (r.is_err()) return r;
            left = makeExpr(ExprNode::Binary{"|", left, r.unwrap()});
        }
        return Ok(left);
    }

    Result<ExprPtr, MatcherError> parseXor()
    {
        auto l = parseAnd();
        if (l.is_err()) return l;
        ExprPtr left = l.unwrap();
        while (match('^')) {
            auto r = parseAnd();
            if (r.is_err()) return r;
            left = makeExpr(ExprNode::Binary{"^", left, r.unwrap()});
        }
        return Ok(left);
    }

    Result<ExprPtr, MatcherError> parseAnd()
    {
        auto l = parseShift();
        if (l.is_err()) return l;
        ExprPtr left = l.unwrap();
        while (match('&')) {
            auto r = parseShift();
            if (r.is_err()) return r;
            left = makeExpr(ExprNode::Binary{"&", left, r.unwrap()});
        }
        return Ok(left);
    }

    Result<ExprPtr, MatcherError> parseShift()
    {
        auto l = parseAddSub();
        if (l.is_err()) return l;
        ExprPtr left = l.unwrap();
        while (true) {
            std::string op;
            if (matchString(">>>")) {
                op = ">>>";
            } else if (matchString("<<")) {
                op = "<<";
            } else if (matchString(">>")) {
                op = ">>";
            } else {
                break;
            }
            auto r = parseAddSub();
            if (r.is_err()) return r;
            left = makeExpr(ExprNode::Binary{op, left, r.unwrap()});
        }
        return Ok(left);
    }

    Result<ExprPtr, MatcherError> parseAddSub()
    {
        auto l = parseMulDiv();
        if (l.is_err()) return l;
        ExprPtr left = l.unwrap();
        while (true) {
            std::string op;
            if (match('+')) {
                op = "+";
            } else if (match('-')) {
                op = "-";
            } else {
                break;
            }
            auto r = parseMulDiv();
            if (r.is_err()) return r;
            left = makeExpr(ExprNode::Binary{op, left, r.unwrap()});
        }
        return Ok(left);
    }

    Result<ExprPtr, MatcherError> parseMulDiv()
    {
        auto l = parseUnary();
        if (l.is_err()) return l;
        ExprPtr left = l.unwrap();
        while (true) {
            std::string op;
            if (match('*')) {
                op = "*";
            } else if (match('/')) {
                op = "/";
            } else if (match('%')) {
                op = "%";
            } else {
                break;
            }
            auto r = parseUnary();
            if (r.is_err()) return r;
            left = makeExpr(ExprNode::Binary{op, left, r.unwrap()});
        }
        return Ok(left);
    }

    Result<ExprPtr, MatcherError> parseUnary()
    {
        skipSpace();
        if (match('-')) {
            auto inner = parseUnary();
            if (inner.is_err()) return inner;
            return Ok(makeExpr(ExprNode::Unary{"-", inner.unwrap()}));
        }
        return parsePrimary();
    }

    Result<ExprPtr, MatcherError> parsePrimary()
    {
        skipSpace();
        if (match('(')) {
            auto inner = parseOr();
            if (inner.is_err()) return inner;
            if (!match(')')) return Err(MatcherError{MatchError::DslSyntax, pos_});
            return inner;
        }
        const std::string_view ident = readIdentifier();
        if (!ident.empty()) {
            skipSpace();
            if (match('(')) {
                auto inner = parseOr();
                if (inner.is_err()) return inner;
                if (!match(')')) return Err(MatcherError{MatchError::DslSyntax, pos_});
                return Ok(makeExpr(ExprNode::Cast{std::string(ident), inner.unwrap()}));
            }
            return Ok(makeExpr(ExprNode::VarRef{std::string(ident)}));
        }
        const std::string_view num = readNumber();
        if (!num.empty()) {
            auto v = parseI64(num, pos_);
            if (v.is_err()) return Err(v.unwrap_err());
            return Ok(makeExpr(ExprNode::IntLit{v.unwrap()}));
        }
        return Err(MatcherError{MatchError::DslSyntax, pos_});
    }
};

Result<PatternStep, MatcherError> parsePatternStep(std::string_view raw)
{
    const std::string_view t = trim(raw);
    const std::string text(t);

    const ca::usize space = t.find_first_of(" \t\r\n");
    const std::string tokenLower = lowerAscii(space == std::string_view::npos
                                                   ? t
                                                   : t.substr(0, space));
    if (isMethodOpcodeToken(tokenLower)) {
        // invoke 族模式：owner name desc 三段或 owner.name 点式（desc 空）。
        const std::string_view rest = space == std::string_view::npos
                                           ? std::string_view{}
                                           : trim(t.substr(space));
        std::string owner, name, desc;
        std::string trimmedRest(rest);
        ca::usize i = 0;
        const auto nextToken = [&](std::string& out) {
            while (i < trimmedRest.size() &&
                   std::isspace(static_cast<unsigned char>(trimmedRest[i])) != 0) {
                ++i;
            }
            const ca::usize start = i;
            while (i < trimmedRest.size() &&
                   std::isspace(static_cast<unsigned char>(trimmedRest[i])) == 0) {
                ++i;
            }
            out = trimmedRest.substr(start, i - start);
            return start != i;
        };
        if (nextToken(owner) && nextToken(name) && nextToken(desc)) {
            return Ok(PatternStep(PatternStep::Method{tokenLower, owner, name, desc}));
        }
        const ca::usize dot = trimmedRest.rfind('.');
        if (dot != std::string::npos) {
            return Ok(PatternStep(PatternStep::Method{
                tokenLower, trimmedRest.substr(0, dot), trimmedRest.substr(dot + 1), ""}));
        }
        return Err(MatcherError{MatchError::DslSyntax, 0});
    }
    if (t.size() > 5 && t.substr(0, 5) == "load(" && t.back() == ')') {
        const std::string_view inside = t.substr(5, t.size() - 6);
        const ca::usize colon = inside.find(':');
        std::string typeTag(inside.substr(0, colon));
        std::string bind;
        if (colon != std::string_view::npos) bind = std::string(inside.substr(colon + 1));
        typeTag = std::string(trim(typeTag));
        bind = std::string(trim(bind));
        ca::i64 constVal = 0;
        bool hasConst = false;
        const ca::usize eq = typeTag.find('=');
        if (eq != std::string::npos) {
            const std::string constStr =
                std::string(trim(std::string_view(typeTag).substr(eq + 1)));
            typeTag = std::string(trim(std::string_view(typeTag).substr(0, eq)));
            auto v = parseI64(constStr, 0);
            if (v.is_err()) return Err(v.unwrap_err());
            constVal = v.unwrap();
            hasConst = true;
        }
        return Ok(PatternStep(
            PatternStep::ConstLoad{typeTag, constVal, hasConst, bind}));
    }
    if (t.size() > 6 && t.substr(0, 6) == "vload(" && t.back() == ')') {
        const std::string_view inside = t.substr(6, t.size() - 7);
        const ca::usize colon = inside.find(':');
        if (colon == std::string_view::npos) {
            return Err(MatcherError{MatchError::DslSyntax, 0});
        }
        return Ok(PatternStep(PatternStep::VarLoad{
            std::string(trim(inside.substr(0, colon))),
            std::string(trim(inside.substr(colon + 1)))}));
    }
    if (t.size() > 7 && t.substr(0, 7) == "vstore(" && t.back() == ')') {
        const std::string_view inside = t.substr(7, t.size() - 8);
        const ca::usize colon = inside.find(':');
        if (colon == std::string_view::npos) {
            return Err(MatcherError{MatchError::DslSyntax, 0});
        }
        return Ok(PatternStep(PatternStep::VarStore{
            std::string(trim(inside.substr(0, colon))),
            std::string(trim(inside.substr(colon + 1)))}));
    }
    return Ok(PatternStep(PatternStep::Opcode{text}));
}

Result<ReplaceStep, MatcherError> parseReplaceStep(std::string_view raw)
{
    const std::string_view t = trim(raw);
    const std::string text(t);

    if (t == "nothing") return Ok(ReplaceStep(ReplaceStep::Nothing{}));
    if (t.size() > 7 && t.substr(0, 7) == "origin[") {
        const ca::usize close = t.find(']');
        if (close == std::string_view::npos) {
            return Err(MatcherError{MatchError::DslSyntax, 0});
        }
        const std::string inside(t.substr(7, close - 7));
        const ca::usize dotdot = inside.find("..");
        if (dotdot != std::string::npos) {
            const auto s = parseI64(std::string_view(inside).substr(0, dotdot), 0);
            const auto e = parseI64(std::string_view(inside).substr(dotdot + 2), 0);
            if (s.is_err() || e.is_err()) return Err(MatcherError{MatchError::DslSyntax, 0});
            return Ok(ReplaceStep(ReplaceStep::OriginRange{
                static_cast<int>(s.unwrap()), static_cast<int>(e.unwrap()) + 1}));
        }
        const auto idx = parseI64(inside, 0);
        if (idx.is_err()) return Err(MatcherError{MatchError::DslSyntax, 0});
        return Ok(ReplaceStep(ReplaceStep::OriginRange{
            static_cast<int>(idx.unwrap()), static_cast<int>(idx.unwrap()) + 1}));
    }
    if (t.size() > 5 && t.substr(0, 5) == "load(" && t.back() == ')') {
        const std::string_view inside = t.substr(5, t.size() - 6);
        const ca::usize colon = inside.find(':');
        if (colon == std::string_view::npos) {
            return Err(MatcherError{MatchError::DslSyntax, 0});
        }
        const std::string typeTag(inside.substr(0, colon));
        ExprParser parser(inside.substr(colon + 1));
        auto expr = parser.parse();
        if (expr.is_err()) return Err(expr.unwrap_err());
        return Ok(ReplaceStep::makeLoadExpr(std::string(trim(typeTag)), expr.unwrap()));
    }

    const ca::usize space = t.find_first_of(" \t\r\n");
    const std::string tokenLower =
        lowerAscii(space == std::string_view::npos ? t : t.substr(0, space));
    if (isMethodOpcodeToken(tokenLower)) {
        const std::string_view rest = space == std::string_view::npos
                                           ? std::string_view{}
                                           : trim(t.substr(space));
        std::string owner, name, desc;
        std::string trimmedRest(rest);
        ca::usize i = 0;
        const auto nextToken = [&](std::string& out) {
            while (i < trimmedRest.size() &&
                   std::isspace(static_cast<unsigned char>(trimmedRest[i])) != 0) {
                ++i;
            }
            const ca::usize start = i;
            while (i < trimmedRest.size() &&
                   std::isspace(static_cast<unsigned char>(trimmedRest[i])) == 0) {
                ++i;
            }
            out = trimmedRest.substr(start, i - start);
            return start != i;
        };
        if (nextToken(owner) && nextToken(name) && nextToken(desc)) {
            return Ok(ReplaceStep(ReplaceStep::Method{tokenLower, owner, name, desc}));
        }
        return Err(MatcherError{MatchError::DslSyntax, 0});
    }
    return Ok(ReplaceStep(ReplaceStep::Opcode{text}));
}

}   // namespace

Result<Rule, MatcherError> MiniDslParser::parseRule(std::string_view dsl)
{
    const std::string text(dsl);
    const ca::usize arrow = text.find("=>");
    if (arrow == std::string::npos) {
        return Err(MatcherError{MatchError::DslSyntax, 0});
    }
    Rule rule;
    {
        auto steps = splitSteps(std::string_view(text).substr(0, arrow));
        if (steps.is_err()) return Err(steps.unwrap_err());
        for (const std::string& s : steps.unwrap()) {
            auto ps = parsePatternStep(s);
            if (ps.is_err()) return Err(ps.unwrap_err());
            rule.matchSteps.push_back(std::move(ps).unwrap());
        }
    }
    {
        auto steps = splitSteps(std::string_view(text).substr(arrow + 2));
        if (steps.is_err()) return Err(steps.unwrap_err());
        for (const std::string& s : steps.unwrap()) {
            auto rs = parseReplaceStep(s);
            if (rs.is_err()) return Err(rs.unwrap_err());
            rule.replaceSteps.push_back(std::move(rs).unwrap());
        }
    }
    if (rule.matchSteps.empty()) {
        return Err(MatcherError{MatchError::DslSyntax, arrow});
    }
    return Ok(std::move(rule));
}

Result<std::vector<Rule>, MatcherError> MiniDslParser::parseRules(std::string_view input)
{
    const std::string text(input);
    std::vector<Rule> rules;
    std::string block;
    const auto flush = [&]() -> Result<void, MatcherError> {
        const std::string_view b = trim(block);
        if (b.empty()) return Ok();
        auto r = parseRule(b);
        if (r.is_err()) return Err(r.unwrap_err());
        rules.push_back(std::move(r).unwrap());
        block.clear();
        return Ok();
    };

    ca::usize begin = 0;
    while (begin <= text.size()) {
        ca::usize end = text.find('\n', begin);
        const bool last = end == std::string::npos;
        if (last) end = text.size();
        const std::string_view line = trim(std::string_view(text).substr(begin, end - begin));
        const bool separator = line.size() >= 3 &&
                               line.find_first_not_of('-') == std::string_view::npos;
        if (separator) {
            auto f = flush();
            if (f.is_err()) return Err(f.unwrap_err());
        } else {
            block.append(std::string_view(text).substr(begin, end - begin));
            block.push_back('\n');
        }
        if (last) break;
        begin = end + 1;
    }
    auto f = flush();
    if (f.is_err()) return Err(f.unwrap_err());
    return Ok(std::move(rules));
}

}   // namespace mjbase::asm_matcher::dsl
