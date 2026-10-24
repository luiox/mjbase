#pragma once

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 类重命名器：按确定性规则把容器内全部类重命名为 `<包>/Class<N>`（移植自
///        mbase pass/opt/class_renamer）。
///
/// 行为矩阵（与旧实现逐条对齐，Multi-Release 族契约为本仓增量）：
///   - 装载面：可解析、有类名的 class 资源参与；空 bytes、不可解析资源跳过。
///     装载后按 (类内部名, URI) 字节序排序（重命名编号的确定性来源）——base
///     与 `META-INF/versions/<N>/` 变体内名相同并列时，URI 是族内唯一的次序
///     决胜键，编号不随容器迭代序漂移。
///   - 命名面：旧名按最后一个 '/' 截断，取 `包前缀 + "Class"`（无 '/' 时用
///     `"Class"`）；编号 `N` 从 0 起全局递增（不按类重置），经集合查重取首个
///     未占用名。**同内名族（base + versions/&lt;N&gt; 变体）只消耗一个编号、
///     共享同一新名**：族成员在 JVM 语义上是同一个类的多个 release 变体
///     （内名必须一致，差异只在条目路径）；命名计划又按内名键控，族内发两个
///     新名既表达不了（后发覆盖先发）也不可加载。
///   - 改写面：类名映射计划（旧名→新名）逐类应用，改写面为 bytecodeOnly——
///     this/super/接口、成员名与描述符、方法体指令操作数、异常表捕获类型、
///     结构元数据与 StackMapTable Object 校验类型、反射字符串常量查找的名称
///     面；与旧栈默认策略一致（Signature 泛型属性不改写，见下方偏差注记）。
///   - 资源面：写回保持 URI 形状——base 条目以 `新名 + ".class"` 入容器，
///     版本化条目以 `META-INF/versions/<N>/新名 + ".class"` 入容器（版本前缀
///     原样保留，同族各成员 URI 互异）。旧 URI 条目删除。全部类先完成改写与
///     序列化，再一次提交容器变更（任一序列化失败整 pass 抛出，容器保持
///     原样——作业级原子，对齐旧栈「任一 writer 失败保持全部不变」）。
///   - 结果：发生重命名返回 none()（资源列表失效），无类可改返回 all()。
///
/// 与旧栈的偏差点：
///   - Signature 泛型属性不改写（旧栈同款 bytecodeOnly 缺陷的忠实保留）：
///     重命名后泛型签名仍指旧名。libjbc 属性层签名手术面（jbc::ir::sig_attr）
///     已就绪，开启签名同步属输出变更的语义增强，须经主线裁决后另批接线。
///   - 写回走 libjbc copy-through：未触碰区段字节保真、常量池不重编号；旧栈
///     经 tree writer 全量重序列化（CP 按 encounter 序重编号、失引条目剪除）。
///     成员/指令语义等价，writer 级差异（对拍以 golden 声明类目收口）。
///   - base 缺失而仅存版本化变体的族：变体照常改名并保持在 versions/&lt;N&gt;
///     前缀下，不升格为 base（输出布局变更不属本 pass，见容器层族视图契约）。
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
