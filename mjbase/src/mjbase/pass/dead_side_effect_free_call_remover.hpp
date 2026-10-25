#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 死无副作用调用清理器：白名单无副作用调用的返回值经纯计算链
///        最终被 pop 丢弃时整链删除（旧栈 mbase pass/opt/dead_code_remover
///        的 DeadSideEffectFreeCallRemover 按 def-use 载体重推导）。
///
/// 适配形态：常量噪声控制流展开后遗留的时钟/随机数探针——调用返回值只
/// 喂给注定弹弃的运算链（典型 [INVOKESTATIC System.nanoTime; POP2] 直弃
/// 形态，或中间隔若干纯计算）。
///
/// 白名单（声明表，吸收旧栈 issue #678 与 UnusedResultCallEliminater
/// 两张表；扩展只增条目）：可安全弹弃的调用——
///   - 零参精确条目：System.nanoTime/currentTimeMillis ()J、
///     Math.random ()D；
///   - 带参条目：String.length()/isEmpty()（虚调用，接收者须为链内
///     纯生产者——常量字符串形态删除安全）；
///   - 家族条目：Math/StrictMath 纯函数名集（abs/max/min/三角/指数/
///     对数/取整/位操作等 31 名），全参确定、无副作用、任意输入值
///     （含 NaN/Inf）不抛不写。
/// 白名单调用必须是链成员才触发删除（纯常量链归 UselessPopCleaner 帧面，
/// 不在本件重复）。带参白名单调用的全部输入定义必须入链且输入使用边数
/// 与描述符参数值数对齐（虚调用含接收者；缺失边 = 看不见的来源，不可删）。
/// 相比旧栈 UnusedResultCallEliminater 的「全参常量」要求，链成员资格
/// 覆盖纯计算实参（如常量经一次 IADD）——按纯性论证为安全扩展，文档化
/// 声明。
///
/// 链成员（def-use 定义边闭包）：
///   - 白名单调用（零参条目为叶子；带参条目按上述 arity 守卫）；
///   - 纯计算生产者：常量装载族（int/long/float/double/string + ACONST_
///     NULL）、算术（IADD..LXOR）、数值转换（I2L..I2S）、ARRAYLENGTH、
///     INSTANCEOF——且全部输入使用边 arity 对齐（缺失边 = 看不见的来源，
///     不可删）。装载是恒等搬运不作链定义；store/return/跳转/iinc/字段/
///     数组读写/对象创建/checkcast/monitor/其余调用不入链。
///
/// 删除条件（对每个 POP/POP2 窗口）：
///   - 弹值全部到达定义入链、链含白名单调用；
///   - 链成员的全部使用边都指向链内或本 pop（共享消费者逐出，不动点
///     级联）；
///   - 区间约束：[链首, pop] 顺序区间内只有链成员与 pop，且区间内部
///     （含 pop 前边界）无存活标签锚定——区间外分支不得中途进入链；
///     链首前边界标签允许（删除经 fallthrough 重定向出区间）。
///
/// 安全门：帧数据流失败或截断整件跳过；方法含异常表整件跳过（handler
/// 入口值无定义边，负向结论不可靠——精细化挂账）；按签名播种入参
/// （参数定义边使含参链自然失配）。
///
/// 发生删除返回 none()，否则 all()。maxStack/maxLocals 不变。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class DeadSideEffectFreeCallRemover : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
