/// @file        local_variable_table_rebuilder_test.cpp
/// @brief       LVT 重建/丢弃 pass 单测：变换后重算（陈旧表修复、内联后
///              重建）、丢弃口径、可验证性门槛（不可信名/多名槽/desc 矛盾
///              落 var 形态与推断 desc）、cat2 物理槽跳号、与
///              LocalVariableAnnotator 双向组合、无 LVT 类安全通过、幂等、
///              注册面与 opt-in 接线钉形（默认注册表种子 + 管线配置启用
///              生效 + default-optimize 零变化）。
///
/// 载体口径：方法体一律 jasm 文本装配（JvmJasmService）；pc 锚点用 quick
/// 形助记符（istore_1/lstore_2 等，定长单字节）保持可推演。陈旧调试表
/// 经 libjbc typed 属性模型挂载（LocalVariableTableAttr/
/// LineNumberTableAttr + AttrSlot::makeNew + Code 槽置脏），不走字节拼装。
/// 断言走「重建 → 重读 backing Code 嵌套表事实」口径；组合用例把
/// LocalVariableAnnotator（容器级）与 MethodInliner（容器级，与
/// method_inliner_test 同款驱动）真件接入驱动路径。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/local_variable_annotator.hpp"
#include "mjbase/pass/local_variable_table_rebuilder.hpp"
#include "mjbase/pass/method_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/pipeline_config.hpp"

using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::pass::LocalVariableTableRebuilder;
using mjbase::pass::LocalVariableTableRebuilderOptions;
using mjbase::pass::LvtRebuildMode;

namespace {

using Bytes = std::vector<ca::u8>;

/// 行事实规格：(startPc, length, name, desc, slot)。
using LvtRowSpec = std::tuple<ca::u32, ca::u32, std::string_view, std::string_view, ca::u32>;

// ── jasm 文本装配 ──

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 文本汇编最小类（单方法；汇编失败即测试失败，诊断逐条落地）。
Bytes assembleClass(const std::string& className, const std::string& methodDecl,
                    std::string_view instructions)
{
    mjbase::jasm::JvmJasmService service;
    const std::string source =
        ".super java/lang/Object\n.class public super " + className + " {\n  .method "
        + methodDecl + " {\n    code: {\n" + std::string(instructions)
        + "    }\n  }\n}\n";
    const auto sourceName = utf16(className + ".jasm");
    const auto sourceText = utf16(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败";
        return {};
    }
    return result.value->bytes;
}

// ── 直行方法体（quick 形助记符定长单字节；pc 注释即重建后的期望锚点）──

/// f(I)I：pc0 iconst_0；pc1 istore_1；pc2 iload_1；pc3 ireturn（len 4）。
/// 存活段：slot0 形参 [0,4)；slot1 [2,4)，均 I。
constexpr std::string_view kIntBody =
    "      iconst_0\n      istore_1\n      iload_1\n      ireturn\n";

/// g(J)J：pc0 lconst_0；pc1 lstore_2；pc2 lload_2；pc3 lreturn（len 4）。
/// slot2 为 cat2 物理槽（占 2/3 两槽，行只落 2）。
constexpr std::string_view kLongBody =
    "      lconst_0\n      lstore_2\n      lload_2\n      lreturn\n";

/// 实例方法 m()V：pc0 aload_0；pc1 pop；pc2 return（len 3）。
constexpr std::string_view kThisBody =
    "      aload_0\n      pop\n      return\n";

/// o()Ljava/lang/Object;：pc0 new String（3B）；pc3 dup；pc4 invokespecial
/// <init>（3B）；pc7 astore_1；pc8 aload_1；pc9 areturn（len 10）。
/// slot1 存活段 [8,10)，Strict 推断 Ljava/lang/String;。
constexpr std::string_view kRefBody =
    "      new java/lang/String\n      dup\n"
    "      invokespecial java/lang/String.<init> ()V\n"
    "      astore_1\n      aload_1\n      areturn\n";

/// H.k(I)I 被调体：pc0 iconst_5；pc1 istore_1；pc2 iload_1；pc3 iload_0；
/// pc4 iadd；pc5 ireturn（纯、无调用面）。
constexpr std::string_view kCalleeBody =
    "      iconst_5\n      istore_1\n      iload_1\n      iload_0\n      iadd\n      ireturn\n";

/// C.m(I)I 调用体：pc0 iconst_1；pc1 iload_0；pc2 invokestatic H.k（3B）；
/// pc5 iadd；pc6 ireturn（len 7，栈形可验证）。
constexpr std::string_view kCallerBody =
    "      iconst_1\n      iload_0\n      invokestatic H.k (I)I\n      iadd\n      ireturn\n";

// ── 陈旧调试表挂载（typed 属性模型；挂载后写回字节再交 pass）──

Bytes withVarTable(const Bytes& bytes, ca::usize methodIdx, std::string_view attrName,
                   const std::vector<LvtRowSpec>& rows)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto session = std::move(created).unwrap();
    jbc::core::ClassFile& cf = session->backing();
    jbc::core::AttrSlot*  codeSlot = nullptr;
    for (jbc::core::AttrSlot& slot : cf.methods[methodIdx].attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, "Code")) codeSlot = &slot;
    }
    EXPECT_NE(codeSlot, nullptr) << "Code 槽缺失";
    if (codeSlot == nullptr) return bytes;
    auto decoded = cf.decodeAttr(*codeSlot);
    EXPECT_TRUE(decoded.is_ok());
    if (decoded.is_err()) return bytes;
    auto* codeAttr = ca::core::tag_cast::cast<jbc::core::CodeAttr>(decoded.unwrap());
    EXPECT_NE(codeAttr, nullptr);
    if (codeAttr == nullptr) return bytes;

    auto table = std::make_unique<jbc::core::LocalVariableTableAttr>();
    table->rows.reserve(rows.size());
    for (const auto& [start, length, name, desc, slot] : rows) {
        jbc::core::LocalVariableTableAttr::Row out;
        out.startPc = start;
        out.length  = length;
        out.nameCp  = cf.cp.addUtf8(name);
        out.descCp  = cf.cp.addUtf8(desc);
        out.slot    = slot;
        table->rows.push_back(out);
    }
    codeAttr->attrs.push_back(
        jbc::core::AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(table)));
    (void)codeSlot->mutate();
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return bytes;
    return std::move(written).unwrap();
}

