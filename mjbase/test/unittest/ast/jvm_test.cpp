#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

#include "jdk_fixture.hpp"
#include "mjbase/ast/control_flow.hpp"
#include "mjbase/ast/int_flow.hpp"
#include "mjbase/ast/jvm.hpp"
#include "shared/jasm_fixture.hpp"

namespace {

using namespace mjbase::ast;
using jbc::core::ByteView;
using mjbase::KeyPool;
using mjbase::test::assembleClass;

TEST(MjAstJvmTest, LiftsArgumentsExpressionsAndBranchTargets)
{
    const auto bytes = assembleClass("AstExample", "java/lang/Object", R"JASM(
    .method public static select (I)I {
      code: {
        iload_0
        ifeq Lzero
        iload_0
        iconst_2
        imul
        invokestatic AstExample.sink (I)I
        ireturn
      Lzero:
        iconst_m1
        ireturn
      }
    }
    .method public static sink (I)I {
      code: {
        iload_0
        ireturn
      }
    }
)JASM");
    auto session = jbc::ir::makeSessionFromBytes(ByteView(bytes));
    ASSERT_TRUE(session.is_ok());
    auto& source = *session.unwrap();
    KeyPool keys;
    auto lifted = liftJvmMethod(source, source.unit().findMethodId("select", "(I)I"), keys);
    ASSERT_TRUE(lifted.is_ok());
    const auto& body = lifted.unwrap();
    ASSERT_EQ(body.blocks.size(), 3u);
    ASSERT_EQ(body.parameters.size(), 1u);
    const auto* branch = std::get_if<Branch>(&body.blocks[0].terminator);
    ASSERT_NE(branch, nullptr);
    EXPECT_EQ(branch->comparison, Comparison::Equal);
    EXPECT_EQ(branch->whenTrue.index, 2u);
    EXPECT_EQ(branch->whenFalse.index, 1u);
    const auto& multiply = std::get<Let>(body.blocks[1].statements[2]);
    EXPECT_EQ(std::get<IntBinary>(multiply.expression).op, IntOp::Multiply);
    const auto sites = directCalls(body);
    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites.front().block.index, 1u);
    EXPECT_EQ(keys.str(sites.front().target.name), "sink");
    EXPECT_EQ(keys.str(sites.front().target.cls), "AstExample");
    EXPECT_EQ(sites.front().dispatch, Dispatch::Static);
}

TEST(MjAstJvmTest, RewritesLiteralAndRelocatesBranch)
{
    const auto bytes = assembleClass("AstExample", "java/lang/Object", R"JASM(
    .method public static select (I)I {
      code: {
        iload_0
        ifeq Lzero
        iconst_1
        ireturn
      Lzero:
        iconst_0
        ireturn
      }
    }
)JASM");
    ASSERT_FALSE(bytes.empty());
    auto rewritten = rewriteJvmLiterals(ByteView(bytes), "select", "(I)I",
        [](const MethodBody&, const KeyPool&, BlockId, ValueId, const Literal& old)
            -> std::optional<Literal> {
            if (std::get<ca::i32>(old) == 1) return Literal{ca::i32{123456}};
            return std::nullopt;
        });
    ASSERT_TRUE(rewritten.is_ok());
    auto session = jbc::ir::makeSessionFromBytes(ByteView(rewritten.unwrap()));
    ASSERT_TRUE(session.is_ok());
    auto& source = *session.unwrap();
    KeyPool keys;
    auto lifted = liftJvmMethod(source, source.unit().findMethodId("select", "(I)I"), keys);
    ASSERT_TRUE(lifted.is_ok());
    const auto& body = lifted.unwrap();
    const auto& branch = std::get<Branch>(body.blocks[0].terminator);
    EXPECT_EQ(branch.whenTrue.index, 2u);
    const auto& arm = body.blocks[branch.whenFalse.index];
    const auto& result = std::get<Return>(arm.terminator);
    ASSERT_TRUE(result.value);
    const auto& expression = std::get<Let>(arm.statements[result.value->index]).expression;
    EXPECT_EQ(std::get<ca::i32>(std::get<Literal>(expression)), 123456);
}

