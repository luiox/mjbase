/// @file        method_pass_test_util.hpp
/// @brief       方法级 pass 单测的共享驱动：建会话 → 管线驱动 → 写回 → 再升格检查。
///
/// pass 域方法级用例共用（本批 A/B 五 pass 起步）。驱动路径与生产一致
/// （runMethodPipelineOverSession：promote → run → 管线统一 lower），检查
/// 路径对写回字节重新建会话再 promote——验证的是真实写回产物，不是内存态。
#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "shared/structural_pass_util.hpp"

namespace mjbase::test {

/// 一次方法级驱动的结果：会话（保活）、写回字节、聚合保留集合。
struct MethodPassRun
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    Bytes                                  out;
    mjbase::spi::PreservedAnalyses         pa;
};

/// @brief 同上，显式给管线选项（B 批帧链接线注入 typeResolver）。
///
/// 选项放包前：MSVC 对「尾随默认参数 + 前置参数包」的推导会把包吞空，
/// 包后置则是干净的逐位匹配。
/// 注意本模板必须先于 runMethodPass 定义：gcc 两阶段查找要求未限定
/// 调用名在模板定义点可见（MSVC permissive 才容许后置声明）。
template<class... PassTs>
MethodPassRun runMethodPassOpts(Bytes                              classBytes,
                                mjbase::spi::PassPipelineOptions   options,
                                PassTs&&...                        passes)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{classBytes});
    EXPECT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    ca::str::Utf8StringArena           arena;
    mjbase::spi::MethodAnalysisManager mam(arena);
    mjbase::KeyPool                    pool;
    mjbase::spi::MethodPassManager     mpm;
    (mpm.addPass(std::make_unique<PassTs>(std::forward<PassTs>(passes))), ...);

    const mjbase::spi::PreservedAnalyses pa =
        mjbase::spi::runMethodPipelineOverSession(*session, mpm, mam, pool, nullptr, options);
    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok());
    return {std::move(session), std::move(written).unwrap(), pa};
}

/// @brief 组字节建会话，经生产管线驱动一组方法级 pass（默认管线选项），写回字节。
template<class... PassTs>
MethodPassRun runMethodPass(Bytes classBytes, PassTs&&... passes)
{
    return runMethodPassOpts(std::move(classBytes), mjbase::spi::PassPipelineOptions{},
                             std::forward<PassTs>(passes)...);
}

/// 写回字节的再升格视图：会话 + 第 methodIdx 个方法的 CodeUnit（保活成对）。
struct Inspection
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    jbc::ir::CodeUnitRef                   cu;
};

/// 对写回字节重建会话并升格第 methodIdx 个方法（检查真实落盘产物）。
inline Inspection inspect(Bytes bytes, ca::usize methodIdx = 0)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{bytes});
    EXPECT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();
    auto promoted = jbc::ir::promoteMethod(*session, methodIdx);
    EXPECT_TRUE(promoted.is_ok());
    return {std::move(session), std::move(promoted).unwrap()};
}

/// 升格方法体的操作码序列。
inline std::vector<jbc::core::Op> opsOf(const Inspection& ins)
{
    std::vector<jbc::core::Op> ops;
    for (const jbc::ir::InsnId id : ins.cu->dom.order()) {
        const jbc::ir::Insn* in = ins.cu->dom.peek(id);
        EXPECT_NE(in, nullptr);
        ops.push_back(in->op);
    }
    return ops;
}

/// LDC/LDC_W 装载的字符串常量（非字符串返回空）。
inline std::string_view ldcStringOf(const Inspection& ins, const jbc::ir::Insn* in)
{
    jbc::ir::ClassSession::ConstSem c;
    if (in != nullptr && ins.session->constOf(*in, c)
        && c.kind == jbc::ir::ClassSession::ConstSem::Kind::String) {
        return c.str;
    }
    return {};
}

/// 方法体里的 maxStack（写回字节重建后读 Code 属性头）。
inline ca::u16 maxStackOf(const Inspection& ins)
{
    return ins.cu->maxStack;
}

}   // namespace mjbase::test
