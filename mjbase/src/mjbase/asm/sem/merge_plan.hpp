/// @file        merge_plan.hpp
/// @brief       MergePlan：outline$ 合成方法的 inline/merge 对偶计划。
///
/// 变换面：inline 是 outline 的逆——识别父方法里 `invokestatic N`（N =
/// `outline$` 前缀 + `ACC_PRIVATE|ACC_STATIC|ACC_SYNTHETIC`）调用点，把 N
/// 的方法体搬回调用点：参数装载消解为原有栈形/局部（调用点装载块与 N 体
/// 栈参序幕互为对偶剥离），返回值接回栈重排的逆过程（前缀 + 恰 1 新栈顶
/// 形态下返回值天然落顶；retIsEntryCopy 时剥离调用点复位 DUP，区域尾 DUP
/// 随体回迁），N 随 apply 删除，N 的 LNT/LVT 行按 outline 侧迁移表的逆
/// 操作迁回父方法。
///
/// 识别口径（fail-closed，沿用 OutlineError）：
///   - N 形态：名字 `outline$` 前缀、访问位恰为 private|static|synthetic、
///     有方法体、异常表为空（outline 产物恒无）、方法级属性仅 Code、嵌套
///     属性仅 LNT/LVT/LVTT/SMT/盖章（kOutlineStampAttr）、体直线（返回
///     指令以外零控制转移）且末条返回指令与描述符返回类型同族。
///   - 单调用点：N 在类内代码域的引用数恰为 1（多调用点/自引用拒绝；
///     反射等非代码引用面不在扫描域）。
///   - 调用点形态：调用点无锚定标签之外的分支面（标签护栏回溯装载链）；
///     调用前栈深 ≥ 参数值数、逐参数类型与描述符同族；装载链 = 调用点
///     紧邻的局部 load 直线（标签锚定的链尾可收、不可越）；N 体头部
///     序幕装载覆盖栈参槽位排布（k ≤ 序幕长）。
///   - retIsEntryCopy 判定（入口栈顶复制形态的对偶）：调用点紧邻复位
///     DUP/DUP2 且 N 体去序幕后尾条为 DUP/DUP2 且首参数与返回值同族。
///     outline 侧该形态要求入口栈整栈成为公共前缀（prefix == dIn），
///     保证本口径不误吞「真切片参数 + 体尾 DUP」的外来同构形态（该形态
///     outline 判据 RetNotProduced 拒绝，不会成为本设施产物）。
///
/// 槽逆映射口径：参数槽（N 槽 → 父槽）取调用点装载链的装载槽（栈参边界
/// 在「装载链 ⊆ 参数值」内可自由滑动——链上原代码装载与 N 体序幕装载互为
/// 对偶，滑动不改变合并结果）；区域内自足槽（先写后读、恒被重写——不进
/// 参数集的槽）消费 N 的 Code 嵌套盖章属性（kOutlineStampAttr，RawAttr
/// 承载 (nSlot, parentSlot) 对）exact 恢复原槽位；无盖章（外来 inline
/// 形态）回落父方法 maxLocals 起 fresh 分配（cat2 占 2 槽），原槽号
/// 无从恢复。
///
/// 红线：plan 值语义、自持有；InsnId/LabelId 句柄为引用口径
/// ——构建与 apply 之间会话不得发生结构变更，apply 前做句柄探测校验。
/// apply 中途失败不回滚（同 OutlinePlan）。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/sem/outline_plan.hpp"

namespace mjbase::asm_sem {

/// @brief N 槽 → 父槽逆映射行（参数槽精确恢复；自足槽盖章 exact / 无章
///        fresh）。
struct SlotBack
{
    ca::u32 nSlot      = 0;   ///< 合成方法内槽号
    ca::u32 parentSlot = 0;   ///< 父方法槽号（参数槽 = 装载链装载槽；自足槽 =
                              ///  盖章档原槽 exact 恢复，无章回落
                              ///  maxLocals 起 fresh）
    bool    selfSufficient = false;   ///< true = 非装载链恢复的区域内自足槽
                                      ///  （盖章 exact 或 fresh 分配）
};

/// @brief inline 落地产物（v1 零字段——产物以会话状态交付：N 已删除、
///        调用点已替换为回迁体；往返差异诊断字段暂未建模）。
struct InlineOutcome
{
};

/// @brief outline$ 合成方法 inline 计划（纯数据 + 校验/落地分离）。
///
/// 计划只存数据、可校验；识别在 planInline（消费 FrameView 完成调用点
/// 形态判定），落地在 apply（走 libjbc 会话，错误 Result 值语义）。
class MergePlan
{
public:
    /// @brief 计划内部一致性校验（数据侧）：回迁体非空、零控制转移、
    ///        体局部操作数全部落在槽逆映射内。判据级语义检查在 planInline
    ///        识别侧完成，本函数不触碰会话。
    ca::core::Result<void, OutlineError> validate() const;

