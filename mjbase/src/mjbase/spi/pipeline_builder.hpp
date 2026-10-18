/// @file        pipeline_builder.hpp
/// @brief       管线装配层（pass-contract §9）：fluent 组装嵌套 PassManager。
///
/// 语义参考 mbase/pass/pass_registry 的 PipelineBuilder，按 mjbase 四 scope
/// 模型重定型。装配序（mbase 纪律照迁）：
/// @code
///   GlobalPM [
///     ClassFileSanitizeGate            (输入净化门，恒为 head)
///     (global passes)
///     GlobalToClassPassAdaptor( ClassPM [
///       (class passes)
///       ClassToMethodPassAdaptor( MethodPM [ (method passes) ] )
///     ] )                              (class/method 段非空才生成 adaptor)
///     (post-class global passes)       (追加在 adaptor 后，观察 class 改动)
///   ]
/// @endcode
///
/// 与 mbase 的差异：
/// - buildNestedPipeline 入参去掉 classWriterFlags/typeResolver——libjbc 写回面
///   没有 COMPUTE_*S 旗标，帧重算经 options.typeResolver 注入（pass-contract
///   §9：resolver 数据获取是调用方的事，管线只消费已建好的图）；
/// - ClassToMethodPassAdaptor 需要 identityPool，从入参 PassJobContext 取
///   （构建期借用，与 adaptor 持引用的寿命契约一致：built 管线须在 job 存活
///   期内 run）；
/// - PassRunStats 只读计数装饰器未随迁（消费方 mjt CLI dry-run 尚未切到
///   mjbase；需要时按同款装饰器形态补）。
///
/// @warning 非线程安全。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

class PipelineBuilder
{
public:
    using MethodFactory = std::function<std::unique_ptr<MethodPass>()>;
    using ClassFactory  = std::function<std::unique_ptr<ClassPass>()>;
    using GlobalFactory = std::function<std::unique_ptr<GlobalPass>()>;

    /// 添加方法级 pass（每次 build 都重新调用 factory，实例不跨构建共享）。
    PipelineBuilder& addMethodPass(MethodFactory factory, std::string nameHint = {});
    /// 添加类级 pass。
    PipelineBuilder& addClassPass(ClassFactory factory, std::string nameHint = {});
    /// 添加容器级 pass（执行于 class 段 adaptor 之前）。
    PipelineBuilder& addGlobalPass(GlobalFactory factory, std::string nameHint = {});
    /// 添加 post-class 容器级 pass（执行于 class 段 adaptor 之后）。
    PipelineBuilder& addPostClassPass(GlobalFactory factory, std::string nameHint = {});

    /// @brief 构建完整嵌套管线（净化门 head + 三段 + post-class，见文件头）。
    ///
    /// @param job       作业上下文：ClassToMethodPassAdaptor 借用其 identityPool；
    ///                  返回的 GlobalPassManager 须在 job 存活期内经
    ///                  runContainerPipeline 消费。
    /// @param predicate 可选方法过滤谓词（-m 目标），透传给 ClassToMethodPassAdaptor。
    /// @param progress  可选进度 sink，透传给 GlobalToClassPassAdaptor。
    /// @param options   容错档位/帧链注入，同一份透传给净化门与两级 adaptor。
    GlobalPassManager buildNestedPipeline(PassJobContext&            job,
                                          MethodPredicate           predicate = nullptr,
                                          ProgressSink*             progress  = nullptr,
                                          const PassPipelineOptions& options  = {}) const;

    bool      empty() const;
    ca::usize methodPassCount() const { return methodFactories_.size(); }
    ca::usize classPassCount() const { return classFactories_.size(); }
    ca::usize globalPassCount() const { return globalFactories_.size(); }
    ca::usize postClassPassCount() const { return postClassFactories_.size(); }

    /// @brief 各段 nameHint 列表（注册序；未传 hint 的槽位为空串）。
    std::vector<std::string> methodPassNames() const;
    std::vector<std::string> classPassNames() const;
    std::vector<std::string> globalPassNames() const;
    std::vector<std::string> postClassPassNames() const;

private:
    std::vector<std::pair<std::string, MethodFactory>> methodFactories_;
    std::vector<std::pair<std::string, ClassFactory>>  classFactories_;
    std::vector<std::pair<std::string, GlobalFactory>> globalFactories_;
    std::vector<std::pair<std::string, GlobalFactory>> postClassFactories_;
};

/// @brief 注册名 → builder 段位投递（§9：按 scope 标签分派，唯一收口）。
///
/// 查名走 registry.find（未知名 Err(UnknownPass)），create 走对应 scope 的
/// create*Pass；命中后按标签投入 method/class/global 段。投递的工厂 lambda
/// 按引用借用 registry 并在每次 build 时重新 create——builder 须在 registry
/// 存活期内 build（defaultPassRegistry 全局单例不受限，测试临时表须同帧）。
///
/// Cf 段未接入容器管线（runClassPipeline/runContainerPipeline 尚无 Cf 载体
/// 槽位）：Cf 名字显式 Err(ScopeMismatch)，不做静默跳过——静默丢 pass 是
/// 「-p 指定了却不执行」的事故形态，fail loud 由调用方转译为用户可见错误。
Result<void, RegistryError> applyTo(const PassNameRegistry& registry, std::string_view name,
                                    PipelineBuilder& builder);

}   // namespace mjbase::spi
