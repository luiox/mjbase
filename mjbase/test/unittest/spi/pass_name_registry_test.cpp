/// pass 名字注册面用例：注册/查名/未知名/重复注册/scope 标签/typed create
/// 的 scope 不符；默认表三个试点的 scope 与「经注册表构造并跑通试点路径」。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/access_modifier_tightener.hpp"
#include "mjbase/pass/class_member_sorter.hpp"
#include "mjbase/pass/illegal_class_file_cleaner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

/// 空转类级 pass（工厂测试靶：不改 IR，返回 all()）。
class NoopClassPass : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&,
                                       mjbase::spi::ClassAnalysisManager&) override
    {
        return mjbase::spi::PreservedAnalyses::all();
    }
};

constexpr ca::u16 kAccPrivate = 0x0002;

/// 自类 GETSTATIC/INVOKESTATIC 引用自己 public static 成员的单类容器
///（收紧器的最小触发形态：普查命中 + 无异类引用 → 收紧 private）。
mjbase::model::ResourceContainer makeSelfRefContainer()
{
    FixtureBuilder builder("Owner");
    builder.addField({"internalField", "I"});   // 缺省 access = public static
    FixtureMethod self;
    self.name     = "self";
    self.desc     = "()V";
    self.maxStack = 1;
    self.maxLocals = 0;
    // GETSTATIC Owner.internalField:I; POP; RETURN
    Bytes code = getStaticPop(builder.fieldRef("Owner", "internalField", "I"));
    code.push_back(0x57);   // POP
    code.push_back(0xb1);   // RETURN
    self.code = std::move(code);
    builder.addMethod(self);

    mjbase::model::ResourceContainer container;
    addClassResource(container, "Owner.class", builder.assemble());
    return container;
}

/// 容器内按 uri 取重读后的 ClassFile（测试进程内静态 keeper 持有模型）。
const jbc::core::ClassFile& rereadContainerClass(mjbase::model::ResourceContainer& container,
                                                 const char* uri)
{
    static std::unique_ptr<jbc::core::ClassFile> keeper;
    static const jbc::core::ClassFile            dummy;   // 读失败兜底，字段/方法为空
    auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr);
    auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource);
    EXPECT_NE(cls, nullptr);
    if (cls == nullptr) {
        keeper = nullptr;
        return dummy;
    }
    auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
    EXPECT_TRUE(read.is_ok());
    keeper = read.is_ok() ? std::move(read).unwrap().file : nullptr;
    return keeper != nullptr ? *keeper : dummy;
}

}   // namespace

// 注册 + 查名 + scope 标签 + has/list 的基本闭环。
TEST(PassNameRegistryTest, RegisterFindScopeTagRoundtrip)
{
    using mjbase::spi::PassScope;
    mjbase::spi::PassNameRegistry registry;
    auto                          empty = registry.list();
    EXPECT_TRUE(empty.empty());

    ASSERT_TRUE(registry.registerMethodPass("M", [] { return nullptr; }).is_ok());
    ASSERT_TRUE(registry.registerClassPass("C", [] { return nullptr; }).is_ok());
    ASSERT_TRUE(registry.registerGlobalPass("A", [] { return nullptr; }).is_ok());
    ASSERT_TRUE(registry.registerGlobalPass("B", [] { return nullptr; }).is_ok());
    ASSERT_TRUE(registry.registerCfClassPass("F", [] { return nullptr; }).is_ok());

    EXPECT_TRUE(registry.has("M"));
    EXPECT_FALSE(registry.has("Nope"));

    auto m = registry.find("M");
    ASSERT_TRUE(m.is_ok());
    EXPECT_EQ(m.unwrap().scope, PassScope::Method);
    auto c = registry.find("C");
    ASSERT_TRUE(c.is_ok());
    EXPECT_EQ(c.unwrap().scope, PassScope::Class);
    auto f = registry.find("F");
    ASSERT_TRUE(f.is_ok());
    EXPECT_EQ(f.unwrap().scope, PassScope::Cf);

    // list 按 name 字典序稳定输出（CLI list 纪律）。
    const auto listed = registry.list();
    ASSERT_EQ(listed.size(), 5u);
    EXPECT_TRUE(std::is_sorted(listed.begin(), listed.end(),
                               [](const mjbase::spi::PassNameEntry& lhs,
                                  const mjbase::spi::PassNameEntry& rhs) {
                                   return lhs.name < rhs.name;
                               }));
    EXPECT_EQ(listed.front().name, std::string_view("A"));
    EXPECT_EQ(listed.back().name, std::string_view("M"));
}

