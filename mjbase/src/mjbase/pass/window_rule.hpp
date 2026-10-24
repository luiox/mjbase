#pragma once

#include <functional>
#include <vector>

#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/datatype.hpp"

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass::window_rule {

/// @brief 方法级 peephole 窗口规则的共享引擎（C 方法级指令变换组）。
///
/// 承载「窗口匹配 + 原子替换」组合协议的唯一处：逐位匹配谓词、语义替换策略
/// （含放弃语义）、边界守卫、标签迁移、来源 pc 继承、右界优先的不动点轮次。
/// libjbc CodeEditor 提供 insertBefore/erase/bindLabel 单个原语，但「替换一段
/// 指令序列并保持边界语义」的组合没有一等入口——replaceWindow 是该组合的承载；
/// libjbc 侧一等 replaceWindow 原语的缺口见模块地图挂账。
///
/// 语义复刻旧栈 PatternMatcher（mbase/asm/matcher）：窗口内不得有标签/行号/
/// 帧边界（旧栈三者都是指令表成员，天然阻断相邻匹配）；替换拥有新节点或 clone，
/// 绝不复用被删成员；pass 迭代至不动点。

/// 单步匹配谓词：会话（CP 语义解析用）+ 候选指令 + 已匹配的前序成员。
/// 第三个参数服务跨位依赖（如规则 11 的 same-slot 检查要读 ISTORE 的 slot）。
using StepFn =
    std::function<bool(const jbc::ir::ClassSession&, const jbc::ir::Insn&,
                       const std::vector<const jbc::ir::Insn*>&)>;

/// 替换策略：据已匹配成员产出替换序列。
///
/// 返回 false = 放弃本次匹配（旧栈 MatchContext::cancelReplacement 语义：已写入
/// 的候选替换作废，原指令保持不变，扫描继续向更早的位置）。策略可改会话（常量
/// intern），故会话按非 const 引用交付。
///
/// 产出的 Insn 须自带正确的 srcPc：clone 承原值、合成指令给 kNoPc——替换头的
/// 锚由应用层统一覆写为窗口首成员的来源 pc（见 replaceWindow 契约）。
using BuildFn = std::function<bool(jbc::ir::ClassSession&,
                                   const std::vector<const jbc::ir::Insn*>&,
                                   std::vector<jbc::ir::Insn>&)>;

/// 一条窗口规则：逐位匹配谓词 + 替换策略。
struct WindowRule
{
    std::vector<StepFn> steps;
    BuildFn             build;
};

/// @brief 窗口替换原语（CodeEditor 组合层）。
///
/// 契约（复刻旧栈 PatternMatcher 的替换语义）：
///   - 替换序列头插到窗口首成员之前；首成员前置边界的存活标签（分支目标 /
///     异常表 / 调试边界）迁移到替换头——空替换时迁移到窗口后继（无后继则
///     方法末尾）。窗口内部成员不得有锚定标签（调用方前置守卫）。
///   - 替换头的 srcPc 继承窗口首成员：LNT 等 pc 索引属性经 pcmap 重定向时，
///     锚在旧首成员 pc 上的条目落到替换头（旧栈标签留原位指向新序列头）；
///     其余替换指令的 srcPc 按调用方给定值落位（插入器可能强制合成指令的
///     pc，此处显式覆写）。
///   - 窗口成员顺序删除（kRejectIfUsed）。任一删除被拒（引用残留）时已发生
///     的改写保留并返回 false——失败语义归管线容错档位。
bool replaceWindow(jbc::ir::CodeUnit& code, const std::vector<jbc::ir::InsnId>& window,
                   const std::vector<jbc::ir::Insn>& replacement);

/// @brief 规则表驱动到不动点。
///
/// 轮次结构复刻旧栈 applyInternal：一轮内每条规则至多一次替换（各自右界命中），
/// 迭代至无规则可应用。roundBudget 为熔断上限（0 = 64 + 32 × 方法指令数，
/// 合法方法体的轮次量为 O(指令数)，留量级余量）；越界抛 std::logic_error——
/// 把不可预见的发散从挂死转为显式失败（已知自匹配形态须由规则内守卫排除）。
///
/// @return 是否发生过替换（pass 据此决定 PA：changed → none()）。
bool applyWindowRules(mjbase::spi::MethodScope& scope, const std::vector<WindowRule>& rules,
                      ca::usize roundBudget = 0);

}   // namespace mjbase::pass::window_rule
