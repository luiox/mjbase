#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/mapping/mapping_format_registry.hpp"

using namespace mjbase::mapping;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef text(const char* value)
{
    return Utf8StringRef::from_cstr(value);
}

/// registry.parse 的 Result 适配：Ok 展开映射，Err 记失败并返回空映射
/// （断言强度与 mbase 直调等价，且避免 Err unwrap 终止进程）。
IntermediateMapping expectParsed(const char* formatName,
                                 ca::Result<IntermediateMapping, MappingError> parsed)
{
    EXPECT_TRUE(parsed.is_ok()) << formatName;
    if (parsed.is_err()) return {};
    return std::move(parsed).unwrap();
}

/// registry.format 的 Result 适配：Ok 展开文本，Err 记失败并返回空串。
std::string expectFormat(const char* formatName,
                         ca::Result<ca::str::Utf8String, MappingError> formatted)
{
    EXPECT_TRUE(formatted.is_ok()) << formatName;
    if (formatted.is_err()) return {};
    return std::move(formatted).unwrap().to_std_string();
}

std::string mappedClass(const IntermediateMapping& mappings, const char* name)
{
    return mappings.getMappedClassName(text(name)).to_std_string();
}

std::string mappedField(const IntermediateMapping& mappings, const char* owner, const char* name,
                        const char* desc)
{
    return mappings.getMappedFieldName(text(owner), text(name), text(desc)).to_std_string();
}

std::string mappedMethod(const IntermediateMapping& mappings, const char* owner, const char* name,
                         const char* desc)
{
    return mappings.getMappedMethodName(text(owner), text(name), text(desc)).to_std_string();
}

IntermediateMapping buildRoundtripMappings()
{
    IntermediateMapping mappings;
    mappings.addPackage(text("pkg/old"), text("pkg/new"));
    mappings.addClass(text("a/Foo"), text("a/FooClean"));
    mappings.addClass(text("java/lang/String"), text("java/lang/String"));
    mappings.addField(text("a/Foo"), text("I"), text("x"), text("count"));
    mappings.addField(text("a/Foo"), text("Ljava/lang/String;"), text("s"), text("name"));
    mappings.addMethod(text("a/Foo"), text("(Ljava/lang/String;I)V"), text("a"), text("run"));
    return mappings;
}

void expectRoundtripCore(const IntermediateMapping& mappings)
{
    EXPECT_EQ(mappedClass(mappings, "a/Foo"), "a/FooClean");
    EXPECT_EQ(mappedField(mappings, "a/Foo", "x", "I"), "count");
    EXPECT_EQ(mappedField(mappings, "a/Foo", "s", "Ljava/lang/String;"), "name");
    EXPECT_EQ(mappedMethod(mappings, "a/Foo", "a", "(Ljava/lang/String;I)V"), "run");
}

IntermediateMapping buildPackageRenameMappings()
{
    IntermediateMapping mappings;
    mappings.addClass(text("a/Foo"), text("com/example/Foo"));
    mappings.addField(text("a/Foo"), text("I"), text("x"), text("count"));
    mappings.addField(text("a/Foo"), text("Ljava/lang/String;"), text("s"), text("name"));
    mappings.addMethod(text("a/Foo"), text("(Ljava/lang/String;I)V"), text("a"), text("run"));
    return mappings;
}

}   // namespace

TEST(MjMappingRegistryTest, registersRecafParityAndLegacyFormats)
{
    auto registry = MappingFormatRegistry::withStandardFormats();

    EXPECT_TRUE(registry.hasFormat(text("Tiny-V1")));
    EXPECT_TRUE(registry.hasFormat(text("Tiny-V2")));
    EXPECT_TRUE(registry.hasFormat(text("Simple")));
    EXPECT_TRUE(registry.hasFormat(text("Proguard")));
    EXPECT_TRUE(registry.hasFormat(text("R8")));
    EXPECT_TRUE(registry.hasFormat(text("SRG")));
    EXPECT_TRUE(registry.hasFormat(text("Enigma")));
    EXPECT_TRUE(registry.hasFormat(text("Jadx (Legacy)")));
    EXPECT_TRUE(registry.hasFormat(text("YGuard")));
    EXPECT_TRUE(registry.hasFormat(text("Allatori")));

    auto names = registry.formatNames();
    ASSERT_FALSE(names.empty());
    EXPECT_EQ(names.front().to_std_string(), "Allatori");
    EXPECT_NE(registry.createFormat(text("Tiny-V2")), nullptr);
}

