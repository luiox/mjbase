#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 无用 pop 清理器：删除「局部装载 + 紧随 POP/POP2」的相邻指令对
///        （旧栈 mbase pass/opt/dead_code_remover 的 UselessPopCleaner 按
///        相邻窗口面迁移的首切片，帧载体形态见文末挂账）。
///
/// 适配形态：值装载后立即丢弃的死装载对——典型如状态槽 store 降级成
/// pop 后暴露的「装载;pop」相邻对、装载填充噪声。
///
/// 匹配矩阵（classfile 可观测行为）：
///   - POP：前驱是 category-1 局部装载（ILOAD/FLOAD/ALOAD 及 _0.._3
///     紧凑形；wide 前缀不改族属）；
///   - POP2：前驱是 category-2 局部装载（LLOAD/DLOAD 及 _0.._3 紧凑形）。
///     常量装载对不入本矩阵——常量装载面已另立清理件，不重复覆盖。
///
/// 消除安全性不依赖局部槽内容：装载恰推 1/2 槽、POP/POP2 恰弹同宽，
/// 配对删除恒栈中性。旧栈用帧溯源认「弹值的生产者」并要求其唯一消费者
/// 是该 pop；本实现按相邻窗口达到同一可观测面——POP 弹栈顶，纯装载与
/// 紧邻 pop 之间若有其他指令必碰栈顶（唯一例外是标签边界，由边界守卫
/// 拒收）。
///
/// 迭代到不动点：前半段删除暴露出的新相邻对在下一轮继续消除（如
/// [ILOAD;ILOAD;POP;POP] 交错形态第二轮清空）。
///
/// 边界守卫：POP 的前置边界有存活标签锚定（分支目标/异常表边界）时跳过
/// 该对。首成员（装载）锚定标签时照常处理：fallthrough 删除把引用重定向
/// 到 POP。行号边界在 CodeDom 不可见（LNT 是 backing 属性），跨行号边界
/// 的相邻对会消除。
///
/// 挂账（旧栈帧载体形态，随帧面批次逐件论证）：非相邻生产者（隔 dup 族
/// 纯搬运后仍被 pop 丢弃）、POP2 双 category-1 的半删降 POP、DUP/DUP2
/// 自身被 pop 吸收、多来源汇合值。
///
/// 发生消除返回 none()，否则 all()。maxStack/maxLocals 不变（消除只降
/// 不升）。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class UselessPopCleaner : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
