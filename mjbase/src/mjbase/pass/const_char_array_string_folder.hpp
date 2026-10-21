#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 常量 char 数组构造的 `new String(char[])` 整窗折叠为字符串 ldc
///        （移植自旧栈 pass/opt/const_char_array_string_folder，旧栈
///        deobfuscate/recover 的 opt-in 件，与已迁的 CharArrToStrSimplifier
///        互补：那件把裸 char[] 字面构造改写为 ldc+"toCharArray"，保留数组
///        语义；本件把整个 `new String(char[])` 构造串折叠为最终字符串）。
///
/// 语义（与）：只处理完整、连续的形状——`new java/lang/String`
/// + `dup` + 常量长度表达式 + `newarray char` + 若干 `dup`/常量下标表达式/
/// 常量值表达式/`castore` + `invokespecial java/lang/String.<init>([C)V`。
/// 长度、下标、每个元素值都必须是可证明的整型常量表达式（`ldc(int)`/
/// `iconst`/`bipush`/`sipush` 与 `iadd`/`isub`/`imul`/`ixor`/`iand`/`ior`/
/// `ishl`/`ishr`/`iushr`/`ineg`/`i2c`/`i2b`/`i2s` 的组合，不含可能陷入的
/// `idiv`/`irem`），表达式窗口内不允许任何其他指令混入，且求值栈严格清空
/// （栈残留即失配）。任意别名、逃逸、未覆盖下标、越界下标、非 `T_CHAR`
/// 数组、非法 surrogate 都保持原指令整组不动。
///
/// 下标允许任意次序与同下标二次写（最后一次生效，Java 数组语义），但全部
/// 下标必须恰好覆盖 `[0, length)`（继承）。
///
/// 替换结果：整窗折叠为单条 `ldc "<解码内容>"`（池下标超出 u1 域升格
/// `ldc_w`，对齐 ClassSession::setString 的宽度陷阱口径）；唯一 String 常量
/// 经 `ClassSession::internString` 追加进池（CP 只追加契约）。首成员
/// （`new`）锚定的标签迁移到新 ldc（旧栈 LabelNode 留原位指向折叠结果的
/// 落点语义）；其余成员在守卫下均无锚定标签，`eraseBatch(kRejectIfUsed)`
/// 原子删除（批内互指不算引用）。maxStack/maxLocals 不变（整窗栈效应守恒：
/// 引用进引用出，虚拟帧面无可观察变动）。
///
/// 形态守卫对齐说明：旧栈以 LABEL/LINE/FRAME 节点为指令序列成员，窗内
/// 出现即失配；新栈 CodeDom 无节点类标签，等价面为「窗内任一成员锚定了
/// incoming 标签即失配」（labelsAnchorAt 守卫，覆盖进入 arm 的跳入面）；
/// 异常表区间引用由 erase 的 use 保护兜底。旧栈 LINE/FRAME 节点形态在新栈
/// promote 语义下不是指令序列成员，无守卫需求。刻意避开的手工形态（窗内
/// 锚定标签的畸形构造）不入 golden 对拍，由本 pass 单测钉死保守口径。
///
/// @par 标签
/// - `scope:method`
/// - `action:fold`
/// - `stability:opt-in`
class ConstCharArrayStringFolder : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
