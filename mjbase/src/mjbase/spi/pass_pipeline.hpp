#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "jbc/ir/units.hpp"
#include "libca/core/datatype.hpp"
#include "libca/log/log_macros.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

namespace jbc::analysis {
class ITypeResolver;
}

namespace mjbase::spi {

class ProgressSink;   // 前向声明：adaptor 持指针做进度反馈，仅用指针无需完整定义。

// ── 方法级作用域单元（promote 前/后的两段视图）──

/// @brief 方法级 pass 的身份面：跨类身份 + 会话内稳定定位（promote 前可用）。
///
/// MemberKey 由驱动方（ClassToMethodPassAdaptor / runClassPipeline）在会话骨架上
/// 构造：owner/name/desc 三段 intern 进作业身份池（PassJobContext::identityPool）。
/// name/desc 视图指向会话符号池，会话存活期内有效。
struct MethodTarget
{
    mjbase::MemberKey key;             ///< 跨类身份（驻留作业身份池）
    jbc::ir::MethodId method;          ///< 会话内稳定身份（lowerMethod 入参）
    std::string_view  name;            ///< 方法名（会话符号池视图）
    std::string_view  desc;            ///< 方法描述符（会话符号池视图）
    ca::u32           access = 0;      ///< 方法 access flags
    jbc::ir::ClassSession& session;    ///< 所属会话（backing + unit + 符号池）
};

/// @brief 方法级 pass 的作用域单元：promote 后的 CodeUnit + MethodTarget 身份。
///
/// 生命周期契约：由驱动方在类作用域内堆分配并持有到本次类级 run 结束
/// （地址互异且全程存活——AnalysisManager 按 unit 地址缓存，栈上复用地址会
/// 伪造跨方法缓存命中）；pass 不得保存 scope 引用越过本次方法管线调用。
struct MethodScope : MethodTarget
{
    jbc::ir::CodeUnit& code;   ///< promote 产物（驱动方保证非空且存活）
};

/// 方法过滤谓词：返回 true 表示对该方法运行 pass。
/// 在 promote 之前按身份面求值（CLI -m 目标过滤），被过滤方法不付解码成本。
using MethodPredicate = std::function<bool(const MethodTarget&)>;

// ── 三级作用域的 AnalysisManager 具体类型 ──
using MethodAnalysisManager = AnalysisManager<MethodScope>;
using ClassAnalysisManager  = AnalysisManager<jbc::ir::ClassSession>;
using GlobalAnalysisManager = AnalysisManager<mjbase::model::ResourceContainer>;

// ── 跨作用域 AM Proxy ──

/// Proxy 结果：持有子级 AM 指针，Adaptor 运行前注入。
template<class ChildAMT>
struct InnerAMProxyResult
{
    ChildAMT* am = nullptr;
    ChildAMT& operator*() const { return *am; }
    ChildAMT* operator->() const { return am; }
};

/// 方法级 AM Proxy：注册在 ClassAnalysisManager 上（pinned），由管线 bootstrap
/// 注入，ClassToMethodPassAdaptor 经 getPinned 取用。
///
/// 与 mbase 版的差异：旧 CurrentClassContext（pinned 注入「当前类」ClassNode*）
/// 被并入 MethodScope——方法级单元自带 owner 身份（key.cls）与会话引用，
/// 方法 pass 直接从作用域单元取所属类，不再需要二级 pinned 上下文及其
/// save/restore 协议。跨层失效联动（S3）仍经本 proxy 传递。
struct MethodAnalysisManagerProxy
{
    using Result = InnerAMProxyResult<MethodAnalysisManager>;
    Result run(jbc::ir::ClassSession&, ClassAnalysisManager&) { return {}; }
};

/// 类级 AM Proxy 预留类型。注入/撤销协议完成前不注册为稳定分析。
struct ClassAnalysisManagerProxy
{
    using Result = InnerAMProxyResult<ClassAnalysisManager>;
    Result run(mjbase::model::ResourceContainer&, GlobalAnalysisManager&) { return {}; }
};

/// @brief Pass 概念基模板：在某作用域 IR 单元上运行，返回保留的分析集合。
///
/// 对标 LLVM PassInfoMixin/PassConcept。三个具体作用域基类由它特化别名而来。
///
/// @tparam IRUnitT          作用域 IR 单元（MethodScope / ClassSession / ResourceContainer）
/// @tparam AnalysisManagerT 对应作用域的 AnalysisManager
template<class IRUnitT, class AnalysisManagerT>
class PassModel
{
public:
    virtual ~PassModel() = default;
    /// 在 unit 上运行；返回本次变换后仍然有效的分析集合（驱动 AM 失效）。
    virtual PreservedAnalyses run(IRUnitT& unit, AnalysisManagerT& am) = 0;
};

// ── 三个具体作用域 Pass 基类 ──
//    旧名 AsmMethodPass/AsmClassPass/AsmGlobalPass 骑在 ASM tree 上；新名去
//    "Asm" 前缀：单元是 libjbc 的 CodeUnit/ClassSession，不是 ASM 树。
using MethodPass = PassModel<MethodScope, MethodAnalysisManager>;
using ClassPass  = PassModel<jbc::ir::ClassSession, ClassAnalysisManager>;
using GlobalPass = PassModel<mjbase::model::ResourceContainer, GlobalAnalysisManager>;

/// @brief 同作用域 pass 的有序编排器，本身也是一个 Pass（可嵌套）。
///
/// 顺序运行所有 pass；每个 pass 后按其 PreservedAnalyses 失效 am 缓存，并把各 pass 的
/// 保留集合取交作为整体返回（只保留所有 pass 都保住的分析）。
///
/// @par 生命周期与异常
///
/// `PassManager` 以 `unique_ptr` 独占 pass；运行期间 IR 和 AnalysisManager 必须由调用方
/// 保持存活。pass 抛出的异常不在编排器中吞掉，调用方负责决定作业回滚或终止；已经完成的
/// 前序变换不会自动回滚，后续 pass 不再运行。异常 pass 可能已修改 IR，因此编排器在
/// 重抛前保守失效当前作用域缓存，并清理可达的子级普通缓存。`addPass(nullptr)` 属于配置
/// 错误，稳定抛出 `std::invalid_argument`。
///
/// @par contract 责任
/// 每个 pass 必须准确返回 `PreservedAnalyses`。编排器只负责调用 invalidate 和求交，
/// 不会替不可靠的 pass 猜测分析是否仍然有效。
///
/// @warning 非线程安全。
template<class IRUnitT, class AnalysisManagerT>
class PassManager : public PassModel<IRUnitT, AnalysisManagerT>
{
public:
    using PassT = PassModel<IRUnitT, AnalysisManagerT>;

