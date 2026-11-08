/// EnumCastCheckRemover 单测：枚举 values() 中 INVOKEVIRTUAL/CHECKCAST 相邻
/// 对的成对删除面（移植自 mbase structure_pass_test 的 EnumCastCheckRemover
/// 三例，语义口径 1:1；并补移植审计两钉：首方法位序面、cast 锚定阻断面）。
///
/// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）。装配器的
/// this 建模为局部槽不入栈——invokevirtual 的接收者须显式 `aload 0`（真实
/// 编译器产物形态）。pass 的匹
/// 配口径是 invoke 方法描述符字面等于 `[L<本类名>;`（数组形，无括号——JVMS
/// 方法描述符恒带括号，装配器按非法形拒收）。按纪律例外①申报：从装配合法
/// 类出发（合法 desc `()[LT;` + 合法字段 `values [LT;` 携带 `[LT;` Utf8），
/// 经 CP DOM 单点改写 Methodref 的 NameAndType desc 索引指向 `[LT;` Utf8。
/// 偏差后 relaxed validate 照常放行（描述符解析 fail-open）——非法面仅在
/// JVM 严格层，自检改为 CP 字面断言钉偏差落点。
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"

#include "mjbase/pass/enum_cast_check_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

constexpr ca::u16 kAccPublic = 0x0001;

/// 枚举骨架 jasm：super = java/lang/Enum + enum 修饰；`values [LT;` 合法
/// 字段把 `[LT;` Utf8 带进 CP（desc 单点改写的落点）。body 为方法体序列。
Bytes enumSkeleton(const std::string& body)
{
    return assembleJasm(R"JASM(
.super java/lang/Enum
.class public enum super T {
  .field private values [LT;
)JASM" + body + R"JASM(}
)JASM");
}

/// 纪律例外①单点偏差：把 clone Methodref 的 NameAndType desc 索引改指
/// `[LT;` Utf8（合法字段描述符已在 CP），写回字节。前后 CP 字面断言钉落点。
Bytes withEnumArrayInvokeDesc(Bytes bytes)
{
    auto made = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return {};
    jbc::core::ReadResult rr = std::move(made).unwrap();
    jbc::core::ClassFile&  cf = *rr.file;

    ca::u32 arrayDescIdx = 0;
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (cf.cp.isUtf8(i) && cf.utf8(i) == "[LT;") arrayDescIdx = i;
    }
    EXPECT_NE(arrayDescIdx, 0u) << "枚举数组 desc Utf8 缺席";

    ca::u32 patched = 0;
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        const jbc::core::ConstantPool::Entry& e = cf.cp.at(i);
        if (e.tag != jbc::core::CpTag::Methodref) continue;
        const jbc::core::ConstantPool::Entry& nat = cf.cp.at(e.b);
        if (cf.utf8(nat.a) != "clone") continue;
        EXPECT_EQ(cf.utf8(nat.b), "()[LT;") << "合法基座口径漂移";
        // NameAndType 的 desc 索引单点改写（mutateAt 置脏——at() 不置脏，
        // writeClass 会静默还原为源字节）。
        cf.cp.mutateAt(e.b).b = arrayDescIdx;
        ++patched;
    }
    EXPECT_EQ(patched, 1u) << "clone Methodref 应恰一处";
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    return written.is_ok() ? std::move(written).unwrap() : Bytes{};
}

}   // namespace

// ─── 非枚举类不处理（roundtripNonEnum 口径）──────────────────────────
TEST(EnumCastCheckRemoverTest, roundtripNonEnum)
{
    const Bytes in = assembleJasm(R"JASM(
.super java/lang/Object
.class public super t/T {
  .method public t ()V {
    code: {
      return
    }
  }
}
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ─── 调用后物理相邻不是 CHECKCAST：保持原样（口径）───────────────────
TEST(EnumCastCheckRemoverTest, keepsEnumInvokeWithoutAdjacentCast)
{
    const Bytes in = withEnumArrayInvokeDesc(enumSkeleton(R"JASM(
  .method public values ()V {
    code: {
      aload 0
      invokevirtual java/lang/Enum.clone ()[LT;
      pop
      return
    }
  }
)JASM"));
    ASSERT_NE(in.size(), 0u);
    const MethodPassRun r = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ─── 成对删除：仅移 INVOKEVIRTUAL + 紧邻 CHECKCAST（口径）────────────
TEST(EnumCastCheckRemoverTest, removesOnlyAdjacentEnumCastPair)
{
    const Bytes in = withEnumArrayInvokeDesc(enumSkeleton(R"JASM(
  .method public values ()V {
    code: {
      aload 0
      invokevirtual java/lang/Enum.clone ()[LT;
      checkcast [LT;
      pop
      return
    }
  }
)JASM"));
    ASSERT_NE(in.size(), 0u);
    const MethodPassRun r = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    // 对删除后仅剩 aload（jasm this 建模的显式接收者装载，非匹配对成员）+
    // pop + return。
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ALOAD_0);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ─── 首方法位序面（移植审计钉）：首方法干净、次方法带对——对不动 ─────────
TEST(EnumCastCheckRemoverTest, keepsPairInNonFirstMethod)
{
    const Bytes in = withEnumArrayInvokeDesc(enumSkeleton(R"JASM(
  .method public values ()V {
    code: {
      return
    }
  }
  .method public other ()V {
    code: {
      aload 0
      invokevirtual java/lang/Enum.clone ()[LT;
      checkcast [LT;
      pop
      return
    }
  }
)JASM"));
    ASSERT_NE(in.size(), 0u);
    const MethodPassRun r = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ─── cast 站点被锚定面（移植审计钉）：同构于标签隔断，保持原样 ────────
TEST(EnumCastCheckRemoverTest, anchorAtCastBlocksPair)
{
    // GOTO 目标落在 cast 站点（return 后死 goto 反向锚定；中标签隔断
    // 调用→cast 物理相邻的同构形状）。
    const Bytes in = withEnumArrayInvokeDesc(enumSkeleton(R"JASM(
  .method public values ()V {
    code: {
      aload 0
      invokevirtual java/lang/Enum.clone ()[LT;
      Lc:
      checkcast [LT;
      pop
      return
      goto Lc
    }
  }
)JASM"));
    ASSERT_NE(in.size(), 0u);
    const MethodPassRun r = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ─── 注册表挂名面：名字可解析且产出本 pass ───────────────────────────────
TEST(EnumCastCheckRemoverTest, registrableByName)
{
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("EnumCastCheckRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
