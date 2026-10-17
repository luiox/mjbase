/// @file        match_step.hpp
/// @brief       匹配 Step 抽象 + 叶子谓词的输入视图。
///
/// 核心契约（语义对齐旧实现，容器换成 CodeDom）：
///   - `match` 返回候选后继状态列表，空列表表示失败；
///     多候选支撑量词的回溯降级（贪婪/懒惰即候选排列顺序）。
///   - 状态不可变（MatchState），每个分支返回独立快照。
///   - 匹配是只读的：CodeDom 以 const 引用传入，匹配器无变更能力。
///   - 候选排列顺序即优先级：贪婪组合子按游标降序，懒惰按升序；
///     Longest 偏好的最终裁决在引擎层（findWinner）完成。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/code_dom.hpp"

#include "match_state.hpp"
#include "match_tracer.hpp"

namespace mjbase::asm_tmatcher {

/// @brief Step 句柄（唯一所有权）。
using StepPtr = std::unique_ptr<class IMatchStep>;

/// @brief 叶子谓词的输入视图：当前游标处的指令 + 所在 DOM。
///
/// 谓词需要 DOM 做两类只读查询：`resolveLabel`（分支目标解析）与
/// `peek`（按捕获句柄读其它指令的操作数，做跨指令条件）。
struct InsnCursorView
{
    const jbc::ir::CodeDom* dom = nullptr;
    ca::i32                 index = -1;              ///< order() 下标
    const jbc::ir::Insn*    insn = nullptr;          ///< peek 结果；游标越界为 nullptr

    /// @brief 视图是否可用（非空且在界内）。
    bool valid() const noexcept { return dom != nullptr && insn != nullptr; }
};

/// @brief 通用叶子谓词：接收当前指令视图与当前捕获表。
using InsnPredicate = std::function<bool(const InsnCursorView&, const CaptureStore&)>;

/// @brief 分支目标谓词：接收 resolveLabel 的解析结果。
using LabelTargetPredicate = std::function<bool(const jbc::ir::LabelTarget&)>;

/// @brief 指令匹配 Step 抽象基类。
class IMatchStep
{
public:
    virtual ~IMatchStep() = default;

    /// @brief 在 dom 的 order() 序列上从 state.cursor 开始尝试匹配。
    /// @return 按本 step 优先级排列的候选后继状态；空向量表示失败。
    virtual std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                          const MatchState&       state,
                                          MatchTracer&            tracer,
                                          ca::i32                 depth) = 0;

    /// @brief 调试标识，例如 "opcode(IADD)" / "seq" / "repeat(min=1,max=3)"。
    virtual std::string name() const = 0;
};

}   // namespace mjbase::asm_tmatcher
