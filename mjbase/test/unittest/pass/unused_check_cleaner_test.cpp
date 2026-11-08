// UnusedCheckCleaner：requireNonNull 三元组删除 + 静态方法豁免 + 形态守卫。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）；实例方法
// 经 `.method public` 形（无 static）走 this 播种，字节级 builder 不再用。
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/unused_check_cleaner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// ALOAD_0; INVOKESTATIC target; POP; RETURN 的方法体片段。
constexpr std::string_view kCheckBody =
    R"JASM(aload 0
invokestatic )JASM";

/// 实例方法 m ()V（this 播种）装载 target 调用面的容器。
Bytes instanceCheck(const std::string& target)
{
    return assembleJasm(R"JASM(
.super java/lang/Object
.class public super t/U {
  .method public m ()V {
    code: {
)JASM" + std::string(kCheckBody) + target + R"JASM(
      pop
      return
    }
  }
}
)JASM");
}

/// 静态方法 m ()V（ACC_STATIC，ALOAD_0 无 this 语义）装载 target 调用面。
Bytes staticCheck(const std::string& target)
{
    return assembleJasm(R"JASM(
.super java/lang/Object
.class public super t/U {
  .method public static m ()V {
    code: {
)JASM" + std::string(kCheckBody) + target + R"JASM(
      pop
      return
    }
  }
}
)JASM");
}

/// requireNonNull 全限定引用面。
const char* kRequireNonNull =
    "java/util/Objects.requireNonNull (Ljava/lang/Object;)Ljava/lang/Object;";

}   // namespace

// ── 实例方法：ALOAD_0 + requireNonNull + POP 三元组删除 ──
TEST(UnusedCheckCleanerTest, removesTripleInInstanceMethod)
{
    const MethodPassRun r =
        runMethodPass(instanceCheck(kRequireNonNull), mjbase::pass::UnusedCheckCleaner{});

    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 静态方法豁免（ALOAD_0 无 this 语义）──
TEST(UnusedCheckCleanerTest, staticMethodUntouched)
{
    const Bytes         in = staticCheck(kRequireNonNull);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::UnusedCheckCleaner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 非 requireNonNull 的 INVOKESTATIC：不动 ──
TEST(UnusedCheckCleanerTest, otherStaticCallUntouched)
{
    const MethodPassRun r =
        runMethodPass(instanceCheck("java/util/Objects.toString "
                                    "(Ljava/lang/Object;)Ljava/lang/String;"),
                      mjbase::pass::UnusedCheckCleaner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 边界守卫：POP 被分支引用时三元组跳过 ──
TEST(UnusedCheckCleanerTest, branchTargetPopBlocksTriple)
{
    // ALOAD_0; INVOKESTATIC; Lb: POP; RETURN; GOTO Lb（死段反向锚定 POP）。
    const Bytes in = assembleJasm(R"JASM(
.super java/lang/Object
.class public super t/U {
  .method public m ()V {
    code: {
      aload 0
      invokestatic )JASM" + std::string(kRequireNonNull)
                                      + R"JASM(
      Lb:
      pop
      return
      goto Lb
    }
  }
}
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UnusedCheckCleaner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[2], Op::POP);
}

// ── 注册面 ──
TEST(UnusedCheckCleanerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("UnusedCheckCleaner"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("UnusedCheckCleaner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