TEST(MjMappingRegistryTest, parsesTinyV2AndExportsTinyV2)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    auto mappings = expectParsed("Tiny-V2",
                                 registry.parse(text("Tiny-V2"),
                                                text("tiny\t2\t0\tintermediary\tnamed\n"
                                                     "c\ta/Foo\tcom/example/Foo\n"
                                                     "\tf\tI\tx\tcount\n"
                                                     "\tm\t(I)V\ta\trun\n")));

    EXPECT_EQ(mappedClass(mappings, "a/Foo"), "com/example/Foo");
    EXPECT_EQ(mappedField(mappings, "a/Foo", "x", "I"), "count");
    EXPECT_EQ(mappedMethod(mappings, "a/Foo", "a", "(I)V"), "run");

    auto out = expectFormat("Tiny-V2", registry.format(text("Tiny-V2"), mappings));
    EXPECT_NE(out.find("tiny\t2\t0\tintermediary\tnamed"), std::string::npos);
    EXPECT_NE(out.find("\tf\tI\tx\tcount"), std::string::npos);
    EXPECT_NE(out.find("\tm\t(I)V\ta\trun"), std::string::npos);
}

TEST(MjMappingRegistryTest, parsesProguardAndR8AsObfuscatedToOriginal)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    auto mappings = expectParsed("R8",
                                 registry.parse(text("R8"),
                                                text("com.example.Foo -> a.b:\n"
                                                     "    int count -> x\n"
                                                     "    1:1:void run(java.lang.String):42:42 -> a\n")));

    EXPECT_EQ(mappedClass(mappings, "a/b"), "com/example/Foo");
    EXPECT_EQ(mappedField(mappings, "a/b", "x", "I"), "count");
    EXPECT_EQ(mappedMethod(mappings, "a/b", "a", "(Ljava/lang/String;)V"), "run");
}

TEST(MjMappingRegistryTest, parsesSimpleSrgEnigmaAndJadx)
{
    auto registry = MappingFormatRegistry::withStandardFormats();

    auto simple = expectParsed("Simple",
                               registry.parse(text("Simple"),
                                              text("a/Foo com/example/Foo\n"
                                                   "a/Foo.x I count\n"
                                                   "a/Foo.a(I)V run\n")));
    EXPECT_EQ(mappedClass(simple, "a/Foo"), "com/example/Foo");
    EXPECT_EQ(mappedField(simple, "a/Foo", "x", "I"), "count");
    EXPECT_EQ(mappedMethod(simple, "a/Foo", "a", "(I)V"), "run");

    auto srg = expectParsed("SRG",
                            registry.parse(text("SRG"),
                                           text("PK: a com/example\n"
                                                "CL: a/Foo com/example/Foo\n"
                                                "FD: a/Foo/x I com/example/Foo/count I\n"
                                                "MD: a/Foo/a (I)V com/example/Foo/run (I)V\n")));
    EXPECT_EQ(mappedClass(srg, "a/Foo"), "com/example/Foo");
    EXPECT_EQ(mappedClass(srg, "a/Bar"), "com/example/Bar");
    EXPECT_EQ(mappedField(srg, "a/Foo", "x", "I"), "count");
    EXPECT_EQ(mappedMethod(srg, "a/Foo", "a", "(I)V"), "run");

    auto enigma = expectParsed("Enigma",
                               registry.parse(text("Enigma"),
                                              text("CLASS a/Foo com/example/Foo\n"
                                                   "\tFIELD x count I\n"
                                                   "\tMETHOD a run (I)V\n")));
    EXPECT_EQ(mappedClass(enigma, "a/Foo"), "com/example/Foo");
    EXPECT_EQ(mappedField(enigma, "a/Foo", "x", "I"), "count");
    EXPECT_EQ(mappedMethod(enigma, "a/Foo", "a", "(I)V"), "run");

    auto jadx = expectParsed("Jadx (Legacy)",
                             registry.parse(text("Jadx (Legacy)"),
                                            text("c a.Foo = FooClean\n"
                                                 "f a.Foo.x:I = count\n"
                                                 "m a.Foo.a(I)V = run\n")));
    EXPECT_EQ(mappedClass(jadx, "a/Foo"), "a/FooClean");
    EXPECT_EQ(mappedField(jadx, "a/Foo", "x", "I"), "count");
    EXPECT_EQ(mappedMethod(jadx, "a/Foo", "a", "(I)V"), "run");
}