    /// 追加一个同作用域 pass（移动持有）。返回 *this 便于链式。
    PassManager& addPass(std::unique_ptr<PassT> pass)
    {
        if (!pass) throw std::invalid_argument("PassManager::addPass: null pass");
        passes_.push_back(std::move(pass));
        return *this;
    }

    PreservedAnalyses run(IRUnitT& unit, AnalysisManagerT& am) override
    {
        auto pa = PreservedAnalyses::all();
        // 诊断探针（MJT_PASS_PROBE=1）：逐 pass 打印起止时间戳，与内存护栏曲线
        // 按时间对齐即可归属内存增长到具体 pass。
        static const bool passProbe = std::getenv("MJT_PASS_PROBE") != nullptr;
        const auto        probeT0   = std::chrono::steady_clock::now();
        ca::usize         passIndex = 0;
        for (auto& pass : passes_) {
            if (passProbe) {
                const auto now = std::chrono::steady_clock::now();
                // RTTI 全局禁用，不能 typeid 取 pass 名；pass 序号按注册顺序对照。
                // 诊断统一走 ca::log 门面，不依赖 stderr/std::cerr。
                CA_LOG_DEBUG("[pass-probe] t={:.1f}s begin #{}",
                             std::chrono::duration<double>(now - probeT0).count(),
                             passIndex);
            }
            PreservedAnalyses passPA;
            try {
                passPA = pass->run(unit, am);
            }
            catch (...) {
                // pass 可能在抛出异常前已经修改 IR；该路径没有任何保留声明，因此调用方捕获异常后
                // 不能继续观察旧的分析结果。
                am.invalidate(unit, PreservedAnalyses::none());
                if constexpr (std::is_same_v<IRUnitT, jbc::ir::ClassSession>) {
                    if (auto* proxy = am.template getPinned<MethodAnalysisManagerProxy>()) {
                        if (proxy->am) proxy->am->clearAll();
                    }
                }
                throw;
            }
            am.invalidate(unit, passPA);   // 失效发生点
            // 类级 pass 可能直接改写会话 IR（unit/method 元数据或未 promote 的 backing）。
            // 类级 pass 改动 IR 时无法证明方法级分析仍有效，经 pinned proxy 清空子级
            // 缓存，避免后续方法 pass 命中旧结果。
            if constexpr (std::is_same_v<IRUnitT, jbc::ir::ClassSession>) {
                if (passPA.changed()) {
                    if (auto* proxy = am.template getPinned<MethodAnalysisManagerProxy>()) {
                        if (proxy->am) proxy->am->clearAll();
                    }
                }
            }
            if (passProbe) {
                const auto now = std::chrono::steady_clock::now();
                CA_LOG_DEBUG("[pass-probe] t={:.1f}s end   #{}",
                             std::chrono::duration<double>(now - probeT0).count(),
                             passIndex);
            }
            ++passIndex;
            pa.intersect(passPA);
        }
        return pa;
    }

