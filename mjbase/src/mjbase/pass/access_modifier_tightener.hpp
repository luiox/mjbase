#pragma once

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 访问修饰符收紧器：把仅被本类引用的 public static 成员收紧为 private
///        （移植自旧栈 pass/opt/access_modifier_tightener）。
///
/// 收紧规则矩阵（与旧实现逐条对齐）：
///   - 普查面：全容器 INVOKESTATIC（方法）与 GETSTATIC/PUTSTATIC（字段）引用；
///     其他操作码（invokevirtual/special/interface、getfield/putfield）不参与。
///   - 记账规则：被引用的成员（owner+name+desc）记一条；引用来源 owner 不同于
///     被引成员所属类时记 usedOuter。
///   - 收紧条件：成员在普查表中存在（至少被引用一次，含仅自类引用）且
///     !usedOuter 且当前 static 且当前 public → access = (access & ~ACC_PUBLIC)
///     | ACC_PRIVATE。从未被引用的成员不动；非 public / 非 static 不动。
///   - 装载面：空 bytes、不可解析、无类名的资源跳过；任一方法体升格失败的
///     类整体跳过（普查证据不全时收紧是不安全方向，对齐旧栈「读不进就整类
///     不参与」的准入粒度）。装载后按类内部名字节序排序（确定性，对齐旧栈）。
///   - 写回：改动的类经两阶段提交（ClassResourcePendingCommit，作业级原子）；
///     序列化失败向上抛出（对齐旧栈 writer 异常传播）。发生收紧返回 none()，
///     否则 all()。
///
/// 依赖边界与旧栈一致：跨类普查只覆盖容器内 class 资源的指令流，反射/Native/
/// 服务查找等外部入口不在普查内——本 pass 为 `opt-in`，不得进入默认优化链。
///
/// @par 标签
/// - `scope:global`
/// - `action:rewrite`
/// - `stability:opt-in`
class AccessModifierTightener : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
