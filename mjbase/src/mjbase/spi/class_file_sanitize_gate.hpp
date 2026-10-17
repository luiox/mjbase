#pragma once

/// @file       class_file_sanitize_gate.hpp
/// @brief      管线入口的 classfile 输入净化门（校验+拒绝）。
///
///             全局管线在所有 pass 之前注入本门：对容器内每个 ClassResource
///             的原始 bytes 跑 CP 引用语义校验（mjbase::spi::sanitizeClassFile），
///             在任何 pass——包括自行 readClass 的容器级 global pass——接触字节
///             之前拒绝畸形输入，收敛「错 tag 静默产出垃圾值 → 下游解引用崩溃」面。
///
///             失败语义跟随容错档位：
///             - perClassFaultIsolation：记 faultReport（reason=sanitize_error，
///               detail 含具体 CP index/期望与实际 tag）后把该资源从容器剔除，
///               其余类继续，作业照常统一提交；
///             - 默认档位：抛 std::runtime_error 终止作业（作业级原子，与
///               reader/pass/writer 失败的既有语义一致）。

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::spi {

class ClassFileSanitizeGate : public GlobalPass
{
public:
    /// @param options 容错档位与诊断收集；faultReport 由调用方
    ///                持有并保证存活至流水线 run() 结束。
    explicit ClassFileSanitizeGate(PassPipelineOptions options)
        : options_(options)
    {}

    PreservedAnalyses run(mjbase::model::ResourceContainer& container,
                          GlobalAnalysisManager& am) override;

private:
    PassPipelineOptions options_;
};

}   // namespace mjbase::spi
