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
///   - 改写面：类名映射计划（旧名→新名）逐类应用，改写面为 bytecode +
///     annotations + signatures——this/super/接口、成员名与描述符、方法体
///     指令操作数、异常表捕获类型、结构元数据与 StackMapTable Object 校验
///     类型、反射字符串常量查找的名称面之外，泛型 Signature（类/字段/方法
///     三级 + LVTT）、Runtime{Visible,Invisible}Annotations、
///     Runtime{Visible,Invisible}TypeAnnotations（含 Code 嵌套）、
///     Runtime{Visible,Invisible}ParameterAnnotations 与 AnnotationDefault
///     的类引用随映射结构化同步：签名走 libjbc jbc::type::signature 解析树
///     span 手术（jbc::ir::sig_attr 面，非字符串替换，'.' 段链/'$' 形态/
///     通配符/类型变量边界正确），注解走 typed 属性模型（jbc::ir::
///     type_annotation_attr 面口径：type_index 与元素值改写、type_path/
///     target_info 随结构往返保真）。反射字符串与资源面不开启（与旧栈默认
///     策略一致）。
///   - 资源面：写回保持 URI 形状——base 条目以 `新名 + ".class"` 入容器，
///     版本化条目以 `META-INF/versions/<N>/新名 + ".class"` 入容器（版本前缀
///     原样保留，同族各成员 URI 互异）。旧 URI 条目删除。全部类先完成改写与
///     序列化，再一次提交容器变更（任一序列化失败整 pass 抛出，容器保持
///     原样——作业级原子，对齐旧栈「任一 writer 失败保持全部不变」）。
///   - 结果：发生重命名返回 none()（资源列表失效），无类可改返回 all()。
///
/// 与旧栈的偏差点：
///   - Signature/注解类引用同步（B8 8.3 收口，输出变更的语义增强）：旧栈
///     默认策略为 bytecodeOnly，重命名后泛型签名与注解类引用滞留旧名（悬
///     垂泛型）；本实现经 libjbc 属性层结构化手术面（jbc::ir::sig_attr /
///     type_annotation_attr 消费口径）同步改写，反射字符串与资源面仍保持
///     旧栈不开口径。
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
