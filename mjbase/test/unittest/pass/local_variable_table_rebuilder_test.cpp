/// @file        local_variable_table_rebuilder_test.cpp
/// @brief       LVT 重建/丢弃 pass 单测：变换后重算（陈旧表修复、内联后
///              重建）、丢弃口径、可验证性门槛（不可信名/多名槽/desc 矛盾
///              落 var 形态与推断 desc）、cat2 物理槽跳号、与
///              LocalVariableAnnotator 双向组合、无 LVT 类安全通过、幂等。
///
/// 载体：FixtureBuilder 装配最小 classfile（Code 嵌套 LVT/LVTT 载荷由
/// 用例内 UTF8 CP 物化），方法体为直行字节（无分支 → 无 SMT 面）。断言
/// 走「重建 → 重读 backing Code 嵌套表事实」口径；组合用例把
/// LocalVariableAnnotator（容器级）与 MethodInliner（容器级，与
/// method_inliner_test 同款驱动）真件接入驱动路径。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/local_variable_annotator.hpp"
#include "mjbase/pass/local_variable_table_rebuilder.hpp"
#include "mjbase/pass/method_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"

#include "shared/structural_pass_util.hpp"

using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::pass::LocalVariableTableRebuilder;
using mjbase::pass::LocalVariableTableRebuilderOptions;
using mjbase::pass::LvtRebuildMode;
using mjbase::test::Bytes;
using mjbase::test::FixtureBuilder;
using mjbase::test::putU16;
using mjbase::test::putU8;

namespace {

constexpr ca::u16 kAccPublic = 0x0001;
constexpr ca::u16 kAccStatic = 0x0008;

// ── 直行方法体（无分支；pc 注释即重建后的期望锚点）──

/// f(I)I：pc0 iconst_0；pc1 istore_1；pc2 iload_1；pc3 ireturn（len 4）。
/// 存活段：slot0 形参 [0,4)；slot1 [2,4)，均 I。
Bytes intLocalBody()
{
    Bytes code;
    putU8(code, 0x03);   // iconst_0
    putU8(code, 0x3C);   // istore_1
    putU8(code, 0x1B);   // iload_1
    putU8(code, 0xAC);   // ireturn
    return code;
}

/// g(J)J：pc0 lconst_0；pc1 lstore_2；pc2 lload_2；pc3 lreturn（len 4）。
/// 存活段：slot0 形参 J [0,4)（占槽 0-1）；slot2 [2,4) J（cat2 物理槽）。
Bytes longLocalBody()
{
    Bytes code;
    putU8(code, 0x09);   // lconst_0
    putU8(code, 0x3F);   // lstore_2
    putU8(code, 0x1F);   // lload_2
    putU8(code, 0xAD);   // lreturn
    return code;
}

/// o()Ljava/lang/Object;：pc0-1 ldc "s"；pc2 astore_1；pc3 aload_1；
/// pc4 areturn（len 5）。slot1 [3,5)，Strict 推断 Ljava/lang/String;。
Bytes refLocalBody(FixtureBuilder& b)
{
    Bytes code;
    putU8(code, 0x12);                  // ldc
    putU16(code, b.strConst("s"));      // String 常量
    putU8(code, 0x4C);                  // astore_1
    putU8(code, 0x2B);                  // aload_1
    putU8(code, 0xB0);                  // areturn
    return code;
}

/// 实例方法 m()V：pc0 aload_0；pc1 pop；pc2 return（len 3）。
/// 存活段：slot0 this [0,3)，描述符 L<class>;。
Bytes thisBody()
{
    Bytes code;
    putU8(code, 0x2A);   // aload_0
    putU8(code, 0x57);   // pop
    putU8(code, 0xB1);   // return
    return code;
}

/// Code 嵌套 LVT/LVTT 载荷（JVMS 4.7.13/4.7.14 同构布局）。
/// 行 = (startPc, length, name, desc, slot)；名字/描述符经 builder 物化。
Bytes varTablePayload(FixtureBuilder&                                    b,
                      const std::vector<std::tuple<ca::u32, ca::u32, std::string_view,
                                                   std::string_view, ca::u32>>& rows)
{
    Bytes p;
    putU16(p, static_cast<ca::u16>(rows.size()));
    for (const auto& [start, length, name, desc, slot] : rows) {
        putU16(p, static_cast<ca::u16>(start));
        putU16(p, static_cast<ca::u16>(length));
        putU16(p, b.utf8(name));
        putU16(p, b.utf8(desc));
        putU16(p, static_cast<ca::u16>(slot));
    }
    return p;
}

struct RebuildRun
{
    std::shared_ptr<jbc::ir::ClassSession>    session;
    mjbase::spi::PreservedAnalyses            pa;
    LocalVariableTableRebuilder::Stats        stats;
};

RebuildRun runRebuild(const Bytes& bytes, const LocalVariableTableRebuilderOptions& opts = {})
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    auto                              session = std::move(created).unwrap();
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    LocalVariableTableRebuilder       pass(opts);
    RebuildRun                        run;
    run.session = session;
    run.pa      = pass.run(*session, cam);
    run.stats   = pass.stats();
    return run;
}

