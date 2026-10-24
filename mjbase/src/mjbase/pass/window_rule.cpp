#include "mjbase/pass/window_rule.hpp"

#include <stdexcept>

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass::window_rule {

namespace {

using jbc::ir::CodeDom;
using jbc::ir::CodeEditor;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

bool replaceWindowAt(jbc::ir::CodeUnit& code, const std::vector<InsnId>& window,
                     const std::vector<Insn>& replacement)
{
    CodeDom&                    dom = code.dom;
    jbc::ir::CodeEditor         editor(code);
    const std::vector<LabelId>  frontLabels = dom.labelsAt(window.front());

    // 替换序列头插到首成员之前（链式 insertAfter 保序）。
    std::vector<InsnId> inserted;
    inserted.reserve(replacement.size());
    InsnId cursor = jbc::ir::kNoInsn;
    for (const Insn& r : replacement) {
        auto res = (cursor == jbc::ir::kNoInsn) ? editor.insertBefore(window.front(), r)
                                                : editor.insertAfter(cursor, r);
        if (res.is_err()) {
            // 插入失败（句柄无效，实际不可达）：回滚已插片段后放弃。
            for (auto it = inserted.rbegin(); it != inserted.rend(); ++it) {
                (void)editor.erase(*it, CodeEditor::ErasePolicy::kRejectIfUsed);
            }
            return false;
        }
        cursor = res.unwrap();
        inserted.push_back(cursor);
    }

    // srcPc 落位（按调用方意图显式覆写；插入器可能强制合成指令的 pc）。
    for (ca::usize k = 0; k < inserted.size(); ++k) {
        if (auto slot = dom.get(inserted[k]); slot.is_ok()) {
            slot.unwrap()->srcPc = replacement[k].srcPc;
        }
    }

    // 首成员标签迁移：非空替换 → 替换头；空替换 → 窗口后继（无后继时方法
    // 末尾）。不迁移则首成员删除被 Boundary/ControlFlow 标签保护而拒绝。
    InsnId labelTarget = inserted.empty() ? jbc::ir::kNoInsn : inserted.front();
    if (inserted.empty()) {
        const std::vector<InsnId>& order = dom.order();
        for (ca::usize i = 0; i + 1 < order.size(); ++i) {
            if (order[i] == window.back()) labelTarget = order[i + 1];
        }
    }
    for (const LabelId label : frontLabels) {
        (void)editor.bindLabel(label, labelTarget);
    }

    // 顺序删除窗口成员（守卫 + 迁移后均无引用）。
    for (const InsnId member : window) {
        if (editor.erase(member, CodeEditor::ErasePolicy::kRejectIfUsed).is_err()) {
            // 防御面：前置检查下不可达。已发生的改写保留，失败语义归管线容错档位。
            return false;
        }
    }
    return true;
}

/// 在 start 处尝试匹配并应用 rule；完成一次替换返回 true。
bool applyRuleAt(jbc::ir::ClassSession& session, CodeDom& dom, jbc::ir::CodeUnit& code,
                 const WindowRule& rule, const std::vector<InsnId>& ids, ca::usize start)
{
    const ca::usize width = rule.steps.size();

    // ── 结构匹配 ──
    std::vector<const Insn*> matched;
    matched.reserve(width);
    for (ca::usize k = 0; k < width; ++k) {
        const Insn* in = dom.peek(ids[start + k]);
        if (in == nullptr) return false;
        if (!rule.steps[k](session, *in, matched)) return false;
        matched.push_back(in);
    }

    // ── 边界守卫：内部成员前置边界有存活标签锚定（分支目标 / 异常表 / 调试
    //    边界）即跳过——旧栈窗口内 LabelNode/LineNumberNode/FrameNode 阻断的
    //    CodeDom 等价。行号边界不可见（LNT 是 backing 属性）是已声明的保守
    //    偏离，SMT 帧目标悬垂由管线帧链接线显式失败。──
    for (ca::usize k = 1; k < width; ++k) {
        if (!dom.labelsAt(ids[start + k]).empty()) return false;
    }

    // ── 语义策略（放弃 = 本次不匹配，扫描继续） ──
    std::vector<Insn> replacement;
    if (!rule.build(session, matched, replacement)) return false;

    // 替换头继承窗口首成员的来源 pc（见 replaceWindowAt 契约）。
    if (!replacement.empty()) replacement.front().srcPc = matched.front()->srcPc;

    const std::vector<InsnId> window(ids.begin() + static_cast<std::ptrdiff_t>(start),
                                     ids.begin() + static_cast<std::ptrdiff_t>(start + width));
    return replaceWindowAt(code, window, replacement);
}

/// 规则的一次应用：从右界扫到左界，命中即替换并返回。
bool applyRuleRightmost(jbc::ir::ClassSession& session, CodeDom& dom, jbc::ir::CodeUnit& code,
                        const WindowRule& rule)
{
    const ca::usize width = rule.steps.size();
    if (width == 0) return false;
    const std::vector<InsnId> ids = dom.order();
    if (ids.size() < width) return false;
    ca::usize start = ids.size() - width;   // 右界起点
    while (true) {
        if (applyRuleAt(session, dom, code, rule, ids, start)) return true;
        if (start == 0) break;
        --start;
    }
    return false;
}

}   // namespace

bool replaceWindow(jbc::ir::CodeUnit& code, const std::vector<InsnId>& window,
                   const std::vector<Insn>& replacement)
{
    if (window.empty()) return false;
    return replaceWindowAt(code, window, replacement);
}

bool applyWindowRules(mjbase::spi::MethodScope& scope, const std::vector<WindowRule>& rules,
                      ca::usize roundBudget)
{
    jbc::ir::ClassSession& session = scope.session;
    CodeDom&                dom     = scope.code.dom;
    if (dom.order().empty()) return false;

    if (roundBudget == 0) roundBudget = 64 + 32 * dom.size();
    ca::usize rounds = 0;
    bool      changed = false;
    for (;;) {
        bool any = false;
        for (const WindowRule& rule : rules) {
            if (applyRuleRightmost(session, dom, scope.code, rule)) any = true;
        }
        if (!any) break;
        changed = true;
        if (++rounds > roundBudget) {
            throw std::logic_error("window_rule: fixpoint did not converge");
        }
    }
    return changed;
}

}   // namespace mjbase::pass::window_rule
