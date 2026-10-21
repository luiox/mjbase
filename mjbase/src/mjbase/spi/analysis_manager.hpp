#pragma once

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/spi/preserved_analyses.hpp"

namespace mjbase::spi {

/// @brief 某作用域 IR 单元的分析结果缓存（对标 LLVM AnalysisManager）。
///
/// 惰性求值：第一次 getResult<A>(unit) 才运行分析 A 并缓存，之后命中缓存。
/// 失效：PassManager 在每个 pass 后调用 invalidate(unit, PA)，删掉未被 PreservedAnalyses
/// 保留的分析结果。下次 getResult 再重算。
///
/// 分析类型契约（AnalysisT）：
/// @code
///   struct CFGAnalysis {
///       using Result = ControlFlowGraph;                       // 结果类型
///       Result run(IRUnitT& unit, AnalysisManager<IRUnitT>& am); // 计算（可依赖其他分析）
///   };
/// @endcode
/// AnalysisT 须可默认构造（注册时存一个实例）。
///
/// 不依赖 RTTI：分析种类用 analysisID<AnalysisT>()（静态成员地址）做 key；结果用
/// 概念基类 + 模板模型做类型擦除，取出时按模板参数 static_cast（ID 匹配保证类型正确）。
///
/// @tparam IRUnitT 作用域 IR 单元类型（ResourceContainer / ClassSession / MethodScope）。
/// @warning 非线程安全。
///
/// @par 失败和所有权
/// 调用 `getResult` 前必须完成 `registerAnalysis`；未注册分析属于调用方契约错误，稳定抛出
/// `std::logic_error`，不会空指针崩溃或静默降级。重复注册幂等，不覆盖既有模型或缓存。
/// 分析结果由管理器内部缓存并在 `invalidate/clear` 时释放，pass 不得保存跨失效周期的结果
/// 引用；pinned 结果不受 unit cache 的 `invalidate/clear/clearAll` 影响，但只在所属管理器
/// 生命周期内有效；借用外部指针的 pinned 结果必须由注入方在借用作用域结束时调用
/// `clearPinned` 或恢复先前值。
///
/// @par 注入口形态
/// 不设 typeResolver 注入口：libjbc 的写回面（lowerMethod relocation 契约 +
/// writeClass copy-through）不需要外部 resolver；公共父类求解由
/// mjbase::spi::CommonSuperClassResolver 独立供给（见 pass-contract.md），出现
/// 重算 StackMapTable 的 pass 时再经管线注入。
template<class IRUnitT>
class AnalysisManager
{
public:
    /// 传递失效查询器（完整定义在类尾部）。分析的 dependencyInvalidated 收它的引用，
    /// 经 inv.invalidate<Dep>() 查询依赖是否失效。构造受限（仅 AnalysisManager 可创建）。
    class Invalidator;

    explicit AnalysisManager(ca::str::Utf8StringArena& arena)
        : arena_(&arena)
    {}

    /// 作业级 arena（单 arena 贯穿作业，pass 经 am.arena() 取，无需 run 第三参数）。
    ca::str::Utf8StringArena& arena() { return *arena_; }

    /// @brief 注册一个分析类型（程序启动时一次性 bootstrap）。
    /// 须在任何 getResult<AnalysisT> 之前注册，否则 `getResult` 抛出 `std::logic_error`。
    /// 同一类型重复注册是幂等操作。
    template<class AnalysisT>
    void registerAnalysis()
    {
        const auto id = analysisID<AnalysisT>();
        if (analyses_.find(id) != analyses_.end()) return;
        analyses_[id] = std::make_unique<AnalysisModel<AnalysisT>>();
    }

    /// 取分析结果（惰性：未缓存则计算并缓存；已缓存直接返回引用）。
    template<class AnalysisT>
    typename AnalysisT::Result& getResult(IRUnitT& unit)
    {
        AnalysisID id        = analysisID<AnalysisT>();
        auto&      unitCache = cache_[&unit];
        auto       it        = unitCache.find(id);
        if (it == unitCache.end()) {
            auto ait = analyses_.find(id);
            // 未注册分析必须稳定失败，不能把空迭代器当作分析模型解引用。
            if (ait == analyses_.end()) {
                throw std::logic_error("AnalysisManager::getResult: analysis is not registered");
            }
            ResultConcept* computed = ait->second->run(unit, *this);
            it = unitCache.emplace(id, std::unique_ptr<ResultConcept>(computed)).first;
        }
        return static_cast<ResultModel<AnalysisT>*>(it->second.get())->result;
    }

