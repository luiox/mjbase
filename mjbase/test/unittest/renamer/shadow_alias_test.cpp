/// @file        shadow_alias_test.cpp
/// @brief       ShadowAliasTable + 机械占位名生成 + prune + jasm 显示层改写
///              （#1072/#1156 mjbase 移植版）。
///
/// 登记/查询/渲染口径、双向与范围查询、mapping roundtrip、生成器字典序稳定 +
/// 幂等 + 手工名保留 + 撞号递增 + 容器遍历序无关、jar 字节零写入（红线）、
/// 过期条目清理、jasm 整 token 替换契约、token8 随行。
///
/// mjbase 底座差异（语义不变）：容器夹具用 jasm 组装；随机补号/生成器的
/// 系统随机源失败走 Result（测试域 unwrap，成功路径与计数逐值一致）。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

#include "renamer_fixture.hpp"

namespace mjbase::renamer {

namespace {

namespace fixture = renamer::test_support;

using ClassBytes = std::vector<std::pair<std::string, std::vector<ca::u8>>>;

std::string toStd(std::string_view value)
{
    return std::string(value);
}

/// bytes 顺序即插入顺序（稳定性用例反序插入）。
model::ResourceContainer buildContainer(const ClassBytes& bytes)
{
    model::ResourceContainer container;
    for (const auto& [uri, data] : bytes) {
        container.addResource(uri, std::make_unique<model::ClassResource>(uri, data));
    }
    return container;
}

/// shadow 测试专用 B：<init> + oldMethod 两个方法；
/// 引擎夹具的 buildB 多一个 X 方法，编号断言不同，不共用）。
std::vector<ca::u8> buildShadowB()
{
    std::string members;
    members += fixture::voidMethod("public", "<init>", "()V");
    members += fixture::voidMethod("public", "oldMethod", "()V");
    return fixture::assembleClass("B", "A", members);
}

/// Object ← A(2 field + 4 method) ← B(2 method)。返回顺序 = Object, A, B。
ClassBytes fixtureBytes()
{
    return {
        {"java/lang/Object.class", fixture::buildBareObject()},
        {"A.class", fixture::buildA()},
        {"B.class", buildShadowB()},
    };
}

/// 生成器 Result 面（随机源失败 = 环境灾难）测试域 unwrap。
ShadowGenStats generateOrDie(model::ResourceContainer& container, ShadowAliasTable& table)
{
    auto generated = generateMechanicalShadowNames(container, table);
    EXPECT_TRUE(generated.is_ok());
    return std::move(generated).unwrap();
}

/// 补号 Result 面测试域 unwrap。
ca::usize backfillOrDie(ShadowAliasTable& table)
{
    auto backfilled = table.backfillTokens();
    EXPECT_TRUE(backfilled.is_ok());
    return std::move(backfilled).unwrap();
}

/// 容器全部 class 资源字节快照（URI → 字节），供零写入断言。
std::vector<std::pair<std::string, std::vector<ca::u8>>> snapshotBytes(
    model::ResourceContainer& container)
{
    std::vector<std::pair<std::string, std::vector<ca::u8>>> before;
    container.visitResources([&before](std::string_view uri, const model::IResource& resource) {
        const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        ASSERT_NE(nullptr, cls);
        const auto& bytes = cls->get();
        before.emplace_back(std::string(uri),
                            std::vector<ca::u8>(bytes.begin(), bytes.end()));
    });
    return before;
}

}   // namespace

TEST(MjShadowAliasTableTest, addDuplicateAndEmptyNameRejected)
{
    ShadowAliasTable table;
    EXPECT_TRUE(table.addClass("a/A", "Class_1"));
    EXPECT_FALSE(table.addClass("a/A", "Class_2"));   // 同 raw 先到者优先
    EXPECT_FALSE(table.addClass("", "Class_2"));
    EXPECT_FALSE(table.addClass("b/B", ""));
    EXPECT_TRUE(table.addClass("b/B", "Class_1"));   // 跨 class 允许同 shadow 名
    EXPECT_EQ(2U, table.size());

    EXPECT_TRUE(table.addMethod("a/A", "()V", "m", "Method_1"));
    EXPECT_FALSE(table.addMethod("a/A", "()V", "m", "Method_2"));
    EXPECT_TRUE(table.addField("a/A", "I", "f", "Field_1"));
    // 同 (owner,raw,desc) 跨 kind 视为同符号：field 已占，method 再登记拒绝。
    EXPECT_FALSE(table.addMethod("a/A", "I", "f", "Method_1"));
    EXPECT_EQ(4U, table.size());   // 2 class + 1 method + 1 field
}

TEST(MjShadowAliasTableTest, forwardQueriesAndKindIsolation)
{
    ShadowAliasTable table;
    table.addClass("a/A", "Class_1");
    table.addMethod("a/A", "(I)V", "m", "Method_1");
    table.addField("a/A", "J", "wide", "Field_1");

    EXPECT_EQ("Class_1", toStd(table.classShadow("a/A")));
    EXPECT_TRUE(table.classShadow("x/X").empty());
    EXPECT_EQ("Method_1", toStd(table.methodShadow("a/A", "m", "(I)V")));
    EXPECT_EQ("Field_1", toStd(table.fieldShadow("a/A", "wide", "J")));
    // kind 隔离：field 查不到 method 条目，反之亦然。
    EXPECT_TRUE(table.methodShadow("a/A", "wide", "J").empty());
    EXPECT_TRUE(table.fieldShadow("a/A", "m", "(I)V").empty());
    // desc 不匹配不命中。
    EXPECT_TRUE(table.methodShadow("a/A", "m", "(J)V").empty());
}

TEST(MjShadowAliasTableTest, renderAppendsRawFormOnlyOnHit)
{
    ShadowAliasTable table;
    table.addClass("a/A", "Class_1");
    table.addMethod("a/A", "(I)V", "m", "Method_1");
    table.addField("a/A", "J", "wide", "Field_1");

    // 命中 → "shadow (raw)"；两段均 printReadable（ASCII 名恒等）。
    EXPECT_EQ("Class_1 (a/A)", toStd(table.renderClass("a/A")));
    EXPECT_EQ("x/X", toStd(table.renderClass("x/X")));
    EXPECT_EQ("Method_1 (m)", toStd(table.renderMemberName("a/A", "m", "(I)V")));
    EXPECT_EQ("n", toStd(table.renderMemberName("a/A", "n", "(I)V")));
    // 双命中：两段各带括号；尾段 ` (完整原始形式)` 与前段逐字段重合
    // （owner/name 在括号内、desc 以 `:desc` 收尾），按 #1106 P0-2 去重。
    EXPECT_EQ("Class_1 (a/A).Method_1 (m):(I)V", toStd(table.renderMember("a/A", "m", "(I)V")));
    // 仅 owner 命中 / 仅名字命中：同样去重（owner/name/desc 均在前段出现）。
    EXPECT_EQ("Class_1 (a/A).n:(I)V", toStd(table.renderMember("a/A", "n", "(I)V")));
    // 完全未命中：与既有 printReadable 拼接逐字节一致（无尾随括号）。
    EXPECT_EQ("b/B.m:(I)V", toStd(table.renderMember("b/B", "m", "(I)V")));
    EXPECT_EQ("b/B.n:(I)V", toStd(table.renderMember("b/B", "n", "(I)V")));
}

TEST(MjShadowAliasTableTest, reverseAndScopeQueries)
{
    ShadowAliasTable table;
    table.addClass("a/A", "Class_1");
    table.addMethod("a/A", "()V", "m", "Method_1");
    table.addMethod("b/B", "()V", "m", "Method_1");   // 跨类重号
    table.addField("b/B", "I", "f", "Field_1");

    const auto byShadow = table.findByShadowName("Method_1");
    ASSERT_EQ(2U, byShadow.size());
    EXPECT_EQ("a/A", toStd(byShadow[0].owner));
    EXPECT_EQ("b/B", toStd(byShadow[1].owner));

    const auto byRaw = table.findByRawName("m");
    ASSERT_EQ(2U, byRaw.size());

    const auto ofA = table.entriesOfClass("a/A");
    ASSERT_EQ(2U, ofA.size());   // class 条目 + 成员条目
    EXPECT_EQ(ShadowKind::Class, ofA[0].kind);
    EXPECT_EQ(ShadowKind::Method, ofA[1].kind);

    const auto ofB = table.entriesOfClass("b/B");
    ASSERT_EQ(2U, ofB.size());   // B 无 class 别名：仅 method+field 成员条目

    ASSERT_NE(nullptr, table.classEntry("a/A"));
    EXPECT_EQ(nullptr, table.classEntry("b/B"));

    EXPECT_TRUE(table.classShadowTaken("Class_1", "b/B"));
    EXPECT_FALSE(table.classShadowTaken("Class_1", "a/A"));   // 除自身
    EXPECT_TRUE(table.memberShadowTaken("b/B", "Method_1"));
    EXPECT_TRUE(table.memberShadowTaken("a/A", "Method_1"));
    EXPECT_FALSE(table.memberShadowTaken("c/C", "Method_1"));   // 未登记作用域
}

TEST(MjShadowAliasTableTest, mappingRoundtripPreservesEntries)
{
    ShadowAliasTable table;
    table.addClass("a/A", "Class_1");
    table.addMethod("a/A", "(I)V", "m", "Method_1");
    table.addField("a/A", "J", "wide", "Field_1");

    const auto mappings = table.toMapping();
    const auto back     = ShadowAliasTable::fromMapping(mappings);
    ASSERT_EQ(table.size(), back.size());
    EXPECT_EQ("Class_1", toStd(back.classShadow("a/A")));
    EXPECT_EQ("Method_1", toStd(back.methodShadow("a/A", "m", "(I)V")));
    EXPECT_EQ("Field_1", toStd(back.fieldShadow("a/A", "wide", "J")));
}

TEST(MjShadowAliasTableTest, generatorNumbersLexicographicAndScoped)
{
    auto       container = buildContainer(fixtureBytes());
    ShadowAliasTable table;
    const auto     stats = generateOrDie(container, table);

    EXPECT_EQ(3U, stats.assignedClasses);
    EXPECT_EQ(6U, stats.assignedMethods);   // A 4 + B 2（Object 无声明方法）
    EXPECT_EQ(2U, stats.assignedFields);
    EXPECT_EQ(0U, stats.keptEntries);

    // class 全局字典序：A < B < java/lang/Object。
    EXPECT_EQ("Class_1", toStd(table.classShadow("A")));
    EXPECT_EQ("Class_2", toStd(table.classShadow("B")));
    EXPECT_EQ("Class_3", toStd(table.classShadow("java/lang/Object")));

    // A 类内 (name, desc) 字典序："<init>" < "X" < "alpha" < "oldMethod"。
    EXPECT_EQ("Method_1", toStd(table.methodShadow("A", "<init>", "()V")));
    EXPECT_EQ("Method_2", toStd(table.methodShadow("A", "X", "()V")));
    EXPECT_EQ("Method_3", toStd(table.methodShadow("A", "alpha", "()V")));
    EXPECT_EQ("Method_4", toStd(table.methodShadow("A", "oldMethod", "()V")));
    // B 作用域从 1 重新起号。
    EXPECT_EQ("Method_1", toStd(table.methodShadow("B", "<init>", "()V")));
    EXPECT_EQ("Method_2", toStd(table.methodShadow("B", "oldMethod", "()V")));

    // field 类内字典序：oldField < other。
    EXPECT_EQ("Field_1", toStd(table.fieldShadow("A", "oldField", "I")));
    EXPECT_EQ("Field_2", toStd(table.fieldShadow("A", "other", "J")));
}

TEST(MjShadowAliasTableTest, generatorIsIdempotent)
{
    auto             container = buildContainer(fixtureBytes());
    ShadowAliasTable table;
    generateOrDie(container, table);
    const auto before = table.size();

    const auto stats = generateOrDie(container, table);
    EXPECT_EQ(0U, stats.assignedClasses);
    EXPECT_EQ(0U, stats.assignedMethods);
    EXPECT_EQ(0U, stats.assignedFields);
    EXPECT_EQ(before, stats.keptEntries);
    EXPECT_EQ(before, table.size());

    // 全部编号不变。
    EXPECT_EQ("Class_1", toStd(table.classShadow("A")));
    EXPECT_EQ("Method_4", toStd(table.methodShadow("A", "oldMethod", "()V")));
}

TEST(MjShadowAliasTableTest, generatorIgnoresContainerInsertionOrder)
{
    auto ordered  = fixtureBytes();
    std::reverse(ordered.begin(), ordered.end());
    auto reversed = buildContainer(ordered);
    auto forward  = buildContainer(fixtureBytes());

    ShadowAliasTable a;
    ShadowAliasTable b;
    generateOrDie(reversed, a);
    generateOrDie(forward, b);

    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(toStd(a.entries()[i].rawName), toStd(b.entries()[i].rawName));
        EXPECT_EQ(toStd(a.entries()[i].shadowName), toStd(b.entries()[i].shadowName));
    }
}