/// LVT 行文本视图（backing Code 嵌套表重读）。
struct LvtRow
{
    ca::u32     startPc = 0;
    ca::u32     length  = 0;
    ca::u32     slot    = 0;
    std::string name;
    std::string desc;
};

std::vector<LvtRow> readLvtRows(jbc::ir::ClassSession& session, ca::usize methodIdx = 0)
{
    std::vector<LvtRow>   out;
    jbc::core::ClassFile& cf = session.backing();
    auto                  decoded = cf.decodeCode(cf.methods[methodIdx]);
    if (decoded.is_err()) return out;
    jbc::core::CodeAttr* code = decoded.unwrap();
    if (code == nullptr) return out;
    for (jbc::core::AttrSlot& slot : code->attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "LocalVariableTable")) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) break;
        const auto* table = ca::core::tag_cast::cast<const jbc::core::LocalVariableTableAttr>(
            slot.peek());
        if (table == nullptr) break;
        for (const jbc::core::LocalVariableTableAttr::Row& row : table->rows) {
            LvtRow entry;
            entry.startPc = row.startPc;
            entry.length  = row.length;
            entry.slot    = row.slot;
            entry.name    = std::string(cf.cp.utf8Of(row.nameCp));
            entry.desc    = std::string(cf.cp.utf8Of(row.descCp));
            out.push_back(std::move(entry));
        }
        break;
    }
    std::sort(out.begin(), out.end(), [](const LvtRow& a, const LvtRow& b) {
        return a.startPc != b.startPc ? a.startPc < b.startPc : a.slot < b.slot;
    });
    return out;
}

/// Code 嵌套属性名集合（LVT/LVTT/LNT 有无断言面）。
std::set<std::string> codeAttrNames(jbc::ir::ClassSession& session, ca::usize methodIdx = 0)
{
    std::set<std::string>   out;
    jbc::core::ClassFile&   cf  = session.backing();
    auto                    decoded = cf.decodeCode(cf.methods[methodIdx]);
    if (decoded.is_err()) return out;
    const jbc::core::CodeAttr* code = decoded.unwrap();
    if (code == nullptr) return out;
    for (const jbc::core::AttrSlot& slot : code->attrs) {
        out.emplace(std::string(cf.cp.utf8Of(slot.nameCp)));
    }
    return out;
}

/// 字节级 LVT 行读取（写回产物断言面）。
std::vector<LvtRow> readLvtRowsOfBytes(const Bytes& bytes, ca::usize methodIdx = 0)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    auto session = std::move(created).unwrap();
    return readLvtRows(*session, methodIdx);
}

/// 行事实断言：expected = (startPc, length, name, desc, slot)，按 (startPc, slot) 序。
void expectRows(const std::vector<LvtRow>&                                 actual,
                const std::vector<std::tuple<ca::u32, ca::u32, std::string, std::string,
                                             ca::u32>>&                    expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (ca::usize i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].startPc, std::get<0>(expected[i])) << "row " << i;
        EXPECT_EQ(actual[i].length, std::get<1>(expected[i])) << "row " << i;
        EXPECT_EQ(actual[i].name, std::get<2>(expected[i])) << "row " << i;
        EXPECT_EQ(actual[i].desc, std::get<3>(expected[i])) << "row " << i;
        EXPECT_EQ(actual[i].slot, std::get<4>(expected[i])) << "row " << i;
    }
}

