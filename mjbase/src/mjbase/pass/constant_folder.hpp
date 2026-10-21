#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 常量折叠器：把操作数栈上的「常量装载 + 同类型二元运算」极大链
///        折叠为单条常量装载（移植自旧栈 pass/opt 的 ConstantFolder 的
///        可观测子集）。
///
/// @par 策略（链识别算法）
///
/// 对 CodeUnit 指令序列做一趟快照扫描。游标 i 处读到常量装载（输入边界见
/// 下）时开启一条链：用虚拟操作数栈跟踪「本链自身压入的值」，自 i+1 起
/// 逐条消费：
///   - 同类型常量装载 → 压入虚拟栈；
///   - 同类型二元运算 → 虚拟栈深 < 2 时停（该运算要消费链外值，整链到此
///     为止，该指令不消费）；否则弹二合一压回结果；
///   - long 链特例：int 常量装载紧随 LSHL/LSHR/LUSHR → 按对消费
///     （弹 long、取 int 移位量、压回 long 结果，栈深不变）；
///   - 其余指令（非常量、类型混用、op 不支持）→ 链结束。
/// 虚拟栈深回到 1 且已至少消费一个运算时，记该位置为可折叠点 foldEnd；
/// 扫描继续向前扩展（贪心取极大链）。链尾 [i..foldEnd] 整体折叠为一条
/// 常量装载，插在首成员之前；首成员锚定的标签迁移到替换头；其余成员
/// 删除。单趟扫描即达不动点（折叠不产生新的可折叠相邻对——替换头是单条
/// 装载，其后是被跳过的区间）。
///
/// 折叠保持栈效应守恒：链作为整体恰好压入一个与替换头同类型的值，链内
/// 任何被消费的运算都要求栈深 ≥ 2，故 maxStack 不升、maxLocals 不变。
///
/// @par 精确语义依据
///
/// - 整数加减乘与位运算按 u32/u64 回绕后按位读回（JVMS §6.5 *add/sub/mul/
///   and/or/xor/shl/shr/ushr* 的「低 32/64 位」语义；C++ 有符号溢出是 UB，
///   一律经无符号域计算）。
/// - 移位量按 int 掩 5 位 / long 掩 6 位（JVMS §6.5 *shl/shr/ushr*）；
///   ishr 对负值用显式符号扩展公式（避免右移负数的实现定义行为）。
/// - idiv/irem 按 JVMS §6.5 与 JLS §15.17.2/§15.17.3：向零截断、余数符号
///   跟随被除数（C++11 起 / 与 % 对正除数即此语义，守卫后无 UB）。
/// - float/double 的 + - * / 用 C++ float/double 运算：x64 SSE 与 JVM 的
///   IEEE-754 基本运算逐位一致（实证前提：编译目标 x64、未开 fast-math；
///   项目 C++17 无快速数学开关）。NaN/Inf 按位物化为常量。fdiv/ddiv 除
///   数为 0 不抛异常（IEEE 产生 ±Inf/NaN），照常折叠。
/// - idiv/irem 除数为常量 0 → 不折叠（保留运行期
///   ArithmeticException，JVMS §6.5 *idiv/irem*「除数不能为 0」）。
/// - int/long 的 MIN / -1 除法取模 → 不折叠。任务书红线要求保留不折；
///   按字节码实际语义注明：idiv 对 INT_MIN/-1 结果为 INT_MIN、irem 为 0
///   （evaluator.cpp divI/remI 同款回落）， HotSpot 不抛异常——此处按
///   红线保守不折，语义差异留档供团队后续决策。
///
/// @par 输入边界（常量装载准入集）
///
/// - int：ICONST_M1..ICONST_5 / BIPUSH / SIPUSH / LDC/LDC_W(Int)
///   （method_rewrite_util::intValueOf）。
/// - long：LCONST_0/1 / ldc 族(Long)（method_rewrite_util::longValueOf）。
/// - float：FCONST_0/1/2 / LDC/LDC_W(Float)（本地读取，util 无 float 臂）。
/// - double：DCONST_0/1 / ldc 族(Double)（method_rewrite_util::doubleValueOf）。
/// 任务书列的最小集（iconst/bipush/sipush/ldc(int,float)/ldc2_w(long,
/// double)）之外，LCONST/FCONST/DCONST 与 ldc 族的跨形态读取是超集——
/// 它们同是精确已知常量，且与旧栈 get*Value 的可观测面对齐。
/// 字符串/Class/MethodHandle 等装载不进链。
///
/// @par 保守停止清单（任一命中 → 整链不动，禁止部分折叠）
///
/// - 任一操作数非常量、类型混用（链内出现异类型值）；
/// - op 不在支持集（含 long 移位的 int 操作数非常量——general 路径拒绝
///   LSHL/LSHR/LUSHR，只认「int 常量 push + long 移位」成对形态）；
/// - div/rem 除数为常量 0；int/long 的 MIN/-1；
/// - 链中段成员（i+1..foldEnd）前置边界有标签锚定（分支目标/异常表边界
///   ——旧栈 LabelNode 阻断语义的 CodeDom 等价，见
///   method_rewrite_util::labelsAnchorAt 头注）；
/// - CP 容量预检失败（cp.size() + 待追加槽数 > 0xFFFF）或 CP 物化失败；
/// - 插入/删除编辑失败（带恢复：回迁标签、删除已插替换头，本链跳过）。
///
/// @par 物化策略（输出形态，对齐）
///
/// - int：-1..5 → ICONST；-128..127 → BIPUSH；-32768..32767 → SIPUSH；
///   其余 → LDC(internInteger，去重)。
/// - long：0/1 → LCONST_0/1；其余 → LDC2_W(cp.addLong，只追加不去重)。
/// - float：+0.0（signbit 检查，负零走 LDC）→ FCONST_0；1.0 → FCONST_1；
///   2.0 → FCONST_2；其余 → LDC(cp.addFloatBits)。
/// - double：+0.0 → DCONST_0；1.0 → DCONST_1；其余 → LDC2_W(cp.addDoubleBits)。
/// LDC/LDC_W 由 CodeEditor::loadConst 按索引宽度选型。CP 只追加契约：
/// 索引永不重编号；internInteger 先去重，addLong/addFloatBits/
/// addDoubleBits 直接追加（ClassSession 无对应 intern 面，见挂账）。
///
/// @par 失败语义
///
/// 本 pass 不吞异常；编辑失败走恢复路径跳过本链，扫描继续。CP 物化在
/// 容量预检之后进行，失败（抛异常）归管线容错档位处置。异常表边界引用
/// 成员指令表现为 Boundary 标签锚定，由链中段标签守卫拦截；首成员的
/// 边界标签随标签迁移保居链首位置，异常区间语义不变。
///
/// @par PreservedAnalyses
///
/// 发生折叠返回 none()（指令序列与 CP 均变），否则 all()。本 pass 不注册
/// 也不消费任何分析结果（mam 未使用）。
///
/// @par 标签
/// - `scope:method`
/// - `action:fold`
/// - `stability:opt-in`
class ConstantFolder : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
