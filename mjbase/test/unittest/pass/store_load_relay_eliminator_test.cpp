// StoreLoadRelayEliminator：store 定义使用边恰为紧邻 reload 的中转对整删。
// 该删（五族/快捷形/不动点）/不该删（多读点/handler 读点）/边界保守保留
// （对内任一侧锚标签整对拒收，fail-closed）三面。
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/store_load_relay_eliminator.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

void put16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v & 0xff));
}

void putInvoke(Bytes& b, ca::u8 op, ca::u16 cp)
{
    b.push_back(op);
    put16(b, cp);
}

ExcSpec exc(ca::u16 start, ca::u16 end, ca::u16 handler, ca::u16 catchCp = 0)
{
    return ExcSpec{start, end, handler, catchCp};
}

std::vector<Op> opsOfOut(const MethodPassRun& r)
{
    return opsOf(inspect(r.out));
}

}   // namespace

// ── int 中转对 → 整删，值留栈 ──
TEST(StoreLoadRelayEliminatorTest, relayPairRemoved)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x04,             // ICONST_1
                  0x36, 0x00,       // ISTORE 0
                  0x15, 0x00,       // ILOAD 0
                  0xac};            // IRETURN
    b.addMethod({"m", "()I", std::move(code)});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::IRETURN}));
}

// ── cat2（long）中转对 → 整删 ──
TEST(StoreLoadRelayEliminatorTest, longRelayRemoved)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x0a,             // LCONST_1
                  0x37, 0x00,       // LSTORE 0
                  0x16, 0x00,       // LLOAD 0
                  0xad};            // LRETURN
    b.addMethod({"m", "()J", std::move(code), 0x0009, {}, 2, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops, (std::vector<Op>{Op::LCONST_1, Op::LRETURN}));
}

// ── 引用族中转对 → 整删 ──
TEST(StoreLoadRelayEliminatorTest, aloadRelayRemoved)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x01,             // ACONST_NULL
                  0x3a, 0x00,       // ASTORE 0
                  0x19, 0x00,       // ALOAD 0
                  0xb0};            // ARETURN
    b.addMethod({"m", "()Ljava/lang/Object;", std::move(code)});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops, (std::vector<Op>{Op::ACONST_NULL, Op::ARETURN}));
}

// ── 快捷形（istore_n/iload_n）同权 ──
TEST(StoreLoadRelayEliminatorTest, shorthandFormsRemoved)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x05,             // ICONST_2
                  0x3c,             // ISTORE_1
                  0x1b,             // ILOAD_1
                  0xac};            // IRETURN
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 1, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IRETURN}));
}

// ── 槽有其他读点 → 单使用不成立 → 保留 ──
TEST(StoreLoadRelayEliminatorTest, foreignReadKept)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x04,             // ICONST_1
                  0x36, 0x00,       // ISTORE 0
                  0x15, 0x00,       // ILOAD 0
                  0x15, 0x00,       // ILOAD 0
                  0x60,             // IADD
                  0xac};            // IRETURN
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 6u);
}

