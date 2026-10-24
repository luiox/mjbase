/// EnumCastCheckRemover 单测：枚举 values() 中 INVOKEVIRTUAL/CHECKCAST 相邻
/// 对的成对删除面（移植自 mbase structure_pass_test 的 EnumCastCheckRemover
/// 三例，语义口径 1:1；并补移植审计两钉：首方法位序面、cast 锚定阻断面）。
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/enum_cast_check_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

constexpr ca::u16 kAccPublic = 0x0001;
constexpr ca::u16 kAccSuper  = 0x0020;
constexpr ca::u16 kAccEnum   = 0x4000;

/// 枚举类骨架：super = java/lang/Enum + ACC_PUBLIC|ACC_SUPER|ACC_ENUM。
/// 类名取裸名 `T`（fixture 原样：desc 字面口径 `[LT;` 依赖它）。
FixtureBuilder makeEnumCastClass()
{
    FixtureBuilder b("T");
    b.setSuper("java/lang/Enum").setClassAccess(kAccPublic | kAccSuper | kAccEnum);
    return b;
}

/// INVOKEVIRTUAL; CHECKCAST; POP; RETURN——fixture 的成对形状
/// （调用描述符 == 枚举数组类型 `[LT;` 的字面口径；owner/cast 目标不被检查）。
Bytes pairBody(ca::u16 cloneCp, ca::u16 castCls)
{
    return {0xB6,
            static_cast<ca::u8>(cloneCp >> 8),
            static_cast<ca::u8>(cloneCp),
            0xC0,
            static_cast<ca::u8>(castCls >> 8),
            static_cast<ca::u8>(castCls),
            0x57,
            0xB1};
}

/// INVOKEVIRTUAL; POP; RETURN——调用后物理相邻不是 cast（保持原样面）。
Bytes noCastBody(ca::u16 cloneCp)
{
    return {0xB6,
            static_cast<ca::u8>(cloneCp >> 8),
            static_cast<ca::u8>(cloneCp),
            0x57,
            0xB1};
}

/// GOTO→cast; INVOKEVIRTUAL; CHECKCAST; POP; RETURN——GOTO 目标落在 cast
/// 站点（GOTO 3 字节，pc0 发出 offset+3 → 目标 pc6 = cast）：中标签
/// 节点隔断调用→cast 物理相邻的同构形状，保持原样面。
Bytes anchorBlockedBody(ca::u16 cloneCp, ca::u16 castCls)
{
    return {0xA7, 0x00, 0x03,   // GOTO +3：目标 pc6 = CHECKCAST（锚定阻断相邻匹配）
            0xB6,
            static_cast<ca::u8>(cloneCp >> 8),
            static_cast<ca::u8>(cloneCp),
            0xC0,
            static_cast<ca::u8>(castCls >> 8),
            static_cast<ca::u8>(castCls),
            0x57,
            0xB1};
}

}   // namespace

// ─── 非枚举类不处理（roundtripNonEnum 口径）──────────────────────────
TEST(EnumCastCheckRemoverTest, roundtripNonEnum)
{
    FixtureBuilder b("t/T");   // 缺省 public super，无 ACC_ENUM
    b.addMethod({"t", "()V", {0xB1}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ─── 调用后物理相邻不是 CHECKCAST：保持原样（口径）───────────────────
TEST(EnumCastCheckRemoverTest, keepsEnumInvokeWithoutAdjacentCast)
{
    FixtureBuilder b  = makeEnumCastClass();
    const ca::u16  cp = b.methodRef("java/lang/Enum", "clone", "[LT;");
    b.addMethod({"values", "()V", noCastBody(cp)});
    const Bytes         in = b.assemble();
    const MethodPassRun r = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ─── 成对删除：仅移 INVOKEVIRTUAL + 紧邻 CHECKCAST（口径）────────────
TEST(EnumCastCheckRemoverTest, removesOnlyAdjacentEnumCastPair)
{
    FixtureBuilder b   = makeEnumCastClass();
    const ca::u16  cp  = b.methodRef("java/lang/Enum", "clone", "[LT;");
    const ca::u16  cls = b.cls("[LT;");
    b.addMethod({"values", "()V", pairBody(cp, cls)});
    const Bytes         in = b.assemble();
    const MethodPassRun r = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::POP);
    EXPECT_EQ(ops[1], Op::RETURN);
}

// ─── 首方法位序面（移植审计钉）：首方法干净、次方法带对——对不动 ─────────
TEST(EnumCastCheckRemoverTest, keepsPairInNonFirstMethod)
{
    FixtureBuilder b   = makeEnumCastClass();
    const ca::u16  cp  = b.methodRef("java/lang/Enum", "clone", "[LT;");
    const ca::u16  cls = b.cls("[LT;");
    b.addMethod({"values", "()V", {0xB1}});   // 首方法无形状
    b.addMethod({"other", "()V", pairBody(cp, cls)});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ─── cast 站点被锚定面（移植审计钉）：同构于标签隔断，保持原样 ────────
TEST(EnumCastCheckRemoverTest, anchorAtCastBlocksPair)
{
    FixtureBuilder b   = makeEnumCastClass();
    const ca::u16  cp  = b.methodRef("java/lang/Enum", "clone", "[LT;");
    const ca::u16  cls = b.cls("[LT;");
    b.addMethod({"values", "()V", anchorBlockedBody(cp, cls)});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::EnumCastCheckRemover{});

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
