/// EmptyClassRemoverTest / UnusedClassRemoverTest / UnusedFieldRemoverTest /
/// GlobalUnusedMethodRemoverTest / GlobalUnusedPassContractTest 段（批 A：
/// 未使用成员清理家族，容器级四件）。夹具经
/// shared/structural_pass_util.hpp 的 FixtureBuilder；注解 / 签名 / Module /
/// ModuleMainClass 等属性面经 libjbc DOM 补槽（AttrSlot::makeNew + RawAttr /
/// AnnotationsAttr）后写回——属性字节受控且不依赖本地语料。
///
/// 与形态差（均已在注释逐用例声明）：
///   - 间接引用保持（MethodHandle / invokedynamic / Class.forName 字符串）的
///     扫描面属 reference 模块自身用例钉死的范围（reference/global_reachability_
///     test.cpp 结构元数据用例 + reference_index_test），本文件只移植类注解 /
///     字段签名两个 DOM 可构造面，间接调用面不在此重复建桩。
///   - LVT/LVTT keep 判定（方法局部变量注解）在 libjbc 同属
///     Runtime*TypeAnnotations 属性（在场即 keep），「局部注解行过滤」
///     等价，由注解 keep 用例覆盖属性族判定路径。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/asm/classpath/resolved_call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/unused_member_remover.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/keep_rule.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublic       = 0x0001;
constexpr ca::u16 kAccPrivate      = 0x0002;
constexpr ca::u16 kAccProtected    = 0x0004;
constexpr ca::u16 kAccStatic       = 0x0008;

// ── 夹具构造 ─────────────────────────────────────────────────────────────

/// buildStructureEmptyClass：public 空类。
Bytes buildEmptyClassBytes(const char* name)
{
    FixtureBuilder builder(name);
    return builder.assemble();
}

/// buildStructureUsedClass：Used.keep()V public static。
Bytes buildKeepClass()
{
    FixtureBuilder builder("Used");
    FixtureMethod keep;
    keep.name      = "keep";
    keep.desc      = "()V";
    keep.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    keep.maxStack  = 0;
    keep.maxLocals = 0;
    keep.code      = bareReturn();
    builder.addMethod(keep);
    return builder.assemble();
}

/// buildStructureAppClass：App.main 调 Used.keep。
Bytes buildAppCallingUsed()
{
    FixtureBuilder builder("App");
    const ca::u16  mref = builder.methodRef("Used", "keep", "()V");
    FixtureMethod  main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 0;
    main.maxLocals = 1;
    main.code      = invokeStatic(mref);
    main.code.push_back(0xB1);   // RETURN
    builder.addMethod(main);
    return builder.assemble();
}