TEST(MjAstJvmTest, PreservesStackArgumentsAtADiamondJoin)
{
    const auto bytes = assembleClass("AstExample", "java/lang/Object", R"JASM(
    .method public static select (I)I {
      code: {
        iload_0
        ifeq Lzero
        iconst_1
        goto Lend
      Lzero:
        iconst_0
      Lend:
        ireturn
      }
    }
)JASM");
    ASSERT_FALSE(bytes.empty());
    auto loaded = jbc::ir::makeSessionFromBytes(ByteView(bytes));
    ASSERT_TRUE(loaded.is_ok());
    auto session = std::move(loaded).unwrap();
    KeyPool keys;
    const auto method = session->unit().findMethodId("select", "(I)I");
    auto lifted = liftJvmMethodWithSources(*session, method, keys);
    ASSERT_TRUE(lifted.is_ok());
    const auto result = std::move(lifted).unwrap();
    const auto& blocks = result.body.blocks;
    ASSERT_EQ(blocks.size(), 4u);
    EXPECT_EQ(blocks[3].parameters, (std::vector<LocalKind>{LocalKind::Int}));
    const auto& ret = std::get<Return>(blocks[3].terminator);
    ASSERT_TRUE(ret.value);
    const auto& expression = std::get<Let>(blocks[3].statements.at(ret.value->index)).expression;
    EXPECT_EQ(std::get<BlockParameter>(expression).index, 0u);
    EXPECT_EQ(result.sources[3].statements.at(ret.value->index), jbc::ir::kNoInsn);
    for (const auto arm : {1u, 2u}) {
        EXPECT_EQ(std::get<Jump>(blocks[arm].terminator).target.index, 3u);
        ASSERT_EQ(blocks[arm].outgoingArguments.size(), 1u);
    }
    const auto facts = analyzeIntFlow(result.body);
    ASSERT_TRUE(facts);
    EXPECT_FALSE(facts->values[3][ret.value->index]);
}

TEST(MjAstJvmTest, StackCopiesReferToOneEffectfulEvaluation)
{
    const auto bytes = assembleClass("AstCopy", "java/lang/Object", R"JASM(
    .method public static twice ()I {
      code: {
        invokestatic AstCopy.produce ()I
        dup
        iadd
        ireturn
      }
    }
    .method public static produce ()I {
      code: {
        iconst_1
        ireturn
      }
    }
)JASM");
    auto loaded = jbc::ir::makeSessionFromBytes(ByteView(bytes));
    ASSERT_TRUE(loaded.is_ok());
    auto session = std::move(loaded).unwrap();
    KeyPool keys;
    auto lifted = liftJvmMethod(*session, session->unit().findMethodId("twice", "()I"), keys);
    ASSERT_TRUE(lifted.is_ok());
    const auto body = std::move(lifted).unwrap();
    ASSERT_EQ(body.blocks.size(), 1u);
    const auto calls = directCalls(body);
    ASSERT_EQ(calls.size(), 1u);
    const auto& statements = body.blocks.front().statements;
    ASSERT_EQ(statements.size(), 2u);
    const auto& binary = std::get<IntBinary>(std::get<Let>(statements[1]).expression);
    EXPECT_EQ(binary.left.index, calls.front().statement);
    EXPECT_EQ(binary.right.index, calls.front().statement);
}