// ── handler 读点（异常边合并）产生额外使用边 → 保留 ──
TEST(StoreLoadRelayEliminatorTest, handlerReadKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 g = b.methodRef("t/C", "g", "()V");
    Bytes code;
    code.push_back(0x04);              // ICONST_1（pc0）
    code.insert(code.end(), {0x36, 0x00});   // ISTORE 0（pc1）
    code.insert(code.end(), {0x15, 0x00});   // ILOAD 0（pc3）
    code.push_back(0x57);              // POP（pc5）
    putInvoke(code, 0xb8, g);          // INVOKESTATIC g()V（pc6，可抛）
    code.push_back(0x03);              // ICONST_0（pc9）
    code.push_back(0xac);              // IRETURN（pc10）
    code.insert(code.end(), {0x3a, 0x01});   // handler@11: ASTORE 1
    code.insert(code.end(), {0x15, 0x00});   // ILOAD 0（pc13，handler 读槽）
    code.push_back(0x57);              // POP（pc15）
    code.push_back(0x03);              // ICONST_0（pc16）
    code.push_back(0xac);              // IRETURN（pc17）
    b.addMethod({"m", "()I", std::move(code), 0x0009, {exc(1, 9, 11)}, 1, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
    EXPECT_EQ(opsOf(ins).size(), 12u);
}

// ── reload 位外来锚标签（分支跳入）→ 拒收（多定义汇合同样不放宽） ──
TEST(StoreLoadRelayEliminatorTest, anchoredReloadKept)
{
    FixtureBuilder b("t/C");
    Bytes code;
    code.push_back(0x03);                       // ICONST_0（pc0）
    code.insert(code.end(), {0x36, 0x00});      // ISTORE 0（pc1）
    code.push_back(0x04);                       // ICONST_1（pc3）
    code.insert(code.end(), {0x99, 0x00, 0x06});   // IFEQ → pc10（pc4）
    code.push_back(0x05);                       // ICONST_2（pc7）
    code.insert(code.end(), {0x36, 0x00});      // ISTORE 0（pc8）
    code.insert(code.end(), {0x15, 0x00});      // L1: ILOAD 0（pc10）
    code.push_back(0xac);                       // IRETURN（pc12）
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 1, 1});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 8u);
}

// ── store 位锚标签（异常表边界）→ 双侧锚守卫整对保留（fail-closed） ──
TEST(StoreLoadRelayEliminatorTest, anchoredStoreKept)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x04,             // ICONST_1（pc0）
                  0x36, 0x00,       // ISTORE 0（pc1，保护区起点锚）
                  0x15, 0x00,       // ILOAD 0（pc3）
                  0xac,             // IRETURN（pc5）
                  // handler@6: pop; iconst_0; ireturn（不读槽 0 → 无 Local 读边）
                  0x57, 0x03, 0xac};
    b.addMethod({"m", "()I", std::move(code), 0x0009, {exc(1, 6, 6)}, 1, 1});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection m = inspect(r.out, 0);
    EXPECT_EQ(opsOf(m).size(), 7u);
    EXPECT_EQ(m.cu->exc.size(), 1u);
}

// ── 不动点：删 b 对后 a 对成形 → 下一轮继续消 ──
TEST(StoreLoadRelayEliminatorTest, fixedPointConvergence)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x04,             // ICONST_1
                  0x05,             // ICONST_2
                  0x36, 0x00,       // ISTORE 0
                  0x36, 0x01,       // ISTORE 1
                  0x15, 0x01,       // ILOAD 1
                  0x15, 0x00,       // ILOAD 0
                  0x57,             // POP
                  0x57,             // POP
                  0xb1};            // RETURN
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 2, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops,
              (std::vector<Op>{Op::ICONST_1, Op::ICONST_2, Op::POP, Op::POP,
                               Op::RETURN}));
}

// ── 槽位不一致 → 不匹配 → 保留 ──
TEST(StoreLoadRelayEliminatorTest, mismatchedSlotKept)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x04,             // ICONST_1
                  0x36, 0x00,       // ISTORE 0
                  0x05,             // ICONST_2
                  0x36, 0x01,       // ISTORE 1
                  0x15, 0x00,       // ILOAD 0
                  0x57,             // POP
                  0xb1};            // RETURN
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 2, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 7u);
}

// ── 幂等：二轮不再变化 ──
TEST(StoreLoadRelayEliminatorTest, idempotent)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x04, 0x36, 0x00, 0x15, 0x00, 0xac};
    b.addMethod({"m", "()I", std::move(code)});
    const Bytes          bytes = b.assemble();
    const MethodPassRun  r = runMethodPass(bytes, mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const MethodPassRun r2 =
        runMethodPass(r.out, mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 注册面 ──
TEST(StoreLoadRelayEliminatorTest, registeredInRegistry)
{
    const auto found =
        mjbase::spi::defaultPassRegistry().find("StoreLoadRelayEliminator");
    EXPECT_TRUE(found.is_ok());
}