TEST(MjShadowAliasTableTest, generatorPreservesManualNamesWithoutConsumingNumbers)
{
    auto             container = buildContainer(fixtureBytes());
    ShadowAliasTable table;
    ASSERT_TRUE(table.addClass("A", "NiceA"));
    ASSERT_TRUE(table.addMethod("A", "()V", "alpha", "handler"));

    const auto stats = generateOrDie(container, table);
    EXPECT_EQ("NiceA", toStd(table.classShadow("A")));
    EXPECT_EQ("handler", toStd(table.methodShadow("A", "alpha", "()V")));
    // A 已有 class 别名不占号：B=Class_1、Object=Class_2；alpha 已有别名不占号。
    EXPECT_EQ("Class_1", toStd(table.classShadow("B")));
    EXPECT_EQ("Class_2", toStd(table.classShadow("java/lang/Object")));
    EXPECT_EQ("Method_3", toStd(table.methodShadow("A", "oldMethod", "()V")));
    EXPECT_EQ(2U, stats.assignedClasses);
    EXPECT_EQ(5U, stats.assignedMethods);
    EXPECT_EQ(2U, stats.keptEntries);
}

TEST(MjShadowAliasTableTest, generatorCollisionIncrementsNumber)
{
    auto             container = buildContainer(fixtureBytes());
    ShadowAliasTable table;
    // jar 外符号先占 Class_1 → 生成器从 Class_2 起号。
    ASSERT_TRUE(table.addClass("z/Z", "Class_1"));

    generateOrDie(container, table);
    EXPECT_EQ("Class_2", toStd(table.classShadow("A")));
    EXPECT_EQ("Class_3", toStd(table.classShadow("B")));
    EXPECT_EQ("Class_4", toStd(table.classShadow("java/lang/Object")));
}