Bytes withLvt(const Bytes& bytes, ca::usize methodIdx, const std::vector<LvtRowSpec>& rows)
{
    return withVarTable(bytes, methodIdx, "LocalVariableTable", rows);
}

Bytes withLvtt(const Bytes& bytes, ca::usize methodIdx, const std::vector<LvtRowSpec>& rows)
{
    return withVarTable(bytes, methodIdx, "LocalVariableTypeTable", rows);
}

/// LNT 挂载（typed LineNumberTableAttr；丢弃口径的「不在收口面」对照物）。
Bytes withLnt(const Bytes& bytes, ca::usize methodIdx,
              const std::vector<std::pair<ca::u32, ca::u32>>& rows)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto session = std::move(created).unwrap();
    jbc::core::ClassFile& cf = session->backing();
    jbc::core::AttrSlot*  codeSlot = nullptr;
    for (jbc::core::AttrSlot& slot : cf.methods[methodIdx].attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, "Code")) codeSlot = &slot;
    }
    EXPECT_NE(codeSlot, nullptr) << "Code 槽缺失";
    if (codeSlot == nullptr) return bytes;
    auto decoded = cf.decodeAttr(*codeSlot);
    EXPECT_TRUE(decoded.is_ok());
    if (decoded.is_err()) return bytes;
    auto* codeAttr = ca::core::tag_cast::cast<jbc::core::CodeAttr>(decoded.unwrap());
    EXPECT_NE(codeAttr, nullptr);
    if (codeAttr == nullptr) return bytes;

    auto table = std::make_unique<jbc::core::LineNumberTableAttr>();
    for (const auto& [startPc, line] : rows) {
        table->rows.push_back({startPc, line});
    }
    codeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(
        cf.cp.addUtf8("LineNumberTable"), std::move(table)));
    (void)codeSlot->mutate();
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return bytes;
    return std::move(written).unwrap();
}

// ── pass 驱动与断言面 ──

struct RebuildRun
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    mjbase::spi::PreservedAnalyses         pa;
    LocalVariableTableRebuilder::Stats     stats;
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
    std::vector<LvtRow>      out;
    jbc::core::ClassFile&    cf      = session.backing();
    auto                     decoded = cf.decodeCode(cf.methods[methodIdx]);
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
    std::set<std::string>    out;
    jbc::core::ClassFile&    cf      = session.backing();
    auto                     decoded = cf.decodeCode(cf.methods[methodIdx]);
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
void expectRows(const std::vector<LvtRow>& actual,
                const std::vector<std::tuple<ca::u32, ca::u32, std::string, std::string,
                                             ca::u32>>& expected)
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
    ca::str::Utf8StringArena             arena;
    mjbase::spi::GlobalAnalysisManager   gam(arena);
    mjbase::pass::LocalVariableAnnotator pass;
    (void)pass.run(container, gam);
}

