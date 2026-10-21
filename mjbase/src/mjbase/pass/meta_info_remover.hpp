#pragma once

#include "jbc/ir/units.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @defgroup meta_info_cleanup meta 元数据清理家族。
///
/// 家族语义面 = 类/字段/方法三层的非语义元数据：SourceFile 与
/// SourceDebugExtension、
/// Signature、Runtime{Visible,Invisible}Annotations、Runtime{Visible,Invisible}-
/// TypeAnnotations、Runtime{Visible,Invisible}ParameterAnnotations、
/// MethodParameters、AnnotationDefault、Code 嵌套 LNT/LVT/LVTT 与非法
/// access 标志位。mjbase 重定型到 ClassSession 载体，落地两条载体重定型纪律：
///
/// - **属性表落点**：三层属性表在 ClassUnit 无模型，唯一真实落点是 backing
///   的 AttrSlot 表（backing 是 writeClass 的序列化源）——属性槽摘除/注解体
///   过滤直接落 backing；access 标志位在 unit() 有模型，按 pass-contract §1
///   双面落盘义务两面同步（unit() + backing）。
/// - **Code 嵌套属性表的破例（仅两成员）**：MetaInfoRemover（LNT/LVT/LVTT
///   清除）与 IllegalTypeAnnotationRemover（code 目标 type 注解过滤）改动
///   全在 Code 嵌套属性表：前者整槽摘除 LNT/LVT/LVTT 与
///   Runtime{Visible,Invisible}TypeAnnotations（全表删除不产生 pc 悬垂面），
///   后者对嵌套 TypeAnnotations 槽做与成员级同规则的槽内过滤；两者改动均
///   指令载荷/异常表/maxes 不被触碰，不经 lower，且父 Code 槽连带置脏驱动
///   重编码。这是对「类级 pass 不直接触碰 backing 的 Code 属性」字面边界的
///   显式、有界破例：破例面 = 属性表编辑（非指令、非 pc 编辑），仅此两处，
///   家族其余成员不触碰 Code。
///
/// 三处保守跳过并挂账的能力面：record 组件的注解（Record 组件嵌套属性面
/// libjbc 已立 typed、组件 AttrSlot 表可达，清空面待其接线批）；
/// Runtime{Visible,Invisible}Parameter-
/// Annotations 的槽内过滤（SpecificAnnotationRemover——解码为 Raw，槽内
/// 过滤需注解表读取面；MetaInfoRemover 对该两槽是整槽摘除，照常落面）；
/// code 目标形态普通注解（JVMS 无此挂载点，如出现保持原样）。
///
/// 家族内注解过滤一律**槽内过滤**（解码 AttrBody → 按描述符谓词删条目 →
/// 槽置脏），槽内删空时连槽摘除。解析失败、属性形状不符时保守保持原样
/// （fail-closed，不产出半截改写）。
///
/// PreservedAnalyses 全家族条件语义：发生改写返回 none()，未发生返回 all()。
/// 一处显式偏离（sorter 试点同款）：本家族严格按声明条件语义落地——
/// 未改写的类不标记改动、不白付重序列化。
///
/// 全家族 `opt-in`，不得进入默认优化链。golden 对拍见
/// doc_ai/pass-golden-pipeline.md（tree 道，CLASSFILE_REENCODE 声明类目 +
/// meta 语义矩阵替代断言）。
/// @{

