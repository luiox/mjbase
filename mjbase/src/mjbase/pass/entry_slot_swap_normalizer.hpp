/// @file        entry_slot_swap_normalizer.hpp
/// @brief       入口参数槽置换还原：删除混淆器插入的槽位搬移序言并把局部访问映射回原槽。
///
/// 这类混淆在方法入口插入槽位搬移，使 this、形参与局部变量在物理槽上错位。
/// 本 pass 识别两种形态：
///   - 搬移置换：连续 load 参数槽、再连续 store 回同一组槽（语料以 4 条
///     `load,load,store,store` 为主，this 与另一槽互换；也有 int/ref/float
///     混合与 `(0,k)` 形态）。序言后整个方法体按置换后的槽位读写。
///   - this 别名：`load 0; store k` 两条前缀，把 this 暂存到槽 k 后 slot 0
///     被复用为局部变量（语料 814 例）。方法体对槽 k 的访问实际是 this，
///     对槽 0 的访问是原局部变量，等价于只对 0 与 k 做置换。
///
/// 搬移置换的判定（只认这一段，不猜更长范围）：
///   (1) 方法体开头是连续的局部 load 段（≥2 条），紧跟等长的连续 store 段；
///   (2) 符号执行整段后操作数栈为空，且「目的槽 → 来源槽」构成槽集合上的
///       非恒等双射（普通拷贝会留下重复来源或未覆盖槽，不满足）；
///   (3) 序言内没有任何标签锚定（分支目标/异常表边界），保证删除不改控制流。
///
/// this 别名的判定：前缀只能是两条引用槽访问；序言内无标签；方法体不含
/// jsr/ret；对槽 0 与槽 k 的访问都是单槽引用；宽值不占用槽 k；槽 0 的首次
/// 方法体访问必须是写（删除前缀后槽 k 不再保留 this 副本），且该写必须在
/// 每条控制流路径上支配所有槽 0 读取。方法体内存在同槽引用/基本类型混用时
/// 跳过：这类槽在 Strict 帧汇合点会判类型冲突，原类靠自带 SMT 容忍，重算
/// 会失败。
///
/// 变换：先整批删除序言（分支 use 重定向到后继），再把方法体内每条局部
/// 访问的槽号改为该槽在序言前的来源槽；紧凑操作码改写为显式形态。long/
/// double 的双槽作为一个值整体映射。
///
/// @warning 本地槽类型随变换改变，带分支的方法必须由管线注入 ITypeResolver
///          （PassPipelineOptions::typeResolver）重算 Strict 帧并再生
///          StackMapTable；不接线会留下按旧槽类型编码的帧。
#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 入口槽置换还原（方法级）。
///
/// @par 标签
/// - `scope:method`
/// - `action:normalize`
/// - `stability:opt-in`
class EntrySlotSwapNormalizer final : public mjbase::spi::MethodPass
{
public:
    struct Stats
    {
        ca::usize normalized         = 0;   ///< 已还原的方法数
        ca::usize normalizedAliases  = 0;   ///< 其中 this 别名形态的数量
        ca::usize skippedErase       = 0;   ///< 序言删除被标签/异常表边界拒绝的次数
    };

    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&           scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;

    const Stats& stats() const noexcept { return stats_; }

private:
    Stats stats_;
};

}   // namespace mjbase::pass