/// MethodInliner 真件驱动（容器级，与 method_inliner_test 同款驱动）。
void runInliner(ResourceContainer& container)
{
    ca::str::Utf8StringArena         arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::MethodInliner      pass;
    (void)pass.run(container, gam);
}

}   // namespace

// ── a) 变换后重算：陈旧/越界行随存活区间更新，可信名与 desc 逐字保留 ──

TEST(MjPassLocalVariableTableRebuilderTest, RebuildsStaleRangesAndCarriesTrustedNames)
{
    const Bytes bytes = withLvt(assembleClass("R", "public static f (I)I", kIntBody), 0,
                                {{0, 9, "n", "I", 0},          // 越界 length
                                 {3, 6, "x", "I", 1}});        // 错位 startPc + 越界
    const auto run = runRebuild(bytes);

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
    addClassResource(container, "H.class",
                     assembleClass("H", "public static k (I)I", kCalleeBody));
    const Bytes callerBytes = assembleClass("C", "public static m (I)I", kCallerBody);
    // 陈旧 LVT：形参行罩住内联前全长（len 7）。
    addClassResource(container, "C.class", withLvt(callerBytes, 0, {{0, 7, "n", "I", 0}}));

    runInliner(container);
    const auto run = runRebuild(containerBytes(container, "C.class"));

    EXPECT_TRUE(run.pa.changed());
    // 内联后布局（static 被调，base=1）：pc0 iconst_1；pc1 iload_0（原实参
    // 装载）；pc2 istore 1（实参落被调槽 0）；pc4 iconst_5；pc5 istore 2
    // （被调 slot1→2）；pc7 iload 2；pc9 iload 1；pc11 iadd；pc12 goto；
    // pc15 iadd；pc16 ireturn（len 17）。形参名 n 保真沿用；被调两新槽无
    // 原表依据 → var 形态兜底。
    expectRows(readLvtRows(*run.session),
               {{0, 17, "n", "I", 0},
                {4, 13, "var1", "I", 1},
                {7, 10, "var2", "I", 2}});
    EXPECT_EQ(run.stats.carriedNames, 1u);
    EXPECT_EQ(run.stats.varNames, 2u);
}

// ── b) 丢弃口径：LVT/LVTT 摘除，产物干净；LNT 不在收口面 ──

