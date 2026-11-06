#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 基础版小方法内联预算：被调方法体指令数上限（写死常量，不做配置面）。
///
/// 32 条 = 典型混淆器注入的辅助函数/转发壳（参数装载 + 少量纯计算 + 返回）
/// 的体量上界；超出即拒绝内联，调用点原样保留。后续放宽须先过 JVM 真跑门。
inline constexpr ca::u32 kMaxInlineCalleeInsns = 32;

/// @brief 基础版小方法内联（MethodInliner）：把容器内满足准入门的小方法体
///        克隆进调用点，替换 invokestatic / invokespecial 调用指令。
///
/// ## 生态位
///
/// 方法级指令变换组的 method_inliner（mbase 旧栈同名 pass 的
/// 新载体重推导；IdentityWrapperInliner 只覆盖其中的纯转发壳形态）。
/// 服务面：混淆器注入的静态纯函数辅助方法与转发链——调用点就地展开，
/// 消除一次跨方法跳转；壳方法本体保留（删除延后 GlobalUnusedMethodRemover
/// 管线，与本 pass 无正确性顺序约束）。opt-in，默认链不收，CLI -p 显式
/// 挂点。
///
/// ## v1 边界（做 / 不做）
///
/// 做：
/// - 调用点 op 仅认 invokestatic（被调 static）与 invokespecial（被调
///   private 且与调用方同类）。跨类内联仅认 static 且类/方法双 public。
/// - 被调体纯度可证（MethodEffectModel Pure，含调用边不动点传播）。
/// - 被调体自身帧可算（BestEffort 形状域预检：栈形失衡/局部越界的畸形体
///   在候选层拒收，调用点原样保留，不升级为 caller 整类放弃）。
/// - 被调体预算内（≤ kMaxInlineCalleeInsns 条指令）、无异常表、无
///   synchronized、无 jsr/ret、无 switch、无回边（全前向分支）、分支
///   目标不落方法末边界。
/// - 参数/接收者落 fresh 局部槽（调用点逆序 store），被调槽位整体平移
///   映射，CP 操作数按语义视图跨会话 re-intern，return 全部改写为
///   GOTO 统一出口（调用点后继），调用点入边标签重锚到首条插入指令。
/// - 类版本 ≥ 50（SMT 语义）：写回走 prepareNestedAttrsForLayout →
///   lower → Strict 帧重算 + maxes 采用 + SMT 再生（frame_rewire 链）。
///
/// 已知不覆盖面（留 v2）：
/// - invokevirtual / invokeinterface 调用点（含 final 收窄面）留 v2；
/// - 跨类 private（nestmate）与跨包非 public 访问面留 v2；
/// - switch 被调体、浅循环（回边）被调体留 v2；
/// - 被调 LNT/LVT 调试元数据不随迁（旧栈 SKIP_DEBUG 同口径）；
/// - 内联体中的调用不递归展开（多级链多轮收敛，交管线重复运行）；
/// - 类初始化触发时机只挡声明类自身 `<clinit>` 一层（旧栈同口径），
///   容器可解析超类链的 `<clinit>` 未查，头注声明为已知保守缺口。
///
/// ## fail-closed 红线（任一不过 → 该调用点原样保留）
///
/// 预算外、异常表、监视器、回边、纯度不可证（含被调体含字段访问/
/// 分配/显式抛出/indy/虚调用）、直接与间接递归（体自调 + 调用边
/// 可达回自身）、跨类体含成员/类型引用指令（改挂调用方访问上下文
/// 不安全）、被调声明类带 `<clinit>` 的跨类 static、类数不增、改写
/// 粒度 = 方法。会话装载/promote/克隆/lower/帧重算/序列化任一失败
/// → 整类放弃不写回（容器原字节保留）。
///
/// ## 等价性论证
///
/// - **单次执行**：调用点替换为「参数落槽 + 被调体克隆 + 出口回跳」，
///   被调体语句恰执行一次，纯度门保证无堆副作用、无类初始化新触发点
///   （体白名单内 op 不可抛异常或异常语义随体逐位随迁）。
/// - **栈形**：store 序列恒弹净实参（cat2 弹宽按描述符），出口 GOTO
///   携返回值落调用点后继，与原 invoke 压值形态逐点相同。
/// - **帧可验证**：fresh 槽仅在克隆体内定值，未赋值路径以 Top 参与汇合
///   （JVMS 4.10.2.4 / libjbc ShapeResolver::merge：Top 并异型取异型），
///   Strict 重算对克隆体直线/前向分支形态恒可算，失败即整类放弃。
/// - **幂等**：调用点已删除，二次运行严格 no-op 返回 all()。
///
/// 变换规则：无站点命中返回 all()；有改写返回 none()。
///
/// @par 标签
/// - `scope:global`
/// - `action:inline`
/// - `stability:opt-in`
class MethodInliner : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&  container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
