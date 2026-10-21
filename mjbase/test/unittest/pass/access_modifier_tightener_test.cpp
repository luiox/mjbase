/// AccessModifierTightenerTest 段（451 行起），并按任务补齐旧栈缺失的
/// 独立直接用例。收紧规则矩阵：仅「普查命中 + 无异类引用 + static + public」
/// 降 private；未引用/异类引用/非 static/非 public 均不动。
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/access_modifier_tightener.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublic  = 0x0001;
constexpr ca::u16 kAccPrivate = 0x0002;
constexpr ca::u16 kAccStatic  = 0x0008;

/// 旧栈 buildTightenerOwner 的移植：internal/external/unused 三字段 +
/// internal/external 两方法 + 引用 internal 成员的 self() 方法。
Bytes buildTightenerOwner()
{
    FixtureBuilder builder("Owner");
    builder.addField({"internalField", "I", static_cast<ca::u16>(kAccPublic | kAccStatic)});
    builder.addField({"externalField", "I", static_cast<ca::u16>(kAccPublic | kAccStatic)});
    builder.addField({"unusedField", "I", static_cast<ca::u16>(kAccPublic | kAccStatic)});

    builder.addMethod({"internalMethod", "()V", bareReturn()});
    builder.addMethod({"externalMethod", "()V", bareReturn()});

    FixtureMethod self;
    self.name      = "self";
    self.desc      = "()V";
    self.maxStack  = 1;
    self.maxLocals = 0;
    // GETSTATIC Owner.internalField:I; POP; INVOKESTATIC Owner.internalMethod:()V; RETURN
    Bytes code = getStaticPop(builder.fieldRef("Owner", "internalField", "I"));
    appendBytes(code, invokeStatic(builder.methodRef("Owner", "internalMethod", "()V")));
    code.push_back(0xb1);   // RETURN
    self.code = std::move(code);
    builder.addMethod(self);
    return builder.assemble();
}

/// 旧栈 buildTightenerCaller 的移植：异类引用 Owner 的 external 成员。
Bytes buildTightenerCaller()
{
    FixtureBuilder builder("Caller");
    FixtureMethod  call;
    call.name      = "call";
    call.desc      = "()V";
    call.maxStack  = 1;
    call.maxLocals = 0;
    Bytes code = getStaticPop(builder.fieldRef("Owner", "externalField", "I"));
    appendBytes(code, invokeStatic(builder.methodRef("Owner", "externalMethod", "()V")));
    code.push_back(0xb1);   // RETURN
    call.code = std::move(code);
    builder.addMethod(call);
    return builder.assemble();
}

/// 重读容器内 class 资源（不可解析即断言失败）。模型由静态 keeper 持有，
/// 引用在本用例内有效。
jbc::core::ClassFile& readClassResource(mjbase::model::ResourceContainer& container,
                                        const char* uri)
{
    static std::unique_ptr<jbc::core::ClassFile> keeper;
    auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr);
    auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource);
    EXPECT_NE(cls, nullptr);
    if (cls == nullptr) {
        keeper = nullptr;
    }
    else {
        auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
        EXPECT_TRUE(read.is_ok());
        keeper = read.is_ok() ? std::move(read).unwrap().file : nullptr;
    }
    return *keeper;
}

const jbc::core::MemberInfo* findMember(const jbc::core::ClassFile& cf,
                                        const std::vector<jbc::core::MemberInfo>& members,
                                        const char* name)
{
    for (const jbc::core::MemberInfo& m : members) {
        if (cf.nameOf(m) == name) return &m;
    }
    return nullptr;
}

}   // namespace