TEST(MjMappingRegistryTest, convertsSrgToTinyV2WithMemberDescriptors)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    auto mappings = expectParsed(
        "SRG",
        registry.parse(
            text("SRG"),
            text("CL: api/Service a\n"
                 "CL: impl/Base c\n"
                 "FD: impl/Base/value I c/a I\n"
                 "MD: api/Service/name ()Ljava/lang/String; a/a ()Ljava/lang/String;\n")));

    const auto tiny = expectFormat("Tiny-V2", registry.format(text("Tiny-V2"), mappings));
    EXPECT_NE(tiny.find("tiny\t2\t0\tintermediary\tnamed"), std::string::npos);
    EXPECT_NE(tiny.find("c\tapi/Service\ta"), std::string::npos);
    EXPECT_NE(tiny.find("c\timpl/Base\tc"), std::string::npos);
    EXPECT_NE(tiny.find("\tf\tI\tvalue\ta"), std::string::npos);
    EXPECT_NE(tiny.find("\tm\t()Ljava/lang/String;\tname\ta"), std::string::npos);

    const auto reparsed =
        expectParsed("Tiny-V2", registry.parse(text("Tiny-V2"), text(tiny.c_str())));
    EXPECT_EQ(mappedClass(reparsed, "api/Service"), "a");
    EXPECT_EQ(mappedField(reparsed, "impl/Base", "value", "I"), "a");
    EXPECT_EQ(mappedMethod(reparsed, "api/Service", "name", "()Ljava/lang/String;"), "a");
}

TEST(MjMappingRegistryTest, parsesYGuardAndAllatoriXmlLogs)
{
    auto registry = MappingFormatRegistry::withStandardFormats();

    auto yguard = expectParsed(
        "YGuard",
        registry.parse(
            text("YGuard"),
            text("<map>\n"
                 "  <class name=\"com.example.Foo\" map=\"a.b\" />\n"
                 "  <field class=\"com.example.Foo\" name=\"count\" map=\"x\" descriptor=\"I\" />\n"
                 "  <method class=\"com.example.Foo\" name=\"run\" map=\"a\" descriptor=\"()V\" />\n"
                 "</map>\n")));
    EXPECT_EQ(mappedClass(yguard, "a/b"), "com/example/Foo");
    EXPECT_EQ(mappedField(yguard, "a/b", "x", "I"), "count");
    EXPECT_EQ(mappedMethod(yguard, "a/b", "a", "()V"), "run");

    auto allatori = expectParsed(
        "Allatori",
        registry.parse(
            text("Allatori"),
            text("<mappings>\n"
                 "  <class old=\"com.example.Bar\" new=\"c.d\" />\n"
                 "  <method owner=\"com.example.Bar\" old=\"touch\" new=\"b\" desc=\"(I)V\" />\n"
                 "</mappings>\n")));
    EXPECT_EQ(mappedClass(allatori, "c/d"), "com/example/Bar");
    EXPECT_EQ(mappedMethod(allatori, "c/d", "b", "(I)V"), "touch");
}

TEST(MjMappingRegistryTest, exportsRegisteredFormatsAsParseableText)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    auto mappings = buildRoundtripMappings();

    std::vector<const char*> formats = {"Tiny-V1",
                                        "Tiny-V2",
                                        "Simple",
                                        "Proguard",
                                        "R8",
                                        "SRG",
                                        "Enigma",
                                        "Jadx (Legacy)",
                                        "YGuard",
                                        "Allatori"};
    for (const auto* formatName : formats) {
        auto exportedResult = registry.format(text(formatName), mappings);
        ASSERT_TRUE(exportedResult.is_ok()) << formatName;
        auto exported = std::move(exportedResult).unwrap();
        ASSERT_FALSE(exported.is_empty()) << formatName;

        auto parsed = expectParsed(formatName, registry.parse(text(formatName), exported.ref()));
        expectRoundtripCore(parsed);
    }
}

