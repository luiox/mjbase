#pragma once

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 保守合并简单的 static 工具类。
///
/// 行为矩阵：
///   - 准入面：仅接受以 Object 为父类、名称不含 `$`、无接口、只含 static
///     字段/方法及构造器的 class；非构造器方法还须「简单体」——无任何方法
///     调用（invoke 全家族含 indy）、无 PUTSTATIC。候选少于 2 整体 no-op。
///     名含 `ToolClassMergedClass_` 的类永不入候选（幂等重入防护）。
///   - 分桶面：候选按容器迭代序逐个投入首个可接纳的宿主（方法数合计
///     >100、字段撞名、方法撞 name+desc 任一命中即换新宿主；宿主命名
///     `me/canrad/morpher/ToolClassMergedClass_<N>`，V1_8 public super，
///     自带调用 super 的 public `<init>`）。
///   - 搬移面：static 字段经 adoptField 搬入宿主（ConstantValue 由宿主侧
///     重建）；static 方法经 adoptMethod
///     直调搬入宿主（libjbc §34.3(2) 契约，语义单一真源在库）。供体的
///     构造器与安全空 `<clinit>` 不搬，随供体类删除。分支体并入 V51+
///     宿主后按契约由本 pass 用 Strict 帧重算 + SMT 再生补帧（帧链经
///     spi/frame_rewire 同一链路，resolver 为容器空层次图 + Object 回退
///     ——工具方法体无跨类型合并点，Strict 不可算即显式失败整批回滚）。
///   - 重映射面：供体→宿主类名映射计划经 reference 面（applyMemberRemapPlan
///     bytecodeOnly，class_renamer 同款）施加于全部宿主与幸存类——搬移体
///     内残留的供体自引用与跨类消费点一并改写。
///   - 资源面：两阶段提交。全部改写与序列化成功后一次提交容器（先删全部
///     供体条目，再以 `宿主名 + ".class"` replaceResource 插入宿主（同
///     URI 覆盖）；任一失败整 pass 抛出，容器保持原样（作业级
///     原子，异常路径保守回滚）。发生合并返回 none()，无可合并返回 all()。
///
/// 声明面（实现形态要点）：
///   - libjbc NotModeled 域保守保持原资源：候选体内含 ldc 宽常量形态
///    （Long/Float/Double/MethodType/MethodHandle/Dynamic，即 adopt 的
///     fail-closed 清单）时该候选整体落选、原类原样保留。
///   - 宿主帧：分支体并入后由本 pass 再生 SMT（golden 对拍按声明类目
///     走宿主语义对拍 + 帧方向钉死）。
///   - 写回走 libjbc copy-through：幸存类未触碰区段字节保真（非全量重
///     序列化）。成员/指令语义等价，writer 级差异（对拍以 golden 声明
///     类目收口）。
///   - 供体方法的非 Code 属性（Exceptions/注解/Signature）与供体字段的
///     非常量属性不随迁（libjbc §34.3(4) 显式契约）；ConstantValue 例外
///     处理见搬移面。
///
/// @par 标签
/// - `scope:global`
/// - `action:rewrite`
/// - `stability:opt-in`
class ToolClassCombiner : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
