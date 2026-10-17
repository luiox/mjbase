#pragma once

#include "libca/core/datatype.hpp"
#include "expr_node.hpp"

namespace mjbase::expr {

/// Bit-width aware evaluation value.
/// Internal storage is int64_t; all operations truncate to the bit width.
class ExprValue
{
public:
    explicit ExprValue(ca::i64 val, BitWidth bw = BitWidth::B32) noexcept
        : val_(truncateToBitWidth(val, bw))
        , bw_(bw)
    {}

    ca::i64  rawValue() const noexcept { return val_; }
    BitWidth bitWidth() const noexcept { return bw_; }

    ca::i64 truncated() const noexcept { return truncateToBitWidth(val_, bw_); }

    // Two's complement truncation arithmetic
    friend ExprValue operator+(const ExprValue& a, const ExprValue& b);
    friend ExprValue operator-(const ExprValue& a, const ExprValue& b);
    friend ExprValue operator*(const ExprValue& a, const ExprValue& b);
    friend ExprValue operator/(const ExprValue& a, const ExprValue& b);
    friend ExprValue operator%(const ExprValue& a, const ExprValue& b);
    friend ExprValue operator^(const ExprValue& a, const ExprValue& b);
    friend ExprValue operator&(const ExprValue& a, const ExprValue& b);
    friend ExprValue operator|(const ExprValue& a, const ExprValue& b);

    ExprValue operator-() const noexcept;
    ExprValue operator~() const noexcept;

    // Shift operations
    static ExprValue shl(const ExprValue& a, const ExprValue& b);
    static ExprValue shr(const ExprValue& a, const ExprValue& b);
    static ExprValue ushr(const ExprValue& a, const ExprValue& b);

    // Comparison
    friend bool operator==(const ExprValue& a, const ExprValue& b);
    friend bool operator!=(const ExprValue& a, const ExprValue& b);

    /// Wider of two bit widths (public for eval usage)
    static BitWidth wider(BitWidth a, BitWidth b) noexcept
    {
        return static_cast<BitWidth>(static_cast<ca::u8>(a) > static_cast<ca::u8>(b) ? a : b);
    }

private:
    ca::i64  val_;
    BitWidth bw_;

    /// Construct WITHOUT sign extension — for shift ops where unsigned semantics apply
    struct UnsignedTag
    {};
    ExprValue(ca::i64 val, BitWidth bw, UnsignedTag) noexcept
        : val_(static_cast<ca::i64>(static_cast<ca::u64>(val) &
                                    static_cast<ca::u64>(bitWidthMask(bw))))
        , bw_(bw)
    {}

    /// Apply operation and truncate to resultant bit width
    template<typename Fn>
    static ExprValue apply(const ExprValue& a, const ExprValue& b, Fn fn)
    {
        BitWidth rw = wider(a.bw_, b.bw_);
        return ExprValue(fn(a.val_, b.val_), rw);
    }
};

}   // namespace mjbase::expr