TEST(MjShadowAliasTableTest, generatorNeverTouchesResourceBytes)
{
    auto       container = buildContainer(fixtureBytes());
    const auto before    = snapshotBytes(container);

    ShadowAliasTable table;
    generateOrDie(container, table);

    const auto after = snapshotBytes(container);
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].first, after[i].first);
        ASSERT_EQ(before[i].second.size(), after[i].second.size()) << before[i].first;
        EXPECT_EQ(0, std::memcmp(before[i].second.data(),
                                 after[i].second.data(),
                                 before[i].second.size()))
            << before[i].first;
    }
}

TEST(MjShadowAliasTableTest, pruneRemovesStaleClassAndMemberEntries)
{
    auto full = buildContainer(fixtureBytes());
    ShadowAliasTable table;
    generateOrDie(full, table);
    const auto fullSize = table.size();

    // jar 缩水：B 整类消失 + A 只剩 <init>/oldField（oldMethod/X/alpha/other 消失）。
    ClassBytes shrunkBytes = {
        {"java/lang/Object.class", fixture::buildBareObject()},
        {"A.class", fixture::buildAShrunk()},
    };
    auto shrunk        = buildContainer(shrunkBytes);
    const auto removed = pruneShadowAliases(shrunk, table);

    // B(class+2 method) + A(oldMethod/X/alpha + other) = 7 条。
    EXPECT_EQ(7U, removed);
    EXPECT_EQ(fullSize - 7U, table.size());
    EXPECT_EQ(nullptr, table.classEntry("B"));
    EXPECT_TRUE(table.methodShadow("B", "<init>", "()V").empty());
    EXPECT_TRUE(table.methodShadow("A", "oldMethod", "()V").empty());
    EXPECT_TRUE(table.methodShadow("A", "alpha", "()V").empty());
    EXPECT_TRUE(table.fieldShadow("A", "other", "J").empty());
    // 存活条目原样。
    EXPECT_EQ("Class_1", toStd(table.classShadow("A")));
    EXPECT_EQ("Class_3", toStd(table.classShadow("java/lang/Object")));
    EXPECT_EQ("Method_1", toStd(table.methodShadow("A", "<init>", "()V")));
    EXPECT_EQ("Field_1", toStd(table.fieldShadow("A", "oldField", "I")));

    // 全存活时 prune 零移除。
    auto            intact = buildContainer(fixtureBytes());
    ShadowAliasTable intactTable;
    generateOrDie(intact, intactTable);
    EXPECT_EQ(0U, pruneShadowAliases(intact, intactTable));
}

