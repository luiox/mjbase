#include "mjbase/spi/pass_manager.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#include "jbc/core/class_file.hpp"
#include "jbc/core/error.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::spi {

const char* to_string(PipelineError::Stage stage) noexcept
{
    switch (stage) {
    case PipelineError::Stage::Read: return "Read";
    case PipelineError::Stage::Pass: return "Pass";
    case PipelineError::Stage::Write: return "Write";
    }
    return "?";
}

namespace {

/// 异常 what() → 诊断 detail（ASCII 文本）。
std::string whatToDetail(const char* what)
{
    return what != nullptr ? std::string(what) : std::string();
}

}   // namespace

Result<std::vector<ca::u8>, PipelineError>
    runClassPipeline(const std::vector<ca::u8>& classBytes, ClassPassManager* classManager,
                     MethodPassManager* methodManager, PassJobContext& job)
{
    using ca::core::Err;
    using ca::core::Ok;

    // 读段：bytes → 会话（readClass + 骨架物化）。解析降级（Recovered）不算失败。
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(classBytes));
    if (sessionResult.is_err()) {
        const jbc::ir::SessionError se = sessionResult.unwrap_err();
        const char* stageText =
            se.stage == jbc::ir::SessionError::Stage::Read
                ? jbc::core::to_string(se.read)
                : jbc::ir::to_string(se.session);
        return Err(PipelineError{PipelineError::Stage::Read, std::string{},
                                 std::string(stageText)});
    }
    auto session = std::move(sessionResult).unwrap();

    // 各级 AM 借用作业 arena（单 arena 贯穿作业）；pinned proxy 对齐 jar 路径，
    // classManager 含 ClassToMethodPassAdaptor 时可直接工作。
    MethodAnalysisManager mam(job.arena());
    ClassAnalysisManager  cam(job.arena());
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});

    try {
        auto total = PreservedAnalyses::all();

        // 类级 pass
        if (classManager && !classManager->empty()) {
            total.intersect(classManager->run(*session, cam));
        }

        // 方法级 pass：对每个方法跑一遍方法流水线（promote → run → lower 统一驱动）。
        if (methodManager && !methodManager->empty()) {
            total.intersect(runMethodPipelineOverSession(*session, *methodManager, mam,
                                                         job.identityPool(), /*predicate*/ {},
                                                         PassPipelineOptions{}));
        }

        // 序列化：仅当有 pass 声明改动时重编码（与 GlobalToClassPassAdaptor 的
        // changed() 写回契约一致）；无改动透传原字节。
        if (!total.changed()) {
            return Ok(classBytes);
        }
        auto written = jbc::core::writeClass(session->backing());
        if (written.is_err()) {
            return Err(PipelineError{PipelineError::Stage::Write, std::string{},
                                     std::string(jbc::core::to_string(written.unwrap_err()))});
        }
        return Ok(std::move(written).unwrap());
    }
    catch (const std::exception& e) {
        return Err(PipelineError{PipelineError::Stage::Pass, std::string{},
                                 whatToDetail(e.what())});
    }
    catch (...) {
        return Err(PipelineError{PipelineError::Stage::Pass, std::string{},
                                 std::string("unknown exception")});
    }
}

Result<void, PipelineError> runContainerPipeline(mjbase::model::ResourceContainer& container,
                                                 GlobalPassManager& gpm, PassJobContext& job)
{
    using ca::core::Err;
    using ca::core::Ok;

    GlobalAnalysisManager gam(job.arena());
    try {
        gpm.run(container, gam);
    }
    catch (const std::exception& e) {
        return Err(PipelineError{PipelineError::Stage::Pass, std::string{},
                                 whatToDetail(e.what())});
    }
    catch (...) {
        return Err(PipelineError{PipelineError::Stage::Pass, std::string{},
                                 std::string("unknown exception")});
    }
    return Ok();
}

}   // namespace mjbase::spi
