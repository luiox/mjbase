#pragma once

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 类重命名器：按确定性规则把容器内全部类重命名为 `<包>/Class<N>`。
///
/// 行为矩阵：
///   - 装载面：可解析、有类名的 class 资源参与；空 bytes、不可解析资源跳过。
///     装载后按类内部名字节序排序（重命名编号的确定性来源）。
///   - 命名面：旧名按最后一个 '/' 截断，取 `包前缀 + "Class"`（无 '/' 时用
///     `"Class"`）；编号 `N` 从 0 起全局递增（不按类重置），经集合查重取首个
///     未占用名——即容器按旧名序得到 `Class0/Class1/...`。
///   - 改写面：类名映射计划（旧名→新名）逐类应用，改写面为 bytecodeOnly——
///     this/super/接口、成员名与描述符、方法体指令操作数、异常表捕获类型、
///     结构元数据与 StackMapTable Object 校验类型、反射字符串常量查找的名称
///     面；Signature 泛型属性不改写（见下方注记）。
///   - 资源面：改写后的类以 `新名 + ".class"` 入容器，旧 URI 条目删除。全部
///     类先完成改写与序列化，再一次提交容器变更（任一序列化失败整 pass 抛出，
///     容器保持原样——作业级原子）。
///   - 结果：发生重命名返回 none()（资源列表失效），无类可改返回 all()。
///
/// 设计注记：
///   - Signature 泛型属性不改写：
///     重命名后泛型签名仍指旧名。libjbc 属性层签名手术面（jbc::ir::sig_attr）
///     已就绪，开启签名同步属输出变更的语义增强，须经主线裁决后另批接线。
///   - 写回走 libjbc copy-through：未触碰区段字节保真、常量池不重编号。
///     成员/指令语义不变；writer 层字节差异以 golden 对拍（声明类目）收口。
///
/// @par 标签
/// - `scope:global`
/// - `action:rewrite`
/// - `stability:opt-in`
class ClassRenamer : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
