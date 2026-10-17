/// @file        keep_rule_test.cpp
/// @brief       keep 规则面单测：DSL 解析（fail-loud 带行号）、glob 语义
///         （单星/双星/?）、预设模式库展开、匹配查询面。
///
/// 集成面（规则进入 unused_member_remover 家族后的「只加不减」不变量）由
/// test/unittest/pass/unused_member_remover_test.cpp 的 keep 规则用例钉死，
/// 本文件只测规则模块自身。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/reference/keep_rule.hpp"

namespace {

using mjbase::reference::KeepPreset;
using mjbase::reference::KeepRuleErrorCode;
using mjbase::reference::KeepRuleKind;
using mjbase::reference::KeepRuleParseError;
using mjbase::reference::KeepRuleSet;

// ── glob 匹配语义 ─────────────────────────────────────────────────────────────

TEST(MjReferenceGlobMatchTest, SingleStarDoesNotCrossSlash)
{
    EXPECT_TRUE(mjbase::reference::globMatch("com/foo/*", "com/foo/Bar"));
    EXPECT_FALSE(mjbase::reference::globMatch("com/foo/*", "com/foo/a/b"));
    EXPECT_FALSE(mjbase::reference::globMatch("com/foo/*", "com/foobar/Bar"));
    EXPECT_TRUE(mjbase::reference::globMatch("com/foo/*", "com/foo/"));   // 空片段
}

TEST(MjReferenceGlobMatchTest, DoubleStarCrossesSlash)
{
    EXPECT_TRUE(mjbase::reference::globMatch("com/foo/**", "com/foo/Bar"));
    EXPECT_TRUE(mjbase::reference::globMatch("com/foo/**", "com/foo/a/b/C"));
    EXPECT_TRUE(mjbase::reference::globMatch("**", "anything/goes/Here"));
    EXPECT_TRUE(mjbase::reference::globMatch("com/**/end", "com/a/b/end"));
    EXPECT_FALSE(mjbase::reference::globMatch("com/**/end", "com/endx"));
}

TEST(MjReferenceGlobMatchTest, QuestionMarkMatchesSingleNonSlash)
{
    EXPECT_TRUE(mjbase::reference::globMatch("com/foo/Ba?", "com/foo/Bar"));
    EXPECT_FALSE(mjbase::reference::globMatch("com/foo/Ba?", "com/foo/Baar"));
    EXPECT_FALSE(mjbase::reference::globMatch("com/foo/Ba?r", "com/foo/Bar"));  // 字面 r 不匹配
}

TEST(MjReferenceGlobMatchTest, LiteralAndExactMatch)
{
    EXPECT_TRUE(mjbase::reference::globMatch("com/foo/Bar", "com/foo/Bar"));
    EXPECT_FALSE(mjbase::reference::globMatch("com/foo/Bar", "com/foo/Bars"));
    // 成员名内的 '$' 是字面（lambda 合成名）。
    EXPECT_TRUE(mjbase::reference::globMatch("lambda$*", "lambda$main$0"));
    EXPECT_TRUE(mjbase::reference::globMatch("$deserializeLambda$*", "$deserializeLambda$Foo"));
}

// ── DSL 解析：合法形态 ────────────────────────────────────────────────────────

TEST(MjKeepRuleParseTest, ParsesClassMemberPresetLines)
{
    const auto parsed = KeepRuleSet::parse(
        "# comment line\n"
        "\n"
        "class com.foo.**\r\n"
        "  member com/foo/Bar#baz(Ljava/lang/String;)V  \n"
        "member com.foo.Bar#anyName\n"
        "preset +lambda\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();
    ASSERT_EQ(set.size(), 5u);   // 3 条显式行 + preset +lambda 展开 2 条

    const std::vector<mjbase::reference::KeepRule>& rules = set.rules();
    EXPECT_EQ(rules[0].kind, KeepRuleKind::Class);
    EXPECT_EQ(rules[0].owner, "com/foo/**");            // '.' 归一为 '/'
    EXPECT_EQ(rules[1].kind, KeepRuleKind::Member);
    EXPECT_EQ(rules[1].owner, "com/foo/Bar");
    EXPECT_EQ(rules[1].name, "baz");
    EXPECT_EQ(rules[1].desc, "(Ljava/lang/String;)V");  // 括号内整体为描述符 glob
    EXPECT_EQ(rules[2].desc, "**");                     // 省略括号 = 任意描述符
    EXPECT_EQ(rules[2].name, "anyName");
    EXPECT_EQ(rules[3].name, "lambda$*");
    EXPECT_EQ(rules[4].name, "$deserializeLambda$*");
}

TEST(MjKeepRuleParseTest, EmptyTextYieldsEmptySet)
{
    const auto parsed = KeepRuleSet::parse("");
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_TRUE(parsed.unwrap().empty());
}

// ── DSL 解析：fail-loud（行号 + 原因码）────────────────────────────────────────

TEST(MjKeepRuleParseTest, UnknownKeywordFailsLoudWithLine)
{
    const auto parsed = KeepRuleSet::parse("class a.B\nkeep com/x/Y\n");
    ASSERT_TRUE(parsed.is_err());
    const KeepRuleParseError err = parsed.unwrap_err();
    EXPECT_EQ(err.line, 2u);
    EXPECT_EQ(err.code, KeepRuleErrorCode::UnknownKeyword);
}

TEST(MjKeepRuleParseTest, MissingArgumentFailsLoud)
{
    const auto parsed = KeepRuleSet::parse("class\n");
    ASSERT_TRUE(parsed.is_err());
    EXPECT_EQ(parsed.unwrap_err().line, 1u);
    EXPECT_EQ(parsed.unwrap_err().code, KeepRuleErrorCode::MissingArgument);
}

TEST(MjKeepRuleParseTest, MalformedMemberFailsLoud)
{
    // 缺 '#'。
    const auto noHash = KeepRuleSet::parse("member com/foo/Bar\n");
    ASSERT_TRUE(noHash.is_err());
    EXPECT_EQ(noHash.unwrap_err().code, KeepRuleErrorCode::MalformedMember);

    // 括号不闭合。
    const auto openParen = KeepRuleSet::parse("member com/foo/Bar#baz(\n");
    ASSERT_TRUE(openParen.is_err());
    EXPECT_EQ(openParen.unwrap_err().code, KeepRuleErrorCode::MalformedMember);

    // 宿主段为空。
    const auto emptyOwner = KeepRuleSet::parse("member #name()V\n");
    ASSERT_TRUE(emptyOwner.is_err());
    EXPECT_EQ(emptyOwner.unwrap_err().code, KeepRuleErrorCode::MalformedMember);
}

TEST(MjKeepRuleParseTest, UnknownPresetFailsLoud)
{
    // 名字必须在预设库里不存在——预设库会扩，用 `+nope` 这种恒不存在的名字，
    // 别拿某个当前未实现的家族名（将来实现了这个用例会静默变绿变假）。
    const auto parsed = KeepRuleSet::parse("class a/B\npreset +nope\n");
    ASSERT_TRUE(parsed.is_err());
    EXPECT_EQ(parsed.unwrap_err().line, 2u);
    EXPECT_EQ(parsed.unwrap_err().code, KeepRuleErrorCode::UnknownPreset);
}

// ── 预设模式库展开 ────────────────────────────────────────────────────────────

TEST(MjKeepRulePresetTest, SerializationPresetExpandsToCallbackFamily)
{
    const auto parsed = KeepRuleSet::parse("preset serialization\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();
    // 9 个成员模式（7 回调 + serialVersionUID + serialPersistentFields）
    // + 无参构造器。
    EXPECT_EQ(set.size(), 10u);
    EXPECT_TRUE(set.matchesMember("any/Owner", "readObject", "(Ljava/io/ObjectInputStream;)V"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "writeObject", "(Ljava/io/ObjectOutputStream;)V"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "readObjectNoData", "()V"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "readExternal", "(Ljava/io/ObjectInput;)V"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "writeExternal", "(Ljava/io/ObjectOutput;)V"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "writeReplace", "()Ljava/lang/Object;"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "readResolve", "()Ljava/lang/Object;"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "serialVersionUID", "J"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "serialPersistentFields",
                                  "[Ljava/io/ObjectStreamField;"));
    EXPECT_TRUE(set.matchesMember("any/Owner", "<init>", "()V"));
    // 精确描述符：差一个字节不匹配。
    EXPECT_FALSE(set.matchesMember("any/Owner", "readObject", "()V"));
    EXPECT_FALSE(set.matchesMember("any/Owner", "<init>", "(I)V"));
}

TEST(MjKeepRulePresetTest, LambdaPresetMatchesSyntheticNames)
{
    const auto parsed = KeepRuleSet::parse("preset +lambda\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();
    EXPECT_TRUE(set.matchesMember("com/foo/Bar", "lambda$main$0", "(I)Ljava/lang/String;"));
    EXPECT_TRUE(set.matchesMember("com/foo/Bar", "$deserializeLambda$X", "(I)Ljava/lang/Object;"));
    EXPECT_FALSE(set.matchesMember("com/foo/Bar", "main", "([Ljava/lang/String;)V"));
}

TEST(MjKeepRulePresetTest, AddPresetProgrammaticPath)
{
    KeepRuleSet set;
    const auto  added = set.addPreset(KeepPreset::Serialization);
    ASSERT_TRUE(added.is_ok());
    EXPECT_FALSE(set.empty());
    EXPECT_TRUE(set.matchesMember("x/Y", "readResolve", "()Ljava/lang/Object;"));
}

// RMI：骨架/存根类按名字反射加载；层级谓词挂账，只落命名部分。
TEST(MjKeepRulePresetTest, RmiPresetCoversSkeletonClassNamesAndRemoteRefCtor)
{
    const auto parsed = KeepRuleSet::parse("preset +rmi\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();

    EXPECT_TRUE(set.matchesClass("com/foo/Payroll_Stub"));
    EXPECT_TRUE(set.matchesClass("com/foo/Payroll_Skel"));
    EXPECT_TRUE(set.matchesClass("com/foo/Payroll_Tie"));
    EXPECT_FALSE(set.matchesClass("com/foo/Payroll"));          // 无特征后缀
    EXPECT_FALSE(set.matchesClass("com/foo/Payroll_StubImpl")); // 后缀必须是结尾

    EXPECT_TRUE(set.matchesMember("com/foo/Payroll_Stub", "<init>",
                                  "(Ljava/rmi/server/RemoteRef;)V"));
    EXPECT_FALSE(set.matchesMember("com/foo/Payroll_Stub", "<init>", "()V"));
}

// EJB：容器按名反射调用生命周期回调；注解族挂账，只落回调名。
TEST(MjKeepRulePresetTest, EjbPresetCoversLifecycleCallbacks)
{
    const auto parsed = KeepRuleSet::parse("preset ejb\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();

    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "ejbCreate", "()V"));
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "ejbCreateWithId", "(I)V"));  // 通配后缀
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "ejbActivate", "()V"));
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "ejbPassivate", "()V"));
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "ejbRemove", "()V"));
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "ejbLoad", "()V"));
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "ejbStore", "()V"));
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "setSessionContext",
                                  "(Ljavax/ejb/SessionContext;)V"));
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "setMessageDrivenContext",
                                  "(Ljavax/ejb/MessageDrivenContext;)V"));
    // 描述符放开（容器按名查，签名各版本不一致）：任意描述符都命中。
    EXPECT_TRUE(set.matchesMember("com/foo/AccountBean", "ejbLoad", "(I)J"));
    // 非回调名不命中（防整类全保）。
    EXPECT_FALSE(set.matchesMember("com/foo/AccountBean", "transfer", "(I)V"));
}

