#pragma once

#include <string_view>
#include <vector>

#include "jbc/ir/units.hpp"
#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @file       class_structure_editor.hpp
/// @brief      类级结构编辑原语：成员重排的双面落盘 + 成员增删 + backingIndex 同步。
///
///             pass-contract §1「类级结构变换的双面落盘义务」的 API 化：类级
///             结构 pass 做成员重排/增删时必须同时落 unit() 与 backing 两面
///             （backing 是 writeClass 的序列化源，只改 unit() 不落盘），且
///             重排/增删后必须保证 MethodUnit.backingIndex 与物理位一致。此前
///             重排纪律由每个结构级 pass 手搓（试点 class_member_sorter 的
///             reorderBy 样板），本原语把样板收敛成正式 API——pass 作者调原语，
///             不再各自落盘。
///
///             职责边界：只做骨架结构变换（成员序列的重排与元数据级增删），
///             不触碰既有成员的内容与指令操作数（后者是会话语义面
///             setMember/setClass/… 的职责）；带方法体的成员著作走会话面
///             （addMethod(withCode) + CodeEditor + lowerMethod，见
///             tool_class_combiner / 全局字符串池化 pass 的消费形态），不属
///             本原语。PreservedAnalyses 的声明仍由调用方 pass 负责（结构
///             变换发生 → 至少 none()，未发生 → all()，本原语不替 pass 猜）。

/// 结构编辑原语的失败码。
enum class StructureEditError : ca::u8
{
    Ok = 0,
    Unsupported,     ///< libjbc 报出本原语契约外的错误形态（如 NotModeled 搬移面）。
                     ///< 当前增删通道不可达，出现即库契约破坏——fail-closed 收口，
                     ///< 不静默降级
    BadPermutation,  ///< 置换非法：长度与成员数不符 / 下标越界 / 非双射
    BadSeed,         ///< 新增种子非法：空名/描述符、超 u16 域、会话内 name+desc 重名
    BadHandle,       ///< 删除目标身份无效：下标越界 / 已删除 / 外来会话
    PoolFull,        ///< backing CP 只追加写入失败（容量/类型不符）
};

const char* to_string(StructureEditError e) noexcept;

/// @brief 字段重排：unit 与 backing 两面同排列落盘。
///
/// @param session 目标会话（backing + unit 同生共死，见 pass-contract §1）
/// @param order   新序到旧序的置换：order[newPos] = oldPos；重排后第 newPos 个
///                成员是重排前第 order[newPos] 个成员
/// @return 非法置换 Err(BadPermutation)（会话保持原样，不落半面）；恒等排列
///         Ok() 且零改动（不触碰 backing 的懒解码属性槽）；否则两面落盘后 Ok()。
///         字段无 backingIndex（FieldUnit 不携带），两面同序即一致。
Result<void, StructureEditError> reorderClassFields(jbc::ir::ClassSession&            session,
                                                    const std::vector<ca::usize>& order);

/// @brief 方法重排：unit 与 backing 两面同排列落盘 + backingIndex 全量同步。
///
/// 与字段重排同契约，另担 §1 的 backingIndex 义务：重排后逻辑位与 backing 槽位
/// 重新自然一致，全部 MethodUnit.backingIndex 改指新物理位。成员条目整体移动：
/// MemberInfo 连同属性槽（含 Code 的 lazy span 状态）一起走、内容不被触碰；
/// MethodUnit 携带稳定 MethodId 与已 promote 的 CodeUnit（unique_ptr 转移所有权，
/// 堆上 CodeUnit 地址不变）——既有 CodeUnitRef 保持有效，lower 经同步后的
/// backingIndex 写回自己的物理成员。
Result<void, StructureEditError> reorderClassMethods(jbc::ir::ClassSession&            session,
                                                     const std::vector<ca::usize>& order);

// ── 成员增删（libjbc A3/A4 通道的 mjbase 骨架面）──
//
// 增删语义由 libjbc ClassSession 的 add/remove 入口承担：名字/描述符 intern 进
// backing CP（命中既有池不膨胀，CP 只追加）；预检先于写入（CP 容量、u16 域、
// 成员数、会话内 name+desc 唯一——JVM 对重复成员是加载期错误，挂入时挡掉），
// 失败不留半截成员；unit 与 backing 两面由库通道同步落盘。本原语补 mjbase 侧的
// 错误族收口（LowerError → StructureEditError）与骨架级定位。
//
// 失效账目（调用方须知）：
// - 新增成员获得稳定身份（优先复用最小已删槽位；被复用槽携带自增后的
//   generation，旧句柄结构性失配，不存在「删后复活旧句柄」的 ABA 形态）。
// - 删除成员：该身份立即失配（查询面 nullptr/kNoMethod，变更面 Err）；已
//   promote 的 CodeUnit 随之销毁，消费者手里的 CodeUnitRef 同步作废（与
//   CodeDom::erase 后旧 Insn* 作废同一纪律）；其余成员身份不动，物理位
//   （backingIndex）由库统一平移维护。
// - CP 引用悬挂口径：删除成员不回收其 name/desc/ref 条目（CP 只追加契约，
//   无 GC 面）；体内指向被删成员的既有引用也不改写——结构级 pass 删除成员前
//   须自证无引用残留（引用索引/可达性判定是 reference 域职责，见未使用成员
//   清理家族的准入形态），本原语不做引用检查、不静默清洗悬挂条目。
// - 增删没有恒等形态（挂入即新成员、摘除即失成员），与重排的恒等零改动口径
//   正交。

/// @brief 新增类字段：种子 = {name, desc, access}，返回稳定字段身份。
///
/// 字段无方法体面，四参种子即全量。非法种子 Err(BadSeed)、CP 追加失败
/// Err(PoolFull)；失败时会话保持原样。
Result<jbc::ir::FieldId, StructureEditError> addClassField(jbc::ir::ClassSession& session,
                                                           std::string_view name,
                                                           std::string_view desc, ca::u32 access);

/// @brief 新增类方法：种子 = {name, desc, access}（无 Code 属性），返回稳定
///        方法身份。
///
/// 骨架原语不携带方法体（withCode=false）；abstract/native 成员即此形态。带体
/// 成员的著作走会话面（addMethod(withCode) + promote/CodeEditor/lower），见
/// 本头注职责边界。错误语义与 addClassField 同构。
Result<jbc::ir::MethodId, StructureEditError> addClassMethod(jbc::ir::ClassSession& session,
                                                             std::string_view name,
                                                             std::string_view desc, ca::u32 access);

/// @brief 按下标删除类字段：解析稳定身份后走库删除通道。
///
/// @param index 当前逻辑序下标；越界/身份已失配 Err(BadHandle)。成功后后续
///        字段逻辑前移，backing 两面同步摘除。
Result<void, StructureEditError> removeClassField(jbc::ir::ClassSession& session, ca::usize index);

/// @brief 按下标删除类方法：语义与 removeClassField 同构，另及方法侧失效账目
///        （见「失效账目」注）。
Result<void, StructureEditError> removeClassMethod(jbc::ir::ClassSession& session,
                                                   ca::usize index);

}   // namespace mjbase::spi
