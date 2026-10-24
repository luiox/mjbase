/// @file        jar_util_test.cpp
/// @brief       JarUtil 测试：条目名后缀判定（.class/.jar/.jmod）与 class 字节嗅探
///              （魔数 + 常量池头部 tag 合法性预检）。
///
/// 22 用例断言逐一对应，
/// 仅适配命名空间与字符串类型（std::string → Utf8StringRef 借用视图）。
#include <gtest/gtest.h>

#include <vector>

#include "mjbase/jar/jar_util.hpp"

using namespace mjbase::jar;

namespace {

/// C 字符串字面量 → 借用视图（字面量静态存储，视图长期有效）。
ca::str::Utf8StringRef S(const char* s) { return ca::str::Utf8StringRef::from_cstr(s); }

}   // namespace

TEST(MjJarUtilIsClassFileTest, classExtension)
{
    EXPECT_TRUE(JarUtil::isClassFile(S("Foo.class")));
    EXPECT_TRUE(JarUtil::isClassFile(S("a/b/Foo.class")));
}

// 混淆器（OS2.9.jar）把类 entry 命名为 *.class/（尾斜杠），isClassFile 需放行。
TEST(MjJarUtilIsClassFileTest, classExtensionWithTrailingSlash)
{
    EXPECT_TRUE(JarUtil::isClassFile(S("Foo.class/")));
    EXPECT_TRUE(JarUtil::isClassFile(S("a/b/Foo.class/")));
}

TEST(MjJarUtilIsClassFileTest, nonClass)
{
    EXPECT_FALSE(JarUtil::isClassFile(S("Foo.jar")));
    EXPECT_FALSE(JarUtil::isClassFile(S("Foo.java")));
    EXPECT_FALSE(JarUtil::isClassFile(S("Fooclass")));
    EXPECT_FALSE(JarUtil::isClassFile(S("")));
    // 双斜杠不属于合法尾缀形式，仍判 false。
    EXPECT_FALSE(JarUtil::isClassFile(S("Foo.class//")));
}

// ── 后缀判定的边界形态（os29/Jbytec 样本实测形态回归，判据溯源：
//    私有侧 survey 笔记）──────────

// 恰好等于尾缀本身的最短输入：当前为纯文本尾缀判定，放行。
// （".class" 是 6 字节、".class/" 是 7 字节，正好达到两个下限长度。）
TEST(MjJarUtilIsClassFileTest, minimalBoundaryInputs)
{
    EXPECT_TRUE(JarUtil::isClassFile(S(".class")));
    EXPECT_TRUE(JarUtil::isClassFile(S(".class/")));
}

// 尾缀判定大小写敏感：JVM entry 名本身大小写敏感，.CLASS 不放行（fail-closed）。
TEST(MjJarUtilIsClassFileTest, caseSensitiveSuffix)
{
    EXPECT_FALSE(JarUtil::isClassFile(S("Foo.CLASS")));
    EXPECT_FALSE(JarUtil::isClassFile(S("Foo.CLASS/")));
    EXPECT_FALSE(JarUtil::isClassFile(S("Foo.Class")));
}

// 双后缀（X.class.class）按文本尾缀命中 .class：名字关放行，是否真按 class
// 处理由内容关（isClassFile(bytes) 双检）决定——此期望锁定的是文本层语义。
TEST(MjJarUtilIsClassFileTest, doubleSuffixTextualMatch)
{
    EXPECT_TRUE(JarUtil::isClassFile(S("X.class.class")));
    EXPECT_TRUE(JarUtil::isClassFile(S("a/b/X.class.class/")));
    // 近似但不构成尾缀的形态一律拒绝。
    EXPECT_FALSE(JarUtil::isClassFile(S("X.classx")));
    EXPECT_FALSE(JarUtil::isClassFile(S("X.classx/")));
    EXPECT_FALSE(JarUtil::isClassFile(S("X.myclass")));
}

