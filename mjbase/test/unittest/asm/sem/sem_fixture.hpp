/// @file        sem_fixture.hpp
/// @brief       asm/sem 测试夹具：手拼 class 字节 + 会话升格 + 按名取方法体。
///
/// 与 asm/analyzer def_use_test 的 Fixture 同款纪律：会话保活（CodeUnit
/// 句柄解析依赖 backing），code() 返回非 const 指针（ControlFlowGraph::build
/// 的签名要求）。
#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <vector>

#include "jbc/jbc.hpp"

#include "analysis/class_bytes_builder.hpp"

/// asm/sem 合成语料夹具：会话保活 + 按名取升格后的 CodeUnit。
struct SemFixture
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    std::vector<jbc::ir::CodeVisit>        visits;

    explicit SemFixture(const mjbase::analysis::test_support::Bytes& bytes)
    {
        auto made = jbc::ir::makeSessionFromBytes(
            jbc::core::ByteView{bytes.data(), bytes.size()});
        EXPECT_TRUE(made.is_ok());
        session = std::move(made).unwrap();
        visits  = jbc::ir::collectCodeUnits(*session);
    }

    const jbc::core::ConstantPool& cp() const { return session->backing().cp; }

    /// 按方法名取升格后的 CodeUnit；非 const（CFG 构建签名要求）。
    jbc::ir::CodeUnit* code(std::string_view name)
    {
        for (const jbc::ir::CodeVisit& visit : visits) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) {
                continue;
            }
            const jbc::ir::MethodUnit& unit = session->unit().methods[visit.methodIdx];
            if (session->unit().symbols().str(unit.name) == name) {
                return visit.unit.get();
            }
        }
        return nullptr;
    }
};

/// MethodSig 速记（name/thisClass 缺省不播）。
inline jbc::analysis::MethodSig sigOf(bool isStatic, std::string_view desc,
                                      std::string_view name = {},
                                      std::string_view thisClass = {})
{
    jbc::analysis::MethodSig sig;
    sig.isStatic  = isStatic;
    sig.desc      = desc;
    sig.name      = name;
    sig.thisClass = thisClass;
    return sig;
}