TEST(MjShadowAliasTableTest, jasmRewriteReplacesWholeTokensOnly)
{
    ShadowAliasTable table;
    table.addClass("A", "Alpha");
    table.addMethod("A", "()V", "oldMethod", "run");

    const std::string jasm =
        ".class public A\n"
        ".super java/lang/Object\n"
        ".method public oldMethod ()V\n"
        "    invokevirtual A.oldMethod ()V\n"
        "    ldc \"oldMethod stays A\"\n"
        "    getstatic java/lang/System.out Ljava/io/PrintStream;\n"
        "    invokespecial A.<init> ()V\n"
        ".end method\n";

    const auto out = rewriteJasmForDisplay(jasm, table, "A");

    // class token（含声明行）与成员裸名 token 替换；复合 token 整体替换。
    EXPECT_NE(std::string::npos, out.find(".class public Alpha"));
    EXPECT_NE(std::string::npos, out.find(".method public run ()V"));
    EXPECT_NE(std::string::npos, out.find("invokevirtual run ()V"));
    // 字符串字面量内部不动。
    EXPECT_NE(std::string::npos, out.find("\"oldMethod stays A\""));
    // 描述符 L 前缀：未命中原样保留。
    EXPECT_NE(std::string::npos, out.find("Ljava/io/PrintStream;"));
    // 未登记的复合 token（A.<init>）不替换。
    EXPECT_NE(std::string::npos, out.find("invokespecial A.<init> ()V"));
    // 旧 token 的裸名出现位（方法声明行）应已消失。
    EXPECT_EQ(std::string::npos, out.find("oldMethod ()V"));
}

TEST(MjShadowAliasTableTest, jasmRewriteScopesMemberTokensToOwnerClass)
{
    ShadowAliasTable table;
    table.addClass("A", "Alpha");
    table.addMethod("A", "()V", "oldMethod", "run");

    const std::string jasm =
        ".method public oldMethod ()V\n"
        "    invokevirtual A.oldMethod ()V\n"
        ".end method\n";

    // ownerClass=B：成员 token 不替换（作用域隔离），class token 仍生效。
    const auto out = rewriteJasmForDisplay(jasm, table, "B");
    EXPECT_NE(std::string::npos, out.find(".method public oldMethod ()V"));
    EXPECT_NE(std::string::npos, out.find("invokevirtual A.oldMethod ()V"));
}

TEST(MjShadowAliasTableTest, jasmRewriteWithoutHitsIsByteIdentical)
{
    ShadowAliasTable table;
    table.addClass("X/Y", "Xy");

    const std::string jasm = ".class public A\n ldc \"keep\"\n";
    EXPECT_EQ(jasm, rewriteJasmForDisplay(jasm, table, "A"));

    ShadowAliasTable empty;
    EXPECT_EQ(jasm, rewriteJasmForDisplay(jasm, empty, "A"));
}