    bool      empty() const { return passes_.empty(); }
    ca::usize size() const { return passes_.size(); }

private:
    std::vector<std::unique_ptr<PassT>> passes_;
};

using MethodPassManager = PassManager<MethodScope, MethodAnalysisManager>;
using ClassPassManager  = PassManager<jbc::ir::ClassSession, ClassAnalysisManager>;
using GlobalPassManager = PassManager<mjbase::model::ResourceContainer, GlobalAnalysisManager>;

// ── 可选容错档位 ──

/// @brief 容错档位的跳过诊断收集器。
///
/// 作业结束汇总的载体：容错档位下 adaptor 每跳过一个单元（类/方法）追加一条，
/// run() 正常返回后调用方即可读取汇总（跳过数 / 失败类清单）。诊断行对齐既有
/// stable-skip 契约格式：`SKIP <scope>=<target> reason=<reason> detail=<detail>`。
/// 调用方拥有实例并负责生命周期与清理；不传入（nullptr）时容错仍然生效，只是不记录。
struct PassFaultReport
{
    /// 单条跳过记录。
    struct Entry
    {
        ca::str::Utf8String scope;    ///< 跳过粒度："class" / "method"
        ca::str::Utf8String target;   ///< 类资源 URI；方法级为 "<classInternalName>.<name><descriptor>"
        ca::str::Utf8String reason;   ///< 失败阶段："sanitize_error" / "reader_error" / "pass_error" /
                                      ///< "writer_error"
        ca::str::Utf8String detail;   ///< 失败详情；空表示无细节

        /// stable-skip 诊断行（与既有契约断言格式一致；stdout/日志输出面）。
        /// 无 detail 时省略该段。
        std::string toLine() const
        {
            std::string line = "SKIP ";
            line += scope.to_std_string();
            line += '=';
            line += target.to_std_string();
            line += " reason=";
            line += reason.to_std_string();
            if (!detail.is_empty()) {
                line += " detail=";
                line += detail.to_std_string();
            }
            return line;
        }
    };

    /// 按发生顺序追加的跳过记录。
    std::vector<Entry> entries;

    /// 追加一条跳过记录。
    void add(Entry entry) { entries.push_back(std::move(entry)); }

    /// 跳过单元总数（作业结束汇总的「跳过数」）。
    ca::usize skippedCount() const { return entries.size(); }

    /// 失败类清单（scope=="class" 的 target，按发生顺序去重）。
    std::vector<ca::str::Utf8String> failedClasses() const
    {
        std::vector<ca::str::Utf8String> classes;
        for (const auto& entry : entries) {
            if (!(entry.scope == "class")) continue;
            bool seen = false;
            for (const auto& existing : classes) {
                if (existing == entry.target) {
                    seen = true;
                    break;
                }
            }
            if (!seen) classes.push_back(entry.target.clone());
        }
        return classes;
    }

    bool empty() const { return entries.empty(); }
    /// 清空（复用同一报告跑下一作业前调用）。
    void clear() { entries.clear(); }
};

/// @brief pass 管线可选容错档位。
///
/// 默认全关：管线语义与历史版本完全一致——任一方法/类失败即抛出，作业级
/// 原子提交。两个档位相互独立、分别由对应 adaptor 消费：
///   - perMethodFaultIsolation：ClassToMethodPassAdaptor 方法循环 per-method
///     try/catch，失败方法跳过并记录诊断，类内其余方法继续；
///   - perClassFaultIsolation：GlobalToClassPassAdaptor per-class try/catch，
///     失败类跳过（不写回，原 bytes 保持原样）并记录诊断，其余类继续，作业
///     结束照常统一提交成功类，汇总经 faultReport 带出。
/// 非 std 异常任何档位都不吞（向上传播，由调用方终止作业）。
struct PassPipelineOptions
{
    /// 方法级失败隔离档位（ClassToMethodPassAdaptor 消费）。
    bool perMethodFaultIsolation = false;
    /// 类级失败隔离档位（GlobalToClassPassAdaptor 消费）。
    bool perClassFaultIsolation = false;
    /// 可选诊断收集；容错档位下每跳过一个单元追加一条。调用方拥有并负责清理。
    PassFaultReport* faultReport = nullptr;