/// buildUnusedClassRemoverFixture：withMain ? App(main→Used.keep)
/// : Library（空类）+ Used + Dead + note.txt。
void addUnusedClassFixture(mjbase::model::ResourceContainer& container, bool withMain)
{
    if (withMain) {
        addClassResource(container, "App.class", buildAppCallingUsed());
    }
    else {
        addClassResource(container, "Library.class", buildEmptyClassBytes("Library"));
    }
    addClassResource(container, "Used.class", buildKeepClass());
    addClassResource(container, "Dead.class", buildEmptyClassBytes("Dead"));
    container.addResource("note.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "note.txt", std::vector<ca::u8>{1, 2, 3}));
}

/// buildMemberCleanupClass：App 上 private live/dead 字段与方法 +
/// public api 面 + main 使用 live 侧（dead 全部不可达）。
Bytes buildMemberCleanupClass()
{
    FixtureBuilder builder("App");
    builder.addField({"livePrivate", "I", kAccPrivate | kAccStatic});
    builder.addField({"deadPrivate", "I", kAccPrivate | kAccStatic});
    builder.addField({"apiField", "I", kAccPublic | kAccStatic});

    FixtureMethod live;
    live.name      = "livePrivateMethod";
    live.desc      = "()V";
    live.access    = static_cast<ca::u16>(kAccPrivate | kAccStatic);
    live.maxStack  = 0;
    live.maxLocals = 0;
    live.code      = bareReturn();
    builder.addMethod(live);

    FixtureMethod dead;
    dead.name      = "deadPrivateMethod";
    dead.desc      = "()V";
    dead.access    = static_cast<ca::u16>(kAccPrivate | kAccStatic);
    dead.maxStack  = 0;
    dead.maxLocals = 0;
    dead.code      = bareReturn();
    builder.addMethod(dead);

    FixtureMethod api;
    api.name      = "apiMethod";
    api.desc      = "()V";
    api.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    api.maxStack  = 0;
    api.maxLocals = 0;
    api.code      = bareReturn();
    builder.addMethod(api);

    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 1;
    main.maxLocals = 1;
    main.code      = getStaticPop(builder.fieldRef("App", "livePrivate", "I"));
    appendBytes(main.code, invokeStatic(builder.methodRef("App", "livePrivateMethod", "()V")));
    main.code.push_back(0xB1);
    builder.addMethod(main);
    return builder.assemble();
}

/// buildVirtualCleanupMethodClass：owner 继承 super，public 实例
/// run()V。
Bytes buildRunnerClass(const char* owner, const char* superName)
{
    FixtureBuilder builder(owner);
    builder.setSuper(superName);
    FixtureMethod run;
    run.name      = "run";
    run.desc      = "()V";
    run.access    = kAccPublic;
    run.maxStack  = 0;
    run.maxLocals = 1;
    run.code      = bareReturn();
    builder.addMethod(run);
    return builder.assemble();
}

/// buildVirtualCleanupAppClass：main 经 INVOKEVIRTUAL Base.run
/// （ACONST_NULL receiver）。
Bytes buildVirtualApp()
{
    FixtureBuilder builder("VirtualCleanupApp");
    const ca::u16  mref = builder.methodRef("VirtualBase", "run", "()V");
    FixtureMethod  main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 1;
    main.maxLocals = 1;
    // ACONST_NULL; INVOKEVIRTUAL Base.run; RETURN
    main.code = {0x01, 0xB6, static_cast<ca::u8>(mref >> 8), static_cast<ca::u8>(mref), 0xB1};
    builder.addMethod(main);
    return builder.assemble();
}

// ── DOM 补槽（注解 / 签名 / Module 属性面）────────────────────────────────

/// putU16 到属性 payload。
void putAttrU16(std::vector<ca::u8>& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

/// 读 classfile（失败即致命断言），交出 DOM 所有权。
std::unique_ptr<jbc::core::ClassFile> loadDom(const Bytes& bytes, const char* what)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(read.is_ok()) << what << ": libjbc readClass failed";
    if (read.is_err()) return nullptr;
    return std::move(read).unwrap().file;
}

/// DOM 写回（失败即致命断言）。
Bytes writeDom(jbc::core::ClassFile& cf, const char* what)
{
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok()) << what << ": libjbc writeClass failed";
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

const jbc::core::MemberInfo* findMember(const jbc::core::ClassFile& cf, bool method,
                                        const char* name);

jbc::core::MemberInfo* findMember(jbc::core::ClassFile& cf, bool method, const char* name)
{
    return const_cast<jbc::core::MemberInfo*>(
        findMember(static_cast<const jbc::core::ClassFile&>(cf), method, name));
}

const jbc::core::MemberInfo* findMember(const jbc::core::ClassFile& cf, bool method,
                                        const char* name)
{
    const std::vector<jbc::core::MemberInfo>& members = method ? cf.methods : cf.fields;
    for (const jbc::core::MemberInfo& m : members) {
        if (cf.nameOf(m) == name) return &m;
    }
    return nullptr;
}

/// 给类 / 字段 / 方法挂一条 RuntimeVisibleAnnotations（单注解无元素）。
Bytes withAnnotation(const Bytes& base, const char* memberName, bool method,
                     std::string_view annoType)
{
    auto cf = loadDom(base, "withAnnotation");
    if (cf == nullptr) return {};
    std::vector<jbc::core::AttrSlot>* attrs = &cf->attrs;
    if (memberName != nullptr) {
        jbc::core::MemberInfo* member = findMember(*cf, method, memberName);
        EXPECT_NE(member, nullptr) << memberName;
        if (member == nullptr) return {};
        attrs = &member->attrs;
    }
    auto anno = std::make_unique<jbc::core::AnnotationsAttr>();
    jbc::core::AnnotationsAttr::Annotation entry;
    entry.typeCp = cf->cp.addUtf8(annoType);
    anno->annos.push_back(std::move(entry));
    attrs->push_back(jbc::core::AttrSlot::makeNew(
        cf->cp.addUtf8("RuntimeVisibleAnnotations"), std::move(anno)));
    return writeDom(*cf, "withAnnotation");
}

/// 给字段挂 Signature（Raw 属性：payload 为 u2 Utf8 索引）。
Bytes withFieldSignature(const Bytes& base, const char* fieldName, std::string_view signature)
{
    auto cf = loadDom(base, "withFieldSignature");
    if (cf == nullptr) return {};
    jbc::core::MemberInfo* field = findMember(*cf, false, fieldName);
    EXPECT_NE(field, nullptr) << fieldName;
    if (field == nullptr) return {};
    const ca::u16      sigCp = cf->cp.addUtf8(signature);
    std::vector<ca::u8> payload;
    putAttrU16(payload, sigCp);
    field->attrs.push_back(jbc::core::AttrSlot::makeNew(cf->cp.addUtf8("Signature"),
                                                        std::make_unique<jbc::core::RawAttr>(
                                                            payload)));
    return writeDom(*cf, "withFieldSignature");
}

/// module-info 语料：Module 属性（uses Service / provides Service→Impl）+
/// ModuleMainClass=example/Main（Raw 字节受控，结构对齐 JVMS 4.7.25）。
Bytes buildModuleInfo()
{
    FixtureBuilder builder("module-info");
    const Bytes    base = builder.assemble();
    auto           cf   = loadDom(base, "module-info");
    if (cf == nullptr) return {};
    const ca::u16 serviceUtf8 = static_cast<ca::u16>(cf->cp.addUtf8("example/Service"));
    const ca::u16 implUtf8    = static_cast<ca::u16>(cf->cp.addUtf8("example/Impl"));
    const ca::u16 mainUtf8    = static_cast<ca::u16>(cf->cp.addUtf8("example/Main"));
    const ca::u16 serviceCp   = static_cast<ca::u16>(cf->cp.addClass(serviceUtf8));
    const ca::u16 implCp      = static_cast<ca::u16>(cf->cp.addClass(implUtf8));
    const ca::u16 mainCp      = static_cast<ca::u16>(cf->cp.addClass(mainUtf8));
    const ca::u16 nameCp      = static_cast<ca::u16>(cf->cp.addUtf8("example.module"));
    const ca::u16 verCp       = static_cast<ca::u16>(cf->cp.addUtf8(""));

    std::vector<ca::u8> module;
    putAttrU16(module, nameCp);    // module_name
    putAttrU16(module, 0);         // module_flags
    putAttrU16(module, verCp);     // module_version
    putAttrU16(module, 0);         // requires_count
    putAttrU16(module, 0);         // exports_count
    putAttrU16(module, 0);         // opens_count
    putAttrU16(module, 1);         // uses_count
    putAttrU16(module, serviceCp);
    putAttrU16(module, 1);         // provides_count
    putAttrU16(module, serviceCp);
    putAttrU16(module, 1);         // provides_with_count
    putAttrU16(module, implCp);
    cf->attrs.push_back(jbc::core::AttrSlot::makeNew(
        cf->cp.addUtf8("Module"), std::make_unique<jbc::core::RawAttr>(module)));

    std::vector<ca::u8> mainClass;
    putAttrU16(mainClass, mainCp);
    cf->attrs.push_back(jbc::core::AttrSlot::makeNew(
        cf->cp.addUtf8("ModuleMainClass"), std::make_unique<jbc::core::RawAttr>(mainClass)));
    return writeDom(*cf, "module-info");
}

// ── 非私有档位 / fail-closed / 悬垂防护夹具（本批新增，新栈策略面）──────────

/// 档位夹具：private live/dead + package-private/protected/public 四档死成员 +
/// main 使用 live 侧（dead 全部无引用；字段面 protected 为实例字段、无 clinit
/// 写入面）。方法与字段共用，两 pass 各删各面。
Bytes buildNonPublicCleanupClass()
{
    FixtureBuilder builder("App");
    builder.addField({"livePrivate", "I", kAccPrivate | kAccStatic});
    builder.addField({"deadPrivateField", "I", kAccPrivate | kAccStatic});
    builder.addField({"deadPackageField", "I", kAccStatic});
    builder.addField({"deadProtectedField", "I", kAccProtected});
    builder.addField({"apiField", "I", kAccPublic | kAccStatic});

    const auto simpleMethod = [&](const char* name, ca::u16 access) {
        FixtureMethod m;
        m.name      = name;
        m.desc      = "()V";
        m.access    = access;
        m.maxStack  = 0;
        m.maxLocals = 0;
        m.code      = bareReturn();
        builder.addMethod(m);
    };
    simpleMethod("livePrivateMethod", kAccPrivate | kAccStatic);
    simpleMethod("deadPackageMethod", kAccStatic);
    simpleMethod("deadProtectedMethod", static_cast<ca::u16>(kAccProtected | kAccStatic));
    simpleMethod("apiMethod", kAccPublic | kAccStatic);
    simpleMethod("deadPrivateMethod", kAccPrivate | kAccStatic);

    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 1;
    main.maxLocals = 1;
    main.code      = getStaticPop(builder.fieldRef("App", "livePrivate", "I"));
    appendBytes(main.code, invokeStatic(builder.methodRef("App", "livePrivateMethod", "()V")));
    main.code.push_back(0xB1);
    builder.addMethod(main);
    return builder.assemble();
}

/// 服务入口 fail-closed 夹具：Main（平凡 main）+ example/SvcImpl（package-private
/// 成员，无容器内调用点）+ services 文件指向 SvcImpl。资源 keep 类声明成员整体
/// 入根 → 非私有档不得删（否则容器外 ServiceLoader 调用悬垂）。
Bytes buildServiceEntrypointMain()
{
    FixtureBuilder builder("example/Main");
    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 0;
    main.maxLocals = 1;
    main.code      = bareReturn();
    builder.addMethod(main);
    return builder.assemble();
}

Bytes buildServiceImpl()
{
    FixtureBuilder builder("example/SvcImpl");
    builder.addField({"implState", "I", kAccStatic});
    FixtureMethod serve;
    serve.name      = "serve";
    serve.desc      = "()V";
    serve.access    = kAccStatic;
    serve.maxStack  = 0;
    serve.maxLocals = 0;
    serve.code      = bareReturn();
    builder.addMethod(serve);
    return builder.assemble();
}

/// 悬垂防护夹具：private caller（可挂注解）引用 package-private 死成员。
/// fieldFace=true 时引用 deadPackageField，否则调用 callee 方法。
Bytes buildCallerCalleeClass(bool annotateCaller, bool fieldFace)
{
    FixtureBuilder builder("App");
    if (fieldFace) builder.addField({"deadPackageField", "I", kAccStatic});

    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = static_cast<ca::u16>(kAccPrivate | kAccStatic);
    caller.maxStack  = 1;
    caller.maxLocals = 0;
    caller.code      = fieldFace
                    ? getStaticPop(builder.fieldRef("App", "deadPackageField", "I"))
                    : invokeStatic(builder.methodRef("App", "callee", "()V"));
    caller.code.push_back(0xB1);
    builder.addMethod(caller);

    if (!fieldFace) {
        FixtureMethod callee;
        callee.name      = "callee";
        callee.desc      = "()V";
        callee.access    = kAccStatic;
        callee.maxStack  = 0;
        callee.maxLocals = 0;
        callee.code      = bareReturn();
        builder.addMethod(callee);
    }

    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 0;
    main.maxLocals = 1;
    main.code      = bareReturn();
    builder.addMethod(main);

    const Bytes bytes = builder.assemble();
    return annotateCaller ? withAnnotation(bytes, "caller", true, "Lsample/Keep;") : bytes;
}

/// 构造器可达夹具：`<init>` 调 package-private deadFromCtor；main 调 package-private
/// factory（App 类可达但不实例化——`<init>` 不经调用边入闭包，必须靠保留面根）。
Bytes buildConstructorReachClass()
{
    FixtureBuilder builder("App");

    FixtureMethod ctor;
    ctor.name      = "<init>";
    ctor.desc      = "()V";
    ctor.access    = kAccPublic;
    ctor.maxStack  = 0;
    ctor.maxLocals = 1;
    ctor.code      = invokeStatic(builder.methodRef("App", "deadFromCtor", "()V"));
    ctor.code.push_back(0xB1);
    builder.addMethod(ctor);

    FixtureMethod factory;
    factory.name      = "factory";
    factory.desc      = "()V";
    factory.access    = kAccStatic;
    factory.maxStack  = 0;
    factory.maxLocals = 0;
    factory.code      = bareReturn();
    builder.addMethod(factory);

    FixtureMethod dead;
    dead.name      = "deadFromCtor";
    dead.desc      = "()V";
    dead.access    = kAccStatic;
    dead.maxStack  = 0;
    dead.maxLocals = 0;
    dead.code      = bareReturn();
    builder.addMethod(dead);

    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 0;
    main.maxLocals = 1;
    main.code      = invokeStatic(builder.methodRef("App", "factory", "()V"));
    main.code.push_back(0xB1);
    builder.addMethod(main);
    return builder.assemble();
}

// ── 运行面辅助 ───────────────────────────────────────────────────────────

/// registerReachabilityAnalyses 对齐：六分析全注册（层次事实由
/// HierarchyBundleAnalysis 承载）。
void registerReachabilityAnalyses(mjbase::spi::GlobalAnalysisManager& gam)
{
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::asm_classpath::HierarchyBundleAnalysis>();
    gam.registerAnalysis<mjbase::asm_classpath::ResolvedCallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::GlobalReachabilityAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedGlobalReachabilityAnalysis>();
}

/// 重读容器内 class 资源（静态 keeper 持模型，引用在本语句内有效）。
const jbc::core::ClassFile& reload(mjbase::model::ResourceContainer& container, const char* uri)
{
    static std::unique_ptr<jbc::core::ClassFile> keeper;
    auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr) << uri;
    auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource);
    EXPECT_NE(cls, nullptr) << uri;
    keeper = cls == nullptr ? nullptr : loadDom(cls->get(), uri);
    EXPECT_NE(keeper, nullptr) << uri;
    return *keeper;
}