void addClassResource(ResourceContainer& container, const std::string& uri, const Bytes& bytes)
{
    container.addResource(uri, std::make_unique<ClassResource>(uri, bytes));
}

Bytes containerBytes(ResourceContainer& container, const std::string& uri)
{
    const auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr) << uri;
    const auto* cls = ca::core::tag_cast::cast<const ClassResource*>(resource);
    EXPECT_NE(cls, nullptr) << uri;
    return cls != nullptr ? cls->get() : Bytes{};
}

/// LocalVariableAnnotator 真件驱动（容器级，与生产一致）。
void runAnnotator(ResourceContainer& container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::LocalVariableAnnotator pass;
    (void)pass.run(container, gam);
}

/// MethodInliner 真件驱动（容器级，与 method_inliner_test 同款驱动）。
void runInliner(ResourceContainer& container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::MethodInliner        pass;
    (void)pass.run(container, gam);
}

}   // namespace

// ── a) 变换后重算：陈旧/越界行随存活区间更新，可信名与 desc 逐字保留 ──

TEST(MjPassLocalVariableTableRebuilderTest, RebuildsStaleRangesAndCarriesTrustedNames)
{
    FixtureBuilder b("R");
    b.addMethod({"f", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                 {},
                 {{"LocalVariableTable",
                   varTablePayload(b,
                                   {{0, 9, "n", "I", 0},       // 越界 length
                                    {3, 6, "x", "I", 1}})}}});   // 错位 startPc + 越界
    const auto run = runRebuild(b.assemble());

    EXPECT_TRUE(run.pa.changed());
    expectRows(readLvtRows(*run.session),
               {{0, 4, "n", "I", 0},          // 形参段全范围，名字/desc 逐字保留
                {2, 2, "x", "I", 1}});        // 存储点后起，offset/length 随之更新
    EXPECT_EQ(run.stats.rebuiltMethods, 1u);
    EXPECT_EQ(run.stats.carriedNames, 2u);
    EXPECT_EQ(run.stats.varNames, 0u);
    EXPECT_EQ(run.stats.methods, 1u);
    const auto names = codeAttrNames(*run.session);
    EXPECT_EQ(names.count("LocalVariableTypeTable"), 0u) << "LVTT 随重建摘除";
}

// ── a) 真变换：MethodInliner 内联后重建（调用方槽位扩展 + 新槽 var 形态）──

TEST(MjPassLocalVariableTableRebuilderTest, RebuildsAfterInlinerTransformation)
{
    ResourceContainer container;
    {
        // H.k(I)I public static 纯：iconst_5; istore_1; iload_1; iload_0; iadd; ireturn
        FixtureBuilder bh("H");
        Bytes callee;
        putU8(callee, 0x04);          // iconst_5
        putU8(callee, 0x3C);          // istore_1
        putU8(callee, 0x1B);          // iload_1
        putU8(callee, 0x1A);          // iload_0
        putU8(callee, 0x60);          // iadd
        putU8(callee, 0xAC);          // ireturn
        bh.addMethod({"k", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 2, 2});
        addClassResource(container, "H.class", bh.assemble());
    }
    {
        // C.m(I)I：iload_0; invokestatic H.k; iadd; ireturn（len 6）。
        // 陈旧 LVT：形参行罩住内联前全长。
        FixtureBuilder bc("C");
        const ca::u16 ref = bc.methodRef("H", "k", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);                       // iload_0
        putU8(caller, 0xB8); putU16(caller, ref);  // invokestatic H.k
        putU8(caller, 0x60);                       // iadd
        putU8(caller, 0xAC);                       // ireturn
        bc.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 2, 1,
                      {},
                      {{"LocalVariableTable", varTablePayload(bc, {{0, 6, "n", "I", 0}})}}});
        addClassResource(container, "C.class", bc.assemble());
    }

    runInliner(container);
    const auto run = runRebuild(containerBytes(container, "C.class"));

    EXPECT_TRUE(run.pa.changed());
    // 内联后布局（base=1）：pc0 iload_0；pc1 istore 1（实参落槽）；pc3 iconst_5；
    // pc4 istore 2（被调 slot1→2）；pc6 iload 2；pc8 iload 1；pc10 iadd；
    // pc11 goto；pc14 iload_0；pc15 iadd；pc16 ireturn（len 17）。
    // 形参名 n 保真沿用（内联 lower 的端点映射已到全长）；被调新槽无原表
    // 依据 → var 形态兜底。
    expectRows(readLvtRows(*run.session),
               {{0, 17, "n", "I", 0},
                {3, 14, "var1", "I", 1},
                {6, 11, "var2", "I", 2}});
    EXPECT_EQ(run.stats.carriedNames, 1u);
    EXPECT_EQ(run.stats.varNames, 2u);
}

