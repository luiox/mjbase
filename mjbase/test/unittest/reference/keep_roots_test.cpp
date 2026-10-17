/// @file        keep_roots_test.cpp
/// @brief       keep 配置 → 可达性根三槽的翻译面用例（审计 P0#1）。
///
/// 覆盖：class 规则/成员规则/两档档位/规则与档位的并集/既有根保留/空规格
/// 恒等/视图指向索引池（生命周期契约）。档位与规则都是纯加法，不关闭任何
/// 既有保留依据——用例里以「未命中者不入根」反向钉死，防「注入即全保」。
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
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/keep_roots.hpp"
#include "mjbase/reference/keep_rule.hpp"
#include "mjbase/reference/reference_index.hpp"

using jbc::core::Op;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::FieldRefKey;
using mjbase::reference::GlobalReachability;
using mjbase::reference::GlobalReachabilityOptions;
using mjbase::reference::KeepRootSpec;
using mjbase::reference::KeepRuleSet;
using mjbase::reference::KeepTier;
using mjbase::reference::MethodRefKey;
using mjbase::reference::ReferenceIndex;
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
constexpr ca::u16 kClassPublicSuper = 0x0021;
constexpr ca::u16 kClassPackageSuper = 0x0020;

void addClassBytes(ResourceContainer& container, std::string_view name, const Bytes& bytes)
{
    const std::string uri(name);
    container.addResource(uri + ".class", std::make_unique<ClassResource>(uri + ".class", bytes));
}

/// 最小方法：()V + RETURN（带 Code，避免被当成 abstract/native 声明）。
MethodSpec method(std::string_view name, std::string_view desc, ca::u16 access)
{
    MethodSpec s;
    s.access    = access;
    s.name      = name;
    s.desc      = desc;
    s.maxStack  = 1;
    s.maxLocals = 1;   // 实例方法带 this；静态方法多声明一个槽无害
    s.emit      = [](CodeEmit& e) { e.op(Op::RETURN); };
    return s;
}

/// p/Pub：public 类，四个可见性档的方法与字段各一。
Bytes buildPubClass()
{
    ClassBytesBuilder b("p/Pub");
    b.setAccess(kClassPublicSuper);
    b.addField(FieldSpec{kPublic, "pubField", "I"});
    b.addField(FieldSpec{kProtected, "protField", "I"});
    b.addField(FieldSpec{kPrivate, "privField", "I"});
    b.addField(FieldSpec{kPackage, "pkgField", "I"});
    std::vector<MethodSpec> ms;
    ms.push_back(method("pubMethod", "()V", kPublic));
    ms.push_back(method("protMethod", "()V", kProtected));
    ms.push_back(method("privMethod", "()V", kPrivate));
    ms.push_back(method("pkgMethod", "()V", kPackage));
    return b.assemble(ms);
}

/// p/Hidden：包私有类，仅含 public 成员（类不可见 ⇒ 其成员不构成对外 API）。
Bytes buildHiddenClass()
{
    ClassBytesBuilder b("p/Hidden");
    b.setAccess(kClassPackageSuper);
    b.addField(FieldSpec{kPublic, "hiddenPubField", "I"});
    std::vector<MethodSpec> ms;
    ms.push_back(method("hiddenPubMethod", "()V", kPublic));
    return b.assemble(ms);
}

/// 夹具的「类名 → 字节码」清单：容器与调用图两侧共用同一份，避免二次装配
/// 造成两边不一致（也省掉从 Resource 向下转型的麻烦）。
std::vector<std::pair<std::string, Bytes>> tierFixtureClasses()
{
    std::vector<std::pair<std::string, Bytes>> out;
    out.emplace_back("p/Pub", buildPubClass());
    out.emplace_back("p/Hidden", buildHiddenClass());
    return out;
}

ResourceContainer buildTierFixture()
{
    ResourceContainer container;
    for (const auto& [name, bytes] : tierFixtureClasses()) {
        addClassBytes(container, name, bytes);
    }
    return container;
}

