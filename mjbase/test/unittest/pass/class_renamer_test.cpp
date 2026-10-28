/// ClassRenamerTest 段，
/// 并补齐独立直接用例（空容器 / 不可解析资源 / 命名确定性）。
/// 断言面：URI 重写（旧条目删除、新条目入容器）、描述符与调用点同步改写、
/// 编号确定性（旧名序 → Class0/Class1/...）。
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "asm/remapper/class_bytes_builder.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/class_renamer.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublic  = 0x0001;
constexpr ca::u16 kAccStatic  = 0x0008;

/// buildRenameTarget：pkg/Target，id(Lpkg/Target;)Lpkg/Target;
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

/// buildRenameCaller：pkg/Caller，call(Lpkg/Target;)Lpkg/Target;
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

/// 属性槽按名字定位（比较名字字节；fresh 解析的 cf 直接可读）。
const jbc::core::AttrSlot* slotOf(const jbc::core::ClassFile&                        cf,
                                  const std::vector<jbc::core::AttrSlot>& attrs,
                                  std::string_view                        name)
{
    for (const jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

}   // namespace

// 用例：类重命名 + 描述符与调用点同步改写 + URI 重写。
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

// ── Multi-Release 族契约 ────────────────────────────────────────────────────
//
// 旧行为（已证实的破坏缺陷，本组用例钉死修复后契约）：装载序只按内名排序，
// base 与 versions/N 变体内名相同并列；命名计划按内名键控，族内第二次发名
// 覆盖第一次；写回一律 `新名.class`——族内两份写回同一 URI，旧 URI 双删后一
// 次 replaceResource 胜出，多版本对坍缩成单条目、另一份静默丢失。

/// app/Main：call(Lapp/Api;)V 内 INVOKESTATIC app/Api.touch()V（族引用改写面）。
Bytes buildApiCaller()
{
    FixtureBuilder builder("app/Main");
    const ca::u16  mref = builder.methodRef("app/Api", "touch", "()V");
    FixtureMethod  call;
    call.name      = "call";
    call.desc      = "()V";
    call.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    call.maxStack  = 0;
    call.maxLocals = 0;
    call.code      = invokeStatic(mref);
    call.code.push_back(0xB1);
    builder.addMethod(call);
    return builder.assemble();
}

/// app/Api 族成员（base 与 versions/N 变体同内名同字节形态即可）。
Bytes buildApiMember()
{
    FixtureBuilder builder("app/Api");
    FixtureMethod  touch;
    touch.name      = "touch";
    touch.desc      = "()V";
    touch.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    touch.maxStack  = 0;
    touch.maxLocals = 0;
    touch.code      = bareReturn();
    builder.addMethod(touch);
    return builder.assemble();
}

/// app/Util：改名目标（编号占位断言用）。
Bytes buildUtilClass()
{
    FixtureBuilder builder("app/Util");
    FixtureMethod  t;
    t.name      = "t";
    t.desc      = "()V";
    t.maxLocals = 0;
    t.code      = bareReturn();
    builder.addMethod(t);
    return builder.assemble();
}

TEST(MjPassClassRenamerTest, RenamesMultiReleaseFamilyWithoutCollapsing)
{
    // 排序键 (内名, URI)：app/Api 族（URI 序 versions/11 < versions/17 <
    // app/Api.class，'M' < 'a'）→ 共享 app/Class0；app/Main → app/Class1；
    // app/Util → app/Class2（族只消耗一个编号）。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "app/Api.class", buildApiMember());
    addClassResource(container, "META-INF/versions/17/app/Api.class", buildApiMember());
    addClassResource(container, "META-INF/versions/11/app/Api.class", buildApiMember());
    addClassResource(container, "app/Main.class", buildApiCaller());
    addClassResource(container, "app/Util.class", buildUtilClass());

    mjbase::pass::ClassRenamer         pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto                         pa = pass.run(container, gam);
    EXPECT_FALSE(pa.areAllPreserved());

    // 旧 URI 全部消失，5 条新 URI 各就各位（无坍缩、无静默丢失）。
    EXPECT_FALSE(container.hasResource("app/Api.class"));
    EXPECT_FALSE(container.hasResource("META-INF/versions/11/app/Api.class"));
    EXPECT_FALSE(container.hasResource("META-INF/versions/17/app/Api.class"));
    EXPECT_FALSE(container.hasResource("app/Main.class"));
    EXPECT_FALSE(container.hasResource("app/Util.class"));
    EXPECT_TRUE(container.hasResource("app/Class0.class"));
    EXPECT_TRUE(container.hasResource("META-INF/versions/11/app/Class0.class"));
    EXPECT_TRUE(container.hasResource("META-INF/versions/17/app/Class0.class"));
    EXPECT_TRUE(container.hasResource("app/Class1.class"));
    EXPECT_TRUE(container.hasResource("app/Class2.class"));
    EXPECT_EQ(container.size(), 5u);

    // 族成员 thisName 一致（新名共享），base 与变体只差条目路径。
    // readClassResource 的 keeper 每次读取即失效，逐条读后立即断言。
    EXPECT_EQ(readClassResource(container, "app/Class0.class").thisName(), "app/Class0");
    EXPECT_EQ(readClassResource(container, "META-INF/versions/11/app/Class0.class").thisName(),
              "app/Class0");
    const jbc::core::ClassFile& variant = readClassResource(
        container, "META-INF/versions/17/app/Class0.class");
    EXPECT_EQ(variant.thisName(), "app/Class0");

    // base 引用面：Main 的调用点 owner 同步指向新名。
    const jbc::core::ClassFile& main = readClassResource(container, "app/Class1.class");
    const auto* call = findMethod(main, "call");
    ASSERT_NE(call, nullptr);
    {
        auto sessionResult = jbc::ir::makeSessionFromBytes(main.source());
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
            EXPECT_EQ(sem.owner, "app/Class0");
            checked = true;
        }
        EXPECT_TRUE(checked);
    }
}

