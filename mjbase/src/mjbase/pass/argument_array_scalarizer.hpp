#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 参数数组标量化（#37 切片 1，全局件）：装箱混淆把真参数序列退化
///        成单 Object[] 参数（签名 `([Ljava/lang/Object;)R`），本件还原——
///        被调方取参窗改直接参数槽位、调用侧装箱窗拆成直接传参、签名经
///        MemberRemapPlan 全局同步（声明面 + 全部引用面）。
///
/// 适配形态（被调侧 / 调用侧成对）：
///   - 被调侧取参窗：`aload p; iconst_i; aaload; checkcast Ti`——每参数
///     一组，checkcast 即类型证据；
///   - 调用侧装箱窗：`iconst n; anewarray Object; (dup; 实参; iconst_j;
///     aastore)*; invoke`——实参按序入数组后整体传参。
///
/// 判定（充分条件，任一失败整目标放弃，fail-closed）：
///   - 被调方容器内声明、desc 恰为 `([Ljava/lang/Object;)R`（static 与
///     实例同面：实例参数槽基 1，调用位点静态性须与候选一致，收
///     invokestatic / invokevirtual / invokespecial）；
///   - 取参窗下标完整覆盖 0..n-1（无缺口；同元素重复取允许）；参数槽
///     全部使用（def-use 入参定义边）都落在取参窗的 aaload 上——数组
///     不逃逸（arraylength / 传参 / 返回 / 再存储整体拒收）；参数槽
///     无 store/iinc/ret 触碰；
///   - 调用侧窗口规范形（切片 3 顺序扫描形）：长度常量 == n、下标序恰
///     0..n-1、实参组非空且至多 kMaxArgInsns 条（窗头锚定回扫，元素
///     边界回溯容嵌套同形序列）；实参类型静态可判面：aconst_null 匹配
///     任意引用 Ti；ldc String 要求 Ti ∈ {String, Object}；ldc Class
///     常量要求 Ti ∈ {常量类, Object}；aload_k 槽类型提升——读点到达
///     定值恰为调用方同槽入参定值（单定义未截断，槽未被重存储）时按
///     调用方声明型（实例槽 0 = this 取调用方类）对齐，否则要求
///     Ti == Object；双指令装箱链 [单值生产者; wrapper.valueOf(prim) /
///     String.valueOf(...)] 按产出型对齐（生产者只做宽度形态校验），
///     其余多指令形态 fail-closed 拒收；
///   - 容器内全部调用点（按 CP owner+name+desc 解析到目标）都是规范窗；
///     CP owner ≠ 声明类的调用（继承静态视图）拒收；他类同名同描述符
///     声明（隐藏面）拒收；MethodHandle/indy 钉死（CP 方法柄引用）拒收；
///   - 窗内待删指令锚定存活标签 → 该窗放弃 → 整目标放弃。
///
/// 改写：
///   - 被调方：p+1 起局部槽整体平移 n-1（load/store/iinc/ret 全族，
///     紧凑形出界退化泛型形；maxLocals += n-1），取参窗四条改一条
///     `aload (p+i)`；
///   - 调用侧：装箱窗删 anewarray/长度常量/每元素的 dup、iconst、aastore
///     （实参原位留栈，按序直接传参），invoke 描述符随 RemapPlan 全局
///     同步；
///   - 两阶段提交：结构编辑（会话 CodeDom → lower → 类字节）先行，
///     描述符同步（applyResourceRemapPlan，默认 bytecodeOnly 面）随后。
///
/// 改写后被调方栈深只降不升（maxStack 偏大方向合法）。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class ArgumentArrayScalarizer : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