// 未知名：find 与 create 族一律 Err(UnknownPass)，不交哨兵空指针。
TEST(PassNameRegistryTest, UnknownNameIsErr)
{
    using mjbase::spi::RegistryError;
    mjbase::spi::PassNameRegistry registry;

    auto found = registry.find("Nope");
    ASSERT_TRUE(found.is_err());
    EXPECT_EQ(found.unwrap_err(), RegistryError::UnknownPass);
    EXPECT_EQ(mjbase::spi::to_string(RegistryError::UnknownPass),
              std::string_view("UnknownPass"));

    auto methodMiss = registry.createMethodPass("Nope");
    ASSERT_TRUE(methodMiss.is_err());
    EXPECT_EQ(methodMiss.unwrap_err(), RegistryError::UnknownPass);
    auto classMiss = registry.createClassPass("Nope");
    ASSERT_TRUE(classMiss.is_err());
    auto globalMiss = registry.createGlobalPass("Nope");
    ASSERT_TRUE(globalMiss.is_err());
    auto cfClassMiss = registry.createCfClassPass("Nope");
    ASSERT_TRUE(cfClassMiss.is_err());
}

// 重复注册显式拒绝（同 scope 与跨 scope 撞名都不覆盖），原条目保持可用。
TEST(PassNameRegistryTest, DuplicateNameRejectedNotOverwritten)
{
    using mjbase::spi::PassScope;
    using mjbase::spi::RegistryError;
    mjbase::spi::PassNameRegistry registry;
    ASSERT_TRUE(
        registry.registerClassPass("X", [] { return std::make_unique<NoopClassPass>(); })
            .is_ok());

    auto sameScope = registry.registerClassPass("X", [] { return nullptr; });
    auto crossScope = registry.registerMethodPass("X", [] { return nullptr; });
    ASSERT_TRUE(sameScope.is_err());
    EXPECT_EQ(sameScope.unwrap_err(), RegistryError::DuplicateName);
    ASSERT_TRUE(crossScope.is_err());
    EXPECT_EQ(crossScope.unwrap_err(), RegistryError::DuplicateName);

    // 原条目未被覆盖：scope 仍是 Class，typed create 照常出 pass。
    auto entry = registry.find("X");
    ASSERT_TRUE(entry.is_ok());
    EXPECT_EQ(entry.unwrap().scope, PassScope::Class);
    auto created = registry.createClassPass("X");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}

// typed create 与注册 scope 不符：Err(ScopeMismatch)，不跨槽强转。
TEST(PassNameRegistryTest, TypedCreateRejectsScopeMismatch)
{
    using mjbase::spi::RegistryError;
    mjbase::spi::PassNameRegistry registry;
    ASSERT_TRUE(registry.registerClassPass("X", [] { return nullptr; }).is_ok());

    auto asMethod = registry.createMethodPass("X");
    auto asGlobal = registry.createGlobalPass("X");
    auto asCf     = registry.createCfClassPass("X");
    ASSERT_TRUE(asMethod.is_err());
    EXPECT_EQ(asMethod.unwrap_err(), RegistryError::ScopeMismatch);
    ASSERT_TRUE(asGlobal.is_err());
    EXPECT_EQ(asGlobal.unwrap_err(), RegistryError::ScopeMismatch);
    ASSERT_TRUE(asCf.is_err());
    EXPECT_EQ(asCf.unwrap_err(), RegistryError::ScopeMismatch);
}

