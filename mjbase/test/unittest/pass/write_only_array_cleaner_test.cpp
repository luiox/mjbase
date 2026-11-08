// WriteOnlyArrayCleaner（#37 第三件）：只写数组分配连存储窗整体删除——
// S1 直通形/S2 暂存形窗匹配、纯常量元素/下标、守卫面（读取逃逸/非常量
// 生产者/异常域相交/窗内锚标/size 生产者非纯）、注册面、与
// ArgumentArrayScalarizer 的管线串联裁决（调用窗无残段 + 哑数组串联清理）。
//
// 夹具纪律：方法体一律 jasm 文本装配（JvmJasmService），原 FixtureBuilder
// 手拼载体退役；窄整数（bipush）装配器规范化为 ldc——size 生产者常量面
// 同实。「窗内锚标」守卫的原字节形（goto 直指窗尾 pop，栈形断裂不可装配）
// 以死段 goto 锚同一位合法重表达（死段不进帧分析、标签锚照计）——例外①
// 申报：被测决策面（锚点落窗内 → 整窗放弃）与原形同实。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/argument_array_scalarizer.hpp"
#include "mjbase/pass/write_only_array_cleaner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 类夹具便捷形：cls 单 public static name/desc 方法（excText 为 exceptions
/// 段条目文本，可空）。
Bytes asmM(const std::string& cls, std::string_view name, std::string_view desc,
           std::string_view codeBody, std::string_view excText = "")
{
    const std::string excSeg = excText.empty()
        ? std::string()
        : "    exceptions: {\n" + std::string(excText) + "    },\n";
    const std::string src = R"JASM(.super java/lang/Object
.class public super )JASM" + cls + R"JASM( {
  .method public static )JASM" + std::string(name) + " " + std::string(desc)
        + R"JASM( {
)JASM" + excSeg + "    code: {\n" + std::string(codeBody) + "    }\n  }\n}\n";
    return assembleJasm(src);
}

const mjbase::model::ClassResource* classResource(
    mjbase::model::ResourceContainer& container, const std::string& uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

/// 读回某方法 promote 后的指令序。
std::vector<Op> methodOps(const Bytes& bytes, const char* methodName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (unit.symbols().str(unit.methods[idx].name) != methodName) continue;
        if (!jbc::ir::hasCode(*session, idx)) return {};
        auto promoted = jbc::ir::promoteMethod(*session, idx);
        EXPECT_TRUE(promoted.is_ok());
        if (promoted.is_err()) return {};
        auto            cuRef = std::move(promoted).unwrap();
        std::vector<Op> ops;
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            EXPECT_NE(in, nullptr);
            if (in == nullptr) return {};
            ops.push_back(in->op);
        }
        return ops;
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return {};
}

std::vector<Op> runAndOps(Bytes in)
{
    const MethodPassRun r = runMethodPass(std::move(in), mjbase::pass::WriteOnlyArrayCleaner{});
    return opsOf(inspect(r.out));
}

/// S1 直通形 k=2：size; anew; (dup; idx; null; aastore)*2; pop; return。
std::string_view directWindowBody()
{
    return "      ldc 3\n      anewarray java/lang/Object\n      dup\n"
           "      iconst_0\n      aconst_null\n      aastore\n      dup\n"
           "      iconst_1\n      aconst_null\n      aastore\n      pop\n"
           "      return\n";
}

/// S2 暂存形 k=1：size; anew; astore 1; aload 1; iconst_0; aconst_null;
/// aastore; return。
std::string_view localWindowBody()
{
    return "      ldc 2\n      anewarray java/lang/Object\n      astore 1\n"
           "      aload 1\n      iconst_0\n      aconst_null\n      aastore\n"
           "      return\n";
}

}   // namespace

