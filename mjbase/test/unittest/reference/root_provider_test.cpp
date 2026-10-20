/// @file        root_provider_test.cpp
/// @brief       根提供者用例：keep 规则/档位、main 发现、组合追加、闭包闭环与
///              池内视图生命周期。
///
/// 重点不在重复 keep_roots_test 的规则语义，而在「provider 是纯值查询」：
/// collect 不碰 options、不注册状态；appendRoots 只追加不覆盖；多个来源的
/// 并集能真的驱动可达性闭包。
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "jbc/core/bytes.hpp"
#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/entry_points.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/keep_rule.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/reference/root_provider.hpp"

using jbc::core::Op;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::appendRoots;
using mjbase::reference::GlobalReachability;
using mjbase::reference::GlobalReachabilityOptions;
using mjbase::reference::FieldRefKey;
using mjbase::reference::IRootProvider;
using mjbase::reference::isJavaMainMethod;
using mjbase::reference::KeepRootProvider;
using mjbase::reference::KeepRuleSet;
using mjbase::reference::KeepTier;
using mjbase::reference::MainMethodRootProvider;
using mjbase::reference::MethodDeclaration;
using mjbase::reference::MethodRefKey;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::RootSet;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::FieldSpec;
using mjbase::reference::test_support::MethodSpec;

namespace {

constexpr ca::u16 kPublic    = 0x0001;
constexpr ca::u16 kPrivate   = 0x0002;
constexpr ca::u16 kProtected = 0x0004;
constexpr ca::u16 kPackage   = 0x0000;
constexpr ca::u16 kPublicStatic = 0x0009;
constexpr ca::u16 kClassPublicSuper = 0x0021;

void addClassBytes(ResourceContainer& container, std::string_view name, const Bytes& bytes)
{
    const std::string uri(name);
    container.addResource(uri + ".class", std::make_unique<ClassResource>(uri + ".class", bytes));
}

MethodSpec method(std::string_view name, std::string_view desc, ca::u16 access)
{
    MethodSpec s;
    s.access    = access;
    s.name      = name;
    s.desc      = desc;
    s.maxStack  = 0;
    s.maxLocals = 1;
    s.emit      = [](CodeEmit& e) { e.op(Op::RETURN); };
    return s;
}

Bytes buildPubClass()
{
    ClassBytesBuilder b("p/Pub");
    b.setAccess(kClassPublicSuper);
    b.addField(FieldSpec{kPublic, "pubField", "I"});
    b.addField(FieldSpec{kProtected, "protField", "I"});
    b.addField(FieldSpec{kPrivate, "privField", "I"});
    std::vector<MethodSpec> ms;
    ms.push_back(method("pubMethod", "()V", kPublic));
    ms.push_back(method("protMethod", "()V", kProtected));
    ms.push_back(method("privMethod", "()V", kPrivate));
    return b.assemble(ms);
}

Bytes buildHiddenClass()
{
    ClassBytesBuilder b("p/Hidden");
    b.setAccess(0x0020);
    b.addField(FieldSpec{kPublic, "hiddenPubField", "I"});
    std::vector<MethodSpec> ms;
    ms.push_back(method("hiddenPubMethod", "()V", kPublic));
    return b.assemble(ms);
}

/// p/App：一个真正的 main 入口；另有一个非 static 的 main 形态（不同描述符，
/// 避免声明重复）用于钉死谓词的访问位校验。
Bytes buildAppClass()
{
    ClassBytesBuilder b("p/App");
    b.setAccess(kClassPublicSuper);
    std::vector<MethodSpec> ms;
    ms.push_back(method("main", "([Ljava/lang/String;)V", kPublicStatic));
    ms.push_back(method("helper", "()V", kPublic));
    return b.assemble(ms);
}

std::vector<std::pair<std::string, Bytes>> fixtureClasses()
{
    std::vector<std::pair<std::string, Bytes>> out;
    out.emplace_back("p/Pub", buildPubClass());
    out.emplace_back("p/Hidden", buildHiddenClass());
    out.emplace_back("p/App", buildAppClass());
    return out;
}

ResourceContainer buildFixture()
{
    ResourceContainer container;
    for (const auto& [name, bytes] : fixtureClasses()) {
        addClassBytes(container, name, bytes);
    }
    return container;
}

KeepRuleSet parseRules(const char* text)
{
    const auto parsed = KeepRuleSet::parse(text);
    EXPECT_TRUE(parsed.is_ok());
    return parsed.is_ok() ? std::move(parsed).unwrap() : KeepRuleSet{};
}

bool hasClassRoot(const RootSet& roots, std::string_view name)
{
    for (std::string_view c : roots.classes) {
        if (c == name) return true;
    }
    return false;
}

bool hasMethodRoot(const RootSet& roots, std::string_view owner, std::string_view name,
                   std::string_view desc)
{
    for (const MethodRefKey& m : roots.methods) {
        if (m.owner == owner && m.name == name && m.desc == desc) return true;
    }
    return false;
}

bool hasFieldRoot(const RootSet& roots, std::string_view owner, std::string_view name,
                  std::string_view desc)
{
    for (const FieldRefKey& f : roots.fields) {
        if (f.owner == owner && f.name == name && f.desc == desc) return true;
    }
    return false;
}

}   // namespace