// 默认表：三个结构级 pass 试点的种子与 scope 标签。
TEST(PassNameRegistryTest, DefaultRegistrySeedsThreePilots)
{
    using mjbase::spi::PassScope;
    mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();

    auto sorter = reg.find("ClassMemberSorter");
    ASSERT_TRUE(sorter.is_ok());
    EXPECT_EQ(sorter.unwrap().scope, PassScope::Class);
    auto cleaner = reg.find("IllegalClassFileCleaner");
    ASSERT_TRUE(cleaner.is_ok());
    EXPECT_EQ(cleaner.unwrap().scope, PassScope::Global);
    auto tightener = reg.find("AccessModifierTightener");
    ASSERT_TRUE(tightener.is_ok());
    EXPECT_EQ(tightener.unwrap().scope, PassScope::Global);
    EXPECT_FALSE(reg.has("NotARegisteredPass"));
}

// 试点经注册表构造并跑通既有试点用例路径（sorter / cleaner / tightener）。
TEST(PassNameRegistryTest, PilotsConstructibleViaRegistryAndRun)
{
    using mjbase::spi::PassScope;
    mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();

    // sorter：类级，b/a 两字段排序为 a/b（对齐 sorter 直接用例的断言口径）。
    FixtureBuilder builder("T");
    builder.addField({"b", "I", 0x0000});
    builder.addField({"a", "I", 0x0000});
    builder.addMethod({"t", "()V", bareReturn()});
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(builder.assemble()));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    auto sorterEntry = reg.find("ClassMemberSorter");
    ASSERT_TRUE(sorterEntry.is_ok());
    EXPECT_EQ(sorterEntry.unwrap().scope, PassScope::Class);
    auto sorter = reg.createClassPass("ClassMemberSorter");
    ASSERT_TRUE(sorter.is_ok());
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    const auto                        pa = std::move(sorter).unwrap()->run(*session, cam);
    EXPECT_TRUE(pa.changed());
    ASSERT_EQ(session->unit().fields.size(), 2u);
    const jbc::ir::ClassUnit& unit = session->unit();
    EXPECT_EQ(unit.symbols().str(unit.fields[0].name), "a");
    EXPECT_EQ(unit.symbols().str(unit.fields[1].name), "b");

    // cleaner：容器级，非法描述符成员剔除、合法保留（对齐 cleaner 直接用例）。
    FixtureBuilder bad("B");
    bad.addField({"goodField", "I"});
    bad.addField({"badField", "HI"});   // 'H' 非字段类型起始
    bad.addMethod({"goodMethod", "()V", bareReturn()});
    mjbase::model::ResourceContainer container;
    addClassResource(container, "B.class", bad.assemble());
    auto cleaner = reg.createGlobalPass("IllegalClassFileCleaner");
    ASSERT_TRUE(cleaner.is_ok());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto                         cleanPa = std::move(cleaner).unwrap()->run(container, gam);
    EXPECT_TRUE(cleanPa.changed());
    const jbc::core::ClassFile& cleaned = rereadContainerClass(container, "B.class");
    ASSERT_EQ(cleaned.fields.size(), 1u);
    EXPECT_EQ(cleaned.nameOf(cleaned.fields[0]), "goodField");
    ASSERT_EQ(cleaned.methods.size(), 1u);
    EXPECT_EQ(cleaned.nameOf(cleaned.methods[0]), "goodMethod");

    // tightener：容器级，仅自类引用的 public static 字段收紧 private。
    mjbase::model::ResourceContainer selfRef = makeSelfRefContainer();
    auto tightener = reg.createGlobalPass("AccessModifierTightener");
    ASSERT_TRUE(tightener.is_ok());
    const auto tightenPa = std::move(tightener).unwrap()->run(selfRef, gam);
    EXPECT_TRUE(tightenPa.changed());
    const jbc::core::ClassFile& owner = rereadContainerClass(selfRef, "Owner.class");
    ASSERT_EQ(owner.fields.size(), 1u);
    EXPECT_NE(owner.fields[0].access & kAccPrivate, 0);
}
