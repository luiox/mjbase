/// @file        flattening_reverser.hpp
/// @brief       控制流反扁平化设施：常量状态传播 + switch 分发静态解析。
///
/// ## 定位：通用引擎 + 形态表（变体不写死）
///
/// 扁平化混淆的变体很多（分发操作数的折叠形态、前驱块尾的状态写形态都
/// 因混淆器而异），但反扁平化的**推理骨架**是共通的：
///
/// 1. 全方法 switch 唯一性审计（多 switch 读同一状态 = 多汇合点，无法
///    单值传播）；
/// 2. 分发操作数解析：switch 键值必须是「状态槽单次装载 + 常量折叠步」；
/// 3. 状态槽读写全量审计：读点唯一（恰为分发块读点）、写点全部可解析；
/// 4. 逐写点用**前驱形态表**解析常量状态 K，静态求值 `fold(K)` 命中
///    switch 案表（未命中走 default——default 是合法状态语义，不盲删）；
/// 5. 入边守卫（非前驱分支锚入分发块区间 / 物理直落进块头 / 案表自引用
///    全部拒收）+ 帧引擎栈深等价门；
/// 6. 事务性统一改写：前驱 goto 直接复用 switch 的 case 标签句柄，分发块
///    失去全部入边变不可达（交死代码清理面），`while(true)+switch` 状态机
///    形态消失。
///
/// 因此本 pass 分两层：上面 1/3/4(求值)/5/6 是**引擎**（与变体无关的通用
/// 推理）；「哪些指令形态算状态装载/常量折叠步/状态写入」是**形态表**
/// （构造期可注入的数据，默认表只是保守首版）。新变体 = 加表行或调规格，
/// 不改引擎；若某混淆器家族的形态表膨胀到自成体系，形态表整体外移到
/// 配方层，引擎留在本仓。
///
/// ## 相对状态更新（s += D 分发形态）
///
/// 前驱块尾不写绝对状态 K，而是相对更新 `s += D`（iinc 紧凑形，或
/// `[iload s; intConst D; iadd/isub/ixor; istore s]`——IADD/IXOR 的常量在左
/// 镜像 `[intConst D; iload s; binop]` 与 s 在左同值，同认；ISUB 的 D 在左
/// 是 `D - s` 形，不在族内）。相对写点的当前值引擎自己解不出来，按状态机
/// 本身做数据流求解（状态迹模拟）：
///
/// 1. 写点分类：形态表命中 = 绝对 K；相对形态命中 = s += D；皆不命中 =
///    拒收。绝对写点自解析，相对写点交模拟定值。
/// 2. 案表供源：每个案表目标（含 default）从目标起**直线**扫描到首个状态
///    写点；控制转移/分发块头/方法尾 = 无直线供源（终态块）。块内分支
///    形态在此 fail-closed。
/// 3. 入口锚：不被任何案表供源的绝对写点，唯一才可锚定（入口边必经方法
///    头侧进入，不取道分发块；被案表供源或存在多个候选 → 拒收）。
/// 4. 迹模拟：v = fold(s) → 案表目标 → 供源写点 w → s' = K（绝对）或
///    s op D（相对）；同一写点被两个不同值走到（路径敏感/多 key 同案）→
///    拒收；环闭合即停。迹终止后任一相对写点未定值 → 拒收。
/// 5. 相对更新里的 `iload s` 是读改写的一部分，读点审计按登记位置豁免；
///    iinc 不再整族禁用，其闭环由覆盖审计承担（不在迹上的 iinc 写点 →
///    拒收）。
///
/// 改写面与绝对形态完全一致：只动前驱 goto 的目标字段，状态链原样保留。
///
/// ## 适配的混淆形态（默认形态表覆盖面）
///
/// 扁平化混淆器把方法的真实功能块拆散成平级块，用状态机串联：
///
/// - 状态变量 `s`（int 局部）：各功能块尾部写常量状态后 `goto D`；
/// - 分发块 `D`（单汇合块）：`iload s` 混常量折叠步收尾于
///   lookupswitch/tableswitch，按 `fold(s)` 派发到功能块入口。操作数序
///   通用化：链上推值（常量与装载，装载恰一）任意排布——常量全在右
///   （`iload s; ldc C; binop`）、全在左（`ldc C; iload s; …; binop`
///   镜像）与混合操作数链（装载居中）同族识别，语义由推值栈模拟统一
///   求值；
/// - 功能块尾再写下一状态回到 D——CFG 成环，反编译器只能渲染成
///   `while(true) + switch` 状态机形态。
///
/// ## 形态示例（JVMS 视角，合成值）
///
///     D:  iload s
///         lookupswitch { 111111111: A, -222222222: B, default: X }
///     A:  <真实块 A 体>
///         ldc -222222222; istore t; iload t; istore s; goto D
///     B:  <真实块 B 体>; iconst_1; ireturn
///     X:  iconst_0; ireturn
///     入口: ldc 111111111; istore s; goto D
///
/// 改写后（只动 goto 目标，指令集与状态链原样保留）：
///
///     入口: ldc 111111111; istore s; goto A
///     A:   <真实块 A 体>
///          ldc -222222222; istore t; iload t; istore s; goto B
///
/// `istore t; iload t` 中转弹跳与状态写序列是死存噪声，属 store→load
/// 转发清理面（见 mjbase issue #45 残留噪声清单），本 pass 不动。
///
/// ## 栈等价门
///
/// 状态链净栈效应为 0，goto 点栈深 = 前驱块入口栈深；原路径经 switch 派发
/// （switch 弹掉派发值）进入 case 目标，入口栈深同为该值。帧引擎逐前驱
/// 比对 goto 点与 case 入口的到达栈深，不等即整方法放弃。
///
/// ## 放弃清单（全部整方法拒收，不做部分改写）
///
/// - switch 数量 != 1；分发操作数不被形态表解析；
/// - 分发块位于方法首指令（入口边不可静态解析）；
/// - 状态槽有第二读点（相对更新的 s 装载除外）、写点为空、写点后继不是
///   goto D（交叠定值）、状态槽作为两槽值（long/double）的高半槽；
/// - 前驱常量链不被形态表解析且不命中相对更新形态（无法静态求值的前驱）；
/// - 相对模式：入口锚不唯一/不存在、案表目标块内含分支（无直线供源且在
///   迹上）、同写点双值、迹终止后仍有相对写点未定值；
/// - 非前驱分支/switch 目标锚入分发块区间（含块头）；物理直落进块头；
/// - switch 案表目标（含 default）落在分发块区间内（自引用）；
/// - 帧计算失败或任一等价比对不成立。
///
/// ## 事务性与幂等
///
/// 改写阶段只做 goto 目标字段赋值（先全量取可变句柄后统一落笔，任一取柄
/// 失败则零副作用返回）。改写后各前驱 goto 不再指向 D：二次运行时写点
/// 审计因「goto 不指向 D」整族拒收，过程无增量副作用。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`（默认链不收，CLI -p 显式挂点）
#pragma once