TEST(MjPassClassRenamerTest, FamilyRenamingIsInsertionOrderIndependent)
{
    // 同一逻辑容器两种插入序 → 输出 URI 集与族新名完全一致（排序键 (内名,
    // URI) 全序，编号不随容器迭代序漂移——旧行为在此非确定）。
    const auto runOnce = [] {
        mjbase::model::ResourceContainer container;
        addClassResource(container, "META-INF/versions/11/app/Api.class", buildApiMember());
        addClassResource(container, "app/Util.class", buildUtilClass());
        addClassResource(container, "app/Api.class", buildApiMember());
        addClassResource(container, "META-INF/versions/17/app/Api.class", buildApiMember());
        mjbase::pass::ClassRenamer         pass;
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        pass.run(container, gam);
        std::set<std::string> uris;
        for (const auto& [uri, resource] : container.getResources()) {
            (void)resource;
            uris.emplace(uri);
        }
        return uris;
    };
    // 插入序 A（见上）与插入序 B（family base 最先）。
    mjbase::model::ResourceContainer other;
    addClassResource(other, "app/Api.class", buildApiMember());
    addClassResource(other, "META-INF/versions/11/app/Api.class", buildApiMember());
    addClassResource(other, "META-INF/versions/17/app/Api.class", buildApiMember());
    addClassResource(other, "app/Util.class", buildUtilClass());
    {
        mjbase::pass::ClassRenamer         pass;
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        pass.run(other, gam);
    }
    std::set<std::string> otherUris;
    for (const auto& [uri, resource] : other.getResources()) {
        (void)resource;
        otherUris.emplace(uri);
    }
    const std::set<std::string> expected = {
        "META-INF/versions/11/app/Class0.class",
        "META-INF/versions/17/app/Class0.class",
        "app/Class0.class",
        "app/Class1.class",
    };
    EXPECT_EQ(runOnce(), expected);
    EXPECT_EQ(otherUris, expected);
}

TEST(MjPassClassRenamerTest, RenamesVariantOnlyFamilyKeepingVersionShape)
{
    // base 缺失而仅存版本化变体的族：照常改名并保持在 versions/<N> 前缀下，
    // 不升格为 base（输出布局变更不属本 pass）。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "META-INF/versions/17/app/Extra.class", buildUtilClass());

    mjbase::pass::ClassRenamer         pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto                         pa = pass.run(container, gam);
    EXPECT_FALSE(pa.areAllPreserved());

    EXPECT_FALSE(container.hasResource("META-INF/versions/17/app/Extra.class"));
    EXPECT_TRUE(container.hasResource("META-INF/versions/17/app/Class0.class"));
    const jbc::core::ClassFile& cf = readClassResource(
        container, "META-INF/versions/17/app/Class0.class");
    EXPECT_EQ(cf.thisName(), "app/Class0");
}