TEST(MjPassLocalVariableTableRebuilderTest, DropModeStripsLvtAndLvtt)
{
    const Bytes bare = assembleClass("DR", "public static f (I)I", kIntBody);
    const Bytes bytes =
        withLnt(withLvtt(withLvt(bare, 0, {{0, 4, "n", "I", 0}}), 0, {{0, 4, "n", "TT;", 0}}),
                0, {{0, 42}});
    LocalVariableTableRebuilderOptions opts;
    opts.mode      = LvtRebuildMode::Drop;
    const auto run = runRebuild(bytes, opts);

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
        const std::string weird = "\xC3\xA5x";   // 首字节 ≥0x80 → lvtNameTrustworthy 拒绝
        const Bytes       bytes =
            withLvt(assembleClass("U", "public static h (I)I", kIntBody), 0,
                    {{0, 4, "p", "I", 0}, {3, 6, weird, "I", 1}});
        const auto run = runRebuild(bytes);
        EXPECT_TRUE(run.pa.changed());
        expectRows(readLvtRows(*run.session),
                   {{0, 4, "p", "I", 0},          // 可信名沿用
                    {2, 2, "var1", "I", 1}});     // 不可信名 → var 形态
        EXPECT_EQ(run.stats.carriedNames, 1u);
        EXPECT_EQ(run.stats.varNames, 1u);
    }
    {
        // 槽多名（作用域复用，位置对应不可证）：整槽 var 形态。
        const Bytes bytes =
            withLvt(assembleClass("AM", "public static h (I)I", kIntBody), 0,
                    {{0, 1, "a", "I", 1}, {1, 3, "b", "I", 1}});
        const auto run = runRebuild(bytes);
        EXPECT_TRUE(run.pa.changed());
        const auto rows = readLvtRows(*run.session);
        ASSERT_EQ(rows.size(), 2u);
        EXPECT_EQ(rows[0].name, "var0");
        EXPECT_EQ(rows[1].name, "var1") << "槽上可信名 ≥2 → var 形态，不猜位置对应";
    }
    {
        // desc 矛盾（声明 J、实际存 int）：声明不可验证 → 落推断值 I。
        const Bytes bytes =
            withLvt(assembleClass("DC", "public static h (I)I", kIntBody), 0,
                    {{0, 9, "n", "I", 0}, {3, 6, "x", "J", 1}});
        const auto run = runRebuild(bytes);
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
        // Object 声明槽存 String（Strict 推断 Ljava/lang/String; 与声明
        // Object 形态可证兼容——javac 擦除槽形态）→ 保 Object，不做
        // Annotator 式类型提升。
        const Bytes bytes =
            withLvt(assembleClass("OB", "public static o ()Ljava/lang/Object;", kRefBody), 0,
                    {{0, 99, "obj", "Ljava/lang/Object;", 1}});
        const auto run = runRebuild(bytes);
        EXPECT_TRUE(run.pa.changed());
        expectRows(readLvtRows(*run.session), {{8, 2, "obj", "Ljava/lang/Object;", 1}});
    }
    {
        // 实例方法 this 行：陈旧行（越界 length）重写回界内时，可信名 +
        // 种子一致的声明 desc 逐字保留。
        const Bytes bytes =
            withLvt(assembleClass("IR", "public m ()V", kThisBody), 0,
                    {{0, 9, "this", "LIR;", 0}});
        const auto run = runRebuild(bytes);
        EXPECT_TRUE(run.pa.changed());
        expectRows(readLvtRows(*run.session), {{0, 3, "this", "LIR;", 0}});
        EXPECT_EQ(run.stats.carriedNames, 1u);
    }
    {
        // cat2：long 局部占物理槽 2（隐含槽 3 不发行），编号自然跳号。
        const Bytes bytes =
            withLvt(assembleClass("CG", "public static g (J)J", kLongBody), 0,
                    {{0, 99, "v", "J", 0}, {0, 99, "w", "J", 2}});
        const auto run = runRebuild(bytes);
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
        const Bytes bytes = assembleClass("N", "public static f (I)I", kIntBody);
        const auto  run   = runRebuild(bytes);
        EXPECT_TRUE(run.pa.changed()) << "重建口径：无原表也按存活区间新建";
        expectRows(readLvtRows(*run.session),
                   {{0, 4, "var0", "I", 0},
                    {2, 2, "var1", "I", 1}});
        EXPECT_EQ(run.stats.carriedNames, 0u);
        EXPECT_EQ(run.stats.varNames, 2u);
    }
    {
        const Bytes bytes = assembleClass("N2", "public static f (I)I", kIntBody);
        LocalVariableTableRebuilderOptions opts;
        opts.mode      = LvtRebuildMode::Drop;
        const auto run = runRebuild(bytes, opts);
        EXPECT_FALSE(run.pa.changed()) << "丢弃口径：无表零改动";
        EXPECT_EQ(run.stats.droppedMethods, 0u);
    }
}

// ── LVTT-only：重建口径摘除 LVTT 并按存活区间新建 LVT ──