bool hasField(const jbc::core::ClassFile& cf, const char* name, const char* desc)
{
    const jbc::core::MemberInfo* f = findMember(cf, false, name);
    return f != nullptr && cf.descOf(*f) == desc;
}

bool hasMethod(const jbc::core::ClassFile& cf, const char* name, const char* desc)
{
    const jbc::core::MemberInfo* m = findMember(cf, true, name);
    return m != nullptr && cf.descOf(*m) == desc;
}

Bytes classBytesOf(mjbase::model::ResourceContainer& container, const char* uri)
{
    auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr) << uri;
    auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource);
    EXPECT_NE(cls, nullptr) << uri;
    return cls == nullptr ? Bytes{} : cls->get();
}

}   // namespace

// ── EmptyClassRemover ────────────────────────────────────────────────────────

// 用例：只删结构空类（无字段无方法），非类资源与其他类原样。
TEST(MjPassEmptyClassRemoverTest, RemovesOnlyStructurallyEmptyClasses)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Empty.class", buildEmptyClassBytes("Empty"));
    addClassResource(container, "Used.class", buildKeepClass());
    container.addResource("note.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "note.txt", std::vector<ca::u8>{1, 2, 3}));

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::EmptyClassRemover    pass;
    const auto                         pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(pa.changed());
    EXPECT_FALSE(container.hasResource("Empty.class"));
    EXPECT_TRUE(container.hasResource("Used.class"));
    EXPECT_TRUE(container.hasResource("note.txt"));
    // 幸存类字节原样（容器级删除不做类内容改写）。
    EXPECT_EQ(classBytesOf(container, "Used.class"), buildKeepClass());
}

// ── 契约面：未注册分析稳定失败 ──────────────────────────────────────────────

// 用例：未注册分析 = 调用方契约错误 → std::logic_error，容器无改写。
TEST(MjPassUnusedMemberContractTest, UnregisteredAnalysisPropagatesWithoutResourceRewrite)
{
    const auto verify = [](auto& pass) {
        ca::str::Utf8StringArena           arena;
        mjbase::model::ResourceContainer   container;
        addClassResource(container, "App.class", buildMemberCleanupClass());
        const Bytes before = classBytesOf(container, "App.class");
        mjbase::spi::GlobalAnalysisManager gam(arena);   // 无任何注册

        EXPECT_THROW(pass.run(container, gam), std::logic_error);
        EXPECT_EQ(classBytesOf(container, "App.class"), before);
    };

    mjbase::pass::UnusedClassRemover        classPass;
    mjbase::pass::UnusedFieldRemover        fieldPass;
    mjbase::pass::GlobalUnusedMethodRemover methodPass;
    verify(classPass);
    verify(fieldPass);
    verify(methodPass);
}

// ── UnusedClassRemover ───────────────────────────────────────────────────────

// 用例：main 闭包外的类删除，幸存类与非类资源原样。
TEST(MjPassUnusedClassRemoverTest, RemovesClassesOutsideMainReachabilityClosure)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addUnusedClassFixture(container, /*withMain*/ true);
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass;
    const auto                       pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(container.hasResource("App.class"));
    EXPECT_TRUE(container.hasResource("Used.class"));
    EXPECT_TRUE(container.hasResource("note.txt"));
    EXPECT_FALSE(container.hasResource("Dead.class"));
}

// 用例：无默认根（library jar）保持不变。
TEST(MjPassUnusedClassRemoverTest, KeepsLibraryJarWhenNoDefaultRootsExist)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addUnusedClassFixture(container, /*withMain*/ false);
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass;
    const auto                       pa = pass.run(container, gam);

    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_TRUE(container.hasResource("Library.class"));
    EXPECT_TRUE(container.hasResource("Used.class"));
    EXPECT_TRUE(container.hasResource("Dead.class"));
    EXPECT_TRUE(container.hasResource("note.txt"));
}

// 用例：resolved 可达性保持虚覆盖目标（Child.run 覆盖 Base.run）。
TEST(MjPassUnusedClassRemoverTest, KeepsOverrideTargetsFromResolvedReachability)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "VirtualBase.class",
                     buildRunnerClass("VirtualBase", "java/lang/Object"));
    addClassResource(container, "VirtualChild.class",
                     buildRunnerClass("VirtualChild", "VirtualBase"));
    addClassResource(container, "VirtualCleanupApp.class", buildVirtualApp());
    addClassResource(container, "VirtualDead.class", buildEmptyClassBytes("VirtualDead"));
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass;
    const auto                       pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(container.hasResource("VirtualCleanupApp.class"));
    EXPECT_TRUE(container.hasResource("VirtualBase.class"));
    EXPECT_TRUE(container.hasResource("VirtualChild.class"));
    EXPECT_FALSE(container.hasResource("VirtualDead.class"));
}

