#include <gtest/gtest.h>

#include <algorithm>

#include "jdk_fixture.hpp"
#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/constant_propagator.hpp"
#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

namespace {

using namespace mjbase::test;
using jbc::core::Op;

TEST(MjConstantPipelineTest, FoldsKnownLocalArithmeticAfterADiamondJoin)
{
    const auto input = assembleJasm(R"JASM(.super java/lang/Object
.class public super ConstantPipelineGate {
  .field public static count I
  .method public static m (I)I {
    code: {
      iload_0
      ifeq Lalt
      iconst_4
      istore_1
      goto Ljoin
    Lalt:
      iconst_4
      istore_1
    Ljoin:
      invokestatic ConstantPipelineGate.observe ()V
      iload_1
      iconst_3
      imul
      iconst_2
      iadd
      ireturn
    }
  }
  .method public static observe ()V {
    code: {
      getstatic ConstantPipelineGate.count I
      iconst_1
      iadd
      putstatic ConstantPipelineGate.count I
      return
    }
  }
}
)JASM", 52);
    ASSERT_FALSE(input.empty());
    const auto folded = runMethodPass(input, mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(folded.pa.changed());
    const auto instructions = opsOf(inspect(folded.out));
    EXPECT_EQ(std::count(instructions.begin(), instructions.end(), Op::IMUL), 0);
    EXPECT_EQ(std::count(instructions.begin(), instructions.end(), Op::IADD), 0);
    EXPECT_EQ(std::count(instructions.begin(), instructions.end(), Op::INVOKESTATIC), 1);
    EXPECT_EQ(std::count(instructions.begin(), instructions.end(), Op::ILOAD_1), 0);
    const auto twice = runMethodPass(folded.out, mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(twice.pa.areAllPreserved());
    EXPECT_EQ(twice.out, folded.out);

    const auto pipeline = runMethodPass(input, mjbase::pass::ConstantPropagator{},
                                        mjbase::pass::ConstantFolder{});
    const auto pipelineInstructions = opsOf(inspect(pipeline.out));
    EXPECT_EQ(std::count(pipelineInstructions.begin(), pipelineInstructions.end(), Op::IMUL), 0);

    const auto jdk = ::test::jdk::probeJdk();
    if (!jdk.available) GTEST_SKIP() << jdk.reason;
    ::test::jdk::RunnerSpec runner;
    runner.runnerBody = "System.out.println(ConstantPipelineGate.m(0));"
                        "System.out.println(ConstantPipelineGate.m(1));"
                        "System.out.println(ConstantPipelineGate.count);";
    for (const auto* bytes : {&input, &folded.out, &pipeline.out}) {
        runner.classes = {{"ConstantPipelineGate", *bytes}};
        const auto run = ::test::jdk::runWithRunnerOutcome(jdk, runner);
        ASSERT_TRUE(run.ok()) << run.output;
        EXPECT_EQ(run.output, "14\n14\n2\n");
    }
}

TEST(MjConstantPipelineTest, LeavesKnownZeroDivisorAndItsException)
{
    const auto input = assembleMethodClass("m", "()I", R"JASM(
      iconst_5
      istore_0
      iload_0
      iconst_0
      idiv
      ireturn
)JASM", "ConstantDivisionGate", 52);
    ASSERT_FALSE(input.empty());
    const auto output = runMethodPass(input, mjbase::pass::ConstantPropagator{},
                                      mjbase::pass::ConstantFolder{});
    const auto instructions = opsOf(inspect(output.out));
    EXPECT_EQ(std::count(instructions.begin(), instructions.end(), Op::IDIV), 1);
    const auto jdk = ::test::jdk::probeJdk();
    if (!jdk.available) GTEST_SKIP() << jdk.reason;
    ::test::jdk::RunnerSpec runner;
    runner.runnerBody = "try { System.out.println(ConstantDivisionGate.m()); }"
                        "catch (ArithmeticException e) { System.out.println(\"division\"); }";
    for (const auto* bytes : {&input, &output.out}) {
        runner.classes = {{"ConstantDivisionGate", *bytes}};
        const auto run = ::test::jdk::runWithRunnerOutcome(jdk, runner);
        ASSERT_TRUE(run.ok()) << run.output;
        EXPECT_EQ(run.output, "division\n");
    }
}

}   // namespace
