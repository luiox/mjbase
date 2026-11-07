#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 重复 catch 表项合并器（Recaf DuplicateCatchMergingTransformer 的
///        表项面吸收）：异常表中「表序相邻 + 同 handler 锚点 + 同 catch 类型
///        面 + 区间无间隙」的两表项合并为一条（保护区取并集），只动异常表、
///        零指令改写。
///
/// 判据（逐对独立，fail-closed：任一门不过该对保留）：
/// 1. 表序相邻：两表项在异常表数组中下标恰为 i, i+1（中间无第三方表项——
///    派发按表序取首个命中，中间表项会在并集区间内抢先命中，合并不保真）。
/// 2. 双项有效：start/handler 标签可锚定到指令（TryCatchCleanup 同口径；
///    end 悬空 = 方法末语义，取指令数），且各自区间非空。
/// 3. 同 handler 面：两表项 handler 标签解析到同一指令（不同 LabelId 锚在
///    同一指令仍是同一 handler）。同锚即同体——handler 体内容无需比对。
/// 4. 同 catch 类型面：双方 catchCp 同为 0（any），或均解析为 Class 条目且
///    内名逐字相等；catchCp 畸形（越界/非 Class）的表项永不参并。
///    任务口径中的「异常类型并联合面」在单 catchCp 编码下只有 any 可表达：
///    异型对改写 any 会把区间内其他异常类型也引入 handler（行为面扩大），
///    fail-closed 拒绝。
/// 5. 区间无间隙：min(end_i, end_j) >= max(start_i, start_j)（相切/相交/
///    包含均并；相离有间隙不并——并集会新保护间隙内的指令，改派发行为）。
///
/// 落笔：仅改写保留项的 start/end 标签（各取位置更小/更大一方的标签，同位
/// 取前者）并删除被并表项；handler/catchCp 与保留项的表序位不动。合并后区
/// 间恰为两表项保护区的并集。
///
/// 字节等价性论证：零指令改写，标签锚点与全部指令 pc 稳定 ⇒ handler 入口
/// pc 不变、StackMapTable 帧表不动；派发等价按对证明——并集区间内任一异常
/// 点 p：改写前表序扫到 i 或 j（同类型面命中）派发至 handler H，改写后合并
/// 项占据 i 的表序位、同类型面命中，同样派发至 H；其余表项相对表序不变，
/// 对 p 先于/后于命中的关系逐点保持。唯一字节差异是 exception_table 收缩。
///
/// 与 Recaf 原件的偏差（逐条申报）：
/// 1. Recaf 合并的是「不同 handler、handler 体内容一致」的对——修剪前者
///    体 + GOTO 重定向 + 帧重算（指令面改写，被剪块之后的指令 pc 全体后
///    移）。本件收窄为「同 handler 锚点」的表项合并：零指令改写、帧表不
///    动，不依赖帧链接线（typeResolver 未接线时同样可用）；体去重面（需
///    指令改写 + 帧重算）不在本件。
/// 2. Recaf 比对体内容时忽略 catch 类型；本件要求同类型面（表项合并的派
///    发保真前提，见判据 4）。
/// 3. Recaf 的 min-block-threshold（默认 4，跳过过小 handler）无对应物——
///    表项合并后不存在残留的重复体，无体量门槛可言。
///
/// 发生合并返回 none()，否则 all()。幂等：合并至不动点后二跑零改写。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class DuplicateCatchMerging : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