// 用例：services 文件名/内容行与 Manifest 入口键都是保留根。
TEST(MjPassUnusedClassRemoverTest, KeepsServiceAndManifestEntrypoints)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "example/Service.class", buildEmptyClassBytes("example/Service"));
    addClassResource(container, "example/Impl.class", buildEmptyClassBytes("example/Impl"));
    addClassResource(container, "example/Agent.class", buildEmptyClassBytes("example/Agent"));
    addClassResource(container, "example/Dead.class", buildEmptyClassBytes("example/Dead"));
    const std::string serviceText = "# provider\nexample.Impl\n";
    container.addResource("META-INF/services/example.Service",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "META-INF/services/example.Service",
                              std::vector<ca::u8>(serviceText.begin(), serviceText.end())));
    mjbase::jar::Manifest manifest;
    manifest.setMainAttribute(ca::str::Utf8StringRef::from_cstr("Agent-Class"),
                              ca::str::Utf8StringRef::from_cstr("example.Agent"));
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<mjbase::model::ManifestResource>(manifest));

    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);
    mjbase::pass::UnusedClassRemover pass;

    EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
    EXPECT_TRUE(container.hasResource("example/Service.class"));
    EXPECT_TRUE(container.hasResource("example/Impl.class"));
    EXPECT_TRUE(container.hasResource("example/Agent.class"));
    EXPECT_FALSE(container.hasResource("example/Dead.class"));
    // 非类资源原样。
    EXPECT_TRUE(container.hasResource("META-INF/services/example.Service"));
    EXPECT_TRUE(container.hasResource("META-INF/MANIFEST.MF"));
}

// 用例：module 描述符类是保留根，其 uses/provides/mainClass 结构引用
// 随可达性扩散（Service/Impl/Main 保，Dead 删）。
TEST(MjPassUnusedClassRemoverTest, KeepsModuleDescriptorEntrypoints)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "module-info.class", buildModuleInfo());
    for (const char* name : {"example/Main", "example/Service", "example/Impl", "example/Dead"}) {
        const std::string uri = std::string(name) + ".class";
        addClassResource(container, uri.c_str(), buildEmptyClassBytes(name));
    }
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass;
    EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
    EXPECT_TRUE(container.hasResource("module-info.class"));
    EXPECT_TRUE(container.hasResource("example/Main.class"));
    EXPECT_TRUE(container.hasResource("example/Service.class"));
    EXPECT_TRUE(container.hasResource("example/Impl.class"));
    EXPECT_FALSE(container.hasResource("example/Dead.class"));
}

// 用例：坏类资源整 pass 放弃（fail-closed），不做部分删除。
TEST(MjPassUnusedClassRemoverTest, MalformedClassFailsClosedWithoutPartialDeletion)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addUnusedClassFixture(container, /*withMain*/ true);
    const std::vector<ca::u8> garbage{0, 1, 2, 3};
    container.addResource("broken.class",
                          std::make_unique<mjbase::model::ClassResource>("broken.class", garbage));
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass;
    EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
    EXPECT_TRUE(container.hasResource("Dead.class"));
    EXPECT_TRUE(container.hasResource("broken.class"));
    EXPECT_EQ(classBytesOf(container, "broken.class"), garbage);
}

// 缩减移植（形态差见文件头）：类注解 + 字段签名引用的类保持可达（结构元数据
// 面）；MethodHandle / indy / 反射字符串三个间接面由 reference 模块用例钉死。
TEST(MjPassUnusedClassRemoverTest, KeepsMetadataReferencedClasses)
{
    // IndirectRoot：main 根 + generic 字段（Signature 引 SignatureTarget）+
    // 类注解（引 AnnotationTarget）。
    FixtureBuilder builder("IndirectRoot");
    builder.addField({"generic", "Ljava/lang/Object;", kAccPublic | kAccStatic});
    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 0;
    main.maxLocals = 1;
    main.code      = bareReturn();
    builder.addMethod(main);

    const Bytes indirectRoot = withAnnotation(
        withFieldSignature(builder.assemble(), "generic", "LSignatureTarget;"), nullptr, false,
        "LAnnotationTarget;");
    ASSERT_FALSE(indirectRoot.empty());

    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "IndirectRoot.class", indirectRoot);
    addClassResource(container, "AnnotationTarget.class", buildEmptyClassBytes("AnnotationTarget"));
    addClassResource(container, "SignatureTarget.class", buildEmptyClassBytes("SignatureTarget"));
    addClassResource(container, "Dead.class", buildEmptyClassBytes("Dead"));
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass;
    EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
    EXPECT_TRUE(container.hasResource("AnnotationTarget.class"));
    EXPECT_TRUE(container.hasResource("SignatureTarget.class"));
    EXPECT_FALSE(container.hasResource("Dead.class"));
}

// ── UnusedFieldRemover ───────────────────────────────────────────────────────

// 用例：只删不可达 private 字段。
TEST(MjPassUnusedFieldRemoverTest, RemovesOnlyUnreachablePrivateFields)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildMemberCleanupClass());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedFieldRemover pass;
    const auto                       pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    const jbc::core::ClassFile& reloaded = reload(container, "App.class");
    EXPECT_TRUE(hasField(reloaded, "livePrivate", "I"));
    EXPECT_TRUE(hasField(reloaded, "apiField", "I"));
    EXPECT_FALSE(hasField(reloaded, "deadPrivate", "I"));
}

// 成员删除两件的容器遍历面：混入非类资源（services / Manifest）时必须按 tag
// 守护跳过（不得对非 ClassResource 做无检查 cast），类删除语义不受影响。
TEST(MjPassUnusedFieldRemoverTest, SkipsNonClassResourcesWithoutUntaggedCast)
{
    const auto verify = [](auto& pass, bool expectFieldRemoval) {
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildMemberCleanupClass());
        // services 与 Manifest 入口键都指向容器外类（指向 App 本身会把 App 变
        // 成 keep 根、其全声明方法成根方法，成员删除语义被根策略压住）。
        const std::string serviceText = "some.Service\n";
        container.addResource("META-INF/services/some.Service",
                              std::make_unique<mjbase::model::UnknownResource>(
                                  "META-INF/services/some.Service",
                                  std::vector<ca::u8>(serviceText.begin(), serviceText.end())));
        mjbase::jar::Manifest manifest;
        // 入口键指向容器外类（App 本身不是 keep 根——否则其全声明方法成根，
        // 成员删除语义被根策略压住）。
        manifest.setMainAttribute(ca::str::Utf8StringRef::from_cstr("Main-Class"),
                                  ca::str::Utf8StringRef::from_cstr("some.ExternalMain"));
        container.addResource("META-INF/MANIFEST.MF",
                              std::make_unique<mjbase::model::ManifestResource>(manifest));
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        EXPECT_TRUE(container.hasResource("META-INF/services/some.Service"));
        EXPECT_TRUE(container.hasResource("META-INF/MANIFEST.MF"));
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        // 各 pass 只动自己语义面：field 面删字段、method 面删方法，另一面原样。
        if (expectFieldRemoval) {
            EXPECT_FALSE(hasField(reloaded, "deadPrivate", "I"));
            EXPECT_TRUE(hasMethod(reloaded, "deadPrivateMethod", "()V"));
        }
        else {
            EXPECT_TRUE(hasField(reloaded, "deadPrivate", "I"));
            EXPECT_FALSE(hasMethod(reloaded, "deadPrivateMethod", "()V"));
        }
    };

    mjbase::pass::UnusedFieldRemover        fieldPass;
    mjbase::pass::GlobalUnusedMethodRemover methodPass;
    verify(fieldPass, /*expectFieldRemoval*/ true);
    verify(methodPass, /*expectFieldRemoval*/ false);
}

// 用例：坏类资源整 pass 放弃，已加载类字节不被部分改写。
TEST(MjPassUnusedFieldRemoverTest, MalformedClassFailsClosedWithoutPartialRewrite)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildMemberCleanupClass());
    const Bytes before = classBytesOf(container, "App.class");
    const std::vector<ca::u8> garbage{0, 1, 2, 3};
    container.addResource("broken.class",
                          std::make_unique<mjbase::model::ClassResource>("broken.class", garbage));
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedFieldRemover pass;
    EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "App.class"), before);
}