// ── b) 丢弃口径：LVT/LVTT 摘除，产物干净；LNT 不在收口面 ──

TEST(MjPassLocalVariableTableRebuilderTest, DropModeStripsLvtAndLvtt)
{
    FixtureBuilder b("DR");
    b.addMethod({"f", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                 {},
                 {{"LineNumberTable", Bytes{0x00, 0x01, 0x00, 0x00, 0x00, 0x2A}},
                  {"LocalVariableTable", varTablePayload(b, {{0, 4, "n", "I", 0}})},
                  {"LocalVariableTypeTable", varTablePayload(b, {{0, 4, "n", "TT;", 0}})}}});
    LocalVariableTableRebuilderOptions opts;
    opts.mode                        = LvtRebuildMode::Drop;
    const auto run                   = runRebuild(b.assemble(), opts);

    EXPECT_TRUE(run.pa.changed());
    const auto names = codeAttrNames(*run.session);
    EXPECT_EQ(names.count("LocalVariableTable"), 0u);
    EXPECT_EQ(names.count("LocalVariableTypeTable"), 0u);
    EXPECT_EQ(names.count("LineNumberTable"), 1u) << "LNT 不在丢弃面";
    EXPECT_EQ(run.stats.droppedMethods, 1u);

    // 丢弃幂等：无表再跑零改动。
    const Bytes settled = [&] {
        auto written = jbc::core::writeClass(run.session->backing());
        EXPECT_TRUE(written.is_ok());
        return std::move(written).unwrap();
    }();
    const auto second = runRebuild(settled, opts);
    EXPECT_FALSE(second.pa.changed());
    EXPECT_EQ(second.stats.droppedMethods, 0u);
}

// ── b) 可验证性门槛：不可信名 / 槽多名 / desc 矛盾 → var 形态与推断 desc ──

TEST(MjPassLocalVariableTableRebuilderTest, UntrustedOrAmbiguousNamesFallToVarForm)
{
    {
        // 不可信名（非 ASCII 字节）：名落 var1，desc 与声明和解一致仍为 I。
        FixtureBuilder b("U");
        const std::string weird = "\xC3\xA5x";   // 首字节 ≥0x80 → lvtNameTrustworthy 拒绝
        b.addMethod({"h", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                     {},
                     {{"LocalVariableTable",
                       varTablePayload(b, {{0, 4, "p", "I", 0}, {3, 6, weird, "I", 1}})}}});
        const auto run = runRebuild(b.assemble());
        EXPECT_TRUE(run.pa.changed());
        expectRows(readLvtRows(*run.session),
                   {{0, 4, "p", "I", 0},          // 可信名沿用
                    {2, 2, "var1", "I", 1}});     // 不可信名 → var 形态
        EXPECT_EQ(run.stats.carriedNames, 1u);
        EXPECT_EQ(run.stats.varNames, 1u);
    }
    {
        // 槽多名（作用域复用，位置对应不可证）：整槽 var 形态。
        FixtureBuilder b("AM");
        b.addMethod({"h", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                     {},
                     {{"LocalVariableTable",
                       varTablePayload(b, {{0, 1, "a", "I", 1}, {1, 3, "b", "I", 1}})}}});
        const auto run = runRebuild(b.assemble());
        EXPECT_TRUE(run.pa.changed());
        const auto rows = readLvtRows(*run.session);
        ASSERT_EQ(rows.size(), 2u);
        EXPECT_EQ(rows[0].name, "var0");
        EXPECT_EQ(rows[1].name, "var1") << "槽上可信名 ≥2 → var 形态，不猜位置对应";
    }
    {
        // desc 矛盾（声明 J、实际存 int）：声明不可验证 → 落推断值 I。
        FixtureBuilder b("DC");
        b.addMethod({"h", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                     {},
                     {{"LocalVariableTable",
                       varTablePayload(b, {{0, 9, "n", "I", 0}, {3, 6, "x", "J", 1}})}}});
        const auto run = runRebuild(b.assemble());
        EXPECT_TRUE(run.pa.changed());
        expectRows(readLvtRows(*run.session),
                   {{0, 4, "n", "I", 0},
                    {2, 2, "x", "I", 1}});   // 名沿用，desc 被推断值替换
    }
}

