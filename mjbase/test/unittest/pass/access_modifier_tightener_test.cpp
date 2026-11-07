/// AccessModifierTightenerTest 段（451 行起），并补齐
/// 独立直接用例。收紧规则矩阵：仅「普查命中 + 无异类引用 + static + public」
/// 降 private；未引用/异类引用/非 static/非 public 均不动。
///
/// 夹具纪律：类骨架一律 jasm 文本装配（JvmJasmService），指令体片段
/// R"JASM 定界。
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
#include "shared/jasm_fixture.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublic  = 0x0001;
constexpr ca::u16 kAccPrivate = 0x0002;
constexpr ca::u16 kAccStatic  = 0x0008;

/// buildTightenerOwner：internal/external/unused 三字段 +
/// internal/external 两方法 + 引用 internal 成员的 self() 方法。
/// jasm 文本装配（JvmJasmService；装配器自带 frame 分析），指令体片段
/// R"JASM 定界。
Bytes buildTightenerOwner()
{
    return assembleJasm(R"JASM(.super java/lang/Object
.class public super Owner {
  .field public static internalField I
  .field public static externalField I
  .field public static unusedField I
  .method public static internalMethod ()V {
    code: {
      return
    }
  }
  .method public static externalMethod ()V {
    code: {
      return
    }
  }
  .method public static self ()V {
    code: {
      getstatic Owner.internalField I
      pop
      invokestatic Owner.internalMethod ()V
      return
    }
  }
}
)JASM");
}

/// buildTightenerCaller：异类引用 Owner 的 external 成员。
Bytes buildTightenerCaller()
{
    return assembleJasm(R"JASM(.super java/lang/Object
.class public super Caller {
  .method public static call ()V {
    code: {
      getstatic Owner.externalField I
      pop
      invokestatic Owner.externalMethod ()V
      return
    }
  }
}
)JASM");
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

// 用例：仅本类引用的 public static 成员收紧 private；异类引用与
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
    // public 实例字段（GETSTATIC 语义不含它；用 public static 的引用源验证
    // 普查不覆盖实例引用——这里直接考察规则矩阵本身）+ private static 字段
    // （普查命中也不会动，非 public）+ public static 方法无引用（对照组）。
    const Bytes bytes = assembleJasm(R"JASM(.super java/lang/Object
.class public super Owner {
  .field public instanceField I
  .field private static privateField I
  .method public static untouched ()V {
    code: {
      return
    }
  }
  .method public static user ()V {
    code: {
      getstatic Owner.privateField I
      pop
      return
    }
  }
}
)JASM");

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
