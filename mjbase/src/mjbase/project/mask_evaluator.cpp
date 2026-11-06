#include "mjbase/project/mask_evaluator.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "mjbase/project/mask_regex_engine.hpp"
#include "mjbase/project/mask_suspect.hpp"

namespace mjbase::project {
namespace {

ca::str::Utf8String asciiMessage(const char* text)
{
    return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(text),
                                                    std::strlen(text));
}

}   // namespace

struct MaskEvaluator::Impl
{
    struct CompiledRule
    {
        MaskRegex    regex;
        MaskRuleSpec spec;
    };

    std::vector<CompiledRule> rules;   ///< (priority, ruleId) 稳定升序
    MaskTrustLevel trust = MaskTrustLevel::Trusted;
};

ca::Result<MaskEvaluator, MaskEvaluatorError> MaskEvaluator::build(
    const std::vector<MaskRuleSpec>& rules, MaskTrustLevel trust)
{
    if (rules.size() > kMaximumMaskRules) {
        MaskEvaluatorError error;
        error.message = asciiMessage("mask rule count exceeds maximum");
        return ca::Err(std::move(error));
    }

    auto impl       = std::make_unique<Impl>();
    impl->trust     = trust;
    impl->rules.reserve(rules.size());
    for (ca::usize index = 0; index < rules.size(); ++index) {
        const MaskRuleSpec& spec = rules[index];
        // pattern 先拷贝进自有存储再编译：入参是借用视图，常来自调用方
        // 临时行表（如 listMaskRules 投影）；悬垂读入是 UB（linux 下 freed
        // 缓冲被分配器涂改成堆指针后"可编译但永不命中"，windows 侥幸存活）。
        // std::regex 在 compile 期自持模式拷贝，编译后不再依赖入参。
        const std::string ownedPattern(spec.pattern.data(), spec.pattern.size());
        auto compiled = MaskRegex::compile(ownedPattern);
        if (compiled.is_err()) {
            MaskEvaluatorError error;
            std::string message = "mask rule[" + std::to_string(index) +
                                  "] failed to compile: " +
                                  std::move(compiled).unwrap_err().message.to_std_string();
            error.message = ca::str::Utf8String::from_data_unchecked(
                reinterpret_cast<const ca::u8*>(message.data()), message.size());
            return ca::Err(std::move(error));
        }
        // CompiledRule 不可默认构造（MaskRegex move-only）：聚合成临时后
        // 移入 vector。
        impl->rules.emplace_back(Impl::CompiledRule{std::move(compiled).unwrap(), spec});    }
    // 先匹配先生效：(priority, ruleId) 稳定升序；同序位保留输入序（稳定
    // 排序），归因取排位 id。
    std::stable_sort(impl->rules.begin(), impl->rules.end(),
                     [](const Impl::CompiledRule& left, const Impl::CompiledRule& right) {
                         if (left.spec.priority != right.spec.priority) {
                             return left.spec.priority < right.spec.priority;
                         }
                         return left.spec.ruleId < right.spec.ruleId;
                     });
    return ca::Ok(MaskEvaluator(std::move(impl)));
}

MaskEvaluator::MaskEvaluator(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

MaskEvaluator::MaskEvaluator(MaskEvaluator&&) noexcept = default;

MaskEvaluator& MaskEvaluator::operator=(MaskEvaluator&&) noexcept = default;

MaskEvaluator::~MaskEvaluator() = default;

MaskDecision MaskEvaluator::evaluate(std::string_view text) const
{
    MaskDecision decision;
    if (impl_ == nullptr) return decision;   ///< move-from 实例：安全直通（Allow）

    const SuspectFlags flags = scanSuspect(text);
    decision.suspect = flags.any();

    const Impl& impl   = *impl_;
    const MaskAction fallback = defaultActionFor(impl.trust);

    // 长度门：超门文本不进任何正则（mask_regex_engine 的长度门纪律），直接 default_action。
    if (text.size() > kMaskMaxEvaluatedBytes) {
        decision.action = fallback;
        decision.via    = MaskDecisionVia::LengthGate;
        return decision;
    }

    bool  allowBlocked     = false;   ///< suspect 已压制过 allow 命中（留痕）
    ca::i64 blockedRuleId  = 0;       ///< 首条被压 allow 的规则 id
    for (const Impl::CompiledRule& rule : impl.rules) {
        if (!rule.regex.matches(text)) continue;
        if (rule.spec.action == MaskAction::Mask) {
            decision.action               = MaskAction::Mask;
            decision.ruleId               = rule.spec.ruleId;
            decision.via                  = MaskDecisionVia::RuleMatch;
            decision.suspectOverrideBlocked = allowBlocked;
            return decision;
        }
        if (!decision.suspect) {
            decision.action = MaskAction::Allow;
            decision.ruleId = rule.spec.ruleId;
            decision.via    = MaskDecisionVia::RuleMatch;
            return decision;
        }
        // suspect 压过 allow：命中作废（记首条留痕），继续匹配后续规则。
        if (!allowBlocked) {
            allowBlocked   = true;
            blockedRuleId  = rule.spec.ruleId;
        }
    }

    if (allowBlocked) {
        decision.action                = fallback;
        decision.ruleId                = blockedRuleId;
        decision.via                   = MaskDecisionVia::SuspectGate;
        decision.suspectOverrideBlocked = true;
        return decision;
    }
    decision.action = fallback;
    decision.via    = MaskDecisionVia::DefaultAction;
    return decision;
}

MaskTrustLevel MaskEvaluator::trust() const noexcept
{
    return impl_ != nullptr ? impl_->trust : MaskTrustLevel::Trusted;
}

ca::usize MaskEvaluator::ruleCount() const noexcept
{
    return impl_ != nullptr ? impl_->rules.size() : 0;
}

ca::str::Utf8String maskedPlaceholderText(const MaskDecision& decision)
{
    if (decision.action != MaskAction::Mask) return ca::str::Utf8String::from_cstr("");
    if (decision.via == MaskDecisionVia::RuleMatch && decision.ruleId > 0) {
        const std::string placeholder = "[masked:" + std::to_string(decision.ruleId) + "]";
        return ca::str::Utf8String::from_data_unchecked(
            reinterpret_cast<const ca::u8*>(placeholder.data()), placeholder.size());
    }
    return ca::str::Utf8String::from_cstr("[masked]");
}

}   // namespace mjbase::project