// ── 保真细节：Object 擦除槽不提升、this 沿用、cat2 物理槽跳号 ──

TEST(MjPassLocalVariableTableRebuilderTest, KeepsObjectSlotAndCarriesThis)
{
    {
        // Object 声明槽存 String：可证兼容（javac 擦除槽形态）→ 保 Object，
        // 不做 Annotator 式类型提升。
        FixtureBuilder b("OB");
        b.addMethod({"o", "()Ljava/lang/Object;", refLocalBody(b), kAccPublic | kAccStatic,
                     {}, 1, 2,
                     {},
                     {{"LocalVariableTable",
                       varTablePayload(b, {{0, 9, "obj", "Ljava/lang/Object;", 1}})}}});
        const auto run = runRebuild(b.assemble());
        EXPECT_TRUE(run.pa.changed());
        expectRows(readLvtRows(*run.session), {{3, 2, "obj", "Ljava/lang/Object;", 1}});
    }
    {
        // 实例方法 this 行：可信名 + 声明 desc 与种子一致 → 逐字保留。
        FixtureBuilder b("IR");
        b.addMethod({"m", "()V", thisBody(), kAccPublic, {}, 1, 1,
                     {},
                     {{"LocalVariableTable", varTablePayload(b, {{0, 3, "this", "LIR;", 0}})}}});
        const auto run = runRebuild(b.assemble());
        EXPECT_TRUE(run.pa.changed());
        expectRows(readLvtRows(*run.session), {{0, 3, "this", "LIR;", 0}});
        EXPECT_EQ(run.stats.carriedNames, 1u);
    }
    {
        // cat2：long 局部占物理槽 2（隐含槽 3 不发行），编号自然跳号。
        FixtureBuilder b("CG");
        b.addMethod({"g", "(J)J", longLocalBody(), kAccPublic | kAccStatic, {}, 2, 4,
                     {},
                     {{"LocalVariableTable",
                       varTablePayload(b, {{0, 99, "v", "J", 0}, {0, 99, "w", "J", 2}})}}});
        const auto run = runRebuild(b.assemble());
        EXPECT_TRUE(run.pa.changed());
        expectRows(readLvtRows(*run.session),
                   {{0, 4, "v", "J", 0},
                    {2, 2, "w", "J", 2}});   // 槽 1/3 无行（cat2 跳号保持）
    }
}

// ── d) 无 LVT 的类安全通过：重建口径无表新建，丢弃口径零改动 ──

TEST(MjPassLocalVariableTableRebuilderTest, PassesClassWithoutLocalVariableTable)
{
    {
        FixtureBuilder b("N");
        b.addMethod({"f", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2});
        const auto run = runRebuild(b.assemble());
        EXPECT_TRUE(run.pa.changed()) << "重建口径：无原表也按存活区间新建";
        expectRows(readLvtRows(*run.session),
                   {{0, 4, "var0", "I", 0},
                    {2, 2, "var1", "I", 1}});
        EXPECT_EQ(run.stats.carriedNames, 0u);
        EXPECT_EQ(run.stats.varNames, 2u);
    }
    {
        FixtureBuilder b("N2");
        b.addMethod({"f", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2});
        LocalVariableTableRebuilderOptions opts;
        opts.mode      = LvtRebuildMode::Drop;
        const auto run = runRebuild(b.assemble(), opts);
        EXPECT_FALSE(run.pa.changed()) << "丢弃口径：无表零改动";
        EXPECT_EQ(run.stats.droppedMethods, 0u);
    }
}

// ── LVTT-only：重建口径摘除 LVTT 并按存活区间新建 LVT ──

