/// @file        pass_registry.hpp
/// @brief       命名流水线注册中心 + 内置流水线注册（pass-contract §9 装配层）。
///
/// 语义参考 pass/pass_registry：名字 → 可查询配置（工厂），解决「常用
/// pass 组合集中配置」的跨二进制复用。避开 Java SPIRegistry 的失败（
/// registerDefaults 是 no-op、零调用）：注册通过显式 registerBuiltinPipelines()
/// bootstrap，查询点在 workspace/CLI 入口。
///
/// 与 旧栈 的差异：内置流水线按 **mjbase 自有 pass 集** 组成（旧栈 的
/// FixPatternSimplifier/ConsStaticFieldReplacer 等属于 旧栈 IR 域，未在
/// mjbase 落地，不虚构同名 pass）；同名 "default-optimize" 的组成以本注册表
/// 为准，消费方经 availablePipelines/pipelineInfo 枚举，不假定与旧栈逐 pass
/// 对齐。下游第二管线消费方（§9 触发条件），本文件即其消费面。
///
/// @warning 非线程安全。注册和查询应在同一线程的初始化阶段完成。
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/spi/pipeline_builder.hpp"

namespace mjbase::spi {

/// pass/pipeline 的发布稳定性与默认链状态。
enum class PassStability : ca::u8
{
    Default,      ///< 属于默认能力，可作为正式发布承诺。
    OptIn,        ///< 只能显式选择，不计入默认能力。
    Experimental, ///< 实验性能力，不属于发布承诺。
    Stub,         ///< 空实现或兼容占位，不计入完成数量。
};

const char* to_string(PassStability stability) noexcept;

/// 已注册流水线的元信息。
struct PassPipelineInfo
{
    std::string   name;
    std::string   description;
    PassStability stability = PassStability::OptIn;
};

/// @brief Pass 流水线注册中心：名字 → 配置（工厂）。
class PassRegistry
{
public:
    using PipelineFactory = std::function<PipelineBuilder()>;

    static PassRegistry& instance();

    /// @brief 注册一个命名流水线。同名重复注册覆盖旧值（与 旧栈 一致：
    ///        流水线集合由本注册表单点定义，覆盖是受控的更新而非撞名事故）。
    /// @return *this 便于链式注册。
    PassRegistry& registerPipeline(const std::string& name, const std::string& description,
                                   PipelineFactory factory,
                                   PassStability   stability = PassStability::OptIn);

    /// 查询是否已注册某名字。
    bool hasPipeline(const std::string& name) const;

    /// 取某名字的 PipelineBuilder（每次都重新调用 factory，得到全新 pass 实例）。
    /// 未注册返回空的 PipelineBuilder。
    PipelineBuilder getPipeline(const std::string& name) const;

    /// @brief 查询单个 pipeline 的元信息；未注册返回 nullptr。
    /// 指针指向 registry 内部数据，仅在 registry 未重新注册同名 pipeline 期间有效。
    const PassPipelineInfo* pipelineInfo(const std::string& name) const;

    /// 列出所有已注册流水线的元信息（按 name 排序，供 CLI/MCP 稳定输出）。
    std::vector<PassPipelineInfo> listPipelines() const;

private:
    PassRegistry() = default;
    struct Entry
    {
        PassPipelineInfo meta;
        PipelineFactory  factory;
    };
    std::unordered_map<std::string, Entry> entries_;
};

/// @brief 注册内置流水线（default-optimize 等）。程序/workspace 入口调用一次；
///        幂等（已注册 "default-optimize" 时直接返回 0）。
///
/// mjbase 内置集（组成 = mjbase pass 域已落地的确定性件，详见各 phase 注册处）：
/// - default-optimize：确定性方法体清理链（Nop/冗余装载消除/冗余空检查）；
/// - phase.*：单面件挂点（常量折叠 / 强度削减 / 元信息清理 / 成员排序 /
///   字符串族化简 / 确定性垃圾插入）。
///
/// @return 已注册的流水线数量（便于启动日志）。
ca::usize registerBuiltinPipelines();

}   // namespace mjbase::spi