// 缩减移植（形态差见文件头）：注解是 private 字段的保留元数据（字段不可达仍
// keep）；无注解的 dead 字段照删。
TEST(MjPassUnusedFieldRemoverTest, KeepsAnnotatedPrivateField)
{
    FixtureBuilder builder("Annotated");
    builder.addField({"annotatedField", "Ljava/util/List;", kAccPrivate | kAccStatic});
    builder.addField({"deadField", "I", kAccPrivate | kAccStatic});
    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 0;
    main.maxLocals = 1;
    main.code      = bareReturn();
    builder.addMethod(main);
    const Bytes patched = withAnnotation(builder.assemble(), "annotatedField", false,
                                         "Lsample/Keep;");
    ASSERT_FALSE(patched.empty());

    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Annotated.class", patched);
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedFieldRemover pass;
    EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
    const jbc::core::ClassFile& reloaded = reload(container, "Annotated.class");
    EXPECT_TRUE(hasField(reloaded, "annotatedField", "Ljava/util/List;"));
    EXPECT_FALSE(hasField(reloaded, "deadField", "I"));
}

// ── GlobalUnusedMethodRemover ────────────────────────────────────────────────

// 用例：只删不可达 private 方法（<init>/<clinit> 豁免面由 JVM 门兜底）。
TEST(MjPassGlobalUnusedMethodRemoverTest, RemovesOnlyUnreachablePrivateMethods)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildMemberCleanupClass());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::GlobalUnusedMethodRemover pass;
    const auto                              pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    const jbc::core::ClassFile& reloaded = reload(container, "App.class");
    EXPECT_TRUE(hasMethod(reloaded, "main", "([Ljava/lang/String;)V"));
    EXPECT_TRUE(hasMethod(reloaded, "livePrivateMethod", "()V"));
    EXPECT_TRUE(hasMethod(reloaded, "apiMethod", "()V"));
    EXPECT_FALSE(hasMethod(reloaded, "deadPrivateMethod", "()V"));
}

// 用例：坏类资源整 pass 放弃，已加载类字节不被部分改写。
TEST(MjPassGlobalUnusedMethodRemoverTest, MalformedClassFailsClosedWithoutPartialRewrite)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildMemberCleanupClass());
    const Bytes before = classBytesOf(container, "App.class");
    const std::vector<ca::u8> garbage{0, 1, 2, 3};
    container.addResource("broken.class",
                          std::make_unique<mjbase::model::ClassResource>("broken.class", garbage));
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::GlobalUnusedMethodRemover pass;
    EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "App.class"), before);
}

// 缩减移植（形态差见文件头）：注解是 private 方法的保留元数据（方法不可达仍
// keep）；无注解的 dead 方法照删。
TEST(MjPassGlobalUnusedMethodRemoverTest, KeepsAnnotatedPrivateMethod)
{
    FixtureBuilder builder("Annotated");
    FixtureMethod annotated;
    annotated.name      = "annotatedMethod";
    annotated.desc      = "()V";
    annotated.access    = static_cast<ca::u16>(kAccPrivate | kAccStatic);
    annotated.maxStack  = 0;
    annotated.maxLocals = 0;
    annotated.code      = bareReturn();
    builder.addMethod(annotated);

    FixtureMethod dead;
    dead.name      = "deadMethod";
    dead.desc      = "()V";
    dead.access    = static_cast<ca::u16>(kAccPrivate | kAccStatic);
    dead.maxStack  = 0;
    dead.maxLocals = 0;
    dead.code      = bareReturn();
    builder.addMethod(dead);

    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 0;
    main.maxLocals = 1;
    main.code      = bareReturn();
    builder.addMethod(main);

    const Bytes patched = withAnnotation(builder.assemble(), "annotatedMethod", true,
                                         "Lsample/Keep;");
    ASSERT_FALSE(patched.empty());

    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Annotated.class", patched);
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::GlobalUnusedMethodRemover pass;
    EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
    const jbc::core::ClassFile& reloaded = reload(container, "Annotated.class");
    EXPECT_TRUE(hasMethod(reloaded, "annotatedMethod", "()V"));
    EXPECT_FALSE(hasMethod(reloaded, "deadMethod", "()V"));
}

// ── 缓存失效契约 ─────────────────────────────────────────────────────────────

// 用例：pass 报告 none() 后 GlobalPassManager 失效全局分析缓存。
TEST(MjPassUnusedMemberInvalidationTest, ChangedResultInvalidatesGlobalAnalyses)
{
    const auto verify = [](auto& pass, mjbase::model::ResourceContainer& container) {
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);
        (void)gam.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);
        (void)gam.getResult<mjbase::reference::ResolvedGlobalReachabilityAnalysis>(container);

        mjbase::spi::GlobalPassManager gpm;
        gpm.addPass(std::make_unique<std::remove_reference_t<decltype(pass)>>());
        EXPECT_FALSE(gpm.run(container, gam).areAllPreserved());
        EXPECT_EQ(gam.getCachedResult<mjbase::reference::ReferenceIndexAnalysis>(container),
                  nullptr);
        EXPECT_EQ(gam.getCachedResult<mjbase::reference::ResolvedGlobalReachabilityAnalysis>(
                      container),
                  nullptr);
    };

    {   // 类删除面：App/Used/Dead 夹具（Dead 必删）。
        mjbase::model::ResourceContainer container;
        addUnusedClassFixture(container, /*withMain*/ true);
        mjbase::pass::UnusedClassRemover pass;
        verify(pass, container);
    }
    {   // 字段删除面：成员清理夹具（deadPrivate 必删）。
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildMemberCleanupClass());
        mjbase::pass::UnusedFieldRemover pass;
        verify(pass, container);
    }
    {   // 方法删除面：同一夹具（deadPrivateMethod 必删）。
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildMemberCleanupClass());
        mjbase::pass::GlobalUnusedMethodRemover pass;
        verify(pass, container);
    }
}

// ── keep 规则集成（审计 P0-1/P0-2）─────────────────────────────────────────────

namespace {

/// 解析规则文本（失败即致命断言）。
std::shared_ptr<const mjbase::reference::KeepRuleSet> parseKeepRules(const char* text)
{
    const auto parsed = mjbase::reference::KeepRuleSet::parse(text);
    EXPECT_TRUE(parsed.is_ok()) << "keep rule parse failed: line "
                                << (parsed.is_err() ? parsed.unwrap_err().line : 0);
    if (parsed.is_err()) return nullptr;
    return std::make_shared<const mjbase::reference::KeepRuleSet>(std::move(parsed).unwrap());
}

/// 序列化/lambda 夹具：main 之外全是不可达私有成员——deadPrivateMethod（无规则
/// 命中，对照删）、readObject（+serialization 命中）、lambda$main$0（+lambda
/// 命中）、serialVersionUID 字段（+serialization 命中）、deadPrivate 字段（无）。
Bytes buildSerializationFixture()
{
    FixtureBuilder builder("App");
    builder.addField({"deadPrivate", "I", kAccPrivate | kAccStatic});
    builder.addField({"serialVersionUID", "J", kAccPrivate | kAccStatic});

    FixtureMethod dead;
    dead.name      = "deadPrivateMethod";
    dead.desc      = "()V";
    dead.access    = static_cast<ca::u16>(kAccPrivate | kAccStatic);
    dead.maxStack  = 0;
    dead.maxLocals = 0;
    dead.code      = bareReturn();
    builder.addMethod(dead);

    // 序列化回调：精确描述符（preset +serialization 命中）。
    FixtureMethod readObject;
    readObject.name      = "readObject";
    readObject.desc      = "(Ljava/io/ObjectInputStream;)V";
    readObject.access    = static_cast<ca::u16>(kAccPrivate);
    readObject.maxStack  = 1;
    readObject.maxLocals = 1;
    readObject.code      = bareReturn();
    builder.addMethod(readObject);

    // lambda 合成方法（preset +lambda 命中）。
    FixtureMethod lambda;
    lambda.name      = "lambda$main$0";
    lambda.desc      = "()V";
    lambda.access    = static_cast<ca::u16>(kAccPrivate | kAccStatic);
    lambda.maxStack  = 0;
    lambda.maxLocals = 0;
    lambda.code      = bareReturn();
    builder.addMethod(lambda);

    FixtureMethod main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 1;
    main.maxLocals = 1;
    main.code      = bareReturn();
    builder.addMethod(main);
    return builder.assemble();
}

}   // namespace