// ── 入口谓词 ──────────────────────────────────────────────────

TEST(MjRootProviderTest, IsJavaMainMethodChecksNameDescAndAccess)
{
    MethodDeclaration decl;
    decl.method = MethodRefKey{"p/App", "main", "([Ljava/lang/String;)V"};
    decl.access = kPublicStatic;
    EXPECT_TRUE(isJavaMainMethod(decl));

    decl.access = kPublic;
    EXPECT_FALSE(isJavaMainMethod(decl));
    decl.access = 0x0008;   // static 但非 public
    EXPECT_FALSE(isJavaMainMethod(decl));

    decl.access = kPublicStatic;
    decl.method.name = "notMain";
    EXPECT_FALSE(isJavaMainMethod(decl));
    decl.method.name = "main";
    decl.method.desc = "()V";
    EXPECT_FALSE(isJavaMainMethod(decl));
}

// ── keep provider ─────────────────────────────────────────────

TEST(MjRootProviderTest, KeepProviderTranslatesClassRulesOnly)
{
    ResourceContainer container = buildFixture();
    ReferenceIndex    index;
    index.build(container);

    KeepRootProvider provider(parseRules("class p/**\n"));
    const RootSet roots = provider.collect(index);

    EXPECT_TRUE(hasClassRoot(roots, "p/Pub"));
    EXPECT_TRUE(hasClassRoot(roots, "p/Hidden"));
    EXPECT_TRUE(hasClassRoot(roots, "p/App"));
    EXPECT_TRUE(roots.methods.empty());   // 类规则不推导成员根
    EXPECT_TRUE(roots.fields.empty());
}

TEST(MjRootProviderTest, KeepProviderTranslatesMemberRules)
{
    ResourceContainer container = buildFixture();
    ReferenceIndex    index;
    index.build(container);

    KeepRootProvider provider(
        parseRules("member p/Pub#pubMethod()V\nmember p/Pub#pubField\n"));
    const RootSet roots = provider.collect(index);

    EXPECT_TRUE(roots.classes.empty());
    ASSERT_EQ(roots.methods.size(), 1u);
    EXPECT_TRUE(hasMethodRoot(roots, "p/Pub", "pubMethod", "()V"));
    ASSERT_EQ(roots.fields.size(), 1u);
    EXPECT_TRUE(hasFieldRoot(roots, "p/Pub", "pubField", "I"));
}

TEST(MjRootProviderTest, KeepProviderTierKeepsVisibleApiOnly)
{
    ResourceContainer container = buildFixture();
    ReferenceIndex    index;
    index.build(container);

    KeepRootProvider provider(KeepTier::PublicProtected);
    const RootSet roots = provider.collect(index);

    // 类根只含 public 类；包私有类不入根。
    EXPECT_TRUE(hasClassRoot(roots, "p/Pub"));
    EXPECT_TRUE(hasClassRoot(roots, "p/App"));
    EXPECT_FALSE(hasClassRoot(roots, "p/Hidden"));

    EXPECT_TRUE(hasMethodRoot(roots, "p/Pub", "pubMethod", "()V"));
    EXPECT_TRUE(hasMethodRoot(roots, "p/Pub", "protMethod", "()V"));
    EXPECT_FALSE(hasMethodRoot(roots, "p/Pub", "privMethod", "()V"));
    EXPECT_TRUE(hasMethodRoot(roots, "p/App", "main", "([Ljava/lang/String;)V"));

    EXPECT_TRUE(hasFieldRoot(roots, "p/Pub", "pubField", "I"));
    EXPECT_TRUE(hasFieldRoot(roots, "p/Pub", "protField", "I"));
    EXPECT_FALSE(hasFieldRoot(roots, "p/Pub", "privField", "I"));
    EXPECT_FALSE(hasFieldRoot(roots, "p/Hidden", "hiddenPubField", "I"));
}

TEST(MjRootProviderTest, EmptyKeepProviderIsIdentity)
{
    ResourceContainer container = buildFixture();
    ReferenceIndex    index;
    index.build(container);

    const RootSet roots = KeepRootProvider{}.collect(index);
    EXPECT_TRUE(roots.classes.empty());
    EXPECT_TRUE(roots.methods.empty());
    EXPECT_TRUE(roots.fields.empty());
}