    /// @brief 落地：调用点替换（回迁体插入 + 装载链/复位 DUP/调用点删除）
    ///        + 父方法 lower/Strict 帧/maxes/SMT 再生 + N 的 LNT/LVT 行
    ///        回迁 + 删除 N。
    ///
    /// @param resolver Strict 引用合并策略（父方法帧重算/SMT 再生用）。
    /// 会话版本 < 50（无 SMT 定义）跳过再生。行回迁锚定回迁体窗口
    /// （LNT 行号原样；LVT 槽按 slotBacks 逆映射、区间覆盖回迁体）。
    ca::core::Result<InlineOutcome, OutlineError>
    apply(jbc::ir::ClassSession& session, jbc::analysis::ITypeResolver& resolver) const;

    // ---- 计划数据（planInline 填装）----

    jbc::ir::MethodId parent{};    ///< 消费方（调用点所在方法）
    jbc::ir::MethodId callee{};    ///< 合成方法 N（apply 末删除）
    jbc::ir::InsnId   callInsn{};  ///< 父方法调用点（invokestatic N）

    /// 调用点锚标签：strip 首条（复位 DUP 或首条装载）或空 strip 时的
    /// 调用点所锚标签（无则无效句柄 = fallthrough 形态）。回迁体插入点。
    jbc::ir::LabelId anchorLabel{};

    bool            retIsEntryCopy = false;   ///< 入口栈顶复制形态的对偶（见文件头注）
    jbc::ir::InsnId entryDup{};    ///< 调用点复位 DUP/DUP2（retIsEntryCopy 时有效）
    std::vector<jbc::ir::InsnId> stripLoads;   ///< 消解的局部入参装载（程序序）

    /// N 体去序幕/去返回后的区域体（程序序；局部操作数已按 slotBacks 逆
    /// 映射改写为父槽显式形态；retIsEntryCopy 时区域尾 DUP 保留——原区域
    /// 产出形态）。apply 逐条插回调用点。
    std::vector<jbc::ir::Insn> body;

    std::vector<SlotBack> slotBacks;   ///< N 槽 → 父槽逆映射（参数 + 自足）
    ca::u32   leadingStrip = 0;        ///< N 体头部剥离的栈参序幕装载数（k）
    ca::u32   freshBase    = 0;        ///< 自足槽 fresh 分配基（父 maxLocals）

    /// N 的 LNT 行回收（OutlinePlan::lntMoves 的逆；bodyIndex 语义 = 回迁体
    /// R 内序——apply 按 rIds 重锚 startPc，行号原样）。
    std::vector<LntRowMove> lntBacks;
    /// N 的 LVT/LVTT 行回收（OutlinePlan::lvtMoves 的逆；oldSlot = N 槽、
    /// newSlot = 父槽、bodyIndex = 回迁体 R 内序；区间长度在迁 N 时已被体尾
    /// 覆盖，回迁按回迁体窗口跨度重建——原区间长度不可恢复）。
    std::vector<LvtRowMove> lvtBacks;
};

/// @brief inline 识别请求：消费面 = 父方法 + FrameView。
struct InlineRequest
{
    jbc::ir::ClassSession& session;    ///< 会话（识别零改动）
    jbc::ir::MethodId      parent;     ///< 消费方（须有方法体）
    jbc::analysis::ITypeResolver& resolver;   ///< Strict 引用合并策略
    /// 指定合成方法；无效句柄 = 自动发现（父方法程序序首个
    /// `invokestatic outline$…` 候选）。
    jbc::ir::MethodId      callee{};
};

/// @brief 识别：对父方法的 inline 候选跑全部判据，通过则交付计划
///        （会话零改动）。判据清单见文件头注；失败返回 Err(OutlineError)。
ca::core::Result<MergePlan, OutlineError> planInline(const InlineRequest& req);

}   // namespace mjbase::asm_sem