// ── S1 直通形：分配 + 两组元素存储 + pop 整体消失 ──
TEST(WriteOnlyArrayCleanerTest, directWindowRemoved)
{
    const std::vector<Op> ops = runAndOps(asmM("t/U", "m", "()V", directWindowBody()));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── S2 暂存形：astore/aload 暂存链连分配整体消失 ──
TEST(WriteOnlyArrayCleanerTest, localWindowRemoved)
{
    const std::vector<Op> ops = runAndOps(asmM("t/U", "m", "()V", localWindowBody()));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── NEWARRAY 原始数组同面（S1 k=1，元素 int 常量） ──
TEST(WriteOnlyArrayCleanerTest, primitiveArrayWindowRemoved)
{
    const std::vector<Op> ops =
        runAndOps(asmM("t/U", "m", "()V",
                       "      ldc 1\n      newarray int\n      dup\n      iconst_0\n"
                       "      iconst_5\n      iastore\n      pop\n      return\n"));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── S1 k=0 特形：分配; pop 死分配 ──
TEST(WriteOnlyArrayCleanerTest, zeroStoreAllocationPopRemoved)
{
    const std::vector<Op> ops =
        runAndOps(asmM("t/U", "m", "()V",
                       "      ldc 1\n      anewarray java/lang/Object\n      pop\n"
                       "      return\n"));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 局部快捷形（astore_1/aload_1）同面（1 槽装配器自动发快捷形） ──
TEST(WriteOnlyArrayCleanerTest, shorthandLocalWindowRemoved)
{
    const std::vector<Op> ops = runAndOps(asmM("t/U", "m", "()V", localWindowBody()));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 读取在窗尾（arraylength）：窗保持 ──
TEST(WriteOnlyArrayCleanerTest, readonlyUseKeepsArray)
{
    const Bytes in = asmM("t/U", "m", "()V",
                          "      ldc 1\n      anewarray java/lang/Object\n      dup\n"
                          "      iconst_0\n      aconst_null\n      aastore\n"
                          "      arraylength\n      pop\n      return\n");
    const MethodPassRun  r = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── S2 逃逸面：窗后局部另有读点（arraylength 喂弹）→ 保守保留 ──
TEST(WriteOnlyArrayCleanerTest, escapedLocalKeepsArray)
{
    const Bytes in = asmM("t/U", "m", "()V",
                          "      ldc 2\n      anewarray java/lang/Object\n"
                          "      astore 1\n      aload 1\n      iconst_0\n"
                          "      aconst_null\n      aastore\n      aload 1\n"
                          "      arraylength\n      pop\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 元素生产者非纯（invokestatic）：整窗放弃 ──
TEST(WriteOnlyArrayCleanerTest, nonConstElementKeepsArray)
{
    const Bytes in = asmM("t/U", "m", "()V",
                          "      ldc 1\n      anewarray java/lang/Object\n      dup\n"
                          "      iconst_0\n      invokestatic Bog.m ()Ljava/lang/Object;\n"
                          "      aastore\n      pop\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── size 生产者非纯 int 常量（invokestatic 产出）：整窗放弃 ──
TEST(WriteOnlyArrayCleanerTest, impureSizeProducerKeepsArray)
{
    const Bytes in = asmM("t/U", "m", "()V",
                          "      invokestatic Bog.size ()I\n"
                          "      anewarray java/lang/Object\n      dup\n"
                          "      iconst_0\n      aconst_null\n      aastore\n"
                          "      pop\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 异常范围与窗相交：分配/存储的抛出可观测，全含亦弃 ──
TEST(WriteOnlyArrayCleanerTest, excOverlapKeepsArray)
{
    // 窗全含于异常域（handler 锚尾随死 return 位）。
    const Bytes in = asmM("t/U", "m", "()V",
                          "      L0:\n      ldc 1\n      anewarray java/lang/Object\n"
                          "      dup\n      iconst_0\n      aconst_null\n      aastore\n"
                          "      pop\n      return\n      Lend:\n      return\n",
                          "      { L0, Lend, Lend, * }\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 窗内锚标签（锚直指窗尾 pop，回边重执行分配语义改变）：整窗放弃 ──
// 例外①申报：原形为可达 goto 直指窗尾 pop（栈形断裂不可装配），以死段
// goto 锚同一位合法重表达——锚点落窗内的守卫决策面同实。
TEST(WriteOnlyArrayCleanerTest, labelAnchorInsideWindowKeepsArray)
{
    const Bytes in = asmM("t/U", "m", "()V",
                          "      L0:\n      ldc 1\n      anewarray java/lang/Object\n"
                          "      dup\n      iconst_0\n      aconst_null\n      aastore\n"
                          "      Lp:\n      pop\n      return\n"
                          "      Ldead:\n      goto Lp\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 管线串联裁决：调用窗由 scalarizer 自删零残段；窗外哑数组经本件清理 ──
TEST(WriteOnlyArrayCleanerTest, pipelineScalarizerResidueZeroThenDummyCleaned)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class",
                     asmM("Svc", "go", "([Ljava/lang/Object;)V",
                          "      aload 0\n      iconst_0\n      aaload\n"
                          "      checkcast java/lang/String\n      astore 1\n"
                          "      return\n"));
    addClassResource(container, "Caller.class",
                     asmM("Caller", "main", "()V",
                          "      iconst_1\n      anewarray java/lang/Object\n"
                          "      dup\n      ldc \"x\"\n      iconst_0\n      aastore\n"
                          "      invokestatic Svc.go ([Ljava/lang/Object;)V\n"
                          "      return\n"));
    addClassResource(container, "Dummy.class",
                     asmM("Dummy", "dummy", "()V", localWindowBody()));

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::ArgumentArrayScalarizer scalarizer;
    EXPECT_TRUE(scalarizer.run(container, gam).changed());

    // 裁决一：调用方装箱窗改写后零 anewarray/aastore 残段（装箱窗由
    // scalarizer 自删，死数组清理不背残留面）。
    const std::vector<Op> cops =
        methodOps(classResource(container, "Caller.class")->get(), "main");
    for (const Op op : cops) {
        EXPECT_NE(op, Op::ANEWARRAY);
        EXPECT_NE(op, Op::AASTORE);
    }

    // 裁决二：哑数组非 scalarizer 候选，survive 全局件；随后本件串联清理。
    const auto* dummy = classResource(container, "Dummy.class");
    ASSERT_NE(dummy, nullptr);
    const std::vector<Op> dopsBefore = methodOps(dummy->get(), "dummy");
    ASSERT_EQ(dopsBefore.size(), 8u);
    EXPECT_EQ(dopsBefore[1], Op::ANEWARRAY);   // size; anewarray; astore; ...
    const MethodPassRun r =
        runMethodPass(dummy->get(), mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> dops = opsOf(inspect(r.out));
    ASSERT_EQ(dops.size(), 1u);
    EXPECT_EQ(dops[0], Op::RETURN);
}

// ── 注册面 ──
TEST(WriteOnlyArrayCleanerTest, registeredInRegistry)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("WriteOnlyArrayCleaner"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("WriteOnlyArrayCleaner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
