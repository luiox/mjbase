#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief GETSTATIC 读点物化（容器级直写）：把容器内 static 读点按收集件
///        发布的 StaticValueView 原位物化为等值常量装载（ldc 族/短形态
///        工厂），消一次字段读。
///
/// 动机来源：Recaf 侦察第一吸收项（StaticValueInliningTransformer 的
/// mjbase 域收编）。消费面只有 StaticValueCollection 经
/// GlobalAnalysisManager::setPinned 发布的 StaticValueViewAnalysis 视图：
/// 链上没有收集件（或收集件未发布任何内容）时本件严格 no-op——两件独立
/// 启停，视图不存在即整件 all() 退出，绝不自行重算。
///
/// ## 读点改写判据（fail-closed 逐桶，任一不满足读点原样保留）
///
/// - **视图命中**：(owner, name, desc) 三段精确查视图；未命中保留
///   （容器外 owner / 未收集字段 / 经父类解析的继承读点全部自然落此桶）。
/// - **读点位置**：`<clinit>` 与 `<init>` 方法内的 GETSTATIC 一律跳过
///   （初始化语义面：clinit 内读点可能落在写点之前（前向引用），构造器
///   可能参与类初始化环）。发布类的 clinit 已被收集件的走廊纯度门限定为
///   无 invoke 形态，clinit 可达同类闭包坍缩为 clinit 自身，无需闭包
///   游走（spec §5.2）。
/// - **类别匹配**：值类别须与字段描述符族一致（cat2 字段（J/D）配
///   Long/Double，cat1 配 Int/Float/Str；I/B/C/S/Z 字段族按 int 物化）。
///   不一致保留（防御桶，收集面已保证一致）。
/// - **volatile 复核**：owner 在容器内时对字段旗标二次复核
///   （ACC_VOLATILE 保留读点——内存语义桶的物化件侧双保险，收集面
///   同桶先拒）。
/// - **CP 容量**：常量物化前预检池容量（对齐 ConstantFolder 的
///   0xFFFF 红线），超界保留读点。
///
/// ## 载体与提交
///
/// 容器级直写（会话内 promote → CodeDom 原位改写 → lower → 分支体
/// Strict 帧再生 → writeClass），两阶段提交（全部类序列化成功才动容器）；
/// 帧链接线与诊断语义与 MethodCallRemover 同款。改写保持栈形状恒等
/// （GETSTATIC 与替换装载同类别同弹宽），单指令替换不引入新的控制流，
/// lower relocation 承接偏移与 StackMapTable 再生。任一 class 不可解析
/// 整件放弃（返回 all()，视图消费者不做部分改写）。发生改写返回 none()；
/// 二次运行严格 no-op（读点已消失）。
///
/// @par 标签
/// - `scope:global`
/// - `action:rewrite`
/// - `stability:opt-in`
class StaticValueInlining : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
