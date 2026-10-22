/// @file        exception_flow_analysis_test.cpp
/// @brief       analysis 异常流模型测试：项目内异常类型传播与处理器覆盖判定。
///
/// propagatesAndHandlesProjectExceptionTypes（夹具字节化重写，
/// 断言逐字等强——含未捕获类型集合、handled/unhandled 边的双向覆盖）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/analysis/exception_flow_analysis.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

using jbc::core::Op;
using mjbase::analysis::ExceptionFlowModel;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::ExcSpec;
using mjbase::analysis::test_support::MethodSpec;

namespace {

/// buildExceptionFixture 的字节化形态：
/// - leaf：NEW IllegalStateException; DUP; INVOKESPECIAL <init>; ATHROW。
/// - uncaught：INVOKESTATIC leaf; RETURN（未捕获沿调用边传播）。
/// - caught：try { INVOKESTATIC leaf } catch (RuntimeException) {}——处理器吞掉。
/// - outsideTry：INVOKESTATIC leaf 在保护区之外——异常照传。
std::vector<MethodSpec> exceptionFixtureMethods()
{
    std::vector<MethodSpec> methods;

    methods.push_back({0x0009,
                       "leaf",
                       "()V",
                       2,
                       0,
                       [](CodeEmit& e) {
                           e.typeInsn(Op::NEW, "java/lang/IllegalStateException");
                           e.op(Op::DUP);
                           e.invoke(Op::INVOKESPECIAL, "java/lang/IllegalStateException",
                                    "<init>", "()V");
                           e.op(Op::ATHROW);
                       },
                       {}});

    methods.push_back({0x0009,
                       "uncaught",
                       "()V",
                       0,
                       0,
                       [](CodeEmit& e) {
                           e.invoke(Op::INVOKESTATIC, "ExceptionFixture", "leaf", "()V");
                           e.op(Op::RETURN);
                       },
                       {}});

    {
        MethodSpec caught;
        caught.name     = "caught";
        caught.desc     = "()V";
        caught.maxStack = 1;
        caught.emit     = [](CodeEmit& e) {
            const std::size_t start   = e.newLabel();
            const std::size_t end     = e.newLabel();
            const std::size_t handler = e.newLabel();
            e.bindLabel(start);
            e.invoke(Op::INVOKESTATIC, "ExceptionFixture", "leaf", "()V");
            e.bindLabel(end);
            e.op(Op::RETURN);
            e.bindLabel(handler);
            e.op(Op::POP);
            e.op(Op::RETURN);
        };
        caught.exceptions = {ExcSpec{0, 1, 2, "java/lang/RuntimeException"}};
        methods.push_back(std::move(caught));
    }

    {
        MethodSpec outside;
        outside.name     = "outsideTry";
        outside.desc     = "()V";
        outside.maxStack = 1;
        outside.emit     = [](CodeEmit& e) {
            const std::size_t start   = e.newLabel();
            const std::size_t end     = e.newLabel();
            const std::size_t handler = e.newLabel();
            e.invoke(Op::INVOKESTATIC, "ExceptionFixture", "leaf", "()V");
            e.bindLabel(start);
            e.op(Op::NOP);
            e.bindLabel(end);
            e.op(Op::RETURN);
            e.bindLabel(handler);
            e.op(Op::POP);
            e.op(Op::RETURN);
        };
        outside.exceptions = {ExcSpec{0, 1, 2, "java/lang/RuntimeException"}};
        methods.push_back(std::move(outside));
    }

    return methods;
}

Bytes buildExceptionFixture()
{
    ClassBytesBuilder b("ExceptionFixture");
    return b.assemble(exceptionFixtureMethods());
}

std::vector<std::string> text(const std::vector<std::string_view>& views)
{
    return {views.begin(), views.end()};
}

}   // namespace

TEST(MjAnalysisExceptionFlowTest, PropagatesAndHandlesProjectExceptionTypes)
{
    mjbase::model::ResourceContainer container;
    container.addResource("ExceptionFixture.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "ExceptionFixture.class", buildExceptionFixture()));

    ExceptionFlowModel model;
    model.build(container);

    const auto* leaf     = model.summaryOf("ExceptionFixture", "leaf", "()V");
    const auto* uncaught = model.summaryOf("ExceptionFixture", "uncaught", "()V");
    const auto* caught   = model.summaryOf("ExceptionFixture", "caught", "()V");
    const auto* outside  = model.summaryOf("ExceptionFixture", "outsideTry", "()V");
    ASSERT_NE(leaf, nullptr);
    ASSERT_NE(uncaught, nullptr);
    ASSERT_NE(caught, nullptr);
    ASSERT_NE(outside, nullptr);
    EXPECT_EQ(text(leaf->uncaughtTypes),
              (std::vector<std::string>{"java/lang/IllegalStateException"}));
    EXPECT_EQ(text(uncaught->uncaughtTypes), text(leaf->uncaughtTypes));
    EXPECT_TRUE(caught->uncaughtTypes.empty());
    EXPECT_EQ(text(outside->uncaughtTypes), text(leaf->uncaughtTypes));

    bool foundHandled         = false;
    bool foundOutsideUncaught = false;
    for (const auto& edge : model.edges()) {
        if (model.pool().str(edge.source.name) == "caught" &&
            model.pool().str(edge.callee.name) == "leaf" && edge.handled) {
            foundHandled = true;
        }
        if (model.pool().str(edge.source.name) == "outsideTry" &&
            model.pool().str(edge.callee.name) == "leaf" && !edge.handled) {
            foundOutsideUncaught = true;
        }
    }
    EXPECT_TRUE(foundHandled);
    EXPECT_TRUE(foundOutsideUncaught);
}