// 旧栈用例移植：仅本类引用的 public static 成员收紧 private；异类引用与
// 从未引用的成员保持 public。
TEST(MjPassAccessModifierTightenerTest, TightensOnlyInnerUsedPublicStaticMembers)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Owner.class", buildTightenerOwner());
    addClassResource(container, "Caller.class", buildTightenerCaller());

    mjbase::pass::AccessModifierTightener pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(pa.changed());

    const jbc::core::ClassFile& owner = readClassResource(container, "Owner.class");

    const auto* internalField = findMember(owner, owner.fields, "internalField");
    const auto* externalField = findMember(owner, owner.fields, "externalField");
    const auto* unusedField   = findMember(owner, owner.fields, "unusedField");
    ASSERT_NE(internalField, nullptr);
    ASSERT_NE(externalField, nullptr);
    ASSERT_NE(unusedField, nullptr);
    EXPECT_NE(internalField->access & kAccPrivate, 0);
    EXPECT_NE(externalField->access & kAccPublic, 0);
    EXPECT_NE(unusedField->access & kAccPublic, 0);

    const auto* internalMethod = findMember(owner, owner.methods, "internalMethod");
    const auto* externalMethod = findMember(owner, owner.methods, "externalMethod");
    const auto* selfMethod     = findMember(owner, owner.methods, "self");
    ASSERT_NE(internalMethod, nullptr);
    ASSERT_NE(externalMethod, nullptr);
    ASSERT_NE(selfMethod, nullptr);
    EXPECT_NE(internalMethod->access & kAccPrivate, 0);
    EXPECT_NE(externalMethod->access & kAccPublic, 0);
    // self 自身从未被 INVOKESTATIC 引用（无普查条目）→ 保持 public 不动。
    EXPECT_NE(selfMethod->access & kAccPublic, 0);
}

// 补齐直接用例：非 public / 非 static 的仅本类引用成员不收紧。
TEST(MjPassAccessModifierTightenerTest, KeepsNonPublicAndNonStaticMembersUntouched)
{
    FixtureBuilder builder("Owner");
    // public 实例字段（GETSTATIC 语义不含它；用 public static 的引用源验证
    // 普查不覆盖实例引用——这里直接考察规则矩阵本身）。
    builder.addField({"instanceField", "I", kAccPublic});
    // private static 字段：普查命中也不会动（非 public）。
    builder.addField({"privateField", "I", static_cast<ca::u16>(kAccPrivate | kAccStatic)});
    // public static 方法，但收紧条件之外：无引用（对照组）。
    builder.addMethod({"untouched", "()V", bareReturn()});
    FixtureMethod user;
    user.name     = "user";
    user.desc     = "()V";
    user.maxLocals = 0;
    // GETSTATIC Owner.privateField:I（本类引用 private 字段，命中普查）。
    user.code = getStaticPop(builder.fieldRef("Owner", "privateField", "I"));
    user.code.push_back(0xb1);
    builder.addMethod(user);
    auto bytes = builder.assemble();

    mjbase::model::ResourceContainer container;
    addClassResource(container, "Owner.class", bytes);

    mjbase::pass::AccessModifierTightener pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    // 无任何成员满足收紧矩阵 → 不改动。
    EXPECT_TRUE(pa.areAllPreserved());
    auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource>(
        container.getResource("Owner.class"));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->get(), bytes);
}

// 补齐直接用例：不可解析与空字节资源跳过、原样保留（不抛异常、不影响他类）。
TEST(MjPassAccessModifierTightenerTest, SkipsUnparseableAndEmptyResources)
{
    mjbase::model::ResourceContainer container;
    const std::vector<ca::u8>        garbage{0xCA, 0xFE, 0xBA};
    const std::vector<ca::u8>        empty{};
    container.addResource("Garbage.class",
                          std::make_unique<mjbase::model::ClassResource>("Garbage.class",
                                                                         garbage));
    container.addResource("Empty.class",
                          std::make_unique<mjbase::model::ClassResource>("Empty.class", empty));

    mjbase::pass::AccessModifierTightener pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    EXPECT_TRUE(pa.areAllPreserved());
    auto* garbageRes = ca::core::tag_cast::cast<mjbase::model::ClassResource>(
        container.getResource("Garbage.class"));
    ASSERT_NE(garbageRes, nullptr);
    EXPECT_EQ(garbageRes->get(), garbage);
    auto* emptyRes = ca::core::tag_cast::cast<mjbase::model::ClassResource>(
        container.getResource("Empty.class"));
    ASSERT_NE(emptyRes, nullptr);
    EXPECT_EQ(emptyRes->get(), empty);
}