    /// @brief A1 帧链接线点：Strict 帧重算 + maxes 采用 + SMT 再生的 resolver
    ///        注入（design §10.1：实现体在 mjbase——通常传
    ///        StrictTypeResolverAdapter；nullptr = 不接线，写回保持 relocation-only）。
    ///
    /// 注入后本管线对每个改动方法的语义：lower 前嵌套属性布局适配
    /// （frame_rewire::prepareNestedAttrsForLayout）→ lower → Strict 帧重算
    /// 覆盖 maxes + StackMapTable 再生挂回（frame_rewire::rewireFramesAfterLower）。
    /// Strict 不可算（含 jsr/ret）抛错——容错档位按 pass_error 跳过该方法，
    /// 默认档位终止作业（不产出缺帧/错帧 class）。未注入时若变换造成既有
    /// SMT 帧目标悬垂，管线显式失败（不产出缺 SMT 的 V50+ class）。
    jbc::analysis::ITypeResolver* typeResolver = nullptr;

    /// @brief E2 ChangeSet 观察点（design.md libjbc §35）：GlobalToClassPassAdaptor
    ///        在一个类的管线成功且变更台账非空时回调——会话仍存活，台账内的
    ///        身份视图存活期 = 会话，消费者须在回调内完成折叠与 intern。
    ///
    /// 参数：类内部名（会话当前 this 名；消费者侧索引若以变换前类名建键，
    /// 改名场景由消费者自己持有旧名——容器资源键即旧名）。作业级原子语义：
    /// sink 在写回提交点前回调；默认档位任一类失败即整作业抛出、容器无写回，
    /// 此时消费者必须整体作废已折叠状态（sink 契约的一部分，由调用方承担）。
    std::function<void(std::string_view className, jbc::ir::ClassSession& session,
                       const jbc::ir::ChangeSet& changes)>
        changeSink;
};

/// @brief 对单个会话逐方法驱动方法级管线（promote → run → lower）。
///
/// 方法级写回的归属裁决：**管线统一驱动 lower**（pass 只改 CodeUnit，绝不
/// 自行 lowerMethod/writeClass）。理由：
///   1. 单一写回者不变量——backing 只被管线触碰，pass 无法绕过 changed()
///      判定与容错档位偷偷落盘；
///   2. changed()==false 时整类零 lower，libjbc copy-through 保真不被打破；
///   3. LowerError/relocation 失败集中在管线一处，容错档位（记录/跳过/中止）
///      不需要每个 pass 自行重实现。
/// pass 需要改方法元数据（name/desc/access）时改 ClassUnit，lowerMethod 会
/// 经 commitMethodMetadata 自动提交，不静默丢失。
///
/// 每方法的处理序：谓词过滤（身份面，promote 前）→ hasCode 分流（abstract/
/// native 无代码是预期形态，直接跳过）→ promoteMethod（失败即方法级失败）→
/// mpm.run（PA 失效经 PassManager 常规路径）→ changed() 时 lowerMethod。
/// MethodScope 逐个堆分配并持有到本函数返回（地址互异，AM 缓存不会跨方法
/// 因地址复用而伪造命中）。
///
/// @param predicate   可选方法过滤（nullptr = 全部方法）
/// @param options     容错档位与诊断收集
/// @return 整个会话上聚合的保留集合
PreservedAnalyses runMethodPipelineOverSession(jbc::ir::ClassSession& session,
                                               MethodPassManager&    mpm,
                                               MethodAnalysisManager& mam,
                                               mjbase::KeyPool&       identityPool,
                                               const MethodPredicate& predicate,
                                               const PassPipelineOptions& options);

/// @brief 把方法级 pipeline 适配成一个类级 pass：对会话内每个方法跑一遍。
///
/// method 级 AnalysisManager 经 cam 上的 MethodAnalysisManagerProxy（pinned）
/// 获取（S3 Proxy 跨层失效联动，Adaptor 不直接持引用）。
/// 缺失或为空的 proxy 属于 pipeline 配置错误，`run()` 抛出 `std::logic_error`，
/// 不得空指针崩溃。
///
/// 容错档位：options.perMethodFaultIsolation 开启时方法循环改为 per-method
/// try/catch（仅 `std::exception`，非 std 异常照常传播），失败方法跳过、
/// 其余方法继续；失败按 `PreservedAnalyses::none()` 参与聚合（异常 pass 可能
/// 已改 IR，保守驱动外层重序列化），诊断经 options.faultReport 记录。默认
/// 关闭，语义不变。
class ClassToMethodPassAdaptor : public PassModel<jbc::ir::ClassSession, ClassAnalysisManager>
{
public:
    /// @param mpm          方法级管线（移动持有）
    /// @param identityPool 作业身份池：MemberKey 的 cls/name/desc 驻留地
    ///                    （通常传 PassJobContext::identityPool()）
    /// @param predicate    可选方法过滤（-m 目标）；nullptr 表示不过滤。
    /// @param options      可选容错档位与诊断收集；默认全关，语义不变。
    explicit ClassToMethodPassAdaptor(MethodPassManager mpm, mjbase::KeyPool& identityPool,
                                      MethodPredicate predicate = nullptr,
                                      PassPipelineOptions options = {})
        : mpm_(std::move(mpm))
        , identityPool_(identityPool)
        , predicate_(std::move(predicate))
        , options_(options)
    {}