// 目录形态（尾斜杠但非 .class/ 尾缀）：OS2.9 中央目录视角下 59 个 class 内容
// 藏在 'yaakov/jni/' 这类目录名 entry 里；名字关必须拒绝（fail-closed 落资源
// 通道），与 Recaf 按内容魔数收编的行为划清界线。
TEST(MjJarUtilIsClassFileTest, directoryLikeNamesRejected)
{
    EXPECT_FALSE(JarUtil::isClassFile(S("yaakov/jni/")));
    EXPECT_FALSE(JarUtil::isClassFile(S("a/b/")));
    EXPECT_FALSE(JarUtil::isClassFile(S("Foo.txt/")));
    // 斜杠后还有别的段：不是尾斜杠形态。
    EXPECT_FALSE(JarUtil::isClassFile(S("Foo.class/x")));
}

TEST(MjJarUtilIsJarFileTest, jarExtension)
{
    EXPECT_TRUE(JarUtil::isJarFile(S("lib.jar")));
    EXPECT_TRUE(JarUtil::isJarFile(S("path/to/lib.jar")));
}

TEST(MjJarUtilIsJarFileTest, jarUpperExtension)
{
    EXPECT_TRUE(JarUtil::isJarFile(S("LIB.JAR")));
}

TEST(MjJarUtilIsJarFileTest, nonJar)
{
    EXPECT_FALSE(JarUtil::isJarFile(S("Foo.class")));
    EXPECT_FALSE(JarUtil::isJarFile(S("java.base.jmod")));
    EXPECT_FALSE(JarUtil::isJarFile(S("lib.zip")));
    EXPECT_FALSE(JarUtil::isJarFile(S("libjar")));
    EXPECT_FALSE(JarUtil::isJarFile(S("")));
}

TEST(MjJarUtilIsJmodFileTest, jmodExtension)
{
    EXPECT_TRUE(JarUtil::isJmodFile(S("java.base.jmod")));
    EXPECT_TRUE(JarUtil::isJmodFile(S("path/to/java.base.jmod")));
}

TEST(MjJarUtilIsJmodFileTest, jmodUpperExtension)
{
    EXPECT_TRUE(JarUtil::isJmodFile(S("JAVA.BASE.JMOD")));
}

TEST(MjJarUtilIsJmodFileTest, nonJmod)
{
    EXPECT_FALSE(JarUtil::isJmodFile(S("Foo.class")));
    EXPECT_FALSE(JarUtil::isJmodFile(S("lib.jar")));
    EXPECT_FALSE(JarUtil::isJmodFile(S("libjmod")));
    EXPECT_FALSE(JarUtil::isJmodFile(S("")));
}

TEST(MjJarUtilIsClassFileBytesTest, classMagic)
{
    std::vector<ca::u8> bytes = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34};
    EXPECT_TRUE(JarUtil::isClassFile(bytes));
}

TEST(MjJarUtilIsClassFileBytesTest, nonClassMagic)
{
    std::vector<ca::u8> bytes = {0x50, 0x4B, 0x03, 0x04};
    EXPECT_FALSE(JarUtil::isClassFile(bytes));
    std::vector<ca::u8> shortBytes = {0xCA, 0xFE};
    EXPECT_FALSE(JarUtil::isClassFile(shortBytes));
}

// ── 常量池头部合法性预检（Recaf 98dd1f528 同类修复）────────────────

namespace {

std::vector<ca::u8> classHeader(ca::u32 cpCount)
{
    return {0xCA, 0xFE, 0xBA, 0xBE,
            0x00, 0x00,   // minor
            0x00, 0x34,   // major 52
            static_cast<ca::u8>((cpCount >> 8) & 0xFF),
            static_cast<ca::u8>(cpCount & 0xFF)};
}

}   // namespace

// magic 对但首个 CP tag 越界（打乱的常量池）：不再判为 class。
TEST(MjJarUtilIsClassFileBytesTest, illegalConstantPoolTagRejected)
{
    auto bytes      = classHeader(0x0005);
    bytes.push_back(121);   // 非法 tag（> 20）
    bytes.push_back(0x00);
    bytes.push_back(0x01);
    bytes.push_back(0x00);
    bytes.push_back(0x02);
    EXPECT_FALSE(JarUtil::isClassFile(bytes));
}