/// @brief 清除类/字段/方法三层已列明的 debug/framework metadata（catch-all）。
///
/// 摘除面：类级 SourceFile/SourceDebug + 类/字段/方法三层
/// Runtime{Visible,Invisible}Annotations 与 Runtime{Visible,Invisible}Type-
/// Annotations（成员级槽内过滤 + Code 嵌套槽整槽摘除）+ 方法级
/// Runtime{Visible,Invisible}ParameterAnnotations、MethodParameters、
/// AnnotationDefault（整槽摘除）+ Code 嵌套 LNT/LVT/LVTT。保持不变：
/// Signature、ConstantValue、Exceptions、未知属性（Raw）与一切指令语义——
/// 载体按头注契约保持未知属性原样。record 组件注解面挂账（见家族头注）。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class MetaInfoRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 移除全部字段的 `ACC_FINAL`。
///
/// 只清标志位，不处理 ConstantValue 与 final 语义；两面落盘（unit() +
/// backing）。无 final 字段时零改动。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class FinalRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 移除类/方法/字段的 synthetic 及方法的 bridge 标志。
///
/// 只清访问标志位，不推断编译器语义；两面落盘。无命中时零改动。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class SyntheticBridgeRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 移除类级 `@kotlin/Metadata` 与方法/字段上的 jetbrains 三注解。
///
/// 槽内过滤：类级只查 RuntimeVisibleAnnotations 中描述符为 `Lkotlin/Metadata;`
/// 的条目；方法/字段查
/// Runtime{Visible,Invisible}Annotations 中 `Lorg/jetbrains/annotations/` 的
/// NotNull/JvmName/Nullable 三件。解析失败保持原样。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class KotlinMetadataRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 清除 SourceFile/SourceDebugExtension。
///
/// 只摘类级两槽，不改指令与 Code；两属性缺席时零改动。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class SourceInfoRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 清除类/方法/字段三层的 Signature。
///
/// 只摘 Signature 槽，不改 descriptor 与指令；三层全无 Signature 时零改动。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class SignatureRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 清理三层 Runtime{Visible,Invisible}Annotations 中描述符形状非法的注解。
///
/// 非法判定：空描述符 / 码点长度 ≤ 3 / 不以 `L` 开头 /
/// 不以 `;` 结尾 / 含换行、NUL、空格。槽内过滤，删空连槽摘除；不查 type
/// annotation 与参数注解。
///
/// @par 标签
/// - `scope:class`
/// - `action:sanitize`
/// - `stability:opt-in`
class IllegalAnnotationRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 清理三层中解析器拒绝的非法 generic signature。
///
/// 合法性按 libjbc signature 解析器判（类/方法用类/方法签名文法，字段用
/// 类型签名文法）；
/// 仅摘解析失败的 Signature 槽，合法签名原样。
///
/// @par 标签
/// - `scope:class`
/// - `action:sanitize`
/// - `stability:opt-in`
class IllegalSignatureRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 清理三层 Runtime{Visible,Invisible}TypeAnnotations 中描述符形状
///        非法的 type annotation 条目。
///
/// 形状判定与 IllegalAnnotationRemover 同规则（施于条目的注解类型描述符）；
/// 槽内过滤，删空连槽摘除。覆盖成员级与 Code 嵌套槽（code 目标形态 cast/
/// instanceof/new/localvar）。不查普通注解槽。
///
/// @par 标签
/// - `scope:class`
/// - `action:sanitize`
/// - `stability:opt-in`
class IllegalTypeAnnotationRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 清除形状非法的 SourceFile/SourceDebugExtension。
///
/// 非法判定：码点长度 > 100 / 不以 `.java` 结尾 / 基名
/// 为空 / 基名首字节为数字 / 基名含非 ASCII 字母数字字节。SourceFile 载荷
/// 经 CP Utf8 解析，SourceDebugExtension 载荷为内联 Utf8 字节（两属性编码
/// 形态不同，读面分别处理）；合法值原样保留。
///
/// @par 标签
/// - `scope:class`
/// - `action:sanitize`
/// - `stability:opt-in`
class IllegalSourceInfoRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 清理三层属性表中未知（非标准）属性槽。
///
/// 判定按属性名：名不在 JVMS 标准属性名封闭集内的槽摘除（名字判定与
/// libjbc 的 Raw 解码类等价——libjbc 未建模属性解码即 Raw；guard 集额外
/// 罩住 ConstantValue/Exceptions/SourceFile 等标准但未建模属性，防止误伤
/// 语义面）。Code 嵌套属性不在本 pass 面。
///
/// @par 标签
/// - `scope:class`
/// - `action:sanitize`
/// - `stability:opt-in`
class UnknownAttributeCleaner : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 清除类/方法/字段/参数上明确非法的 access 标志位。
///
/// 非法位表：类
/// {STATIC, VOLATILE, VARARGS, NATIVE, STRICT}；方法 {INTERFACE, ANNOTATION,
/// ENUM, MODULE}（RECORD 位 0x10000 超 u16 载体恒零，无面）；
/// 字段 {SUPER, NATIVE, INTERFACE, ABSTRACT, STRICT, ANNOTATION, MODULE}，
/// 另有规则：类非 enum 时清字段的 ENUM 位；参数（MethodParameters 属性载荷）
/// 按 ILLEGAL_PARAM_ACCESS 掩码清位、FINAL/SYNTHETIC/MANDATED 幸存——
/// MethodParameters 在 libjbc 为 Raw 槽，按 JVMS 4.7.24 布局（u2 count）或
/// 遗留 u1 count 布局校验后
/// 原位改写 access u16，其余形状保守跳过。全部两面落盘（unit() + backing）。
///
/// @par 标签
/// - `scope:class`
/// - `action:sanitize`
/// - `stability:opt-in`
class IllegalAccessRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

/// @brief 移除指定描述符（黑名单）或形状非法的注解。
///
/// 槽内过滤，谓词 = IllegalAnnotationRemover 同款形状判定 ∪ 黑名单描述符
/// 封闭集（origincloud/jvavx/jetbrains/skidonium 族；原表重复条目保留——
/// 集合语义，重复不影响判定）。查类/字段/方法的 visible/invisible 注解
/// 两族；不查 type annotation。参数注解两槽不做槽内过滤（libjbc 未建模
/// 该族，解码 Raw）——保守跳过并挂账（见家族头注）。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class SpecificAnnotationRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

}   // namespace mjbase::pass