TEST(MjAstJvmTest, PreservesOrderedHandlersAndAnalyzesExceptionalReachability)
{
    const auto bytes = assembleClass("AstProtected", "java/lang/Object", R"JASM(
    .method public static divide (I)I {
      exceptions: {
        { Lstart, Lend, Larithmetic, Ljava/lang/ArithmeticException; },
        { Lstart, Lend, Lall, * }
      },
      code: {
      Lstart:
        bipush 6
        iload_0
        idiv
      Lend:
        ireturn
      Larithmetic:
        astore_1
        iconst_m1
        ireturn
      Lall:
        astore_1
        iconst_0
        ireturn
      }
    }
)JASM");
    auto loaded = jbc::ir::makeSessionFromBytes(ByteView(bytes));
    ASSERT_TRUE(loaded.is_ok());
    auto session = std::move(loaded).unwrap();
    KeyPool keys;
    auto lifted = liftJvmMethod(*session, session->unit().findMethodId("divide", "(I)I"), keys);
    ASSERT_TRUE(lifted.is_ok());
    const auto body = std::move(lifted).unwrap();
    ASSERT_EQ(body.blocks.size(), 4u);
    const auto& entry = body.blocks.front();
    ASSERT_EQ(entry.handlers.size(), 2u);
    ASSERT_TRUE(entry.handlers[0].type);
    EXPECT_EQ(keys.str(entry.handlers[0].type->id), "java/lang/ArithmeticException");
    EXPECT_EQ(entry.handlers[0].target.index, 2u);
    EXPECT_FALSE(entry.handlers[1].type);
    EXPECT_EQ(entry.handlers[1].target.index, 3u);
    for (const auto& handler : entry.handlers) {
        EXPECT_EQ(body.blocks[handler.target.index].parameters,
                  (std::vector<LocalKind>{LocalKind::Reference}));
    }
    EXPECT_EQ(body.blocks[1].parameters, (std::vector<LocalKind>{LocalKind::Int}));
    const auto graph = ControlFlowGraph::build(body);
    ASSERT_TRUE(graph.is_ok());
    const DominatorTree dominators(graph.unwrap());
    for (const auto& handler : entry.handlers) {
        EXPECT_TRUE(dominators.isReachable(handler.target));
        EXPECT_TRUE(dominators.dominates(BlockId{0}, handler.target));
    }
    EXPECT_FALSE(analyzeIntFlow(body));
}

TEST(MjAstJvmTest, RewrittenClassPassesJvmVerificationAndPreservesJavaStringCodeUnits)
{
    const auto jdk = test::jdk::probeJdk();
    if (!jdk.available) GTEST_SKIP() << jdk.reason;
    const auto java = test::getTempOutPath("AstJvmGate.java");
    const auto directory = java.parent_path();
    {
        std::ofstream source(java);
        ASSERT_TRUE(source);
        source << R"JAVA(
public class AstJvmGate {
    static int choose(boolean high) {
        if (high) return 1;
        return -1;
    }

    static String text() { return "before"; }

    public static void main(String[] args) {
        System.out.println(choose(true));
        System.out.println(choose(false));
        System.out.println(text().length());
        System.out.println((int) text().charAt(1));
        System.out.println((int) text().charAt(2));
    }
}
)JAVA";
    }
    std::string error;
    ASSERT_TRUE(test::jdk::compileJava(jdk, java, directory, &error)) << error;
    const auto classfile = directory / "AstJvmGate.class";
    std::vector<ca::u8> bytes;
    {
        std::ifstream source(classfile, std::ios::binary);
        ASSERT_TRUE(source);
        bytes.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
    }
    auto changedInteger = rewriteJvmLiterals(ByteView(bytes), "choose", "(Z)I",
        [](const MethodBody&, const KeyPool&, BlockId, ValueId, const Literal& old)
            -> std::optional<Literal> {
            if (std::get<ca::i32>(old) == 1) return Literal{ca::i32{123456}};
            return std::nullopt;
        });
    ASSERT_TRUE(changedInteger.is_ok());
    const std::u16string codeUnits{u'A', u'\0', static_cast<char16_t>(0xD800)};
    auto changedString = rewriteJvmLiterals(ByteView(changedInteger.unwrap()), "text",
        "()Ljava/lang/String;",
        [&](const MethodBody&, const KeyPool&, BlockId, ValueId, const Literal& old)
            -> std::optional<Literal> {
            EXPECT_EQ(std::get<std::u16string>(old), u"before");
            return Literal{codeUnits};
        });
    ASSERT_TRUE(changedString.is_ok());

    auto unchanged = rewriteJvmLiterals(ByteView(changedString.unwrap()), "text",
        "()Ljava/lang/String;",
        [&](const MethodBody&, const KeyPool&, BlockId, ValueId, const Literal& old)
            -> std::optional<Literal> {
            EXPECT_EQ(std::get<std::u16string>(old), codeUnits);
            return old;
        });
    ASSERT_TRUE(unchanged.is_ok());
    EXPECT_EQ(unchanged.unwrap(), changedString.unwrap());
    {
        const auto& output = changedString.unwrap();
        std::ofstream target(classfile, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(target.write(reinterpret_cast<const char*>(output.data()), output.size()));
    }
    const auto result = test::jdk::runJavaMain(jdk, directory, "AstJvmGate", {}, {"-Xverify:all"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(test::jdk::normalizeOutput(result.out), "123456\n-1\n3\n0\n55296\n");
}

}   // namespace