/// 解析规则文本（失败即致命断言）。
KeepRuleSet parseRules(const char* text)
{
    const auto parsed = KeepRuleSet::parse(text);
    EXPECT_TRUE(parsed.is_ok());
    return parsed.is_ok() ? std::move(parsed).unwrap() : KeepRuleSet{};
}

bool hasClassRoot(const GlobalReachabilityOptions& options, std::string_view name)
{
    for (std::string_view c : options.rootClasses) {
        if (c == name) return true;
    }
    return false;
}

bool hasMethodRoot(const GlobalReachabilityOptions& options, std::string_view owner,
                   std::string_view name, std::string_view desc)
{
    for (const MethodRefKey& m : options.rootMethods) {
        if (m.owner == owner && m.name == name && m.desc == desc) return true;
    }
    return false;
}

bool hasFieldRoot(const GlobalReachabilityOptions& options, std::string_view owner,
                  std::string_view name, std::string_view desc)
{
    for (const FieldRefKey& f : options.rootFields) {
        if (f.owner == owner && f.name == name && f.desc == desc) return true;
    }
    return false;
}

// ── 规则 → 三槽 ────────────────────────────────────────────────

// class 规则：入根的是声明名本身（glob 只用于匹配，不当根）。
TEST(MjKeepRootsTest, classRuleRootsDeclaredNameNotPattern)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    const KeepRuleSet                 rules = parseRules("class p/**\n");
    mjbase::reference::KeepRootSpec   spec;
    spec.rules = &rules;

    GlobalReachabilityOptions options;
    mjbase::reference::applyKeepRoots(spec, index, options);

    ASSERT_EQ(options.rootMethods.size(), 0u);   // 类规则不推导成员根
    ASSERT_EQ(options.rootFields.size(), 0u);
    EXPECT_TRUE(hasClassRoot(options, "p/Pub"));
    EXPECT_TRUE(hasClassRoot(options, "p/Hidden"));
    // 入根的是声明名，不是模式文本 `p/**`。
    for (std::string_view c : options.rootClasses) {
        EXPECT_NE(c, "p/**");
    }
}

// member 规则：按三段匹配落到方法根与字段根。
TEST(MjKeepRootsTest, memberRuleRootsMatchingDeclarations)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    const KeepRuleSet rules =
        parseRules("member p/Pub#privMethod()V\nmember p/Pub#privField\n");
    KeepRootSpec spec;
    spec.rules = &rules;

    GlobalReachabilityOptions options;
    mjbase::reference::applyKeepRoots(spec, index, options);

    EXPECT_TRUE(hasMethodRoot(options, "p/Pub", "privMethod", "()V"));
    EXPECT_TRUE(hasFieldRoot(options, "p/Pub", "privField", "I"));
    // 未命中的同档成员不入根（防过宽）。
    EXPECT_FALSE(hasMethodRoot(options, "p/Pub", "privMethod", "(I)V"));
    EXPECT_FALSE(hasMethodRoot(options, "p/Hidden", "hiddenPubMethod", "()V"));
    EXPECT_EQ(options.rootClasses.size(), 0u);   // 成员规则不反推类根
}

// ── 档位 ──────────────────────────────────────────────────────

// Public 档：public 类 + public 成员入根；protected/包私有/私有不入根。
TEST(MjKeepRootsTest, publicTierRootsOnlyPublicSurface)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    KeepRootSpec spec;
    spec.tier = KeepTier::Public;

    GlobalReachabilityOptions options;
    mjbase::reference::applyKeepRoots(spec, index, options);

    EXPECT_TRUE(hasClassRoot(options, "p/Pub"));
    EXPECT_FALSE(hasClassRoot(options, "p/Hidden"));   // 包私有类不可见

    EXPECT_TRUE(hasMethodRoot(options, "p/Pub", "pubMethod", "()V"));
    EXPECT_FALSE(hasMethodRoot(options, "p/Pub", "protMethod", "()V"));
    EXPECT_FALSE(hasMethodRoot(options, "p/Pub", "pkgMethod", "()V"));
    EXPECT_FALSE(hasMethodRoot(options, "p/Pub", "privMethod", "()V"));

    EXPECT_TRUE(hasFieldRoot(options, "p/Pub", "pubField", "I"));
    EXPECT_FALSE(hasFieldRoot(options, "p/Pub", "protField", "I"));
    EXPECT_FALSE(hasFieldRoot(options, "p/Pub", "pkgField", "I"));
    EXPECT_FALSE(hasFieldRoot(options, "p/Pub", "privField", "I"));

    // 宿主门槛：包私有类的 public 成员不构成对外面，不入档位根（否则成员根
    // 会经 enqueueClass(owner) 把整个内部类拉回闭包，档位退化成"全保"）。
    EXPECT_FALSE(hasMethodRoot(options, "p/Hidden", "hiddenPubMethod", "()V"));
    EXPECT_FALSE(hasFieldRoot(options, "p/Hidden", "hiddenPubField", "I"));
}

