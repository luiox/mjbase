#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 恒等包装不动点内联（identity wrapper inliner）：把容器内对「纯转发
///        壳方法」的 invokestatic 调用点直接改写到壳的最终目标（路径压缩），
///        壳方法本体不动、删除延后到 GlobalUnusedMethodRemover 管线。
///
/// ## 背景与目标形态
///
/// 混淆器（Iridium 族样本中占 89.3% 方法数的壳网络）大量生成「恒等壳」：
/// 静态方法体只有参数逐个按序装载 + 一条调用 + 一条返回。本 pass 识别两类
/// 形态并把**调用点**（而非壳体）改写为直达最终目标：
///
/// - **形态 A（静态直通）**：壳体 `invokestatic T.m` 且目标描述符 == 壳描述符。
///   调用点 `invokestatic W.w` 原位改目标为 `T.m`，op 不变。JDK 装箱
///   `Integer.valueOf(I)Ljava/lang/Integer;` 是其公共目标特例。
/// - **形态 B（虚转发）**：壳体 `invokevirtual/interface T.m` 且目标描述符 ==
///   壳描述符去掉首参、返回类别一致。调用点 `invokestatic W.w` 改 op 为壳体
///   同款虚调用并改目标，**接收者 = 原首实参**（栈形逐点不变）。JDK 拆箱
///   `Integer.intValue()I` 是其公共目标特例。
///
/// ## 等价性论证（改写为何安全）
///
/// - **栈形不变**：调用点原 `invokestatic W.w(p0..pn)` 弹全部实参压返回值；
///   形态 A 目标描述符全同；形态 B `invokevirtual T.m(p1..pn)` 弹接收者
///   （原 p0）+ 余参压同类别返回值——两条栈轨迹逐点相同，StackMapTable
///   无需变化，lower 只做重定位。
/// - **类型合法（verifier）**：形态 B 要求接收者静态类型 P0 <: T。壳体自身
///   `aload_0; invokevirtual T.m` 已通过原类验证，即 P0（壳的首参声明类型）
///   <: T 成立；调用点实参静态类型同为 P0（invokestatic 按壳描述符定型），
///   改写后仍满足。**前提公理：容器内类文件可验证**（与全 pass 链同公理）。
/// - **无隐藏副作用**：壳体形态白名单（见下）保证除转发外无任何计算——无
///   常量、无字段、无分支、无异常表、无 monitor。ACC_SYNCHRONIZED 壳
///   拒收（monitor acquire 属被跳过的可观测行为）。
/// - **链与环**：壳→壳链做路径压缩（一次改写到最终目标，非迭代改写）；
///   环（含自递归）整族拒收。跨形态链（静态壳转发到虚壳）同样压缩——
///   描述符约束在每一级独立成立，逐级传递。
///
/// ## 形态白名单（壳体判定，全过才入表）
///
/// 方法 static、非 ACC_SYNCHRONIZED、名非 '<' 开头、有代码；代码恰为
/// nparams 条按序参数装载（槽位 0,1,... 按 J/D 双槽推进；_n 与泛型形态
/// 皆认，wide 前缀同）+ 1 条 invoke + 1 条匹配返回，异常表为空。参数装载
/// 操作数类型须与壳描述符逐位一致。invoke 仅认 static/virtual/interface
/// 三族（special 与 invokedynamic 不进壳表）。
///
/// ## 调用点改写门（缺一即放弃该站点）
///
/// 站点 op == INVOKESTATIC（壳是静态方法，合法调用面只有它）；编码目标
/// （owner.name+desc）命中壳表；最终目标从调用者类可达——容器内声明：
/// public，或同类（private 恒可达），或同包（package-private/protected，
/// **protected 的跨包子类调用不认**，保守首切面）；容器外目标只认 JDK
/// 装拆箱白名单（八个 box 类的 valueOf/<x>Value，皆 public final 类）。
/// 编码目标 owner 是壳类子类的静态解析形态不进表（§5.4.3.3 上扫未建模）。
///
/// ## 放弃与幂等
///
/// 站点级放弃：上述任一门不过。类级放弃（整类不写回，容器不动）：会话
/// 装载 / promote / lower / 序列化任一失败。幂等：改写后站点 op 已非
/// invokestatic 或目标不再命中壳表，二次运行严格 no-op；壳体在被改写后
/// 仍是同形态壳（转发到更近目标），表重建结果收敛。
///
/// ## 后续与删除策略
///
/// 壳方法**不删**——反射面（getDeclaredMethods）与潜在未重写引用面保持
/// 原样；无引用壳的清除是 GlobalUnusedMethodRemover 管线的职责（本 pass
/// 在头注显式声明该依赖顺序）。改写只增 CP（setMember intern 纪律）。
///
/// 变换规则：无站点命中返回 all()；有改写返回 none()。
///
/// @par 标签
/// - `scope:global`
/// - `action:rewrite`
/// - `stability:opt-in`
class IdentityWrapperInliner : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&  container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
