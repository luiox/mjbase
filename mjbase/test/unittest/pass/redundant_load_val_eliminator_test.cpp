// RedundantLoadValEliminator：POP 吸收可折叠常量的不动点扫描 + 怪癖保留 +
// 边界守卫 + PA 纪律。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）。LDC2_W 指向
// 非 cat2 常量的防御面按纪律例外①申报：从装配合法类出发单点最小偏差（CP
// DOM 改写 LDC2_W 操作数索引）+ 改动前后 relaxed validate 自检（relaxed 面
// 不建模 LDC2_W 常量 tag，偏差后照常放行——防线在消除件自身，非 validate）。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/class_file.hpp"
#include "libca/core/tag_cast.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"

#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器（指令体为 jasm 文本片段）。
Bytes oneMethod(std::string_view name, std::string_view codeBody)
{
    return assembleMethodClass(name, "()V", codeBody);
}

}   // namespace

// ── int 常量对（ICONST 族）被 POP 吸收 ──
TEST(RedundantLoadValEliminatorTest, intPairFolded)
{
    // ICONST_5; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
iconst_5
pop
return
)JASM"),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── string 常量对被 POP 吸收 ──
TEST(RedundantLoadValEliminatorTest, stringPairFolded)
{
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
ldc "x"
pop
return
)JASM"),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── POP2 吸收 long 常量对（LCONST 与 LDC2_W 装载都在消除面）──
TEST(RedundantLoadValEliminatorTest, longPairFoldedByPop2)
{
    // LCONST_1; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
lconst_1
pop2
return
)JASM"),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 不动点：嵌套两层对一次跑净 ──
TEST(RedundantLoadValEliminatorTest, fixpointSweepsNestedPairs)
{
    // BIPUSH 9; BIPUSH 9; POP; POP; RETURN → RETURN（装配器把窄常量规范化
    // 为 LDC，消除面同收）。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
bipush 9
bipush 9
pop
pop
return
)JASM"),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── POP2 吸收 LDC2_W 装载的 long（tree 折叠后的可观测行为）──
TEST(RedundantLoadValEliminatorTest, ldc2wLongFoldedByPop2)
{
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
ldc2_w 5L
pop2
return
)JASM"),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── POP2 吸收 DCONST_0（getDoubleValue 的 DCONST 臂）──
TEST(RedundantLoadValEliminatorTest, dconstPairFoldedByPop2)
{
    // DCONST_0; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
dconst_0
pop2
return
)JASM"),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── LDC2_W 指向非 cat2 常量：不折叠（防御面：消除只认 long/double）──
TEST(RedundantLoadValEliminatorTest, nonCat2Ldc2wNotFolded)
{
    // LDC2_W 指向非 long/double 条目——constOf 解析不出 Long/Double → 不折叠。
    // 合法 classfile 无此形态（long/double 恒走 LDC2_W），按纪律例外①：从
    // 装配合法类出发，CP DOM 单点改写 LDC2_W 操作数（改指方法名 Utf8），前后
    // relaxed validate 自检——relaxed 面建模 LDC2_W 常量 tag，偏差后拒收
    //（expectValidateFail 钉死拒收面），pass 侧的 constOf 防御为第二道防线。
    const Bytes legal = oneMethod("m",
                                  R"JASM(
ldc2_w 5L
pop2
return
)JASM");
    expectValidateOk(legal);

    // CP DOM：定位方法名 Utf8（"m"）与 LDC2_W 的 CP 索引，改写操作数索引。
    auto read = jbc::core::readClass(jbc::core::ByteView(legal));
    ASSERT_TRUE(read.is_ok());
    jbc::core::ReadResult rr = std::move(read).unwrap();
    jbc::core::ClassFile& cf = *rr.file;
    ca::u32               nameIdx = 0;
    ca::u32               ldc2wIdx = 0;
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        const jbc::core::ConstantPool::Entry& e = cf.cp.at(i);
        if (e.tag == jbc::core::CpTag::Utf8 && cf.utf8(i) == "m") nameIdx = i;
        if (e.tag == jbc::core::CpTag::Long) ldc2wIdx = i;
    }
    ASSERT_NE(nameIdx, 0u);
    ASSERT_NE(ldc2wIdx, 0u);
    const ca::u32 patched = static_cast<ca::u32>(nameIdx);
    for (jbc::core::MemberInfo& m : cf.methods) {
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
            if (!slot.decoded()) {
                ASSERT_FALSE(cf.decodeAttr(slot).is_err());
            }
            auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
            ASSERT_NE(code, nullptr);
            ASSERT_GE(code->code.size(), 3u);
            ASSERT_EQ(code->code[0], 0x14u);   // LDC2_W 打头（唯一指令前 3 字节）
            code->code[1] = static_cast<ca::u8>(patched >> 8);
            code->code[2] = static_cast<ca::u8>(patched);
        }
    }
    auto written = jbc::core::writeClass(cf);
    ASSERT_TRUE(written.is_ok());
    const Bytes illegal = std::move(written).unwrap();
    EXPECT_NE(illegal, legal);
    expectValidateFail(illegal);   // relaxed 面建模 LDC2_W tag，偏差即拒（申报注记）

    const MethodPassRun r = runMethodPass(illegal, mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, illegal);
}

// ── 非常量被 POP：不动 ──
TEST(RedundantLoadValEliminatorTest, nonConstantOperandUntouched)
{
    // ALOAD_0; POP; RETURN（static 方法里 ALOAD_0 也照常不折叠——非常量）。
    const Bytes in = oneMethod("m",
                               R"JASM(
aload 0
pop
return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 边界守卫：POP 是分支目标（有标签锚定）时整对跳过 ──
TEST(RedundantLoadValEliminatorTest, branchTargetPopBlocksPair)
{
    // ICONST_1; Lb: POP; RETURN; GOTO Lb（GOTO 挂在 return 后的死代码段，
    // 标签锚定在 POP）。
    const Bytes in = oneMethod("m",
                               R"JASM(
iconst_1
Lb:
pop
return
goto Lb
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::RedundantLoadValEliminator{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::POP);
}

// ── 注册面 ──
TEST(RedundantLoadValEliminatorTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("RedundantLoadValEliminator"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("RedundantLoadValEliminator");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