TEST(MjShadowAliasTableTest, jasmRewriteDiscriminatesSameNameFieldAndMethod)
{
    ShadowAliasTable table;
    table.addClass("A", "Alpha");
    // JVM 合法：field 与 method 同名（混淆器产物，#1072 真实 jar 实测）。
    table.addField("A", "I", "dup", "Field_1");
    table.addMethod("A", "()I", "dup", "Method_1");

    const std::string jasm =
        ".field private dup I\n"
        ".method public dup ()I\n"
        "    getstatic A.dup I\n"
        "    invokevirtual A.dup ()I\n"
        ".end method\n";

    const auto out = rewriteJasmForDisplay(jasm, table, "A");
    // 描述符是否以 ( 开头区分方法/字段位：声明行、getstatic/putstatic 字段操作数
    // → Field 影子名；.method 声明与 invoke 操作数 → Method 影子名。
    EXPECT_NE(std::string::npos, out.find(".field private Field_1 I"));
    EXPECT_NE(std::string::npos, out.find(".method public Method_1 ()I"));
    EXPECT_NE(std::string::npos, out.find("getstatic Field_1 I"));
    EXPECT_NE(std::string::npos, out.find("invokevirtual Method_1 ()I"));
    EXPECT_EQ(std::string::npos, out.find("dup"));
}

TEST(MjShadowAliasTableTest, clearInvalidatesAllViews)
{
    ShadowAliasTable table;
    table.addClass("a/A", "Class_1");
    table.clear();
    EXPECT_TRUE(table.empty());
    EXPECT_EQ(0U, table.size());
    EXPECT_TRUE(table.classShadow("a/A").empty());
}

// ═══════════════════════════════════════════════════════════════
// #1156 shadow 引用 token —— 表层（T1 切片）
// ═══════════════════════════════════════════════════════════════

TEST(MjShadowAliasTableTest, token8PlumbsEntriesAndMappingPivot)
{
    // 按 (kind, owner, raw, desc) 精确定位条目（mapping 枢纽往返后条目序不保证）。
    auto findEntry = [](const ShadowAliasTable& t, ShadowKind kind, const char* owner,
                        const char* raw, const char* desc) -> const ShadowAliasEntry* {
        for (const auto& entry : t.entries()) {
            if (entry.kind == kind && toStd(entry.owner) == owner &&
                toStd(entry.rawName) == raw && toStd(entry.desc) == desc) {
                return &entry;
            }
        }
        return nullptr;
    };

    // 登记 API 带 token：entries() 逐条保真（登记序）；查询口径不受影响。
    ShadowAliasTable table;
    ASSERT_TRUE(table.addClass("a/A", "Class_1", "0123abcd"));
    ASSERT_TRUE(table.addMethod("a/A", "(I)V", "run", "Method_1", "deadbeef"));
    ASSERT_TRUE(table.addField("a/A", "I", "flag", "Field_1", "00000000"));
    ASSERT_TRUE(table.addClass("z/Z", "Class_9"));   // 无 token（缺省空）

    ASSERT_EQ(4U, table.size());
    EXPECT_EQ("0123abcd", toStd(table.entries()[0].token8));
    EXPECT_EQ("deadbeef", toStd(table.entries()[1].token8));
    EXPECT_EQ("00000000", toStd(table.entries()[2].token8));
    EXPECT_TRUE(table.entries()[3].token8.empty());
    EXPECT_EQ("Class_1", toStd(table.classShadow("a/A")));
    EXPECT_EQ("Method_1", toStd(table.methodShadow("a/A", "run", "(I)V")));

    // mapping 枢纽往返（sidecar 解析产物 ⇄ 表）token 随行。
    const auto mappings = table.toMapping();
    ASSERT_NE(nullptr,
              mappings.getClassMapping(ca::str::Utf8StringRef::from_cstr("a/A")));
    EXPECT_EQ("0123abcd",
              mappings.getClassMapping(ca::str::Utf8StringRef::from_cstr("a/A"))
                  ->token.to_std_string());
    const auto* methods =
        mappings.getClassMethodMappings(ca::str::Utf8StringRef::from_cstr("a/A"));
    ASSERT_NE(nullptr, methods);
    ASSERT_EQ(1U, methods->size());
    EXPECT_EQ("deadbeef", (*methods)[0].token.to_std_string());
    const auto* fields =
        mappings.getClassFieldMappings(ca::str::Utf8StringRef::from_cstr("a/A"));
    ASSERT_NE(nullptr, fields);
    ASSERT_EQ(1U, fields->size());
    EXPECT_EQ("00000000", (*fields)[0].token.to_std_string());

    const auto back = ShadowAliasTable::fromMapping(mappings);
    ASSERT_EQ(table.size(), back.size());
    ASSERT_NE(nullptr, findEntry(back, ShadowKind::Class, "", "a/A", ""));
    EXPECT_EQ("0123abcd", toStd(findEntry(back, ShadowKind::Class, "", "a/A", "")->token8));
    EXPECT_EQ("deadbeef",
              toStd(findEntry(back, ShadowKind::Method, "a/A", "run", "(I)V")->token8));
    EXPECT_EQ("00000000", toStd(findEntry(back, ShadowKind::Field, "a/A", "flag", "I")->token8));
    const auto* zZ = findEntry(back, ShadowKind::Class, "", "z/Z", "");
    ASSERT_NE(nullptr, zZ);
    EXPECT_TRUE(zZ->token8.empty());

    // 无 token 登记路径（v1 产物）token 留空，其余字段无损。
    mapping::IntermediateMapping legacyMappings;
    legacyMappings.addClass(ca::str::Utf8StringRef::from_cstr("w/W"),
                            ca::str::Utf8StringRef::from_cstr("Class_7"));
    const auto legacy = ShadowAliasTable::fromMapping(legacyMappings);
    ASSERT_EQ(1U, legacy.size());
    EXPECT_TRUE(legacy.entries()[0].token8.empty());
    EXPECT_EQ("Class_7", toStd(legacy.classShadow("w/W")));
}