// JMX：MBean/MXBean 接口由 MBeanServer 按名发现。
TEST(MjKeepRulePresetTest, JmxPresetCoversMbeanClassNames)
{
    const auto parsed = KeepRuleSet::parse("preset +jmx\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();

    EXPECT_TRUE(set.matchesClass("com/foo/CacheMBean"));
    EXPECT_TRUE(set.matchesClass("com/foo/CacheMXBean"));
    EXPECT_TRUE(set.matchesClass("java/lang/management/MemoryMXBean"));
    EXPECT_FALSE(set.matchesClass("com/foo/Cache"));       // 无特征后缀
    EXPECT_FALSE(set.matchesClass("com/foo/CacheMBeanEx"));// 后缀必须是结尾
}

// 枚举合成面：values()/valueOf(String) 的返回类型是枚举自身，故描述符前缀钉
// 参数侧、放开返回侧。
TEST(MjKeepRulePresetTest, EnumPresetCoversSyntheticEnumSurface)
{
    const auto parsed = KeepRuleSet::parse("preset +enum\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();

    EXPECT_TRUE(set.matchesMember("com/foo/Color", "values", "()[Lcom/foo/Color;"));
    EXPECT_TRUE(set.matchesMember("com/foo/Color", "valueOf", "(Ljava/lang/String;)Lcom/foo/Color;"));
    EXPECT_TRUE(set.matchesMember("com/foo/Color", "$VALUES", "[Lcom/foo/Color;"));
    // 参数侧必须精确：valueOf(int) 不是枚举的反射契约入口。
    EXPECT_FALSE(set.matchesMember("com/foo/Color", "valueOf", "(I)Lcom/foo/Color;"));
    EXPECT_FALSE(set.matchesMember("com/foo/Color", "values", "(I)V"));
    // 非枚举类同名方法也命中——预设是保守的按名档，不是「只保真枚举」。
    EXPECT_TRUE(set.matchesMember("com/foo/NotAnEnum", "values", "()[Lcom/foo/X;"));
}

// Swing 可插拔 L&F：UI 委托由 UIManager 反射按名实例化。
TEST(MjKeepRulePresetTest, SwingPresetCoversPluggableLookAndFeelEntries)
{
    const auto parsed = KeepRuleSet::parse("preset +swing\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();

    EXPECT_TRUE(set.matchesMember("com/foo/MyButtonUI", "createUI",
                                  "(Ljavax/swing/JComponent;)Ljavax/swing/plaf/ComponentUI;"));
    EXPECT_TRUE(set.matchesMember("com/foo/MyButton", "getUI",
                                  "()Ljavax/swing/plaf/ComponentUI;"));
    // 参数侧精确：createUI() 无参不是 L&F 工厂入口。
    EXPECT_FALSE(set.matchesMember("com/foo/MyButtonUI", "createUI", "()V"));
    EXPECT_FALSE(set.matchesMember("com/foo/MyButton", "getUI", "()Ljava/lang/Object;"));
}

// JAXB：只保 ObjectFactory（反射实例化入口）；注解族挂账。
TEST(MjKeepRulePresetTest, JaxbPresetCoversObjectFactoryOnly)
{
    const auto parsed = KeepRuleSet::parse("preset +jaxb\n");
    ASSERT_TRUE(parsed.is_ok());
    const KeepRuleSet& set = parsed.unwrap();

    EXPECT_EQ(set.size(), 1u);   // 一行数据：注解族尚未接入，不得假装覆盖
    EXPECT_TRUE(set.matchesClass("com/foo/generated/ObjectFactory"));
    EXPECT_TRUE(set.matchesClass("ObjectFactory"));   // 默认包
    EXPECT_FALSE(set.matchesClass("com/foo/generated/Customer"));
    EXPECT_FALSE(set.matchesClass("com/foo/ObjectFactoryHolder"));
}

// 预设名 ↔ 枚举往返：DSL 文本面与程序化面必须同一张表。
TEST(MjKeepRulePresetTest, PresetNameRoundTripsThroughTheSameTable)
{
    for (const KeepPreset preset : {KeepPreset::Serialization, KeepPreset::Lambda,
                                    KeepPreset::Rmi, KeepPreset::Ejb, KeepPreset::Jmx,
                                    KeepPreset::Enum, KeepPreset::Swing, KeepPreset::Jaxb}) {
        const std::string_view name = mjbase::reference::to_string(preset);
        EXPECT_NE(name, "?") << "to_string 漏了某个枚举值";
        const auto back = mjbase::reference::keepPresetFrom(name);
        ASSERT_TRUE(back.is_ok()) << name;
        EXPECT_EQ(back.unwrap(), preset) << name;

        // 程序化入口与 DSL 入口必须展开出同样多的规则（两处不得分家）。
        KeepRuleSet viaEnum;
        ASSERT_TRUE(viaEnum.addPreset(preset).is_ok());
        const auto viaText = KeepRuleSet::parse(std::string("preset ").append(name).append("\n"));
        ASSERT_TRUE(viaText.is_ok()) << name;
        EXPECT_EQ(viaEnum.size(), viaText.unwrap().size()) << name;
    }
}

TEST(MjKeepRulePresetTest, UnknownPresetNameFailsFrom)
{
    const auto unknown = mjbase::reference::keepPresetFrom("nope");
    ASSERT_TRUE(unknown.is_err());
    EXPECT_EQ(unknown.unwrap_err(), KeepRuleErrorCode::UnknownPreset);
    // 空名不是「未指定」，是配置错误（与档位不同：档位空串按 None 处理）。
    EXPECT_TRUE(mjbase::reference::keepPresetFrom("").is_err());
    // 前导 '+' 可省：DSL 与程序化两处口径一致。
    EXPECT_TRUE(mjbase::reference::keepPresetFrom("+lambda").is_ok());
}

// ── 匹配查询面 ────────────────────────────────────────────────────────────────

TEST(MjKeepRuleMatchTest, ClassAndMemberRulesAreUnions)
{
    const auto parsed = KeepRuleSet::parse(
        "class com.keep.**\n"
        "member com/foo/Bar#api#(\n");
    // 上一行故意畸形：先确认失败，再用合法文本测并集。
    EXPECT_TRUE(parsed.is_err());

    const auto ok = KeepRuleSet::parse(
        "class com.keep.**\n"
        "member com/foo/Bar#api()V\n");
    ASSERT_TRUE(ok.is_ok());
    const KeepRuleSet& set = ok.unwrap();

    EXPECT_TRUE(set.matchesClass("com/keep/Anything"));
    EXPECT_TRUE(set.matchesClass("com/keep/deep/Nested"));
    EXPECT_FALSE(set.matchesClass("com/other/Anything"));

    EXPECT_TRUE(set.matchesMember("com/foo/Bar", "api", "()V"));
    EXPECT_FALSE(set.matchesMember("com/foo/Bar", "api", "(I)V"));   // 描述符精确
    EXPECT_FALSE(set.matchesMember("com/foo/Other", "api", "()V"));  // 宿主不同
    // class 规则不参与成员匹配；member 规则不参与类匹配（种类隔离）。
    EXPECT_FALSE(set.matchesMember("com/keep/Anything", "api", "()V"));
}

}   // namespace
