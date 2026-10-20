#pragma once

#include <vector>

#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief 管线入口失败记录：哪一阶段、哪个单元、什么细节。
///
/// 管线内部（PassManager / adaptor）的失败策略是异常语义（编排语义照旧）；
/// 本入口是模块边界，统一收敛为 Result 值语义。Result 的错误通道要求可拷贝，
/// 故诊断文本用 std::string（输出/异常文本外部边界）；detail 为失败异常的
/// what() 或 libjbc 错误码文本，target 为失败单元（类 URI / 方法目标；
/// 作业级失败为空）。
struct PipelineError
{
    enum class Stage : ca::u8
    {
        Read,    ///< 字节 → 会话失败（readClass / makeSession）
        Pass,    ///< class/method pass 抛出
        Write,   ///< writeClass 序列化失败
    };

    Stage       stage  = Stage::Read;
    std::string target;
    std::string detail;
};

const char* to_string(PipelineError::Stage stage) noexcept;

/// @brief 单类编排入口：bytes → ClassSession → managers → writeClass → bytes。
///
/// 用 pass 框架（PassManager 模板 + AnalysisManager）跑一个 class 的 pass 流水线。
/// 各级 AnalysisManager 借用 job.arena()；方法/类身份驻留 job.identityPool()。
/// 方法级管线经 pin 好的 MethodAnalysisManagerProxy 驱动：
///   - classManager 含 ClassToMethodPassAdaptor 时照常工作（proxy 已前置 pin）；
///   - methodManager 直接给（裸方法路径）时按 runMethodPipelineOverSession
///     逐方法 promote → run → lower，方法 pass 经 MethodScope 自带 owner 身份。
///
/// @param classBytes    输入 class 字节
/// @param classManager  可选类级 pass 编排器（nullptr 或空跳过）
/// @param methodManager 可选方法级 pass 编排器（nullptr 或空跳过；与 classManager 可并用，
///                      先类级后方法级，与 旧栈 阶段序一致）
/// @param job           作业级根上下文（arena + 身份池）
/// @return 转换后的 class 字节；仅当有 pass 声明改动时重序列化，否则原字节透传。
Result<std::vector<ca::u8>, PipelineError>
    runClassPipeline(const std::vector<ca::u8>& classBytes, ClassPassManager* classManager,
                     MethodPassManager* methodManager, PassJobContext& job);

/// @brief Jar 级字节码编排入口：容器 → GlobalPassManager → 统一写回。
///
/// 在 ResourceContainer 上跑全局流水线（含 GlobalToClassPassAdaptor 驱动的跨类编排）。
/// 与 旧栈 runJarPipeline 的差异：不再在此注册 旧栈 内置分析（那些分析属于
/// 旧栈分析域，未随迁）；分析注册是 pass 自己的责任（registerAnalysis 幂等）。
/// 内置分析缺位不改变编排语义：AM 的惰性求值 + PreservedAnalyses 失效协议不变。
///
/// @param container 资源容器（import 后传入，处理后由调用方 export）
/// @param gpm       全局 PassManager（含嵌套的 Adaptor + class/method passes）
/// @param job       作业级根上下文（arena + 身份池）
Result<void, PipelineError> runContainerPipeline(mjbase::model::ResourceContainer& container,
                                                 GlobalPassManager& gpm, PassJobContext& job);

}   // namespace mjbase::spi