TEST(MjPassLocalVariableTableRebuilderTest, RebuildsFromLvttOnlyFixture)
{
    FixtureBuilder b("LO");
    b.addMethod({"f", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                 {},
                 {{"LocalVariableTypeTable", varTablePayload(b, {{0, 4, "n", "TT;", 1}})}}});
    const auto run = runRebuild(b.assemble());
    EXPECT_TRUE(run.pa.changed());
    const auto names = codeAttrNames(*run.session);
    EXPECT_EQ(names.count("LocalVariableTypeTable"), 0u);
    expectRows(readLvtRows(*run.session),
               {{0, 4, "var0", "I", 0},
                {2, 2, "var1", "I", 1}});
}

// ── c) 与 LocalVariableAnnotator 组合：双向 ──

TEST(MjPassLocalVariableTableRebuilderTest, ComposesWithLocalVariableAnnotator)
{
    {
        // Annotator → Rebuilder：varN 规范形是重建的不动点（零改动确认）。
        ResourceContainer container;
        FixtureBuilder    b("AF");
        b.addMethod({"f", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                     {},
                     {{"LocalVariableTable",
                       varTablePayload(b, {{0, 4, "x", "I", 0}, {3, 6, "y", "I", 1}})}}});
        addClassResource(container, "AF.class", b.assemble());
        runAnnotator(container);

        const Bytes annotated = containerBytes(container, "AF.class");
        const auto  run       = runRebuild(annotated);
        EXPECT_FALSE(run.pa.changed()) << "varN 名可信、desc 与推断一致 → 重建不动点";
        expectRows(readLvtRows(*run.session),
                   {{0, 4, "var0", "I", 0},
                    {2, 2, "var1", "I", 1}});
    }
    {
        // Rebuilder → Annotator：保真名沿用、范围修复；Annotator 随后按自身
        // 契约把保真名归一为 varN（双向组合各归各位，互不越界）。
        ResourceContainer container;
        FixtureBuilder    b("RA");
        b.addMethod({"f", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                     {},
                     {{"LocalVariableTable",
                       varTablePayload(b, {{0, 4, "x", "I", 0}, {3, 6, "y", "I", 1}})}}});
        addClassResource(container, "RA.class", b.assemble());

        {
            const auto first = runRebuild(containerBytes(container, "RA.class"));
            EXPECT_TRUE(first.pa.changed());
            expectRows(readLvtRows(*first.session),
                       {{0, 4, "x", "I", 0},
                        {2, 2, "y", "I", 1}});   // 保真名沿用（陈旧范围已修）
            auto written = jbc::core::writeClass(first.session->backing());
            ASSERT_TRUE(written.is_ok());
            container.replaceResource(
                "RA.class", std::make_unique<ClassResource>("RA.class", std::move(written).unwrap()));
        }
        runAnnotator(container);
        expectRows(readLvtRowsOfBytes(containerBytes(container, "RA.class")),
                   {{0, 4, "var0", "I", 0},
                    {2, 2, "var1", "I", 1}});
    }
}

// ── 幂等：同会话与跨会话二次运行严格 no-op ──

TEST(MjPassLocalVariableTableRebuilderTest, IdempotentSecondRun)
{
    FixtureBuilder b("R2");
    b.addMethod({"f", "(I)I", intLocalBody(), kAccPublic | kAccStatic, {}, 1, 2,
                 {},
                 {{"LocalVariableTable",
                   varTablePayload(b, {{0, 9, "n", "I", 0}, {3, 6, "x", "I", 1}})}}});
    const Bytes original = b.assemble();

    const auto first = runRebuild(original);
    EXPECT_TRUE(first.pa.changed());

    // 同会话二次运行（重建槽已解码）。
    {
        ca::str::Utf8StringArena         arena;
        mjbase::spi::ClassAnalysisManager cam(arena);
        LocalVariableTableRebuilder       pass;
        const auto pa = pass.run(*first.session, cam);
        EXPECT_FALSE(pa.changed());
        EXPECT_EQ(pass.stats().rebuiltMethods, 0u);
    }
    // 跨会话二次运行（写回字节重载，嵌套槽回到 lazy 形态）。
    {
        auto written = jbc::core::writeClass(first.session->backing());
        EXPECT_TRUE(written.is_ok());
        const Bytes settled = std::move(written).unwrap();
        const auto second = runRebuild(settled);
        EXPECT_FALSE(second.pa.changed()) << "嵌套 LVT 槽 lazy 解码路径下的幂等门";
        expectRows(readLvtRows(*second.session),
                   {{0, 4, "n", "I", 0}, {2, 2, "x", "I", 1}});
    }
}
