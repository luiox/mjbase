/// @file        match_step.cpp
/// @brief       MatchStep 组合子求值。
#include <utility>

#include "match_step.hpp"

namespace mjbase::asm_matcher {

MatchStep MatchStep::op(jbc::core::Op opcode)
{
    MatchStep s;
    s.kind_ = Kind::Op;
    s.op_ = opcode;
    return s;
}

MatchStep MatchStep::opIf(jbc::core::Op opcode, InsnPredicate pred)
{
    MatchStep s;
    s.kind_ = Kind::Op;
    s.op_ = opcode;
    s.pred_ = std::move(pred);
    return s;
}

MatchStep MatchStep::any()
{
    MatchStep s;
    s.kind_ = Kind::Any;
    return s;
}

MatchStep MatchStep::anyIf(InsnPredicate pred)
{
    MatchStep s;
    s.kind_ = Kind::Any;
    s.pred_ = std::move(pred);
    return s;
}

MatchStep MatchStep::anyMulti()
{
    MatchStep s;
    s.kind_ = Kind::AnyMulti;
    return s;
}

MatchStep MatchStep::allOf(std::vector<MatchStep> steps)
{
    MatchStep s;
    s.kind_ = Kind::All;
    s.children_ = std::move(steps);
    return s;
}

MatchStep MatchStep::anyOf(std::vector<MatchStep> steps)
{
    MatchStep s;
    s.kind_ = Kind::Or;
    s.children_ = std::move(steps);
    return s;
}

MatchStep MatchStep::notOf(MatchStep inner)
{
    MatchStep s;
    s.kind_ = Kind::Not;
    s.children_.push_back(std::move(inner));
    return s;
}

MatchStep MatchStep::captureAs(std::string_view name) &&
{
    capture_ = name;
    return std::move(*this);
}

bool MatchStep::tryMatch(const jbc::ir::Insn& insn, ca::usize remaining) const
{
    switch (kind_) {
    case Kind::Op:
        matchedSize_ = 1;
        if (insn.op != op_) return false;
        return !pred_ || pred_(insn);
    case Kind::Any:
        matchedSize_ = 1;
        return !pred_ || pred_(insn);
    case Kind::AnyMulti:
        // 贪婪吞掉全部剩余指令；零条剩余不算命中（与旧 AnyMultiStep 一致，
        // 正常调用路径下当前位置必有指令，此分支只是防御）。
        matchedSize_ = remaining;
        return remaining > 0;
    case Kind::All:
        matchedSize_ = 1;
        for (const MatchStep& child : children_) {
            if (!child.tryMatch(insn, remaining)) return false;
        }
        return true;
    case Kind::Or:
        matchedSize_ = 1;
        for (const MatchStep& child : children_) {
            if (child.tryMatch(insn, remaining)) return true;
        }
        return false;
    case Kind::Not:
        matchedSize_ = 1;
        return !children_.empty() && !children_.front().tryMatch(insn, remaining);
    }
    return false;
}

}   // namespace mjbase::asm_matcher
