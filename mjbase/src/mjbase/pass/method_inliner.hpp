#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 小方法内联预算：被调方法体指令数上限（写死常量，不做配置面）。
///
/// 典型混淆器辅助函数/转发壳的体量上界，超出即拒收；放宽须先过 JVM 真跑门。
/// 依据 doc:method-inliner-spec §6。
inline constexpr ca::u32 kMaxInlineCalleeInsns = 32;

/// @brief 浅循环预算：单个被调体允许的回边数上限（风险帽，非语义门）。
///
/// 单循环（含 continue 第二回边）或一层嵌套的浅形态，更深拒收；循环执行次数
/// 语义由纯度门与槽位幂等保证。依据 doc:method-inliner-spec §6。
inline constexpr ca::u32 kMaxInlineCalleeBackEdges = 2;

/// @brief 基础版小方法内联（MethodInliner）：把容器内满足准入门的小方法体
///        克隆进调用点，替换 invokestatic / invokespecial / invokevirtual /
///        invokeinterface 调用指令。
///
/// 服务面：混淆器注入的静态纯函数辅助方法与转发链——调用点就地展开，消除
/// 一次跨方法跳转；壳方法本体保留（删除延后 GlobalUnusedMethodRemover 管线，
/// 与本 pass 无正确性顺序约束）。opt-in，默认链不收，CLI -p 显式挂点；注册名
/// 固定 ``method_inliner``。
///
/// 站点判据契约（一句话口径，判定流程与逐面展开依据 doc:method-inliner-spec）：
/// - static/special 面（v1）：被调 static 同类任意访问跨类双 public；special
///   仅同类 private 非 static 非 ``<init>``。
/// - virtual/interface 面去虚化：声明者落解后按序匹配三路 JVM 强制证据——
///   声明方法 ACC_FINAL ｜ 声明类 ACC_FINAL 且声明该方法 ｜ 声明 ACC_PRIVATE
///   （JEP 181 不参与分派，覆盖 javac 11+ 三形态）；invokeinterface 只收
///   private 声明。
/// - 跨类非 public 访问面：private 走 nestmate 属性链（JVMS 4.7.29 镜像）；
///   protected 走容器内子类链；package-private 跨类不做（loader 身份不可证）。
/// - 被调体：纯度可证（MethodEffectModel Pure）、预算内、无异常表、无
///   synchronized、无 jsr/ret、分支/switch 目标不落方法末边界、回边数不超
///   kMaxInlineCalleeBackEdges。
///
/// 拼接形契约：参数/接收者落 fresh 槽（接收者 = base+0 与被调槽 0 对齐，克隆
/// 体槽映射恒等平移 k → base+k）；实例被调统一带接收者 null 守卫（ALOAD →
/// IFNONNULL 越过 → ACONST_NULL → ATHROW）；return 改写 GOTO 统一出口（调用点
/// 后继）；CP 操作数按语义视图跨会话 re-intern；调用点入边标签重锚到首条插入
/// 指令。类版本 ≥ 50（SMT 语义）；写回走 prepareNestedAttrsForLayout → lower
/// → Strict 帧重算 + maxes 采纳 + SMT 再生。
///
/// fail-closed 红线：任一门不中该调用点原样保留（字节逐位不变）；捕获/克隆/
/// 写回中途失败整类放弃不写回（容器原字节保留）；无站点命中返回 all()，有
/// 改写返回 none()；二次运行严格 no-op（幂等）。已知保守缺口：跨类 static
/// 只挡声明类自身 ``<clinit>`` 一层，超类链未查；被调调试元数据不随迁。
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
