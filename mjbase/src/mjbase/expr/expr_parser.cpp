#include "expr_parser.hpp"

#include <cctype>
#include <sstream>
#include <string>

namespace mjbase::expr {

namespace {

// ── Lexer ──

enum class TokenKind
{
    Number,
    Ident,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Amp,
    Pipe,
    Caret,
    Tilde,
    LShift,
    RShift,
    RShiftU,
    LParen,
    RParen,
    End,
    Error
};

struct Token
{
    TokenKind   kind;
    ca::i64     intVal;
    std::string strVal;

    Token()
        : kind(TokenKind::End)
        , intVal(0)
    {}
};

class Lexer
{
public:
    explicit Lexer(const std::string& input)
        : input_(input)
        , pos_(0)
    {
        advance();
    }

    const Token& peek() const { return tok_; }
    Token        consume()
    {
        auto t = tok_;
        advance();
        return t;
    }

    std::string error() const { return err_; }

private:
    std::string input_;
    std::string err_;
    ca::usize   pos_;
    Token       tok_;

    char peekChar() const { return pos_ < input_.size() ? input_[pos_] : '\0'; }
    char advanceChar() { return pos_ < input_.size() ? input_[pos_++] : '\0'; }

    void advance()
    {
        // Skip whitespace
        while (std::isspace(static_cast<unsigned char>(peekChar()))) ++pos_;

        char c = peekChar();
        if (c == '\0') {
            tok_      = Token{};
            tok_.kind = TokenKind::End;
            return;
        }

        // Numbers (decimal or hex)
        if (std::isdigit(static_cast<unsigned char>(c))) {
            // 以 u64 累积：超界字面量按位回绕（与 ExprValue 的回绕语义一致），
            // 避免 i64 有符号溢出 UB。
            ca::u64 val = 0;
            // Check for hex: peekChar() returns input_[pos_] = '0', look at next char
            if (c == '0' && pos_ + 1 < input_.size() &&
                (input_[pos_ + 1] == 'x' || input_[pos_ + 1] == 'X')) {
                ++pos_;   // skip '0'
                ++pos_;   // skip 'x'
                while (true) {
                    c = peekChar();
                    if (c >= '0' && c <= '9') {
                        val = val * 16 + static_cast<ca::u64>(c - '0');
                    }
                    else if (c >= 'a' && c <= 'f') {
                        val = val * 16 + static_cast<ca::u64>(c - 'a' + 10);
                    }
                    else if (c >= 'A' && c <= 'F') {
                        val = val * 16 + static_cast<ca::u64>(c - 'A' + 10);
                    }
                    else
                        break;
                    ++pos_;
                }
            }
            else {
                while (std::isdigit(static_cast<unsigned char>(peekChar()))) {
                    val = val * 10 + static_cast<ca::u64>(peekChar() - '0');
                    ++pos_;
                }
            }
            Token t;
            t.kind   = TokenKind::Number;
            t.intVal = static_cast<ca::i64>(val);
            tok_     = t;
            return;
        }

        // Identifiers (variables)
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::string s;
            while (std::isalnum(static_cast<unsigned char>(peekChar())) || peekChar() == '_') {
                s += advanceChar();
            }
            Token t;
            t.kind   = TokenKind::Ident;
            t.strVal = s;
            tok_     = t;
            return;
        }

        // Multi-char operators
        if (c == '<' && (pos_ + 1 < input_.size() && input_[pos_ + 1] == '<')) {
            pos_ += 2;
            Token t;
            t.kind = TokenKind::LShift;
            tok_   = t;
            return;
        }
        if (c == '>' && (pos_ + 1 < input_.size() && input_[pos_ + 1] == '>')) {
            if (pos_ + 2 < input_.size() && input_[pos_ + 2] == '>') {
                pos_ += 3;
                Token t;
                t.kind = TokenKind::RShiftU;
                tok_   = t;
                return;
            }
            pos_ += 2;
            Token t;
            t.kind = TokenKind::RShift;
            tok_   = t;
            return;
        }

        // Single-char tokens
        ++pos_;
        switch (c) {
        case '+': tok_.kind = TokenKind::Plus; break;
        case '-': tok_.kind = TokenKind::Minus; break;
        case '*': tok_.kind = TokenKind::Star; break;
        case '/': tok_.kind = TokenKind::Slash; break;
        case '%': tok_.kind = TokenKind::Percent; break;
        case '&': tok_.kind = TokenKind::Amp; break;
        case '|': tok_.kind = TokenKind::Pipe; break;
        case '^': tok_.kind = TokenKind::Caret; break;
        case '~': tok_.kind = TokenKind::Tilde; break;
        case '(': tok_.kind = TokenKind::LParen; break;
        case ')': tok_.kind = TokenKind::RParen; break;
        default:
            err_      = std::string("unexpected character: ") + c;
            tok_.kind = TokenKind::Error;
            break;
        }
    }
};

// ── Parser (recursive descent) ──

class Parser
{
public:
    explicit Parser(const std::string& input)
        : lexer_(input)
    {}

