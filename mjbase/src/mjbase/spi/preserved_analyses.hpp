#pragma once

#include <unordered_set>

namespace mjbase::spi {

/// @brief 分析类型的进程内唯一 ID（不依赖 RTTI）。
///
/// 每个分析类型 AnalysisT 都有一个独立的 AnalysisKey<AnalysisT> 静态实例，
/// 其地址在整个进程内唯一（模板静态成员 vague linkage，链接器跨 TU 合并）。
/// 用 `analysisID<AnalysisT>()` 取该地址作为 key，替代 RTTI 的 typeid。
using AnalysisID = const void*;

namespace detail {
template<class AnalysisT>
struct AnalysisKeyStorage
{
    static const char id;
};
template<class AnalysisT>
const char AnalysisKeyStorage<AnalysisT>::id = 0;
}   // namespace detail

/// 取某分析类型的进程内唯一 ID。
template<class AnalysisT>
inline AnalysisID analysisID()
{
    return &detail::AnalysisKeyStorage<AnalysisT>::id;
}

/// @brief 按「分析种类」记录哪些分析在一次变换后仍然有效（对标 LLVM PreservedAnalyses）。
///
/// pass 的 run() 返回它，PassManager 据此调用 AnalysisManager::invalidate 失效未保留的
/// 分析。不是一个全局 bool —— 按 AnalysisID 分别标记，避免「一个全局标志让缓存复用变乱」。
///
/// 两种特殊集合：
///   - all()  : 所有分析保留（纯分析 / 只读 pass 返回这个）。
///   - none() : 全部失效（改了 IR 且无法精确声明保留集时的安全默认）。
///
/// @par 使用约束
/// 变更 pass 必须保守声明：无法证明仍然有效的分析不得调用 `preserve<T>()`。
/// `all()` 只适用于确认没有改变 IR 的只读/分析 pass；`none()` 是发生变换但
/// 无法精确列出保留集合时的安全结果。
///
/// @warning 非线程安全。
class PreservedAnalyses
{
public:
    /// 啥都没改：保留所有分析（也意味着未改动 IR，changed()==false）。
    static PreservedAnalyses all()
    {
        PreservedAnalyses pa;
        pa.all_ = true;
        return pa;
    }

    /// 全部失效：不保留任何分析。伴随 IR 改动（changed()==true）。
    static PreservedAnalyses none()
    {
        PreservedAnalyses pa;
        pa.changed_ = true;
        return pa;
    }

    /// @brief 标记「本 pass 改动了 IR」，与分析保留集合正交。
    ///
    /// 写回（重新序列化字节码 / lower）与子级缓存清理据此触发，不再拿
    /// areAllPreserved() 当改动信号——那把「保留了全部分析」错当成「没改 IR」。
    /// 既改了 IR 又能精确声明保留全部/部分分析的 pass，在 all()/preserve<T>()
    /// 之外显式调用本方法。
    /// @return *this 便于链式（如 `return PreservedAnalyses::all().markChanged();`）。
    PreservedAnalyses& markChanged()
    {
        changed_ = true;
        return *this;
    }

    /// 本次结果是否伴随 IR 改动（驱动写回/子缓存清理，与分析失效解耦）。
    /// none() 与任何非全保留结果默认视为已改动；all() 默认未改动。
    bool changed() const { return changed_ || !all_; }

    /// 声明保住了某类分析（如变换不破坏 CFG 拓扑则 preserve<CFGAnalysis>()）。
    template<class AnalysisT>
    void preserve()
    {
        if (all_) return;   // all 已蕴含全部，无需逐个登记
        preserved_.insert(analysisID<AnalysisT>());
    }

    /// 按 ID 保留（供 AnalysisManager 内部转发）。
    void preserve(AnalysisID id)
    {
        if (all_) return;
        preserved_.insert(id);
    }

    /// 查询某类分析是否被保留。
    template<class AnalysisT>
    bool isPreserved() const
    {
        return isPreserved(analysisID<AnalysisT>());
    }

    /// 按 ID 查询是否被保留。
    bool isPreserved(AnalysisID id) const
    {
        return all_ || preserved_.find(id) != preserved_.end();
    }

    /// 是否保留了全部分析。
    bool areAllPreserved() const { return all_; }

    /// @brief 取与 other 的交集（PassManager 聚合多个 pass 的保留结果）。
    ///
    /// 整体只保留「所有 pass 都保住」的分析：any pass 失效某分析 → 整体失效它。
    void intersect(const PreservedAnalyses& other)
    {
        // 改动信号在聚合中取并集：任一 pass 改了 IR，整体即视为已改动。必须在下面
        // 的早退分支之前传播，否则「保留全部但改了 IR」的 pass 会丢失 changed 位。
        changed_ = changed_ || other.changed_;
        if (other.all_) return;   // other 保留全部，交集 = 自身，不变
        if (all_) {               // 自身保留全部，交集 = other
            all_       = false;
            preserved_ = other.preserved_;
            return;
        }
        // 两边都是具体集合：删掉 other 没保留的
        for (auto it = preserved_.begin(); it != preserved_.end();) {
            if (other.preserved_.find(*it) == other.preserved_.end()) {
                it = preserved_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

private:
    bool                           all_     = false;
    bool                           changed_ = false;
    std::unordered_set<AnalysisID> preserved_;
};

}   // namespace mjbase::spi
