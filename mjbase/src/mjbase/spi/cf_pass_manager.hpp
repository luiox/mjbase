#pragma once

#include <memory>
#include <vector>

#include "jbc/core/class_file.hpp"

#include "mjbase/spi/cf_pass.hpp"

namespace mjbase::spi {

// ── CfClassPassManager ──

/// 管理 CfClassPass 的有序集合。
///
/// 编排语义：顺序运行全部 pass；初始化/收尾由调用方显式编排
/// （doInitialization / doFinalization 各自遍历全部 pass）。
/// 与 PassManager（指令级）的差异：classfile 级无 AnalysisManager/失效协议——
/// 载体是低层 DOM，未建模分析缓存；pass 间不交换分析结果。
class CfClassPassManager
{
public:
    /// 追加一个 pass（移动持有）。null pass 属于配置错误，稳定抛出
    /// `std::invalid_argument`（对齐 PassManager 的同款契约）。
    CfClassPassManager& addPass(std::unique_ptr<CfClassPass> pass)
    {
        if (!pass) throw std::invalid_argument("CfClassPassManager::addPass: null pass");
        passes_.push_back(std::move(pass));
        return *this;
    }

    void run(jbc::core::ClassFile& classFile, CfClassContext& context)
    {
        for (auto& pass : passes_) {
            pass->run(classFile, context);
        }
    }

    void doInitialization(CfGlobalContext& context)
    {
        for (auto& pass : passes_) {
            pass->doInitialization(context);
        }
    }

    void doFinalization(CfGlobalContext& context)
    {
        for (auto& pass : passes_) {
            pass->doFinalization(context);
        }
    }

    bool   empty() const { return passes_.empty(); }
    size_t size() const { return passes_.size(); }

private:
    std::vector<std::unique_ptr<CfClassPass>> passes_;
};

}   // namespace mjbase::spi
