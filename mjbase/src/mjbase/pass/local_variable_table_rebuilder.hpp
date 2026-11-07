/// @file        local_variable_table_rebuilder.hpp
/// @brief       LVT 重建：方法体变换后按存活区间重算 LocalVariableTable，
///              或按显式丢弃口径产出无 LVT 的干净产物。
///
/// 内联、死代码清理、跳转重排等变换会让既有 LVT 与新字节码漂移：行范围
/// 悬垂（startPc/length 越界或罩住已删指令）、槽位失配、名称与实际存储
/// 矛盾。lower 路径的 relocation 只做端点吸附（平移旧表），本 pass 提供
/// 「重算或丢弃」的一等口径——由调用方显式声明，不停在「留着旧表」。
///
/// 两种口径（LvtRebuildMode，由调用方声明）：
///   - Rebuild（默认）：按变换后存活区间重算槽位与范围表。变量生命周期
///     切分与 LocalVariableAnnotator 同引擎（帧数据流按「槽位 + 赋入类型」
///     切段），但契约是保真而非规范化：名称/描述符沿用原表可验证项——
///     原名经 lvtNameTrustworthy 形态判定且该槽可信名唯一才沿用（复用段
///     落原名_2/原名_3 序号），槽上可信名 ≥2（作用域复用、位置对应不可证）
///     或不可信时按显示策略落 var\<槽位\>（与 lvt_display_policy 呈现口径
///     一致）；描述符只在「推断类型与声明矛盾」时才被推断值替换，原表
///     Object 槽存引用类型照旧保 Object（javac 擦除槽形态），不做类型提升。
///   - Drop：摘除 LVT 与 LVTT，产出无调试表的干净产物。
///
/// 与 LocalVariableAnnotator 的边界：Annotator 是规范化件——无条件 varN
/// 改名 + 推断类型覆盖声明（反编译精度优先，原名不保）。本 pass 是保真件
/// ——范围表重算但原名/原描述符在可验证范围内逐字保留（deobf 证据不劣化），
/// var 形态只是不可验证项的兜底。两者可组合：Annotator 产物是本 pass 的
/// 不动点（varN 名可信、desc 与推断一致，重跑本 pass 零改动）。
///
/// LVTT 两口径下一律摘除：重建切分后行集与原表不再一一配对，泛型签名的
/// 范围/槽位无法重新验证（与 Annotator 丢弃 LVTT 同一判据）。
///
/// 重建结果与 javac -g 语义兼容：可信名不劣化、cat2 槽位按物理槽跳号
/// （long/double 行占首槽，无补位行）、范围表不越界（越界行重算回界内，
/// 全部行失效时连表摘除）。
///
/// 幂等：重算结果与现表逐行一致且无 LVTT 时零改动（PA all），二次运行
/// 严格 no-op——不同于 Annotator 的无条件重写。
///
/// 载体与边界：scope:class（变换后处理路径挂在类管线、方法变换之后）；
/// 只编辑 backing Code 嵌套属性表（LVT/LVTT 两槽），指令载荷、异常表、
/// StackMapTable、maxes、LNT 一律不动，父 Code 槽置脏驱动重编码——
/// MetaInfoRemover/LineNumberRemover 同款显式有界破例。帧计算 Strict
/// 失败降 BestEffort（本 pass 只有单会话层次图，跨类合并需求更弱），
/// 仍失败则仅按存储指令推导基本类型，引用类型落声明或 Object。
///
/// 本 pass opt-in：default pass registry 未注册、默认链不收，调用方经
/// 显式构造实例或自定义管线挂点驱动。
#pragma once

#include <string>

#include "jbc/ir/units.hpp"

#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 变换后 LVT 处理口径。
enum class LvtRebuildMode
{
    /// 按变换后存活区间重算并重写 LVT（名称/描述符沿用原表可验证项）。
    Rebuild,
    /// 摘除 LVT 与 LVTT，产出无调试表的干净产物。
    Drop,
};

/// @brief LVT 重建选项。
struct LocalVariableTableRebuilderOptions
{
    /// @brief 处理口径；默认重建。
    LvtRebuildMode mode = LvtRebuildMode::Rebuild;
    /// @brief var 形态兜底名前缀，最终名 `<prefix><槽位>[_<序号>]`。
    std::string prefix = "var";
};

/// @brief 变换后 LVT 重建/丢弃 pass（类级）。
///
/// @par 标签
/// - `scope:class`
/// - `action:annotate`
/// - `stability:opt-in`
class LocalVariableTableRebuilder final : public mjbase::spi::ClassPass
{
public:
    /// @brief 运行统计（测试与工具观测面；pass 不因个别方法失败中断）。
    struct Stats
    {
        ca::usize methods        = 0;   ///< 带 Code 且成功升格的方法
        ca::usize rebuiltMethods = 0;   ///< 重建口径下表被重写的方法
        ca::usize droppedMethods = 0;   ///< 丢弃口径下表被摘除的方法
        ca::usize rows           = 0;   ///< 重建写入的行数
        ca::usize carriedNames   = 0;   ///< 沿用原表可信名的行
        ca::usize varNames       = 0;   ///< 落 var 形态兜底名的行
        ca::usize strictFailures = 0;   ///< Strict 帧失败、降级 BestEffort 的次数
        ca::usize skippedMethods = 0;   ///< 升格/Code 解码失败跳过的方法
    };

    explicit LocalVariableTableRebuilder(LocalVariableTableRebuilderOptions options = {});

    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;

    const Stats& stats() const noexcept { return stats_; }

private:
    LocalVariableTableRebuilderOptions options_;
    Stats                              stats_;
};

}   // namespace mjbase::pass