    PreservedAnalyses run(jbc::ir::ClassSession& session, ClassAnalysisManager& cam) override;

private:
    MethodPassManager    mpm_;
    mjbase::KeyPool&     identityPool_;
    MethodPredicate      predicate_;
    PassPipelineOptions  options_;
};

/// @brief 把类级 pipeline 适配成一个全局级 pass：对容器内每个 class 跑一遍。
///
/// 跨类 pass 的载体 + jar 级编排核心。对 ResourceContainer 内每个 ClassResource：
/// 读字节 → jbc::ir::ClassSession → 跑 class pipeline → 改动时 writeClass 序列化
/// 到 pending → 统一提交。arena 经 GlobalAnalysisManager::arena() 取；类循环内的
/// 各级 AM 使用每类作用域的临时 arena（迭代结束整体释放，大罐不累积）。
///
/// Class/Method 级 AM 在 run() 内部逐类创建（方法级 AM 经 ClassAnalysisManagerProxy
/// 向类级 pass 暴露）。Class/Method 普通分析缓存只在当前 class 内有效；切换资源前
/// 随每类作用域一并消亡，不因地址复用跨 class 命中。pinned proxy 贯穿本次 adaptor 调用。
/// 读段（readClass/建会话）、class pass 或写段（writeClass）失败时抛出包含资源 URI
/// 的 `std::runtime_error`；作业级原子性：写回推迟到全部 class 成功后统一提交，
/// 失败时容器保持原样（无部分写回）。
///
/// 容错档位：options.perClassFaultIsolation 开启时改为 per-class 跳过模式——仅捕获
/// `std::exception`（非 std 异常照常传播终止作业），失败类记录诊断后跳过（不进
/// pending，原 bytes 保持原样），其余类继续；作业结束照常统一提交全部成功类，
/// 汇总（跳过数/失败类清单）经 options.faultReport 带出。诊断 reason 按失败阶段
/// 区分：reader_error / pass_error / writer_error。默认关闭，语义不变。
///
/// 与 mbase 版的差异：classWriterFlags/typeResolver 参数移除——libjbc 写回面
/// （lowerMethod relocation 契约 + writeClass copy-through）没有 COMPUTE_MAXS/
/// COMPUTE_FRAMES 旗标，栈帧重算职责在方法级 lower，公共父类求解由
/// CommonSuperClassResolver 独立供给（接frame 重算 pass 时再入管线）。
class GlobalToClassPassAdaptor : public PassModel<mjbase::model::ResourceContainer,
                                                  GlobalAnalysisManager>
{
public:
    /// @param progress 可选进度 sink；nullptr 表示无进度反馈（默认，单测路径）。
    /// @param options  可选容错档位与诊断收集；默认全关，语义不变。
    explicit GlobalToClassPassAdaptor(ClassPassManager cpm, ProgressSink* progress = nullptr,
                                      PassPipelineOptions options = {})
        : cpm_(std::move(cpm))
        , progress_(progress)
        , options_(options)
    {}

    PreservedAnalyses run(mjbase::model::ResourceContainer& container,
                          GlobalAnalysisManager& gam) override;

private:
    ClassPassManager    cpm_;
    ProgressSink*       progress_;
    PassPipelineOptions options_;
};

}   // namespace mjbase::spi
