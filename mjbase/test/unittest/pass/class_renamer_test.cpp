/// 覆盖面（空容器 / 不可解析资源 / 命名确定性）。
/// 断言面：URI 重写（旧条目删除、新条目入容器）、描述符与调用点同步改写、
/// 编号确定性（旧名序 → Class0/Class1/...）。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/class_renamer.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublic  = 0x0001;
constexpr ca::u16 kAccStatic  = 0x0008;

/// pkg/Target：id(Lpkg/Target;)Lpkg/Target;
/// （ALOAD 0; ARETURN 形态，self 描述符含自身类型）。
Bytes buildRenameTarget()
{
    FixtureBuilder builder("pkg/Target");
    FixtureMethod id;
    id.name     = "id";
    id.desc     = "(Lpkg/Target;)Lpkg/Target;";
    id.access   = static_cast<ca::u16>(kAccPublic | kAccStatic);
    id.maxStack = 1;
    id.maxLocals = 0;
    id.code     = {0x2A, 0xB0};   // ALOAD 0; ARETURN
    builder.addMethod(id);
    return builder.assemble();
}

/// pkg/Caller：call(Lpkg/Target;)Lpkg/Target;
/// 内 INVOKESTATIC pkg/Target.id:(Lpkg/Target;)Lpkg/Target;。
Bytes buildRenameCaller()
{
    FixtureBuilder builder("pkg/Caller");
    const ca::u16  mref = builder.methodRef("pkg/Target", "id", "(Lpkg/Target;)Lpkg/Target;");
    FixtureMethod call;
    call.name      = "call";
    call.desc      = "(Lpkg/Target;)Lpkg/Target;";
    call.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    call.maxStack  = 1;
    call.maxLocals = 1;
    // ALOAD 0; INVOKESTATIC Target.id; ARETURN
    Bytes code{0x2A, 0xB8, static_cast<ca::u8>(mref >> 8), static_cast<ca::u8>(mref), 0xB0};
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

const jbc::core::MemberInfo* findMethod(const jbc::core::ClassFile& cf, const char* name)
{
    for (const jbc::core::MemberInfo& m : cf.methods) {
        if (cf.nameOf(m) == name) return &m;
    }
    return nullptr;
}

}   // namespace

// 验证：类重命名 + 描述符与调用点同步改写 + URI 重写。
TEST(MjPassClassRenamerTest, RenamesClassesAndRewritesDescriptorsAndCallSites)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Target.class", buildRenameTarget());
    addClassResource(container, "pkg/Caller.class", buildRenameCaller());

    mjbase::pass::ClassRenamer            pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(pa.changed());
    EXPECT_FALSE(container.hasResource("pkg/Caller.class"));
    EXPECT_FALSE(container.hasResource("pkg/Target.class"));
    EXPECT_TRUE(container.hasResource("pkg/Class0.class"));
    EXPECT_TRUE(container.hasResource("pkg/Class1.class"));

    // 装载序按旧名排序：Caller < Target → Caller→Class0，Target→Class1。
    const jbc::core::ClassFile& caller = readClassResource(container, "pkg/Class0.class");
    EXPECT_EQ(caller.thisName(), "pkg/Class0");
    const auto* call = findMethod(caller, "call");
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(caller.descOf(*call), "(Lpkg/Class1;)Lpkg/Class1;");

    // 调用点 owner/desc 同步指向新名（指令操作数语义面经会话解析）。
    {
        auto sessionResult = jbc::ir::makeSessionFromBytes(caller.source());
        ASSERT_TRUE(sessionResult.is_ok());
        auto session = std::move(sessionResult).unwrap();
        const auto promoted = jbc::ir::promoteMethod(*session, 0);
        ASSERT_TRUE(promoted.is_ok());
        bool checked = false;
        for (const jbc::ir::InsnId insnId : promoted.unwrap()->dom.order()) {
            const jbc::ir::Insn* in = promoted.unwrap()->dom.peek(insnId);
            ASSERT_NE(in, nullptr);
            jbc::ir::ClassSession::MemberSem sem;
            if (!session->memberOf(*in, sem)) continue;
            EXPECT_EQ(sem.owner, "pkg/Class1");
            EXPECT_EQ(sem.name, "id");
            EXPECT_EQ(sem.desc, "(Lpkg/Class1;)Lpkg/Class1;");
            checked = true;
        }
        EXPECT_TRUE(checked);
    }

    const jbc::core::ClassFile& target = readClassResource(container, "pkg/Class1.class");
    EXPECT_EQ(target.thisName(), "pkg/Class1");
    const auto* id = findMethod(target, "id");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(target.descOf(*id), "(Lpkg/Class1;)Lpkg/Class1;");
}

// 补齐直接用例：默认包类（旧名无 '/'）落在 "Class<N>"。
TEST(MjPassClassRenamerTest, RenamesDefaultPackageClassesToRootName)
{
    mjbase::model::ResourceContainer container;
    FixtureBuilder                   builder("Lonely");
    FixtureMethod                    t;
    t.name     = "t";
    t.desc     = "()V";
    t.maxLocals = 0;
    t.code     = bareReturn();
    builder.addMethod(t);
    addClassResource(container, "Lonely.class", builder.assemble());

    mjbase::pass::ClassRenamer         pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto                         pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_FALSE(container.hasResource("Lonely.class"));
    EXPECT_TRUE(container.hasResource("Class0.class"));
    const jbc::core::ClassFile& cf = readClassResource(container, "Class0.class");
    EXPECT_EQ(cf.thisName(), "Class0");
}

// 补齐直接用例：空容器 / 仅不可解析资源 → all()，容器原样。
TEST(MjPassClassRenamerTest, KeepsContainerUntouchedWhenNothingLoadable)
{
    mjbase::model::ResourceContainer container;
    const std::vector<ca::u8>        garbage{0xCA, 0xFE, 0xBA};
    container.addResource("Garbage.class",
                          std::make_unique<mjbase::model::ClassResource>("Garbage.class",
                                                                         garbage));

    mjbase::pass::ClassRenamer         pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto                         pa = pass.run(container, gam);

    EXPECT_TRUE(pa.areAllPreserved());
    auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource>(
        container.getResource("Garbage.class"));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->get(), garbage);
}