TEST(MjMappingRegistryTest, parsesAndWritesTinyV1MemberColumnOrder)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    auto mappings = expectParsed("Tiny-V1",
                                 registry.parse(text("Tiny-V1"),
                                                text("v1\tintermediary\tnamed\n"
                                                     "CLASS\ta/Foo\tcom/example/Foo\n"
                                                     "FIELD\ta/Foo\tI\tx\tcount\n"
                                                     "METHOD\ta/Foo\t(I)V\ta\trun\n")));

    EXPECT_EQ(mappedClass(mappings, "a/Foo"), "com/example/Foo");
    EXPECT_EQ(mappedField(mappings, "a/Foo", "x", "I"), "count");
    EXPECT_EQ(mappedMethod(mappings, "a/Foo", "a", "(I)V"), "run");

    auto output = expectFormat("Tiny-V1", registry.format(text("Tiny-V1"), buildRoundtripMappings()));
    EXPECT_EQ(output.find("v1\tintermediary\tnamed\n"), 0u);
    EXPECT_NE(output.find("FIELD\ta/Foo\tI\tx\tcount\n"), std::string::npos);
    EXPECT_NE(output.find("METHOD\ta/Foo\t(Ljava/lang/String;I)V\ta\trun\n"), std::string::npos);
    expectRoundtripCore(expectParsed("Tiny-V1", registry.parse(text("Tiny-V1"), text(output.c_str()))));
}

TEST(MjMappingRegistryTest, preservesUnmodifiedFormatSpecificSourceText)
{
    auto       registry = MappingFormatRegistry::withStandardFormats();
    const auto raw      = text("tiny\t2\t0\tintermediary\tnamed\textra\n"
                               "# keep this comment and namespace metadata\n"
                               "c\ta/Foo\ta/FooClean\n");
    auto       mappings = expectParsed("Tiny-V2", registry.parse(text("Tiny-V2"), raw));
    EXPECT_EQ(expectFormat("Tiny-V2", registry.format(text("Tiny-V2"), mappings)),
              raw.to_std_string());

    mappings.addClass(text("a/Bar"), text("a/BarClean"));
    auto normalized = expectFormat("Tiny-V2", registry.format(text("Tiny-V2"), mappings));
    EXPECT_NE(normalized, raw.to_std_string());
    EXPECT_NE(normalized.find("a/Bar\ta/BarClean"), std::string::npos);
}

TEST(MjMappingRegistryTest, retainsExplicitIdentityRowsAfterMutation)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    auto mappings = expectParsed("Tiny-V2",
                                 registry.parse(text("Tiny-V2"),
                                                text("tiny\t2\t0\tintermediary\tnamed\n"
                                                     "c\tjava/lang/Object\tjava/lang/Object\n"
                                                     "\tf\tI\tserialVersionUID\tserialVersionUID\n"
                                                     "\tm\t()V\tinit\tinit\n")));

    mappings.addClass(text("pkg/New"), text("pkg/Renamed"));
    const auto output = expectFormat("Tiny-V2", registry.format(text("Tiny-V2"), mappings));
    EXPECT_NE(output.find("c\tjava/lang/Object\tjava/lang/Object"), std::string::npos);
    EXPECT_NE(output.find("\tf\tI\tserialVersionUID\tserialVersionUID"), std::string::npos);
    EXPECT_NE(output.find("\tm\t()V\tinit\tinit"), std::string::npos);
}

TEST(MjMappingRegistryTest, carriesPrivateSourceTextAcrossLineFormatConversion)
{
    auto       registry = MappingFormatRegistry::withStandardFormats();
    const auto tiny     = text("tiny\t2\t0\tintermediary\tnamed\textra\n"
                               "# private namespace metadata\n"
                               "c\ta/Foo\ta/FooClean\n");
    auto       parsed       = expectParsed("Tiny-V2", registry.parse(text("Tiny-V2"), tiny));
    auto       proguardText = expectFormat("Proguard", registry.format(text("Proguard"), parsed));
    EXPECT_NE(proguardText.find("# mjbase-source Tiny-V2"), std::string::npos);

    auto converted = expectParsed("Proguard",
                                  registry.parse(text("Proguard"), text(proguardText.c_str())));
    EXPECT_EQ(expectFormat("Tiny-V2", registry.format(text("Tiny-V2"), converted)),
              tiny.to_std_string());
}

