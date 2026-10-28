#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief store/load 中转对消除器：store 的定义使用边恰为紧邻 reload 时，
///        整对删除，值改由栈保持（旧栈 mbase pass/opt/
///        label_dead_var_store_eliminator 的中转面按 def-use 载体重推导）。
///
/// 适配形态：混淆器在计算与消费之间插入 `istore s; iload s` 中转对打断
/// 模式匹配。对是恒栈中性的（store 弹多少 load 推回多少），删除后值留在
/// 栈上；槽 s 不再被写入，但单使用证明保证写值无任何其他观察点。
///
/// 载体：analyzeDefUse 到达定义（签名播种入参）。判据 = store 定义的
/// 使用边集合恰为一条边、且该边指向紧邻的 reload。handler 读点（异常边
/// 精确合并）、iinc 读改写、后续任何读都会产生额外使用边 → 保守保留。
///
/// 相对旧实现偏离（逐条申报）：
/// 1. 载体换 def-use 单使用证明。旧栈按标签段窗口 + 指令白名单（const/
///    arith/stack/var）近似「无其他观察点」，窗口 bookend（首 iload/尾
///    istore 同槽）是载体噪声非语义必要；新栈不设窗口，跨段形态同样可判。
/// 2. 旧栈窗内多对面不健全，不复刻：删除窗内一对后，窗内后续同槽读点会
///    观察到陈旧槽值（如 `iload v; iconst_1; iadd; istore v; iload v;
///    iload v; iadd; istore v` 删中间对后结果从 2v+2 变 2v+1）。新栈单
///    使用判据天然拒绝该形态（后续读点即额外使用边），「不该删」面单测
///    钉死。
/// 3. 新增边界守卫：对内任一侧锚标签（分支目标/异常表边界）整对拒收——
///    reload 位锚入 = 分支跳入时期望弹值在栈，删对后该路径栈形破坏；
///    store 位锚入涉及边界重锚，对齐全家「不重锚异常表边界」惯例整对
///    保留。fail-closed：任一不确定整对保留。
/// 4. 宽度族扩展：旧栈仅 ILOAD/ISTORE；新栈按五族（i/l/f/d/a）全宽覆盖，
///    对恒栈中性、弹宽一致。
///
/// 不动点：删除暴露出的新相邻对（`istore a; istore b; iload b; iload a`
/// 删 b 对后 a 对成形）下一轮继续消。
///
/// 处置：整对删除（CodeEditor fallthrough 重定向），不降级不重写。
/// maxStack/maxLocals 不变（删除只降不升）。
///
/// 安全门：帧数据流失败或到达集合溢出整件跳过（单使用是负向结论）。
///
/// 发生删除返回 none()，否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class StoreLoadRelayEliminator : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&            scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