// PublicProtected 档：protected 也入根；包私有与私有仍不入根。
TEST(MjKeepRootsTest, publicProtectedTierRootsProtectedToo)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    KeepRootSpec spec;
    spec.tier = KeepTier::PublicProtected;

    GlobalReachabilityOptions options;
    mjbase::reference::applyKeepRoots(spec, index, options);

    EXPECT_TRUE(hasMethodRoot(options, "p/Pub", "pubMethod", "()V"));
    EXPECT_TRUE(hasMethodRoot(options, "p/Pub", "protMethod", "()V"));
    EXPECT_FALSE(hasMethodRoot(options, "p/Pub", "pkgMethod", "()V"));
    EXPECT_FALSE(hasMethodRoot(options, "p/Pub", "privMethod", "()V"));

    EXPECT_TRUE(hasFieldRoot(options, "p/Pub", "pubField", "I"));
    EXPECT_TRUE(hasFieldRoot(options, "p/Pub", "protField", "I"));
    EXPECT_FALSE(hasFieldRoot(options, "p/Pub", "pkgField", "I"));
    EXPECT_FALSE(hasFieldRoot(options, "p/Pub", "privField", "I"));

    EXPECT_FALSE(hasMethodRoot(options, "p/Hidden", "hiddenPubMethod", "()V"));
    EXPECT_FALSE(hasFieldRoot(options, "p/Hidden", "hiddenPubField", "I"));
}

/// 带单个 public 方法的命名类（预设用例的原料）。
Bytes buildNamedClass(std::string_view name, std::string_view methodName)
{
    ClassBytesBuilder b(name);
    b.setAccess(kClassPublicSuper);
    std::vector<MethodSpec> ms;
    ms.push_back(method(methodName, "()V", kPublic));
    return b.assemble(ms);
}

// 宿主门槛只管档位，不管规则：显式 member 规则点名的成员（含包私有类里的）
// 必须照常入根——否则规则面对内部类形同虚设。
TEST(MjKeepRootsTest, memberRuleOverridesTierOwnerGate)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    const KeepRuleSet rules = parseRules("member p/Hidden#hiddenPubMethod()V\n");
    KeepRootSpec      spec;
    spec.rules = &rules;
    spec.tier  = KeepTier::PublicProtected;

    GlobalReachabilityOptions options;
    mjbase::reference::applyKeepRoots(spec, index, options);

    EXPECT_TRUE(hasMethodRoot(options, "p/Hidden", "hiddenPubMethod", "()V"));
    EXPECT_TRUE(hasMethodRoot(options, "p/Pub", "pubMethod", "()V"));   // 档位照常
}

// ── 预设 → 三槽（P0-2 接 P0-1 的匹配面）───────────────────────────