// 废弃槽位 tag（2/13/14）同样非法。
TEST(MjJarUtilIsClassFileBytesTest, reservedConstantPoolTagRejected)
{
    for (ca::u8 reserved : {2, 13, 14}) {
        auto bytes = classHeader(0x0002);
        bytes.push_back(reserved);
        bytes.push_back(0x00);
        bytes.push_back(0x01);
        EXPECT_FALSE(JarUtil::isClassFile(bytes));
    }
}

// 合法 CP 头（Utf8 + Class 前两个条目）照常放行。
TEST(MjJarUtilIsClassFileBytesTest, legalConstantPoolHeaderAccepted)
{
    auto bytes = classHeader(0x0003);
    bytes.push_back(1);    // Utf8
    bytes.push_back(0x00);
    bytes.push_back(0x03);
    bytes.insert(bytes.end(), {'F', 'o', 'o'});
    bytes.push_back(7);    // Class
    bytes.push_back(0x00);
    bytes.push_back(0x01);
    EXPECT_TRUE(JarUtil::isClassFile(bytes));
}

// 条目中途截断：宽松放行（精确诊断交给解析层）。
TEST(MjJarUtilIsClassFileBytesTest, truncatedHeaderLenient)
{
    auto bytes = classHeader(0xFFFF);
    bytes.push_back(1);    // Utf8
    bytes.push_back(0xFF); // 声明长度 0xFFFF
    bytes.push_back(0xFF);
    bytes.push_back('F');  // 实际只有 1 字节
    EXPECT_TRUE(JarUtil::isClassFile(bytes));
}

// Long/Double 占两个槽位的步进语义：tag 合法时不应误判。
TEST(MjJarUtilIsClassFileBytesTest, wideConstantSkipsTwoSlots)
{
    auto bytes = classHeader(0x0003);
    bytes.push_back(5);    // Long
    bytes.insert(bytes.end(), 8, 0x00);
    bytes.push_back(1);    // Utf8
    bytes.push_back(0x00);
    bytes.push_back(0x01);
    bytes.push_back('A');
    EXPECT_TRUE(JarUtil::isClassFile(bytes));
}

// Long 双槽位尾随截断：载荷只给出 4/8 字节即到文件尾——截断在 walk 中途，
// 按预检的宽容约定放行（真 class 可能被切尾），不得误判为非法 tag。
TEST(MjJarUtilIsClassFileBytesTest, wideConstantTruncatedTailIsLenient)
{
    auto bytes = classHeader(0x0002);
    bytes.push_back(5);    // Long
    bytes.insert(bytes.end(), 4, 0x00);   // 载荷 8 字节只有 4 字节即截断
    EXPECT_TRUE(JarUtil::isClassFile(bytes));
}

// stub 前缀 JAR（上游 98dd1f528 InfoImporterTest 用例移植）：文件头部形似
// class（magic 合法）但首个 CP tag 越界（121），其后紧跟真实的 ZIP 数据
// （"PK\x03\x04"…）。内容级嗅探必须判非 class——上游修复前这类文件被误判
// 为"单个损坏 class"，整个容器按 illegal-class-suspect 处理导致条目全都不
// 能加载；修复后内容嗅探放行给 ZIP/普通文件路径。
TEST(MjJarUtilIsClassFileBytesTest, stubPrefixedJarBlobIsNotAClass)
{
    auto bytes = classHeader(0x0005);
    bytes.push_back(121);   // 越界 CP tag：混淆器打乱的伪 class 头
    bytes.push_back(0x00);
    bytes.push_back(0x01);
    bytes.push_back(0x00);
    bytes.push_back(0x02);
    // 追加真实 ZIP 头部字节（stub 前缀后的容器数据）。
    const ca::u8 zipLocal[] = {0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00};
    bytes.insert(bytes.end(), zipLocal, zipLocal + sizeof(zipLocal));
    EXPECT_FALSE(JarUtil::isClassFile(bytes));
}