TEST(MjMappingRegistryTest, carriesPrivateXmlAcrossFormatConversion)
{
    auto       registry  = MappingFormatRegistry::withStandardFormats();
    const auto yguard    = text("<map>\n"
                                "  <property name=\"private\" value=\"keep\" />\n"
                                "  <class name=\"pkg.Foo\" map=\"a.b\" />\n"
                                "</map>\n");
    auto       parsed    = expectParsed("YGuard", registry.parse(text("YGuard"), yguard));
    auto       tinyResult = registry.format(text("Tiny-V2"), parsed);
    ASSERT_TRUE(tinyResult.is_ok());
    auto tinyText = std::move(tinyResult).unwrap();
    auto converted =
        expectParsed("Tiny-V2", registry.parse(text("Tiny-V2"), tinyText.ref()));
    EXPECT_EQ(expectFormat("YGuard", registry.format(text("YGuard"), converted)),
              yguard.to_std_string());

    auto allatori = expectFormat("Allatori", registry.format(text("Allatori"), parsed));
    EXPECT_NE(allatori.find("<!-- mjbase-source YGuard\t"), std::string::npos);
    EXPECT_NE(allatori.find(" -->"), std::string::npos);
}

TEST(MjMappingRegistryTest, proguardWriterUsesJavaTypes)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    auto mappings = buildPackageRenameMappings();

    auto out = expectFormat("Proguard", registry.format(text("Proguard"), mappings));
    EXPECT_NE(out.find("com.example.Foo -> a.Foo:"), std::string::npos);
    EXPECT_NE(out.find("int count -> x"), std::string::npos);
    EXPECT_NE(out.find("java.lang.String name -> s"), std::string::npos);
    EXPECT_NE(out.find("void run(java.lang.String,int) -> a"), std::string::npos);
}

TEST(MjMappingRegistryTest, srgWriterEmitsDeterministicSortedLines)
{
    // 排序口径：PK 行按旧名字典序、CL 行按旧名字典序、FD/MD 组内按
    // 按 (oldName, desc) 排序输出——底座 unordered_map 迭代序不得渗进导出文本，
    // 跨实现逐行对账需要确定序。插入顺序刻意倒置，期望输出与插入序无关。
    auto                registry = MappingFormatRegistry::withStandardFormats();
    IntermediateMapping mappings;
    mappings.addPackage(text("com/example/api"), text("b/api"));
    mappings.addPackage(text("app"), text("b/core"));
    mappings.addClass(text("zoo/Animal"), text("a/A"));
    mappings.addClass(text("app/Main"), text("a/M"));
    mappings.addMethod(text("zoo/Animal"), text("(J)V"), text("b"), text("run"));
    mappings.addMethod(text("zoo/Animal"), text("(I)V"), text("a"), text("run"));
    mappings.addField(text("zoo/Animal"), text("J"), text("y"), text("wide"));
    mappings.addField(text("zoo/Animal"), text("I"), text("x"), text("count"));
    mappings.addField(text("app/Main"), text("I"), text("n"), text("num"));

    const auto out = expectFormat("SRG", registry.format(text("SRG"), mappings));
    EXPECT_EQ(out,
              "PK: app b/core\n"
              "PK: com/example/api b/api\n"
              "CL: app/Main a/M\n"
              "CL: zoo/Animal a/A\n"
              "FD: app/Main/n I a/M/num I\n"
              "FD: zoo/Animal/x I a/A/count I\n"
              "FD: zoo/Animal/y J a/A/wide J\n"
              "MD: zoo/Animal/a (I)V a/A/run (I)V\n"
              "MD: zoo/Animal/b (J)V a/A/run (J)V\n");
}

TEST(MjMappingRegistryTest, srgWriterUsesStandardMemberLines)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    auto mappings = buildPackageRenameMappings();

    auto out = expectFormat("SRG", registry.format(text("SRG"), mappings));
    EXPECT_NE(out.find("CL: a/Foo com/example/Foo"), std::string::npos);
    // 带类型字段输出 desc token（parse 两侧兼容）。
    // 会让 parse 产物按 (owner,name,desc) 查找失配）。
    EXPECT_NE(out.find("FD: a/Foo/x I com/example/Foo/count I"), std::string::npos);
    EXPECT_NE(out.find("MD: a/Foo/a (Ljava/lang/String;I)V com/example/Foo/run "
                       "(Ljava/lang/String;I)V"),
              std::string::npos);
}