TEST(MjShadowAliasTableTest, backfillTokensFillsMissingUniqueAndIdempotent)
{
    ShadowAliasTable table;
    ASSERT_TRUE(table.addClass("a/A", "Class_1", "ffffffff"));   // 已有号保留
    ASSERT_TRUE(table.addClass("b/B", "Class_2"));               // 缺号
    ASSERT_TRUE(table.addMethod("b/B", "(I)V", "m", "Method_1"));

    EXPECT_EQ(2U, backfillOrDie(table));
    EXPECT_EQ("ffffffff", toStd(table.entries()[0].token8));   // 原样
    const auto second = toStd(table.entries()[1].token8);
    const auto third  = toStd(table.entries()[2].token8);
    EXPECT_NE(second, third);   // 表内唯一
    EXPECT_NE("ffffffff", second);
    EXPECT_NE("ffffffff", third);
    for (const auto* token : {&second, &third}) {
        ASSERT_EQ(8U, token->size()) << *token;
        for (const char c : *token) {
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << *token;
        }
    }

    EXPECT_EQ(0U, backfillOrDie(table));   // 幂等：已全有号零补
}

TEST(MjShadowAliasTableTest, generatorAssignsWellFormedUniqueTokensAndKeepsExisting)
{
    auto             container = buildContainer(fixtureBytes());
    ShadowAliasTable table;
    // 手工条目：class 带号（保留原号）、method 缺号（生成期补齐）。
    ASSERT_TRUE(table.addClass("A", "NiceA", "0badc0de"));
    ASSERT_TRUE(table.addMethod("A", "()V", "alpha", "handler"));

    const auto stats = generateOrDie(container, table);
    EXPECT_EQ(2U, stats.assignedClasses);
    EXPECT_EQ(5U, stats.assignedMethods);

    // 全表 11 条：3 class + 6 method + 2 field；token 全体 8 位小写 hex 且唯一。
    ASSERT_EQ(11U, table.size());
    std::unordered_set<std::string> tokens;
    for (const auto& entry : table.entries()) {
        const auto token = toStd(entry.token8);
        ASSERT_EQ(8U, token.size()) << token;
        for (const char c : token) {
            ASSERT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << token;
        }
        EXPECT_TRUE(tokens.insert(token).second) << token;   // 表内唯一
    }
    EXPECT_EQ(11U, tokens.size());

    // 编号与 token 无关：既有编号口径不变。
    EXPECT_EQ("NiceA", toStd(table.classShadow("A")));
    EXPECT_EQ("0badc0de", toStd(table.entries()[0].token8));   // 手工 class 原号保留
    EXPECT_EQ("Class_1", toStd(table.classShadow("B")));
    EXPECT_EQ("Class_2", toStd(table.classShadow("java/lang/Object")));
    EXPECT_EQ("handler", toStd(table.methodShadow("A", "alpha", "()V")));
    // 手工 method 原缺号 → 生成期补齐。
    EXPECT_FALSE(table.methodShadow("A", "alpha", "()V").empty());

    // 幂等：重跑零改写，token 全体原样（含编号）。
    std::vector<std::string> tokensBefore;
    for (const auto& entry : table.entries()) tokensBefore.push_back(toStd(entry.token8));
    generateOrDie(container, table);
    ASSERT_EQ(tokensBefore.size(), table.size());
    for (std::size_t i = 0; i < tokensBefore.size(); ++i) {
        EXPECT_EQ(tokensBefore[i], toStd(table.entries()[i].token8));
    }
}