// 预设不是另一条通路：它展开成规则后与手写 class/member 行走同一个匹配面。
// 本用例钉死「preset 文本 → KeepRuleSet → 三槽根 → 可达性闭包」的整链。
TEST(MjKeepRootsTest, presetExpansionsReachRootsThroughTheRuleSurface)
{
    ResourceContainer container;
    addClassBytes(container, "p/Svc_Stub", buildNamedClass("p/Svc_Stub", "invoke"));
    addClassBytes(container, "p/Bean", buildNamedClass("p/Bean", "ejbCreate"));
    addClassBytes(container, "p/Plain", buildNamedClass("p/Plain", "run"));
    ReferenceIndex index;
    index.build(container);

    const KeepRuleSet rules = parseRules("preset +rmi\npreset +ejb\n");
    KeepRootSpec      spec;
    spec.rules = &rules;

    GlobalReachabilityOptions options;
    options.includeMainMethods = false;
    mjbase::reference::applyKeepRoots(spec, index, options);

    EXPECT_TRUE(hasClassRoot(options, "p/Svc_Stub"));   // +rmi 的类名模式
    EXPECT_FALSE(hasClassRoot(options, "p/Plain"));
    EXPECT_TRUE(hasMethodRoot(options, "p/Bean", "ejbCreate", "()V"));   // +ejb 回调名
    EXPECT_FALSE(hasMethodRoot(options, "p/Plain", "run", "()V"));

    // 闭环：p/Plain 无根可依 → 不可达；被预设点名的两类可达。
    mjbase::asm_classpath::CallGraphBuilder builder;
    for (const auto& [name, bytes] :
         {std::pair<std::string, Bytes>{"p/Svc_Stub", buildNamedClass("p/Svc_Stub", "invoke")},
          std::pair<std::string, Bytes>{"p/Bean", buildNamedClass("p/Bean", "ejbCreate")},
          std::pair<std::string, Bytes>{"p/Plain", buildNamedClass("p/Plain", "run")}}) {
        (void)name;
        ASSERT_TRUE(builder.addClassBytes(jbc::core::ByteView(bytes)).is_ok());
    }
    auto graphResult = builder.build();
    ASSERT_TRUE(graphResult.is_ok());
    mjbase::asm_classpath::CallGraph callGraph = std::move(graphResult).unwrap();

    GlobalReachability reachability;
    reachability.build(index, callGraph, options);

    EXPECT_TRUE(reachability.isClassReachable("p/Svc_Stub"));
    EXPECT_TRUE(reachability.isMethodReachable("p/Bean", "ejbCreate", "()V"));
    EXPECT_FALSE(reachability.isClassReachable("p/Plain"));
    EXPECT_FALSE(reachability.isMethodReachable("p/Plain", "run", "()V"));
}

// 规则与档位并集：只扩大保留集（各自命中的成员都在）。
TEST(MjKeepRootsTest, rulesAndTierUnionIsAdditive)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    const KeepRuleSet rules = parseRules("member p/Pub#privMethod()V\n");
    KeepRootSpec      spec;
    spec.rules = &rules;
    spec.tier  = KeepTier::Public;

    GlobalReachabilityOptions options;
    mjbase::reference::applyKeepRoots(spec, index, options);

    EXPECT_TRUE(hasMethodRoot(options, "p/Pub", "pubMethod", "()V"));    // 档位
    EXPECT_TRUE(hasMethodRoot(options, "p/Pub", "privMethod", "()V"));   // 规则
    EXPECT_TRUE(hasFieldRoot(options, "p/Pub", "pubField", "I"));        // 档位
    EXPECT_FALSE(hasFieldRoot(options, "p/Pub", "privField", "I"));      // 两者皆未命中
}

// ── 不变式 ────────────────────────────────────────────────────

// 既有根保留并追加（纯增量：不清空调用方已填的入口）。
TEST(MjKeepRootsTest, existingRootsPreservedAndAppended)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    // 借用索引池内的声明文本模拟"调用方既有根"（资源 keep 同款来源）。
    GlobalReachabilityOptions options;
    ASSERT_FALSE(index.classDeclarations().empty());
    options.rootClasses.emplace_back(index.classDeclarations().front().name);
    ASSERT_FALSE(index.methodDeclarations().empty());
    options.rootMethods.push_back(index.methodDeclarations().front().method);
    const ca::usize classBefore  = options.rootClasses.size();
    const ca::usize methodBefore = options.rootMethods.size();

    KeepRootSpec spec;
    spec.tier = KeepTier::PublicProtected;
    mjbase::reference::applyKeepRoots(spec, index, options);

    ASSERT_GE(options.rootClasses.size(), classBefore);
    ASSERT_GE(options.rootMethods.size(), methodBefore);
    EXPECT_EQ(options.rootClasses.front(), index.classDeclarations().front().name);
    EXPECT_EQ(options.rootMethods.front().owner, index.methodDeclarations().front().method.owner);
}