// 类规则：EmptyClassRemover 对规则命名的空类停手（其余空类照删）。
TEST(MjPassKeepRuleTest, ClassRuleProtectsEmptyClassFromEmptyClassRemover)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Empty.class", buildEmptyClassBytes("Empty"));
    addClassResource(container, "Guarded.class", buildEmptyClassBytes("Guarded"));
    addClassResource(container, "Used.class", buildKeepClass());

    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::EmptyClassRemover    pass(parseKeepRules("class Guarded\n"));
    const auto                         pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());   // Empty 照删（Guarded 被规则保住）
    EXPECT_TRUE(pa.changed());
    EXPECT_TRUE(container.hasResource("Guarded.class"));
    EXPECT_FALSE(container.hasResource("Empty.class"));
    EXPECT_TRUE(container.hasResource("Used.class"));
}

// 类规则：UnusedClassRemover 对规则命名的不可达类停手。
TEST(MjPassKeepRuleTest, ClassRuleProtectsUnreachableClassFromUnusedClassRemover)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addUnusedClassFixture(container, /*withMain*/ true);
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass(parseKeepRules("class Dead\n"));
    EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
    EXPECT_TRUE(container.hasResource("Dead.class"));
}

// 类规则：成员删除两件对规则命名的可达类整类跳过（dead 成员全留）。
TEST(MjPassKeepRuleTest, ClassRuleSkipsWholeClassInMemberRemovers)
{
    const auto verify = [](auto& pass) {
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildMemberCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        EXPECT_TRUE(hasField(reloaded, "deadPrivate", "I"));
        EXPECT_TRUE(hasMethod(reloaded, "deadPrivateMethod", "()V"));
    };

    mjbase::pass::UnusedFieldRemover        fieldPass(parseKeepRules("class App\n"));
    mjbase::pass::GlobalUnusedMethodRemover methodPass(parseKeepRules("class App\n"));
    verify(fieldPass);
    verify(methodPass);
}

// 成员规则：精确描述符只保对应成员；无括号形态（任意描述符）保字段；描述符
// 不符的同名规则不保（防过宽保持）。
TEST(MjPassKeepRuleTest, MemberRuleProtectsSpecificPrivateMembers)
{
    {   // 方法：精确 desc 命中 → 留。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildMemberCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::GlobalUnusedMethodRemover pass(
            parseKeepRules("member App#deadPrivateMethod()V\n"));
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        EXPECT_TRUE(hasMethod(reload(container, "App.class"), "deadPrivateMethod", "()V"));
    }
    {   // 字段：省略括号 = 任意描述符 → 留。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildMemberCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::UnusedFieldRemover pass(parseKeepRules("member App#deadPrivate\n"));
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        EXPECT_TRUE(hasField(reload(container, "App.class"), "deadPrivate", "I"));
    }
    {   // 描述符不符：同名不同 desc 的规则不保死方法。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildMemberCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::GlobalUnusedMethodRemover pass(
            parseKeepRules("member App#deadPrivateMethod(I)V\n"));
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        EXPECT_FALSE(hasMethod(reload(container, "App.class"), "deadPrivateMethod", "()V"));
    }
}

// 预设模式库：序列化回调/lambda 合成名/serialVersionUID 按 preset 保留；
// 无规则命中的死成员在两个对照组里照删（preset 只保枚举内的名字）。
TEST(MjPassKeepRuleTest, PresetPatternsProtectFrameworkCallbacks)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildSerializationFixture());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::GlobalUnusedMethodRemover methodPass(
        parseKeepRules("preset +serialization\npreset +lambda\n"));
    // 有删除（deadPrivateMethod 不在任何预设内，照删）→ 非 all-preserved。
    EXPECT_FALSE(methodPass.run(container, gam).areAllPreserved());
    const jbc::core::ClassFile& reloaded = reload(container, "App.class");
    EXPECT_TRUE(hasMethod(reloaded, "readObject", "(Ljava/io/ObjectInputStream;)V"));
    EXPECT_TRUE(hasMethod(reloaded, "lambda$main$0", "()V"));
    EXPECT_FALSE(hasMethod(reloaded, "deadPrivateMethod", "()V"));

    // 对照组 1：无规则时死成员照删。
    ca::str::Utf8StringArena         arena2;
    mjbase::model::ResourceContainer container2;
    addClassResource(container2, "App.class", buildSerializationFixture());
    mjbase::spi::GlobalAnalysisManager gam2(arena2);
    registerReachabilityAnalyses(gam2);
    mjbase::pass::GlobalUnusedMethodRemover bareMethodPass;
    EXPECT_FALSE(bareMethodPass.run(container2, gam2).areAllPreserved());
    const jbc::core::ClassFile& reloaded2 = reload(container2, "App.class");
    EXPECT_FALSE(hasMethod(reloaded2, "readObject", "(Ljava/io/ObjectInputStream;)V"));
    EXPECT_FALSE(hasMethod(reloaded2, "lambda$main$0", "()V"));
    EXPECT_FALSE(hasMethod(reloaded2, "deadPrivateMethod", "()V"));

    // 对照组 2：+serialization 把 serialVersionUID 字段留在字段删除面。
    ca::str::Utf8StringArena         arena3;
    mjbase::model::ResourceContainer container3;
    addClassResource(container3, "App.class", buildSerializationFixture());
    mjbase::spi::GlobalAnalysisManager gam3(arena3);
    registerReachabilityAnalyses(gam3);
    mjbase::pass::UnusedFieldRemover fieldPass(parseKeepRules("preset +serialization\n"));
    // 有删除（deadPrivate 无规则命中，照删）→ 非 all-preserved。
    EXPECT_FALSE(fieldPass.run(container3, gam3).areAllPreserved());
    const jbc::core::ClassFile& reloaded3 = reload(container3, "App.class");
    EXPECT_TRUE(hasField(reloaded3, "serialVersionUID", "J"));
    EXPECT_FALSE(hasField(reloaded3, "deadPrivate", "I"));
}

// 不命中规则的注入不改变既有行为（防「注入即全保」实现错误）。
TEST(MjPassKeepRuleTest, NonMatchingRuleKeepsBaselineBehavior)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildMemberCleanupClass());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::GlobalUnusedMethodRemover pass(parseKeepRules("member other/Cls#x()V\n"));
    EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
    EXPECT_FALSE(hasMethod(reload(container, "App.class"), "deadPrivateMethod", "()V"));
    EXPECT_TRUE(hasMethod(reload(container, "App.class"), "livePrivateMethod", "()V"));
}

// ── 可见性档位（本批放开面：非私有死成员删除）────────────────────────────────
//
// 档位语义见 unused_member_remover.hpp 模块头注：NonPublic（默认）候选全部非
// public 死成员、public 面 fail-closed 保守；PrivateOnly 是旧策略回归档；
// All 是闭程序激进档。保留面根提升（keep metadata / keep 规则 / 资源 keep 类 /
// 构造器）是删除安全不变量的前提，本节一并钉死。

// 默认档：package-private/protected 死成员删除，public 面保留，private live 面
// 保留。方法与字段两面各自验证。
TEST(MjPassNonPublicTierTest, DefaultTierRemovesNonPublicDeadMembersAndKeepsPublicFace)
{
    const auto runMethodPass = [] {
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildNonPublicCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::GlobalUnusedMethodRemover pass;
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        EXPECT_TRUE(hasMethod(reloaded, "main", "([Ljava/lang/String;)V"));
        EXPECT_TRUE(hasMethod(reloaded, "livePrivateMethod", "()V"));
        EXPECT_TRUE(hasMethod(reloaded, "apiMethod", "()V"));   // public 面 fail-closed
        EXPECT_FALSE(hasMethod(reloaded, "deadPackageMethod", "()V"));
        EXPECT_FALSE(hasMethod(reloaded, "deadProtectedMethod", "()V"));
        EXPECT_FALSE(hasMethod(reloaded, "deadPrivateMethod", "()V"));
    };
    const auto runFieldPass = [] {
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildNonPublicCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::UnusedFieldRemover pass;
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        EXPECT_TRUE(hasField(reloaded, "livePrivate", "I"));
        EXPECT_TRUE(hasField(reloaded, "apiField", "I"));   // public 面 fail-closed
        EXPECT_FALSE(hasField(reloaded, "deadPrivateField", "I"));
        EXPECT_FALSE(hasField(reloaded, "deadPackageField", "I"));
        EXPECT_FALSE(hasField(reloaded, "deadProtectedField", "I"));
    };
    runMethodPass();
    runFieldPass();
}