TEST(MjPassLocalVariableTableRebuilderTest, RebuildsFromLvttOnlyFixture)
{
    const Bytes bytes =
        withLvtt(assembleClass("LO", "public static f (I)I", kIntBody), 0,
                 {{0, 4, "n", "TT;", 1}});
    const auto run = runRebuild(bytes);
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
        const Bytes       bytes =
            withLvt(assembleClass("AF", "public static f (I)I", kIntBody), 0,
                    {{0, 4, "x", "I", 0}, {3, 6, "y", "I", 1}});
        addClassResource(container, "AF.class", bytes);
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
        const Bytes       bytes =
            withLvt(assembleClass("RA", "public static f (I)I", kIntBody), 0,
                    {{0, 4, "x", "I", 0}, {3, 6, "y", "I", 1}});
        addClassResource(container, "RA.class", bytes);

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
    const Bytes original =
        withLvt(assembleClass("R2", "public static f (I)I", kIntBody), 0,
                {{0, 9, "n", "I", 0}, {3, 6, "x", "I", 1}});

    const auto first = runRebuild(original);
    EXPECT_TRUE(first.pa.changed());

    // 同会话二次运行（重建槽已解码）。
    {
        ca::str::Utf8StringArena          arena;
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

// ── e) 注册面与 opt-in 接线：默认注册表种子 + 管线配置启用 + 默认链零变化 ──

/// 注册名 = 真实类名，scope 标签 class；注册表工厂产默认口径（Rebuild）
/// 实例且开箱可用（#104 StackOperationExpander 同款注册钉形）。
TEST(MjPassLocalVariableTableRebuilderTest, RegisteredAsLocalVariableTableRebuilder)
{
    using mjbase::spi::PassScope;
    mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    EXPECT_TRUE(reg.has("LocalVariableTableRebuilder"));
    const auto found = reg.find("LocalVariableTableRebuilder");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, PassScope::Class);
    EXPECT_FALSE(found.unwrap().description.empty());

    auto created = reg.createClassPass("LocalVariableTableRebuilder");
    ASSERT_TRUE(created.is_ok());
    const Bytes bytes = withLvt(assembleClass("RG", "public static f (I)I", kIntBody), 0,
                                {{0, 9, "n", "I", 0}, {3, 6, "x", "I", 1}});
    auto createdSession = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(createdSession.is_ok());
    auto                              session = std::move(createdSession).unwrap();
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    const auto pa = std::move(created).unwrap()->run(*session, cam);
    EXPECT_TRUE(pa.changed()) << "注册表工厂默认 Rebuild 口径开箱可用";
    expectRows(readLvtRows(*session), {{0, 4, "n", "I", 0}, {2, 2, "x", "I", 1}});
}

/// opt-in 启用形态集成钉形：管线配置 JSON 直引注册名 → applyConfigTo 装配
/// → 容器管线真跑 → 陈旧 LVT 重算（全链路经默认注册表工厂，无直接构造）。
TEST(MjPassLocalVariableTableRebuilderTest, PipelineConfigOptInEnablesRebuild)
{
    ResourceContainer container;
    const Bytes       staleBytes =
        withLvt(assembleClass("PI", "public static f (I)I", kIntBody), 0,
                {{0, 9, "n", "I", 0}, {3, 6, "x", "I", 1}});
    addClassResource(container, "PI.class", staleBytes);

    const auto loaded = mjbase::spi::loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"LocalVariableTableRebuilder"}]})json",
        mjbase::spi::defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    EXPECT_EQ(loaded.unwrap().sequence.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(loaded.unwrap().sequence[0].scope, mjbase::spi::PassScope::Class);

    mjbase::spi::PipelineBuilder builder;
    ASSERT_TRUE(mjbase::spi::applyConfigTo(mjbase::spi::defaultPassRegistry(),
                                           loaded.unwrap(), builder)
                    .is_ok());
    EXPECT_EQ(builder.classPassCount(), static_cast<ca::usize>(1));
    const auto classNames = builder.classPassNames();
    ASSERT_EQ(classNames.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(classNames[0], "LocalVariableTableRebuilder");

    mjbase::spi::PassJobContext job;
    auto                        mgr = builder.buildNestedPipeline(job);
    const auto                  ran = mjbase::spi::runContainerPipeline(container, mgr, job);
    ASSERT_TRUE(ran.is_ok()) << ran.unwrap_err().detail;

    expectRows(readLvtRowsOfBytes(containerBytes(container, "PI.class")),
               {{0, 4, "n", "I", 0}, {2, 2, "x", "I", 1}});
}

/// 默认行为零变化：default-optimize 组成不含本 pass（opt-in 不入链），
/// 跑陈旧表类 LVT 原样悬垂（不配置时不生效）。
TEST(MjPassLocalVariableTableRebuilderTest, DefaultChainLeavesStaleTableUntouched)
{
    (void)mjbase::spi::registerBuiltinPipelines();
    const auto& pipelines = mjbase::spi::PassRegistry::instance();
    ASSERT_TRUE(pipelines.hasPipeline("default-optimize"));
    const auto pipeline = pipelines.getPipeline("default-optimize");
    EXPECT_TRUE(pipeline.classPassNames().empty()) << "默认链无类级段";
    const auto names = pipeline.methodPassNames();
    for (const auto& name : names) {
        EXPECT_NE(name, "LocalVariableTableRebuilder") << "opt-in 不入默认链";
    }

    ResourceContainer container;
    const Bytes       staleBytes =
        withLvt(assembleClass("PD", "public static f (I)I", kIntBody), 0,
                {{0, 9, "n", "I", 0}, {3, 6, "x", "I", 1}});
    addClassResource(container, "PD.class", staleBytes);

    mjbase::spi::PassJobContext job;
    auto                        mgr = pipeline.buildNestedPipeline(job);
    const auto                  ran = mjbase::spi::runContainerPipeline(container, mgr, job);
    ASSERT_TRUE(ran.is_ok()) << ran.unwrap_err().detail;
    // 默认链不收：陈旧表原样悬垂。
    expectRows(readLvtRowsOfBytes(containerBytes(container, "PD.class")),
               {{0, 9, "n", "I", 0}, {3, 6, "x", "I", 1}});
}