// ── main provider ────────────────────────────────────────────

TEST(MjRootProviderTest, MainProviderCollectsOnlyStartupEntries)
{
    ResourceContainer container = buildFixture();
    ReferenceIndex    index;
    index.build(container);

    const RootSet roots = MainMethodRootProvider{}.collect(index);
    ASSERT_EQ(roots.methods.size(), 1u);
    EXPECT_TRUE(hasMethodRoot(roots, "p/App", "main", "([Ljava/lang/String;)V"));
    EXPECT_TRUE(roots.classes.empty());
    EXPECT_TRUE(roots.fields.empty());
}

// ── 组合与闭环 ────────────────────────────────────────────────

TEST(MjRootProviderTest, AppendRootsPreservesExistingAndAddsProviders)
{
    ResourceContainer container = buildFixture();
    ReferenceIndex    index;
    index.build(container);

    GlobalReachabilityOptions options;
    options.rootClasses.push_back("com/legacy/Keep");   // 既有来源
    options.includeMainMethods = false;                 // main 由 provider 提供

    KeepRootProvider keep(parseRules("class p/Hidden\n"));
    MainMethodRootProvider mains;
    const std::vector<const IRootProvider*> providers{&keep, &mains};
    appendRoots(providers, index, options);

    ASSERT_FALSE(options.rootClasses.empty());
    EXPECT_EQ(options.rootClasses.front(), "com/legacy/Keep");   // 追加不覆盖
    bool hasHidden = false;
    for (std::string_view c : options.rootClasses) {
        if (c == "p/Hidden") hasHidden = true;
    }
    EXPECT_TRUE(hasHidden);

    bool hasMain = false;
    for (const MethodRefKey& m : options.rootMethods) {
        if (m.owner == "p/App" && m.name == "main" &&
            m.desc == "([Ljava/lang/String;)V") {
            hasMain = true;
        }
    }
    EXPECT_TRUE(hasMain);

    // 根必须真的进入可达闭包，而不只是填了 vector。
    CallGraphBuilder builder;
    for (const auto& [name, bytes] : fixtureClasses()) {
        (void)name;
        const auto added = builder.addClassBytes(jbc::core::ByteView{bytes.data(), bytes.size()});
        ASSERT_TRUE(added.is_ok());
    }
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    GlobalReachability reachability;
    reachability.build(index, graph, options);
    EXPECT_TRUE(reachability.isClassReachable("p/Hidden"));
    EXPECT_TRUE(reachability.isMethodReachable("p/App", "main", "([Ljava/lang/String;)V"));
    EXPECT_TRUE(reachability.isClassReachable("com/legacy/Keep"));
}

TEST(MjRootProviderTest, AppendRootsSkipsNullProviders)
{
    ResourceContainer container = buildFixture();
    ReferenceIndex    index;
    index.build(container);

    GlobalReachabilityOptions options;
    const std::vector<const IRootProvider*> providers{nullptr};
    appendRoots(providers, index, options);

    EXPECT_TRUE(options.rootClasses.empty());
    EXPECT_TRUE(options.rootMethods.empty());
    EXPECT_TRUE(options.rootFields.empty());
}

// ── 生命周期契约 ──────────────────────────────────────────────

TEST(MjRootProviderTest, RootViewsPointAtIndexPoolText)
{
    ResourceContainer container = buildFixture();
    ReferenceIndex    index;
    index.build(container);

    const RootSet roots = KeepRootProvider(KeepTier::Public).collect(index);
    ASSERT_FALSE(roots.classes.empty());
    ASSERT_FALSE(roots.methods.empty());

    for (const std::string_view root : roots.classes) {
        bool pointerMatched = false;
        for (const auto& decl : index.classDeclarations()) {
            if (decl.name == root && decl.name.data() == root.data()) pointerMatched = true;
        }
        EXPECT_TRUE(pointerMatched) << "类根视图未指向索引池文本: " << root;
    }
    for (const MethodRefKey& root : roots.methods) {
        bool pointerMatched = false;
        for (const auto& decl : index.methodDeclarations()) {
            if (decl.method.owner == root.owner &&
                decl.method.owner.data() == root.owner.data()) {
                pointerMatched = true;
            }
        }
        EXPECT_TRUE(pointerMatched) << "方法根 owner 未指向索引池文本: " << root.owner;
    }
    for (const auto& root : roots.fields) {
        bool pointerMatched = false;
        for (const auto& decl : index.fieldDeclarations()) {
            if (decl.field.owner == root.owner &&
                decl.field.owner.data() == root.owner.data()) {
                pointerMatched = true;
            }
        }
        EXPECT_TRUE(pointerMatched) << "字段根 owner 未指向索引池文本: " << root.owner;
    }
}
