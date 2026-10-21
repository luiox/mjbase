#pragma once

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 容器级畸形 class 结构清洗器（两级流水：libjclassfile 前置级 + libjbc 级）。
///
/// 中文契约：
/// 输入是 ResourceContainer 中的 class bytes；非 class 资源不触碰。逐资源两级
/// 流水（中间层设计 §3.0 敌意输入前置清洗级裁决的落地批）：
///   1. 前置级（libjclassfile）：畸形容忍读入（全量属性建模 + 指令解码）→
///      IllegalStrippingTransformer 全量规则面 → 写出。规则触发按产物与输入的
///      字节比较判定，未触发的
///      资源不重编码——合法输入零重编码、字节不动。
///   2. libjbc 级：对进入字节跑既有 2 条结构规则（见下），读/写失败零产出。
/// 每级独立原子（级内任一步失败该级原样放行），级间以字节衔接；bytes 发生
/// 替换走两阶段提交（ClassResourcePendingCommit，作业级原子），全部保持时
/// 返回 all()，发生替换返回 none()。容器拥有资源 bytes，本 pass 不保存裸
/// 指针、不抛异常。
///
/// 清洗规则面：
///   前置级（5 条挂账规则 + transformer 一体携带项，语义逐条）：
///     1. 属性级 CP 校验剔除：属性引用的 CP 条目语义非法（tag/内容谓词不匹配，
///        如 ConstantValue 与字段类型不符、SourceFile 指向非 Utf8）时剔除该属性；
///     2. 越界跳转 / 越界与种类不符 LDC 的 RETURN 桩替换（含 null entry 的
///        CpRef 统一 stub）；
///     3. LVT/LVTT/LineNumberTable 修剪（越界/悬垂条目，指令边界判定）；
///     4. BSM 表压缩与 condy 常量池手术（无效 BSM 条目剔除、dynamic 的
///        bootstrap 索引重映射、无引用 condy 出池、无 dynamic 引用时整表移除）；
///     5. 无效 MethodHandle 池清除（reference kind 与被引用条目 tag 不匹配）。
///   libjbc 级（快路径，既有规则不动）：
///     1. 成员描述符校验：字段描述符非法（jbc::type::parseFieldType 失败）或
///        方法描述符非法（parseMethodDescriptor 失败）的成员整条剔除——这类
///        类永不可能被 JVM 加载（LinkageError），剔除只为让下游工具能读；
///     2. Code 异常表垃圾条目剪除：startPc >= endPc（空区间）或 startPc/endPc
///        /handlerPc 越过 code_length 的条目剔除。
///
/// 实现形态差异点（载体重定型导致的必要偏差）：
///   - 前置级以 IllegalStrippingTransformer::transform() 整体接入：除 5 条
///     挂账规则外还携带同一规则面一体的单例属性去重与 type-annotation 目标
///     修剪——拆单项需改 libjclassfile 公共面，本线不碰库源码；
///   - 成员描述符剔除与异常表剪除在两级都存在：语义幂等、收敛一致；libjbc
///     级保留作快路径兜底（libjclassfile 读失败时仍生效）；
///   - 异常表剪除边界：前置级为指令级精确上界（「最后一条指令起始偏移」），
///     libjbc 级保守用 code_length（CodeAttr 无指令级解码），仅兜底路径生效；
///   - libjclassfile 读失败不再整资源放弃：原 bytes 仍进 libjbc 级清洗，产出只会更干净、不会更差；
///   - 读级宽容吸收（坏上下文属性/重复注解/EOF 截断属性等 reader 配置族）与
///     计入规则触发。
///
/// @par 标签
/// - `scope:global`
/// - `action:sanitize`
/// - `stability:opt-in`
class IllegalClassFileCleaner : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