    /// 仅查缓存，不触发计算（未算/已失效返回 nullptr）。
    template<class AnalysisT>
    typename AnalysisT::Result* getCachedResult(IRUnitT& unit)
    {
        auto cit = cache_.find(&unit);
        if (cit == cache_.end()) return nullptr;
        auto it = cit->second.find(analysisID<AnalysisT>());
        if (it == cit->second.end()) return nullptr;
        return &static_cast<ResultModel<AnalysisT>*>(it->second.get())->result;
    }

    /// 某分析类型是否已注册。
    template<class AnalysisT>
    bool isRegistered() const
    {
        return analyses_.find(analysisID<AnalysisT>()) != analyses_.end();
    }

    /// @brief 按 PreservedAnalyses 失效某 unit 的缓存：未被保留、或其依赖被失效的分析结果删除。
    ///
    /// PassManager 在每个 pass 后调用。areAllPreserved() 时整体跳过（纯分析/只读 pass）。
    /// 经 Invalidator 做传递失效（对标 LLVM）：派生分析即使被 PreservedAnalyses 显式
    /// 保留，只要其依赖被失效，也会一并失效，避免陈旧的派生结果被保留。叶子分析退化为
    /// 「自身 ID 未保留即失效」。
    void invalidate(IRUnitT& unit, const PreservedAnalyses& pa)
    {
        if (pa.areAllPreserved()) return;
        auto cit = cache_.find(&unit);
        if (cit == cache_.end()) return;
        auto&       unitCache = cit->second;
        Invalidator inv(unitCache);
        // Pass 1：为每个缓存结果计算失效判定（Invalidator 带 memo，递归查依赖）。
        for (auto& entry : unitCache) {
            inv.invalidateID(entry.first, unit, pa);
        }
        // Pass 2：删除判定为失效的结果（与 Invalidator 查询分离，避免遍历中改动缓存）。
        for (auto it = unitCache.begin(); it != unitCache.end();) {
            if (inv.wasInvalidated(it->first)) {
                it = unitCache.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    /// 清空某 unit 的全部缓存（unit 被删除/重建时调用）。
    void clear(IRUnitT& unit) { cache_.erase(&unit); }

    /// @brief 直接注入某 unit 上的分析结果（绕过惰性计算）。
    ///
    /// 用于 Adaptor 在运行前给 Proxy 分析注入子级 AM 引用。
    /// 已缓存的结果会被覆盖。
    template<class AnalysisT>
    void setResult(IRUnitT& unit, typename AnalysisT::Result result)
    {
        AnalysisID id     = analysisID<AnalysisT>();
        cache_[&unit][id] = std::make_unique<ResultModel<AnalysisT>>(std::move(result));
    }

    /// 清空全部缓存（不动注册表）。
    void clearAll() { cache_.clear(); }

    /// @brief 设置一个「钉子」结果（per-AM，不受 invalidate/clear 影响）。
    ///
    /// 用于跨层 Proxy 等基础设施分析。钉子结果一旦设置，在整个 AM 生命周期内
    /// 都不会被失效（PassManager 的 invalidate 调用会跳过它）。
    /// 主要用于：MethodAnalysisManagerProxy 等必须常驻的分析。
    template<class AnalysisT>
    void setPinned(typename AnalysisT::Result result)
    {
        AnalysisID id = analysisID<AnalysisT>();
        pinned_[id]   = std::make_unique<ResultModel<AnalysisT>>(std::move(result));
    }

    /// 取钉子结果（同 setPinned 配套，未设置时返回 nullptr）。
    template<class AnalysisT>
    typename AnalysisT::Result* getPinned()
    {
        AnalysisID id = analysisID<AnalysisT>();
        auto       it = pinned_.find(id);
        if (it == pinned_.end()) return nullptr;
        return &static_cast<ResultModel<AnalysisT>*>(it->second.get())->result;
    }

    /// 查询 pinned 结果是否存在，不触发计算。
    template<class AnalysisT>
    bool hasPinned() const
    {
        return pinned_.find(analysisID<AnalysisT>()) != pinned_.end();
    }

    /// 撤销一个 pinned 结果；用于 adaptor 离开借用作用域时解除代理/上下文注入。
    template<class AnalysisT>
    void clearPinned()
    {
        pinned_.erase(analysisID<AnalysisT>());
    }

private:
    // ── 类型擦除：结果 ──
    /// 缓存结果的概念基类（统一持有，按需 static_cast 回具体模型）。
    struct ResultConcept
    {
        virtual ~ResultConcept() = default;
        /// 判定本结果在给定 PreservedAnalyses 下是否应失效（true=删除）。inv 供派生分析
        /// 查询其依赖是否被失效，从而实现传递失效。
        virtual bool invalidate(IRUnitT& unit, const PreservedAnalyses& pa, Invalidator& inv) = 0;
    };

    /// 检测 AnalysisT 是否声明了可选的静态依赖失效钩子
    /// `static bool dependencyInvalidated(IRUnitT&, const PreservedAnalyses&, Invalidator&)`。
    /// 有依赖的分析（如 ResolvedCallGraph 依赖 CallGraph）声明它；叶子分析不声明。
    /// 依赖声明挂在 Analysis 包装类型上（框架 facing），领域 Result 数据类保持零框架耦合。
    template<class A, class = void>
    struct HasDependencyInvalidated : std::false_type
    {};
    template<class A>
    struct HasDependencyInvalidated<
        A, std::void_t<decltype(A::dependencyInvalidated(
               std::declval<IRUnitT&>(), std::declval<const PreservedAnalyses&>(),
               std::declval<Invalidator&>()))>> : std::true_type
    {};

    template<class AnalysisT>
    struct ResultModel : ResultConcept
    {
        using ResultT = typename AnalysisT::Result;
        explicit ResultModel(ResultT r)
            : result(std::move(r))
        {}
        bool invalidate(IRUnitT& unit, const PreservedAnalyses& pa, Invalidator& inv) override
        {
            // 自身未被保留 → 失效（叶子分析的唯一判据）。
            if (!pa.isPreserved(analysisID<AnalysisT>())) return true;
            // 自身被保留：若声明了依赖，任一依赖被失效则本分析连锁失效（传递失效）。
            if constexpr (HasDependencyInvalidated<AnalysisT>::value) {
                return AnalysisT::dependencyInvalidated(unit, pa, inv);
            }
            return false;
        }
        ResultT result;
    };

    using UnitResultMap = std::unordered_map<AnalysisID, std::unique_ptr<ResultConcept>>;

    // ── 类型擦除：分析计算器 ──
    /// 分析计算器的概念基类：在 unit 上运行分析，产出类型擦除的结果。
    struct AnalysisConcept
    {
        virtual ~AnalysisConcept()                                     = default;
        virtual ResultConcept* run(IRUnitT& unit, AnalysisManager& am) = 0;
    };
    template<class AnalysisT>
    struct AnalysisModel : AnalysisConcept
    {
        ResultConcept* run(IRUnitT& unit, AnalysisManager& am) override
        {
            return new ResultModel<AnalysisT>(analysis.run(unit, am));
        }
        AnalysisT analysis;   // 持一个实例（AnalysisT 须可默认构造）
    };


    ca::str::Utf8StringArena* arena_;
    // 注册表：AnalysisID → 可计算该分析的模型（类型擦除）。
    std::unordered_map<AnalysisID, std::unique_ptr<AnalysisConcept>> analyses_;
    // 钉子结果（per-AM，不受 invalidate/clear 影响）—— 用于基础设施 Proxy。
    std::unordered_map<AnalysisID, std::unique_ptr<ResultConcept>> pinned_;
    // 缓存：unit 指针 → (AnalysisID → 结果，类型擦除)。
    std::unordered_map<const void*, UnitResultMap> cache_;

public:
    /// @brief 传递失效查询器（对标 LLVM AnalysisManager::Invalidator）。
    ///
    /// 分析的 `dependencyInvalidated` 经 `inv.invalidate<Dep>(unit, pa)` 递归查询依赖是否
    /// 失效。带 memo：每个分析在一轮失效里只判定一次，DAG 依赖下线性收敛。
    /// 只能由 AnalysisManager 构造（构造函数私有 + 友元），分析侧仅借用其引用查询。
    class Invalidator
    {
    public:
        /// 查询某分析（对该 unit）本轮是否被失效。
        template<class AnalysisT>
        bool invalidate(IRUnitT& unit, const PreservedAnalyses& pa)
        {
            return invalidateID(analysisID<AnalysisT>(), unit, pa);
        }

        bool invalidateID(AnalysisID id, IRUnitT& unit, const PreservedAnalyses& pa)
        {
            auto memoIt = memo_.find(id);
            if (memoIt != memo_.end()) return memoIt->second;
            auto rit = cache_->find(id);
            if (rit == cache_->end()) {
                // 结果不在缓存（从未计算或已失效）→ 视为已失效，令依赖它的分析连锁失效。
                memo_.emplace(id, true);
                return true;
            }
            memo_[id]   = false;   // 占位防环（DAG 依赖下不触发）
            bool result = rit->second->invalidate(unit, pa, *this);
            memo_[id]   = result;
            return result;
        }

        bool wasInvalidated(AnalysisID id) const
        {
            auto it = memo_.find(id);
            return it != memo_.end() && it->second;
        }

    private:
        friend class AnalysisManager;
        explicit Invalidator(UnitResultMap& cache)
            : cache_(&cache)
        {}
        UnitResultMap*                       cache_;
        std::unordered_map<AnalysisID, bool> memo_;
    };
};

}   // namespace mjbase::spi