// PrivateOnly 档：旧策略回归——非私有死成员保留，只删 private。
TEST(MjPassNonPublicTierTest, PrivateOnlyTierKeepsLegacyBehavior)
{
    {   // 字段面。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildNonPublicCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::UnusedFieldRemover pass{
            nullptr, mjbase::pass::UnusedMemberAccessTier::PrivateOnly};
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        EXPECT_TRUE(hasField(reloaded, "deadPackageField", "I"));
        EXPECT_TRUE(hasField(reloaded, "deadProtectedField", "I"));
        EXPECT_TRUE(hasField(reloaded, "apiField", "I"));
        EXPECT_FALSE(hasField(reloaded, "deadPrivateField", "I"));
        EXPECT_TRUE(hasField(reloaded, "livePrivate", "I"));
    }
    {   // 方法面。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildNonPublicCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::GlobalUnusedMethodRemover pass{
            nullptr, mjbase::pass::UnusedMemberAccessTier::PrivateOnly};
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        EXPECT_TRUE(hasMethod(reloaded, "deadPackageMethod", "()V"));
        EXPECT_TRUE(hasMethod(reloaded, "deadProtectedMethod", "()V"));
        EXPECT_TRUE(hasMethod(reloaded, "apiMethod", "()V"));
        EXPECT_FALSE(hasMethod(reloaded, "deadPrivateMethod", "()V"));
        EXPECT_TRUE(hasMethod(reloaded, "livePrivateMethod", "()V"));
    }
}

// All 档：public 死成员也候选（闭程序专用；库型容器语义上不该开此档）。
TEST(MjPassNonPublicTierTest, AllTierAlsoRemovesPublicDeadMembers)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildNonPublicCleanupClass());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::GlobalUnusedMethodRemover methodPass{
        nullptr, mjbase::pass::UnusedMemberAccessTier::All};
    EXPECT_FALSE(methodPass.run(container, gam).areAllPreserved());
    const jbc::core::ClassFile& reloaded = reload(container, "App.class");
    EXPECT_TRUE(hasMethod(reloaded, "main", "([Ljava/lang/String;)V"));
    EXPECT_TRUE(hasMethod(reloaded, "livePrivateMethod", "()V"));
    EXPECT_FALSE(hasMethod(reloaded, "apiMethod", "()V"));
    EXPECT_FALSE(hasMethod(reloaded, "deadPackageMethod", "()V"));
    EXPECT_FALSE(hasMethod(reloaded, "deadProtectedMethod", "()V"));

    ca::str::Utf8StringArena         arena2;
    mjbase::model::ResourceContainer container2;
    addClassResource(container2, "App.class", buildNonPublicCleanupClass());
    mjbase::spi::GlobalAnalysisManager gam2(arena2);
    registerReachabilityAnalyses(gam2);
    mjbase::pass::UnusedFieldRemover fieldPass{nullptr, mjbase::pass::UnusedMemberAccessTier::All};
    EXPECT_FALSE(fieldPass.run(container2, gam2).areAllPreserved());
    const jbc::core::ClassFile& reloaded2 = reload(container2, "App.class");
    EXPECT_TRUE(hasField(reloaded2, "livePrivate", "I"));
    EXPECT_FALSE(hasField(reloaded2, "apiField", "I"));
    EXPECT_FALSE(hasField(reloaded2, "deadPackageField", "I"));
    EXPECT_FALSE(hasField(reloaded2, "deadProtectedField", "I"));
}

/// 服务入口容器装配（Main + SvcImpl + services 文件）。
void addServiceEntrypointFixture(mjbase::model::ResourceContainer& container)
{
    addClassResource(container, "example/Main.class", buildServiceEntrypointMain());
    addClassResource(container, "example/SvcImpl.class", buildServiceImpl());
    const std::string serviceText = "example.SvcImpl\n";
    container.addResource("META-INF/services/example.Svc",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "META-INF/services/example.Svc",
                              std::vector<ca::u8>(serviceText.begin(), serviceText.end())));
}

// fail-closed：services 指向的容器内实现类，其 package-private 成员无容器内调用
// 点——资源 keep 类声明成员整体入根，非私有档不得删（否则容器外 ServiceLoader 调用悬垂）。
TEST(MjPassNonPublicTierTest, ServiceEntrypointMembersSurviveDefaultTier)
{
    {   // 方法面：serve（package-private，无容器内调用点）保留 → 整 pass no-op。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addServiceEntrypointFixture(container);
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);
        mjbase::pass::GlobalUnusedMethodRemover pass;
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        EXPECT_TRUE(hasMethod(reload(container, "example/SvcImpl.class"), "serve", "()V"));
    }
    {   // 字段面：implState（package-private，无容器内访问点）保留 → 整 pass no-op。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addServiceEntrypointFixture(container);
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);
        mjbase::pass::UnusedFieldRemover pass;
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        EXPECT_TRUE(hasField(reload(container, "example/SvcImpl.class"), "implState", "I"));
    }
}

// 悬垂防护（soundness）：保留但不可达的注解 caller 引用 package-private 死成员
// 时，callee/callee 字段必须随 caller 的保留面根入闭包——不得删出悬垂引用。
TEST(MjPassNonPublicTierTest, RetainedUnreachableCallerKeepsCalleeFromDeletion)
{
    {   // 方法面 + 注解 caller：callee 保留（整 pass no-op）。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildCallerCalleeClass(/*annotate*/ true,
                                                                        /*fieldFace*/ false));
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);
        mjbase::pass::GlobalUnusedMethodRemover pass;
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        EXPECT_TRUE(hasMethod(reload(container, "App.class"), "callee", "()V"));
        EXPECT_TRUE(hasMethod(reload(container, "App.class"), "caller", "()V"));
    }
    {   // 方法面 + 无注解 caller：caller 与 callee 都死，一起删（无悬垂残留）。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildCallerCalleeClass(/*annotate*/ false,
                                                                        /*fieldFace*/ false));
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);
        mjbase::pass::GlobalUnusedMethodRemover pass;
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        EXPECT_FALSE(hasMethod(reloaded, "caller", "()V"));
        EXPECT_FALSE(hasMethod(reloaded, "callee", "()V"));
    }
    {   // 字段面 + 注解 caller：caller 保留 → 其字段访问出边必须保住字段。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildCallerCalleeClass(/*annotate*/ true,
                                                                        /*fieldFace*/ true));
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);
        mjbase::pass::UnusedFieldRemover pass;
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        EXPECT_TRUE(hasField(reload(container, "App.class"), "deadPackageField", "I"));
    }
}

// 悬垂防护（soundness）：构造器从不候选删除且无条件入根——类可达但不实例化时，
// `<init>` 引用的死成员不得删（否则保留的 `<init>` 悬垂）。
TEST(MjPassNonPublicTierTest, ConstructorReachKeepsCalleeFromDeletion)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildConstructorReachClass());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::GlobalUnusedMethodRemover pass;
    EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
    const jbc::core::ClassFile& reloaded = reload(container, "App.class");
    EXPECT_TRUE(hasMethod(reloaded, "<init>", "()V"));
    EXPECT_TRUE(hasMethod(reloaded, "factory", "()V"));
    EXPECT_TRUE(hasMethod(reloaded, "deadFromCtor", "()V"));
}

// 幂等固定点：第二遍运行严格 no-op（all() 且字节不变）。
TEST(MjPassNonPublicTierTest, SecondRunIsStrictNoOp)
{
    const auto verify = [](auto& pass) {
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildNonPublicCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        const Bytes afterFirst = classBytesOf(container, "App.class");

        mjbase::spi::GlobalAnalysisManager gam2(arena);
        registerReachabilityAnalyses(gam2);
        EXPECT_TRUE(pass.run(container, gam2).areAllPreserved());
        EXPECT_EQ(classBytesOf(container, "App.class"), afterFirst);
    };
    mjbase::pass::UnusedFieldRemover        fieldPass;
    mjbase::pass::GlobalUnusedMethodRemover methodPass;
    verify(fieldPass);
    verify(methodPass);
}