// 空规格 = 恒等变换（消费方无需分支判断）。
TEST(MjKeepRootsTest, emptySpecLeavesSlotsUntouched)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    GlobalReachabilityOptions options;
    KeepRootSpec              spec;   // rules=nullptr, tier=None
    mjbase::reference::applyKeepRoots(spec, index, options);

    EXPECT_TRUE(options.rootClasses.empty());
    EXPECT_TRUE(options.rootMethods.empty());
    EXPECT_TRUE(options.rootFields.empty());
}

// 生命周期契约：写入的视图必须指向索引池内声明文本（同指针），不是临时串。
TEST(MjKeepRootsTest, rootViewsPointAtIndexPoolText)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    KeepRootSpec spec;
    spec.tier = KeepTier::PublicProtected;

    GlobalReachabilityOptions options;
    mjbase::reference::applyKeepRoots(spec, index, options);
    ASSERT_FALSE(options.rootClasses.empty());

    for (std::string_view root : options.rootClasses) {
        EXPECT_FALSE(root.empty());
        bool pointerMatched = false;
        for (const auto& decl : index.classDeclarations()) {
            if (decl.name == root && decl.name.data() == root.data()) pointerMatched = true;
        }
        EXPECT_TRUE(pointerMatched) << "根视图未指向索引池文本（悬垂风险）: " << root;
    }
    for (const MethodRefKey& root : options.rootMethods) {
        bool pointerMatched = false;
        for (const auto& decl : index.methodDeclarations()) {
            if (decl.method.owner == root.owner && decl.method.owner.data() == root.owner.data()) {
                pointerMatched = true;
            }
        }
        EXPECT_TRUE(pointerMatched) << "方法根 owner 未指向索引池文本: " << root.owner;
    }
}

// ── 闭环集成 ──────────────────────────────────────────────────

// 三槽根必须真进可达性闭包——而不只是填了 vector：档位下 public/protected
// 成员、`p/Hidden` 的 public 成员（类不可见）均不可达。
TEST(MjKeepRootsTest, rootsReachTheClosureNotJustTheVectors)
{
    ResourceContainer container = buildTierFixture();
    ReferenceIndex    index;
    index.build(container);

    mjbase::asm_classpath::CallGraphBuilder builder;
    for (const auto& [name, bytes] : tierFixtureClasses()) {
        (void)name;
        const auto added = builder.addClassBytes(jbc::core::ByteView(bytes));
        ASSERT_TRUE(added.is_ok());
    }
    auto graphResult = builder.build();
    ASSERT_TRUE(graphResult.is_ok());
    mjbase::asm_classpath::CallGraph callGraph = std::move(graphResult).unwrap();

    KeepRootSpec spec;
    spec.tier = KeepTier::PublicProtected;

    GlobalReachabilityOptions options;
    options.includeMainMethods = false;   // 隔离：只由 keep 根驱动闭包
    mjbase::reference::applyKeepRoots(spec, index, options);

    GlobalReachability reachability;
    reachability.build(index, callGraph, options);

    EXPECT_TRUE(reachability.isClassReachable("p/Pub"));
    EXPECT_FALSE(reachability.isClassReachable("p/Hidden"));
    EXPECT_TRUE(reachability.isMethodReachable("p/Pub", "pubMethod", "()V"));
    EXPECT_TRUE(reachability.isMethodReachable("p/Pub", "protMethod", "()V"));
    EXPECT_FALSE(reachability.isMethodReachable("p/Pub", "privMethod", "()V"));
    EXPECT_FALSE(reachability.isMethodReachable("p/Hidden", "hiddenPubMethod", "()V"));
    EXPECT_TRUE(reachability.isFieldReachable("p/Pub", "pubField", "I"));
    EXPECT_FALSE(reachability.isFieldReachable("p/Pub", "privField", "I"));
}

}   // namespace