    ExprRef parse(std::string* error)
    {
        auto result = expr();
        if (lexer_.peek().kind != TokenKind::End) {
            if (error) *error = "unexpected token after expression";
            return nullptr;
        }
        if (!lexer_.error().empty()) {
            if (error) *error = lexer_.error();
            return nullptr;
        }
        return result;
    }

private:
    Lexer lexer_;

    const Token& peek() const { return lexer_.peek(); }
    Token        consume() { return lexer_.consume(); }

    // Precedence (low to high):
    //   |  ^  &  << >> >>>  + -  * / %  unary

    // expr → xor ('|' xor)*   (bitwise OR — lowest)
    ExprRef expr()
    {
        auto left = xor_();
        if (!left) return nullptr;
        while (peek().kind == TokenKind::Pipe) {
            consume();
            auto right = xor_();
            if (!right) return nullptr;
            left = exprOr(std::move(left), std::move(right));
        }
        return left;
    }

    // xor → and ('^' and)*   (bitwise XOR)
    ExprRef xor_()
    {
        auto left = and_();
        if (!left) return nullptr;
        while (peek().kind == TokenKind::Caret) {
            consume();
            auto right = and_();
            if (!right) return nullptr;
            left = exprXor(std::move(left), std::move(right));
        }
        return left;
    }

    // and → shift ('&' shift)*   (bitwise AND)
    ExprRef and_()
    {
        auto left = shift();
        if (!left) return nullptr;
        while (peek().kind == TokenKind::Amp) {
            consume();
            auto right = shift();
            if (!right) return nullptr;
            left = exprAnd(std::move(left), std::move(right));
        }
        return left;
    }

    // shift → addsub (('<<' | '>>' | '>>>') addsub)*
    ExprRef shift()
    {
        auto left = addsub();
        if (!left) return nullptr;
        while (peek().kind == TokenKind::LShift || peek().kind == TokenKind::RShift ||
               peek().kind == TokenKind::RShiftU) {
            auto op    = consume();
            auto right = addsub();
            if (!right) return nullptr;
            switch (op.kind) {
            case TokenKind::LShift: left = exprShl(std::move(left), std::move(right)); break;
            case TokenKind::RShift: left = exprShr(std::move(left), std::move(right)); break;
            case TokenKind::RShiftU: left = exprUShr(std::move(left), std::move(right)); break;
            default: break;
            }
        }
        return left;
    }

    // addsub → muldiv (('+' | '-') muldiv)*
    ExprRef addsub()
    {
        auto left = muldiv();
        if (!left) return nullptr;
        while (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus) {
            auto op    = consume();
            auto right = muldiv();
            if (!right) return nullptr;
            if (op.kind == TokenKind::Plus)
                left = exprAdd(std::move(left), std::move(right));
            else
                left = exprSub(std::move(left), std::move(right));
        }
        return left;
    }

    // muldiv → unary (('*' | '/' | '%') unary)*
    ExprRef muldiv()
    {
        auto left = unary();
        if (!left) return nullptr;
        while (peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash ||
               peek().kind == TokenKind::Percent) {
            auto op    = consume();
            auto right = unary();
            if (!right) return nullptr;
            switch (op.kind) {
            case TokenKind::Star: left = exprMul(std::move(left), std::move(right)); break;
            case TokenKind::Slash: left = exprDiv(std::move(left), std::move(right)); break;
            case TokenKind::Percent: left = exprRem(std::move(left), std::move(right)); break;
            default: break;
            }
        }
        return left;
    }

    // unary → ('-' | '~') unary | primary
    ExprRef unary()
    {
        if (peek().kind == TokenKind::Minus) {
            consume();
            auto child = unary();
            return child ? exprNeg(std::move(child)) : nullptr;
        }
        if (peek().kind == TokenKind::Tilde) {
            consume();
            auto child = unary();
            return child ? exprNot(std::move(child)) : nullptr;
        }
        return primary();
    }

    // primary → Number | Ident | '(' expr ')'
    ExprRef primary()
    {
        if (peek().kind == TokenKind::Number) {
            auto t = consume();
            return exprConst(t.intVal);
        }
        if (peek().kind == TokenKind::Ident) {
            auto t = consume();
            return exprVar(t.strVal);
        }
        if (peek().kind == TokenKind::LParen) {
            consume();   // '('
            auto inner = expr();
            if (peek().kind != TokenKind::RParen || !inner) {
                return nullptr;
            }
            consume();   // ')'
            return inner;
        }
        return nullptr;
    }
};

}   // anonymous namespace

ExprRef parseExpression(const std::string& input, std::string* errorMsg)
{
    Parser parser(input);
    return parser.parse(errorMsg);
}

}   // namespace mjbase::expr