// B8 8.3 收口：泛型 Signature 与 type annotation 的类引用随类映射结构化同步
//（此前 bytecodeOnly 面滞留旧名）。边界钉死：通配符载荷、'.' 段链（对齐映射
// 逐段替换；独立新名整链坍缩为扁平新名）、注解 type_index 与 'c' 元素值全部
// 落新名，非类名字节（类型变量/通配符）原样。
TEST(MjPassClassRenamerTest, RewritesGenericSignatureAndTypeAnnotationReferences)
{
    namespace test = mjbase::asm_remapper::test_support;
    using test::ClassBytesBuilder;

    // 依赖类：Tool（泛型被引用类）、Tool$Inner（'.' 段链后缀段→独立新名）、
    // Mark（注解
    // 类型）。Holder 承载全被改写面。装载序按旧名排序：Holder→pkg/Class0、
    // Mark→pkg/Class1、Tool→pkg/Class2、Tool$Inner→pkg/Class3。
    mjbase::model::ResourceContainer container;
    {
        ClassBytesBuilder tool("pkg/Tool");
        const Bytes       toolBytes = tool.assemble({});
        addClassResource(container, "pkg/Tool.class", toolBytes);
    }
    {
        ClassBytesBuilder inner("pkg/Tool$Inner");
        const Bytes       innerBytes = inner.assemble({});
        addClassResource(container, "pkg/Tool$Inner.class", innerBytes);
    }
    {
        ClassBytesBuilder mark("pkg/Mark");
        addClassResource(container, "pkg/Mark.class", mark.assemble({}));
    }
    {
        ClassBytesBuilder holder("pkg/Holder");
        holder.setClassSignature("Ljava/lang/Object;Lpkg/Tool;");

        // 类级 RVTA：supertype 位 + 'c' 元素值指 Tool。
        test::TypeAnnotationSpec classTa;
        classTa.targetType = 0x10;
        classTa.index16    = 0xFFFF;
        classTa.anno.type  = "Lpkg/Mark;";
        classTa.anno.pairs.push_back({"v", test::ElementValue{'c', "Lpkg/Tool;"}});
        holder.addClassTypeAnnotation(classTa);

        // 字段：通配符上界签名 + '.' 段链签名（后缀段属另一被改名类，改名后独立新名）。
        test::FieldSpec list;
        list.access    = 0x0001;
        list.name      = "list";
        list.desc      = "Ljava/util/List;";
        list.signature = "Ljava/util/List<+Lpkg/Tool;>;";
        holder.addField(list);
        test::FieldSpec inner;
        inner.access    = 0x0001;
        inner.name      = "inner";
        inner.desc      = "Lpkg/Tool;";
        inner.signature = "Lpkg/Tool<Lpkg/Tool;>.Inner;";
        holder.addField(inner);

        // 方法：类型参数界 + 返回位指 Tool。
        test::MethodSpec make;
        make.access    = 0x0009;
        make.name      = "make";
        make.desc      = "()Ljava/lang/Object;";
        make.signature = "<T:Lpkg/Tool;>(TT;)Lpkg/Tool;";
        make.emit      = [](test::CodeEmit& e) {
            e.op(jbc::core::Op::ACONST_NULL);
            e.op(jbc::core::Op::ARETURN);
        };
        addClassResource(container, "pkg/Holder.class", holder.assemble({make}));
    }

    mjbase::pass::ClassRenamer         pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto                         pa = pass.run(container, gam);
    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(container.hasResource("pkg/Class0.class"));
    EXPECT_TRUE(container.hasResource("pkg/Class1.class"));
    EXPECT_TRUE(container.hasResource("pkg/Class2.class"));
    EXPECT_TRUE(container.hasResource("pkg/Class3.class"));

    jbc::core::ClassFile& holder = readClassResource(container, "pkg/Class0.class");
    EXPECT_EQ(holder.thisName(), "pkg/Class0");

    // 类签名：父类实现的接口名落新名。
    {
        const jbc::core::AttrSlot* slot =
            slotOf(holder, holder.attrs, "Signature");
        ASSERT_TRUE(slot != nullptr);
        auto d = holder.decodeAttr(const_cast<jbc::core::AttrSlot&>(*slot));
        ASSERT_TRUE(d.is_ok());
        const auto* sig = ca::core::tag_cast::cast<const jbc::core::SignatureAttr>(
            slot->peek());
        ASSERT_TRUE(sig != nullptr);
        EXPECT_EQ("Ljava/lang/Object;Lpkg/Class2;", std::string(holder.utf8(sig->signatureCp)));
    }

    // 字段签名：通配符上界落新名；段链见 inner 断言（坍缩语义）。
    {
        const jbc::core::MemberInfo* f = nullptr;
        for (const jbc::core::MemberInfo& m : holder.fields) {
            if (holder.nameOf(m) == "list") f = &m;
        }
        ASSERT_TRUE(f != nullptr);
        const jbc::core::AttrSlot* slot = slotOf(holder, f->attrs, "Signature");
        ASSERT_TRUE(slot != nullptr);
        auto d = holder.decodeAttr(const_cast<jbc::core::AttrSlot&>(*slot));
        ASSERT_TRUE(d.is_ok());
        const auto* sig =
            ca::core::tag_cast::cast<const jbc::core::SignatureAttr>(slot->peek());
        ASSERT_TRUE(sig != nullptr);
        EXPECT_EQ("Ljava/util/List<+Lpkg/Class2;>;",
                  std::string(holder.utf8(sig->signatureCp)));
    }
    {
        const jbc::core::MemberInfo* f = nullptr;
        for (const jbc::core::MemberInfo& m : holder.fields) {
            if (holder.nameOf(m) == "inner") f = &m;
        }
        ASSERT_TRUE(f != nullptr);
        const jbc::core::AttrSlot* slot = slotOf(holder, f->attrs, "Signature");
        ASSERT_TRUE(slot != nullptr);
        auto d = holder.decodeAttr(const_cast<jbc::core::AttrSlot&>(*slot));
        ASSERT_TRUE(d.is_ok());
        const auto* sig =
            ca::core::tag_cast::cast<const jbc::core::SignatureAttr>(slot->peek());
        ASSERT_TRUE(sig != nullptr);
        // 段链坍缩：pkg/Tool$Inner 改名为扁平顶级类 pkg/Class3，二元名整体
        // 易主——'.' 链（连同链上实参）坍缩为 `Lpkg/Class3;`。
        EXPECT_EQ("Lpkg/Class3;", std::string(holder.utf8(sig->signatureCp)));
    }

    // 方法签名：类型参数界与返回位落新名，类型变量 T 原样。
    {
        const auto* m = findMethod(holder, "make");
        ASSERT_TRUE(m != nullptr);
        const jbc::core::AttrSlot* slot = slotOf(holder, m->attrs, "Signature");
        ASSERT_TRUE(slot != nullptr);
        auto d = holder.decodeAttr(const_cast<jbc::core::AttrSlot&>(*slot));
        ASSERT_TRUE(d.is_ok());
        const auto* sig =
            ca::core::tag_cast::cast<const jbc::core::SignatureAttr>(slot->peek());
        ASSERT_TRUE(sig != nullptr);
        EXPECT_EQ("<T:Lpkg/Class2;>(TT;)Lpkg/Class2;",
                  std::string(holder.utf8(sig->signatureCp)));
    }

    // 类级 RVTA：注解类型（type_index）与 'c' 元素值落新名，target 位保真。
    {
        const jbc::core::AttrSlot* slot =
            slotOf(holder, holder.attrs, "RuntimeVisibleTypeAnnotations");
        ASSERT_TRUE(slot != nullptr);
        auto d = holder.decodeAttr(const_cast<jbc::core::AttrSlot&>(*slot));
        ASSERT_TRUE(d.is_ok());
        const auto* body =
            ca::core::tag_cast::cast<const jbc::core::TypeAnnotationsAttr>(slot->peek());
        ASSERT_TRUE(body != nullptr);
        ASSERT_EQ(1U, body->annotations.size());
        EXPECT_EQ(0x10, body->annotations[0].targetType);
        EXPECT_EQ("Lpkg/Class1;",
                  std::string(holder.utf8(body->annotations[0].typeIndexCp)));
        ASSERT_EQ(1U, body->annotations[0].values.size());
        EXPECT_EQ("Lpkg/Class2;",
                  std::string(holder.utf8(body->annotations[0].values[0].second.cp)));
    }
}