TEST(MjShadowAliasTableTest, prunePreservesTokensOnSurvivors)
{
    auto full = buildContainer(fixtureBytes());
    ShadowAliasTable table;
    generateOrDie(full, table);
    ASSERT_EQ(11U, table.size());

    // 快照 Shrunk 后应存活的条目：A(class) + A.<init> + A.oldField +
    // java/lang/Object(class)（B 整类 + A 的 X/alpha/oldMethod/other 消失）。
    auto survives = [](const ShadowAliasEntry& entry) {
        const auto owner = toStd(entry.owner);
        const auto raw   = toStd(entry.rawName);
        const auto desc  = toStd(entry.desc);
        if (owner == "B") return false;
        if (entry.kind == ShadowKind::Class) {
            return raw == "A" || raw == "java/lang/Object";
        }
        return owner == "A" &&
               ((raw == "<init>" && desc == "()V") || (raw == "oldField" && desc == "I"));
    };
    std::vector<std::pair<std::string, std::string>> kept;   // (定位键, token)
    for (const auto& entry : table.entries()) {
        if (!survives(entry)) continue;
        kept.emplace_back(toStd(entry.owner) + "|" + toStd(entry.rawName) + "@" +
                              toStd(entry.desc),
                          toStd(entry.token8));
    }
    ASSERT_EQ(4U, kept.size());

    ClassBytes shrunkBytes = {
        {"java/lang/Object.class", fixture::buildBareObject()},
        {"A.class", fixture::buildAShrunk()},
    };
    auto shrunk        = buildContainer(shrunkBytes);
    const auto removed = pruneShadowAliases(shrunk, table);
    EXPECT_EQ(7U, removed);   // B(class+2 method) = 3 + A(X/alpha/oldMethod/other) = 4

    ASSERT_EQ(kept.size(), table.size());
    for (std::size_t i = 0; i < kept.size(); ++i) {
        const auto& entry = table.entries()[i];
        const auto  key   = toStd(entry.owner) + "|" + toStd(entry.rawName) + "@" +
                          toStd(entry.desc);
        EXPECT_EQ(kept[i].first, key);
        EXPECT_EQ(kept[i].second, toStd(entry.token8)) << key;   // token 随行不丢
    }
}

// ─────────────────────────── 重载拆分（同名异签视图层） ───────────────────────────

namespace {

/// 三同名重载（乱序声明）+ 单方法 + 同名 field 的类（拆分主夹具）。
ClassBytes overloadBytes()
{
    std::string members;
    members += fixture::voidMethod("public", "foo", "(Ljava/lang/String;)V");
    members += fixture::voidMethod("public", "foo", "()V");
    members += fixture::voidMethod("public", "foo", "(I)V");
    members += fixture::voidMethod("public", "bar", "()V");   // 无重载：不拆
    members += fixture::fieldDecl("private", "foo", "I");     // 真实名占位
    return {{"A.class", fixture::assembleClass("A", "java/lang/Object", members)}};
}

/// 拆分 Result 面（随机源失败 = 环境灾难）测试域 unwrap。
OverloadSplitStats splitOrDie(const model::ResourceContainer& container,
                              ShadowAliasTable&               table)
{
    auto split = generateOverloadSplitShadows(container, table);
    EXPECT_TRUE(split.is_ok());
    return std::move(split).unwrap();
}

/// 批量指派 Result 面（校验失败走显式 Err 面）测试域 unwrap。
ca::usize batchOrDie(const model::ResourceContainer&    container,
                     ShadowAliasTable&                  table,
                     std::vector<OverloadShadowSpec> specs)
{
    auto assigned = assignMethodShadowBatch(container, table, specs);
    EXPECT_TRUE(assigned.is_ok());
    return std::move(assigned).unwrap();
}

}   // namespace

TEST(MjShadowAliasTableTest, overloadSplitAssignsIndexedSuffixesInDescOrder)
{
    auto             container = buildContainer(overloadBytes());
    ShadowAliasTable table;
    const auto stats = splitOrDie(container, table);
    EXPECT_EQ(1U, stats.splitGroups);
    EXPECT_EQ(3U, stats.assignedMethods);
    EXPECT_EQ(0U, stats.keptEntries);

    // 组内按 desc 字典序编号（声明序无关）。
    EXPECT_EQ("foo$0", toStd(table.methodShadow("A", "foo", "()V")));
    EXPECT_EQ("foo$1", toStd(table.methodShadow("A", "foo", "(I)V")));
    EXPECT_EQ("foo$2", toStd(table.methodShadow("A", "foo", "(Ljava/lang/String;)V")));
    EXPECT_TRUE(table.methodShadow("A", "bar", "()V").empty());   // 无重载不分配

    // 反查稳定：shadow → 成员三元组。
    const auto hits = table.findByShadowName("foo$1");
    ASSERT_EQ(1U, hits.size());
    EXPECT_EQ("A", toStd(hits[0].owner));
    EXPECT_EQ("foo", toStd(hits[0].rawName));
    EXPECT_EQ("(I)V", toStd(hits[0].desc));
}