// 删除后引用索引一致：重建索引后已删成员零入边，保留成员入边原样
//（「删除后 invoke 指令面无需清扫」的观测面）。
TEST(MjPassNonPublicTierTest, RebuiltIndexHasNoIncomingEdgesToDeletedMembers)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class", buildNonPublicCleanupClass());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::GlobalUnusedMethodRemover pass;
    EXPECT_FALSE(pass.run(container, gam).areAllPreserved());

    // 新管理器全量重扫删除后的容器（pass 返回 none() 使旧缓存失效的等价观测）。
    mjbase::spi::GlobalAnalysisManager gam2(arena);
    gam2.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    auto& index = gam2.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);

    EXPECT_TRUE(index.methodReferencesTo("App", "deadPackageMethod", "()V").empty());
    EXPECT_TRUE(index.methodReferencesTo("App", "deadProtectedMethod", "()V").empty());
    EXPECT_TRUE(index.methodReferencesTo("App", "deadPrivateMethod", "()V").empty());
    // 保留成员的入边原样：main → livePrivateMethod。
    EXPECT_EQ(index.methodReferencesTo("App", "livePrivateMethod", "()V").size(), 1u);
    // 声明面同步：已删成员不再有声明事实。
    EXPECT_EQ(index.declaredMethod("App", "deadPackageMethod", "()V"), nullptr);
    EXPECT_NE(index.declaredMethod("App", "livePrivateMethod", "()V"), nullptr);
}

// keep 规则（只加不减）在默认档下保住非私有死成员。
TEST(MjPassNonPublicTierTest, MemberRuleProtectsNonPublicDeadMembers)
{
    {   // 方法：protected 死方法被成员规则点名 → 保留；其余死方法照删。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildNonPublicCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::GlobalUnusedMethodRemover pass(
            parseKeepRules("member App#deadProtectedMethod()V\n"));
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        EXPECT_TRUE(hasMethod(reloaded, "deadProtectedMethod", "()V"));
        EXPECT_FALSE(hasMethod(reloaded, "deadPackageMethod", "()V"));
        EXPECT_TRUE(hasMethod(reloaded, "apiMethod", "()V"));
    }
    {   // 字段：package-private 死字段被成员规则点名（任意描述符形态）→ 保留。
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "App.class", buildNonPublicCleanupClass());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        mjbase::pass::UnusedFieldRemover pass(parseKeepRules("member App#deadPackageField\n"));
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        const jbc::core::ClassFile& reloaded = reload(container, "App.class");
        EXPECT_TRUE(hasField(reloaded, "deadPackageField", "I"));
        EXPECT_FALSE(hasField(reloaded, "deadProtectedField", "I"));
    }
}

// ── Multi-Release 族删除契约 ────────────────────────────────────────────────
//
// 删除面按声明内名决策（可达性/引用索引全以内名键控），同名 base + versions/N
// 变体「天然同进退」在实现上是巧合——本组用例把巧合固化为显式契约：
//   - 类级：族成员要么同时删除、要么同时保留，绝不出现只删一份的半删态；
//   - 成员级：族内每份各按（同名等价的）可达集独立结算，结果一致。

/// app/Api 族成员：public static live()V + private static dead()V +
/// private static deadField I（字段面删除载体）。
Bytes buildApiFamilyMember()
{
    FixtureBuilder builder("app/Api");
    builder.addField({"deadField", "I", kAccPrivate | kAccStatic});
    FixtureMethod  live;
    live.name      = "live";
    live.desc      = "()V";
    live.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    live.maxStack  = 0;
    live.maxLocals = 0;
    live.code      = bareReturn();
    builder.addMethod(live);
    FixtureMethod dead;
    dead.name      = "dead";
    dead.desc      = "()V";
    dead.access    = static_cast<ca::u16>(kAccPrivate | kAccStatic);
    dead.maxStack  = 0;
    dead.maxLocals = 0;
    dead.code      = bareReturn();
    builder.addMethod(dead);
    return builder.assemble();
}

/// app/Main：main 调 app/Api.live()V（可选引用面：withRef=false 时不引用 Api）。
Bytes buildApiFamilyMain(bool withRef)
{
    FixtureBuilder builder("app/Main");
    FixtureMethod  main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
    main.maxStack  = 0;
    main.maxLocals = 1;
    if (withRef) {
        main.code = invokeStatic(builder.methodRef("app/Api", "live", "()V"));
        main.code.push_back(0xB1);
    }
    else {
        main.code = {0xB1};
    }
    builder.addMethod(main);
    return builder.assemble();
}

TEST(MjPassUnusedClassRemoverTest, RemovesMultiReleaseFamilyTogether)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "app/Main.class", buildApiFamilyMain(/*withRef*/ false));
    addClassResource(container, "app/Api.class", buildEmptyClassBytes("app/Api"));
    addClassResource(container, "META-INF/versions/17/app/Api.class",
                     buildEmptyClassBytes("app/Api"));
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass;
    const auto                       pa = pass.run(container, gam);

    // app/Api 不可达 → 族两份同时删除（「只删一份」的半删态不发生）。
    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_FALSE(container.hasResource("app/Api.class"));
    EXPECT_FALSE(container.hasResource("META-INF/versions/17/app/Api.class"));
    EXPECT_TRUE(container.hasResource("app/Main.class"));
    EXPECT_EQ(container.size(), 1u);
}

TEST(MjPassUnusedClassRemoverTest, KeepsMultiReleaseFamilyTogether)
{
    ca::str::Utf8StringArena         arena;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "app/Main.class", buildApiFamilyMain(/*withRef*/ true));
    addClassResource(container, "app/Api.class", buildApiFamilyMember());
    addClassResource(container, "META-INF/versions/17/app/Api.class",
                     buildApiFamilyMember());
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);

    mjbase::pass::UnusedClassRemover pass;
    const auto                       pa = pass.run(container, gam);

    // app/Api 可达 → 族两份同时保留。
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_TRUE(container.hasResource("app/Api.class"));
    EXPECT_TRUE(container.hasResource("META-INF/versions/17/app/Api.class"));
    EXPECT_TRUE(container.hasResource("app/Main.class"));
}

TEST(MjPassUnusedMemberRemoverMultiReleaseTest, SettlesFamilyMembersConsistently)
{
    // 成员级删除对族内每份独立遍历、各按同名等价的可达集结算：两份的 dead()
    // 同删、live() 同留，不因遍历序分叉。
    const auto verify = [](auto& pass, auto&& check) {
        ca::str::Utf8StringArena         arena;
        mjbase::model::ResourceContainer container;
        addClassResource(container, "app/Main.class", buildApiFamilyMain(/*withRef*/ true));
        addClassResource(container, "app/Api.class", buildApiFamilyMember());
        addClassResource(container, "META-INF/versions/17/app/Api.class",
                         buildApiFamilyMember());
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);

        EXPECT_FALSE(pass.run(container, gam).areAllPreserved());
        for (const char* uri : {"app/Api.class", "META-INF/versions/17/app/Api.class"}) {
            SCOPED_TRACE(uri);
            check(reload(container, uri));
        }
        EXPECT_TRUE(container.hasResource("app/Main.class"));
    };

    mjbase::pass::GlobalUnusedMethodRemover methodPass;
    mjbase::pass::UnusedFieldRemover        fieldPass;
    verify(methodPass, [](const jbc::core::ClassFile& cf) {
        EXPECT_TRUE(hasMethod(cf, "live", "()V"));
        EXPECT_FALSE(hasMethod(cf, "dead", "()V"));
    });   // 方法面：dead() 两份同删
    verify(fieldPass, [](const jbc::core::ClassFile& cf) {
        EXPECT_FALSE(hasField(cf, "deadField", "I"));
    });   // 字段面：deadField 两份同删
}