#include <functional>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// 控制流反扁平化 pass：通用引擎 + 可注入形态表。
class FlatteningReverser : public mjbase::spi::MethodPass
{
public:
    /// 前驱块尾形态行：给定状态写点位置 w（order[w] 是对状态槽的 int 写入），
    /// 从写点向前解析常量状态 K；命中返回 true 并写出 K。行内自行校验
    /// 指令形态（直存 / 中转弹跳 / 其他变体），引擎不做行内假设。
    using PredFormFn = std::function<bool(const jbc::ir::ClassSession& session,
                                          const jbc::ir::CodeDom&       dom,
                                          const std::vector<jbc::ir::InsnId>& order,
                                          ca::usize w, ca::i32& k)>;

    /// 分发操作数折叠规格：自 switch 向上至多 maxSteps 个
    /// `binop∈binops`，再向下 |binops|+1 个推值（intConst 或 `iload s`
    /// 恰一）——常量全在右、全在左（镜像）与混合操作数链（装载居中）
    /// 由推值栈模拟统一识别。
    struct DispatchSpec
    {
        ca::u8                     maxSteps = 1;
        std::vector<jbc::core::Op> binops{};
    };

    /// 默认前驱形态表：intConst 直存 + 单级中转弹跳（istore t; iload t）。
    static std::vector<PredFormFn> defaultPredForms();

    /// 默认分发规格：单步 {iadd, isub, ixor} 偏移（含零步裸 iload）。
    static DispatchSpec defaultDispatchSpec();

    explicit FlatteningReverser(std::vector<PredFormFn> predForms = defaultPredForms(),
                                DispatchSpec            dispatch  = defaultDispatchSpec());

    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;

private:
    std::vector<PredFormFn> predForms_;
    DispatchSpec            dispatch_;
};

}   // namespace mjbase::pass