TEST(MjShadowAliasTableTest, overloadSplitAvoidsRealNameAndShadowCollisions)
{
    std::string members;
    members += fixture::voidMethod("public", "foo", "()V");
    members += fixture::voidMethod("public", "foo", "(I)V");
    members += fixture::voidMethod("public", "foo$0", "()V");   // 撞候选的真实方法名
    members += fixture::voidMethod("public", "bar", "()V");
    auto container =
        buildContainer({{"A.class", fixture::assembleClass("A", "java/lang/Object", members)}});
    ShadowAliasTable table;
    ASSERT_TRUE(table.addMethod("A", "()V", "bar", "foo$1"));   // 已占 shadow（他成员）

    const auto stats = splitOrDie(container, table);
    EXPECT_EQ(1U, stats.splitGroups);
    EXPECT_EQ(2U, stats.assignedMethods);
    EXPECT_EQ("foo$2", toStd(table.methodShadow("A", "foo", "()V")));
    EXPECT_EQ("foo$3", toStd(table.methodShadow("A", "foo", "(I)V")));
}

TEST(MjShadowAliasTableTest, overloadSplitKeepsManualNamesAndIsIdempotent)
{
    auto             container = buildContainer(overloadBytes());
    ShadowAliasTable table;
    ASSERT_TRUE(table.addMethod("A", "()V", "foo", "handmade"));   // 手工名先到

    const auto first = splitOrDie(container, table);
    EXPECT_EQ(2U, first.assignedMethods);
    EXPECT_EQ(1U, first.keptEntries);
    EXPECT_EQ("handmade", toStd(table.methodShadow("A", "foo", "()V")));
    EXPECT_EQ("foo$0", toStd(table.methodShadow("A", "foo", "(I)V")));
    EXPECT_EQ("foo$1", toStd(table.methodShadow("A", "foo", "(Ljava/lang/String;)V")));

    // 幂等：重跑零分配、全组保留、手工名不丢。
    const auto second = splitOrDie(container, table);
    EXPECT_EQ(0U, second.assignedMethods);
    EXPECT_EQ(3U, second.keptEntries);
    EXPECT_EQ("handmade", toStd(table.methodShadow("A", "foo", "()V")));
    EXPECT_EQ(3U, table.size());
}

TEST(MjShadowAliasTableTest, overloadSplitNeverTouchesClassBytes)
{
    auto             container = buildContainer(overloadBytes());
    ShadowAliasTable table;
    const auto before = snapshotBytes(container);
    (void)splitOrDie(container, table);
    EXPECT_EQ(before, snapshotBytes(container));   // 视图层红线：字节码零写入
}

TEST(MjShadowAliasTableTest, batchAssignAppliesAndValidatesTransactionally)
{
    auto             container = buildContainer(overloadBytes());
    ShadowAliasTable table;

    EXPECT_EQ(2U, batchOrDie(container, table,
                             {{"A", "foo", "()V", "foo$0"},
                              {"A", "foo", "(I)V", "foo$1"}}));
    EXPECT_EQ("foo$0", toStd(table.methodShadow("A", "foo", "()V")));
    EXPECT_EQ("foo$1", toStd(table.methodShadow("A", "foo", "(I)V")));

    // 撞已分配 shadow：整批拒绝，表零改动。
    EXPECT_TRUE(assignMethodShadowBatch(
                    container, table, {{"A", "foo", "(Ljava/lang/String;)V", "foo$0"}})
                    .is_err());
    EXPECT_TRUE(table.methodShadow("A", "foo", "(Ljava/lang/String;)V").empty());

    // 撞类内真实名（方法 bar）。
    EXPECT_TRUE(assignMethodShadowBatch(
                    container, table, {{"A", "foo", "(Ljava/lang/String;)V", "bar"}})
                    .is_err());

    // 未知类 / 未知方法 / 未知描述符。
    EXPECT_TRUE(
        assignMethodShadowBatch(container, table, {{"X", "foo", "()V", "s"}}).is_err());
    EXPECT_TRUE(
        assignMethodShadowBatch(container, table, {{"A", "nope", "()V", "s"}}).is_err());
    EXPECT_TRUE(
        assignMethodShadowBatch(container, table, {{"A", "foo", "(J)V", "s"}}).is_err());

    // 幂等重指派同 shadow：Ok(0)。
    auto again = assignMethodShadowBatch(container, table, {{"A", "foo", "()V", "foo$0"}});
    ASSERT_TRUE(again.is_ok());
    EXPECT_EQ(0U, std::move(again).unwrap());

    // 异 shadow 重指派：Err（先到者优先契约）。
    EXPECT_TRUE(
        assignMethodShadowBatch(container, table, {{"A", "foo", "()V", "other"}}).is_err());

    // 同批双成员同 shadow：Err，整批零改动。
    EXPECT_TRUE(assignMethodShadowBatch(container, table,
                                        {{"A", "foo", "(Ljava/lang/String;)V", "s"},
                                         {"A", "bar", "()V", "s"}})
                    .is_err());
    EXPECT_TRUE(table.methodShadow("A", "bar", "()V").empty());

    // 冲突过后合法批次仍可落表。
    EXPECT_EQ(1U, batchOrDie(container, table,
                             {{"A", "foo", "(Ljava/lang/String;)V", "foo$2"}}));
    EXPECT_EQ("foo$2", toStd(table.methodShadow("A", "foo", "(Ljava/lang/String;)V")));
}

}   // namespace mjbase::renamer
