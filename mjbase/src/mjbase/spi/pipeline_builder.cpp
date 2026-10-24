#include "mjbase/spi/pipeline_builder.hpp"

#include <stdexcept>

#include "mjbase/spi/class_file_sanitize_gate.hpp"

namespace mjbase::spi {

// ── 注册 ──

PipelineBuilder& PipelineBuilder::addMethodPass(MethodFactory factory, std::string nameHint)
{
    methodFactories_.emplace_back(std::move(nameHint), std::move(factory));
    return *this;
}

PipelineBuilder& PipelineBuilder::addClassPass(ClassFactory factory, std::string nameHint)
{
    classFactories_.emplace_back(std::move(nameHint), std::move(factory));
    return *this;
}

PipelineBuilder& PipelineBuilder::addGlobalPass(GlobalFactory factory, std::string nameHint)
{
    globalFactories_.emplace_back(std::move(nameHint), std::move(factory));
    return *this;
}

PipelineBuilder& PipelineBuilder::addPostClassPass(GlobalFactory factory, std::string nameHint)
{
    postClassFactories_.emplace_back(std::move(nameHint), std::move(factory));
    return *this;
}

// ── 装配 ──

GlobalPassManager PipelineBuilder::buildNestedPipeline(PassJobContext&             job,
                                                       MethodPredicate             predicate,
                                                       ProgressSink*               progress,
                                                       const PassPipelineOptions&  options) const
{
    GlobalPassManager mgr;
    // 输入净化门（issue #963）：head 位置先于一切 pass 校验每个
    // ClassResource 的 CP 引用语义。失败语义跟随容错档位。
    mgr.addPass(std::make_unique<ClassFileSanitizeGate>(options));

    for (const auto& [hint, factory] : globalFactories_) {
        (void)hint;
        auto pass = factory();
        if (!pass) throw std::invalid_argument("PipelineBuilder: global factory returned null");
        mgr.addPass(std::move(pass));
    }

    // class + method 段经 adaptor 包装；即使 class 段为空，有 method pass 也
    // 需要生成 adaptor（否则 method pass 不会跑）。两段全空才省略。
    MethodPassManager mpm;
    for (const auto& [hint, factory] : methodFactories_) {
        (void)hint;
        auto pass = factory();
        if (!pass) throw std::invalid_argument("PipelineBuilder: method factory returned null");
        mpm.addPass(std::move(pass));
    }
    ClassPassManager cpm;
    for (const auto& [hint, factory] : classFactories_) {
        (void)hint;
        auto pass = factory();
        if (!pass) throw std::invalid_argument("PipelineBuilder: class factory returned null");
        cpm.addPass(std::move(pass));
    }
    if (!mpm.empty()) {
        cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(
            std::move(mpm), job.identityPool(), std::move(predicate), options));
    }
    if (!cpm.empty()) {
        mgr.addPass(std::make_unique<GlobalToClassPassAdaptor>(std::move(cpm), progress, options));
    }

    // post-class 段追加在 adaptor 之后，可观察 class 段的修改结果。
    for (const auto& [hint, factory] : postClassFactories_) {
        (void)hint;
        auto pass = factory();
        if (!pass) throw std::invalid_argument("PipelineBuilder: global factory returned null");
        mgr.addPass(std::move(pass));
    }
    return mgr;
}

// ── 查询 ──

bool PipelineBuilder::empty() const
{
    return methodFactories_.empty() && classFactories_.empty() && globalFactories_.empty() &&
           postClassFactories_.empty();
}

std::vector<std::string> PipelineBuilder::methodPassNames() const
{
    std::vector<std::string> names;
    names.reserve(methodFactories_.size());
    for (const auto& [hint, factory] : methodFactories_) names.push_back(hint);
    return names;
}

std::vector<std::string> PipelineBuilder::classPassNames() const
{
    std::vector<std::string> names;
    names.reserve(classFactories_.size());
    for (const auto& [hint, factory] : classFactories_) names.push_back(hint);
    return names;
}

std::vector<std::string> PipelineBuilder::globalPassNames() const
{
    std::vector<std::string> names;
    names.reserve(globalFactories_.size());
    for (const auto& [hint, factory] : globalFactories_) names.push_back(hint);
    return names;
}

std::vector<std::string> PipelineBuilder::postClassPassNames() const
{
    std::vector<std::string> names;
    names.reserve(postClassFactories_.size());
    for (const auto& [hint, factory] : postClassFactories_) names.push_back(hint);
    return names;
}

// ── applyTo：注册名 → 段位投递（§9 唯一收口）──
//
// 工厂 lambda 按引用借用 registry、每次构建都重新 create——与 addXxxPass 的
// 「每次 build 产新实例」契约一致（单实例捕获会让第二次 build 拿到空 pass）。
// 调用方须保证 builder 在 registry 存活期内 build（defaultPassRegistry 全局
// 单例不受限；测试用临时表须同帧）。

Result<void, RegistryError> applyTo(const PassNameRegistry& registry, std::string_view name,
                                    PipelineBuilder& builder)
{
    const auto found = registry.find(name);
    if (found.is_err()) return Err(std::move(found).unwrap_err());
    switch (found.unwrap().scope) {
    case PassScope::Method:
    {
        builder.addMethodPass(
            [&registry, name = std::string(name)]() -> std::unique_ptr<MethodPass> {
                auto pass = registry.createMethodPass(name);
                if (pass.is_err())
                    throw std::invalid_argument("applyTo: createMethodPass failed: " + name);
                return std::move(std::move(pass).unwrap());
            },
            std::string(name));
        return Ok();
    }
    case PassScope::Class:
    {
        builder.addClassPass(
            [&registry, name = std::string(name)]() -> std::unique_ptr<ClassPass> {
                auto pass = registry.createClassPass(name);
                if (pass.is_err())
                    throw std::invalid_argument("applyTo: createClassPass failed: " + name);
                return std::move(std::move(pass).unwrap());
            },
            std::string(name));
        return Ok();
    }
    case PassScope::Global:
    {
        builder.addGlobalPass(
            [&registry, name = std::string(name)]() -> std::unique_ptr<GlobalPass> {
                auto pass = registry.createGlobalPass(name);
                if (pass.is_err())
                    throw std::invalid_argument("applyTo: createGlobalPass failed: " + name);
                return std::move(std::move(pass).unwrap());
            },
            std::string(name));
        return Ok();
    }
    case PassScope::Cf:
        // 容器管线尚无 Cf 载体槽位：fail loud，不做静默跳过（见头文件注）。
        return Err(RegistryError::ScopeMismatch);
    }
    return Err(RegistryError::ScopeMismatch);
}

}   // namespace mjbase::spi
