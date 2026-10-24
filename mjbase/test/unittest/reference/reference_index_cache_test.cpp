/// @file        reference_index_cache_test.cpp
/// @brief       ReferenceIndex 持久化 cache（.ridx）读写用例（5 用例
///              逐字等强 + 字节级逐段对拍）。
///
/// 夹具为手拼 classfile 字节（class_bytes_builder.hpp）；
/// 写出路径走 test-helper 的 test/ 输出区。RoundTrip 用例等强断言之
/// 上追加字节级对拍基线：
///
/// - raw 头布局断言（magic/version/88 字节 pack(1) 头/6 段计数）；
/// - 全 kind 覆盖夹具（Direct/MethodHandle/Lambda/Reflection/ReflectionExecution、
///   ReflectionAccess、TypeInstruction/ClassConstant/MethodOwner/FieldOwner/
///   ClassSuper/ClassInterface/Signature/Annotation/StructuralMetadata/
///   ReflectionString），六主表逐条逐字段与 restore 前对拍；
/// - 写→读→再写字节等价（reader/writer 互逆 + 段布局逐字节稳定的双保险）。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "class_bytes_builder.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/reference/reference_index_cache_format.hpp"
#include "mjbase/reference/reference_index_cache_reader.hpp"
#include "mjbase/reference/reference_index_cache_writer.hpp"
#include "test_helper.hpp"

using jbc::core::Op;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::ClassDeclaration;
using mjbase::reference::FieldDeclaration;
using mjbase::reference::FieldReference;
using mjbase::reference::FieldReferenceKind;
using mjbase::reference::MethodDeclaration;
using mjbase::reference::MethodReference;
using mjbase::reference::MethodReferenceKind;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::ReferenceSite;
using mjbase::reference::TypeReference;
using mjbase::reference::TypeReferenceKind;
using mjbase::reference::test_support::BsmArg;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::FieldSpec;
using mjbase::reference::test_support::MethodSpec;

namespace {

constexpr ca::u16 kAccPublic    = 0x0001;
constexpr ca::u16 kAccPrivate   = 0x0002;
constexpr ca::u16 kAccStatic    = 0x0008;
constexpr ca::u16 kAccInterface = 0x0200;
constexpr ca::u16 kAccAbstract  = 0x0400;

constexpr std::string_view kForNameDesc =
    "(Ljava/lang/String;)Ljava/lang/Class;";
constexpr std::string_view kGetDeclaredMethodDesc =
    "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;";
constexpr std::string_view kGetDeclaredFieldDesc =
    "(Ljava/lang/String;)Ljava/lang/reflect/Field;";
constexpr std::string_view kMethodInvokeDesc =
    "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;";
constexpr std::string_view kGetIntDesc = "(Ljava/lang/Object;)I";

MethodSpec m(std::string_view name, std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals,
             std::function<void(CodeEmit&)> emit, ca::u16 access = kAccPublic | kAccStatic)
{
    MethodSpec out;
    out.access    = access;
    out.name      = name;
    out.desc      = desc;
    out.maxStack  = maxStack;
    out.maxLocals = maxLocals;
    out.emit      = std::move(emit);
    return out;
}

Bytes buildTargetClass()
{
    ClassBytesBuilder b("B");
    b.addField(FieldSpec{kAccPublic | kAccStatic, "value", "I"});
    std::vector<MethodSpec> ms;
    ms.push_back(m("<init>", "()V", 1, 1,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
                       e.op(Op::RETURN);
                   },
                   kAccPublic));
    ms.push_back(m("callee", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

Bytes buildCallerClass()
{
    ClassBytesBuilder b("A");
    std::vector<MethodSpec> ms;
    ms.push_back(m("caller", "()V", 2, 0, [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "B");
        e.op(Op::DUP);
        e.invoke(Op::INVOKESPECIAL, "B", "<init>", "()V");
        e.op(Op::POP);
        e.fieldInsn(Op::GETSTATIC, "B", "value", "I");
        e.op(Op::POP);
        e.invoke(Op::INVOKESTATIC, "B", "callee", "()V");
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

ReferenceIndex buildFixtureIndex(ResourceContainer& container)
{
    container.addResource("A.class",
                          std::make_unique<ClassResource>("A.class", buildCallerClass()));
    container.addResource("B.class",
                          std::make_unique<ClassResource>("B.class", buildTargetClass()));
    ReferenceIndex index;
    index.build(container);
    return index;
}

// ── 全 kind 覆盖夹具（字节级逐段对拍基线）────────────────────

Bytes buildRichIfaceClass()
{
    ClassBytesBuilder b("cache/Iface");
    b.setAccess(kAccPublic | kAccInterface | kAccAbstract);
    std::vector<MethodSpec> ms;
    ms.push_back(m("ifm", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }, kAccPublic));
    return b.assemble(ms);
}

Bytes buildRichBaseClass()
{
    ClassBytesBuilder b("cache/P");
    b.addField(FieldSpec{kAccPublic, "obj", "Lcache/P;"});
    std::vector<MethodSpec> ms;
    ms.push_back(m("virt", "()V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); }, kAccPublic));
    return b.assemble(ms);
}

/// C：Direct/MethodHandle/Lambda/Reflection/ReflectionExecution/ReflectionAccess、
/// ClassConstant/TypeInstruction/ReflectionString、类/字段级 Signature 与注解、
/// 结构元数据（NestMember）、ClassSuper/ClassInterface/字段声明描述符类型。
Bytes buildRichCallerClass()
{
    ClassBytesBuilder b("cache/C");
    b.setSuper("cache/P");
    b.setInterfaces({"cache/Iface"});
    b.setClassSignature("Ljava/lang/Object;Lcache/SignatureDep;");
    b.addClassAnnotation({"Lcache/AnnoDep;", {}});
    b.addNestMember("cache/NestDep");
    b.addField(FieldSpec{kAccPublic | kAccStatic, "counter", "I"});
    b.addField(FieldSpec{kAccPublic, "peer", "Lcache/FieldDep;", "Lcache/FieldSigDep;",
                         {{"Lcache/FieldAnnoDep;", {}}}});
    std::vector<MethodSpec> ms;
    ms.push_back(m("stat", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(m("use", "()V", 4, 4,
                   [](CodeEmit& e) {
                       // Direct（invokestatic）+ MethodOwner
                       e.invoke(Op::INVOKESTATIC, "cache/C", "stat", "()V");
                       // TypeInstruction：ANEWARRAY + CHECKCAST
                       e.i32const(1);
                       e.typeInsn(Op::ANEWARRAY, "cache/P");
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.typeInsn(Op::CHECKCAST, "cache/P");
                       e.op(Op::POP);
                       // Virtual（invokevirtual cache/P.virt）+ MethodOwner + MethodDescriptor
                       e.varInsn(Op::ALOAD, 0);
                       e.invoke(Op::INVOKEVIRTUAL, "cache/P", "virt", "()V");
                       // Interface（invokeinterface，isInterface = true）
                       e.varInsn(Op::ALOAD, 0);
                       e.invokeInterface("cache/Iface", "ifm", "()V", 1);
                       // ClassConstant（ldc 类常量）
                       e.ldcClass("cache/P");
                       e.op(Op::POP);
                       // MethodHandle（ldc handle，kind 6 = H_INVOKESTATIC）
                       e.ldcMethodHandle(6, "cache/C", "stat", "()V");
                       e.op(Op::POP);
                       // Lambda（indy + LambdaMetafactory bsm + bsm 内 MethodHandle 实参）
                       e.invokedynamic(0, "run", "()Ljava/lang/Runnable;");
                       e.op(Op::POP);
                       // ReflectionString（forName 前置 ldc 字符串）
                       e.ldcString("cache.FieldDep");
                       e.invoke(Op::INVOKESTATIC, "java/lang/Class", "forName", kForNameDesc);
                       e.op(Op::POP);
                       // 字段访问：Direct + FieldOwner + FieldDescriptor
                       e.fieldInsn(Op::GETSTATIC, "cache/C", "counter", "I");
                       e.op(Op::POP);
                       e.fieldInsn(Op::GETSTATIC, "cache/P", "obj", "Lcache/P;");
                       e.op(Op::POP);
                       // Reflection（getDeclaredMethod，内联 Class[0]，argDesc "()" 唯一命中
                       // cache/C.stat → desc 回填 "()V"）
                       e.ldcClass("cache/C");
                       e.ldcString("stat");
                       e.i32const(0);
                       e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
                       e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                                kGetDeclaredMethodDesc);
                       e.varInsn(Op::ASTORE, 1);
                       // ReflectionExecution（局部别名 Method.invoke）
                       e.varInsn(Op::ALOAD, 1);
                       e.op(Op::ACONST_NULL);
                       e.op(Op::ACONST_NULL);
                       e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Method", "invoke",
                                kMethodInvokeDesc);
                       e.op(Op::POP);
                       // Reflection（getDeclaredField）+ ReflectionAccess（Field.getInt 局部别名）
                       e.ldcClass("cache/C");
                       e.ldcString("counter");
                       e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredField",
                                kGetDeclaredFieldDesc);
                       e.varInsn(Op::ASTORE, 2);
                       e.varInsn(Op::ALOAD, 2);
                       e.op(Op::ACONST_NULL);
                       e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Field", "getInt",
                                kGetIntDesc);
                       e.op(Op::POP);
                       e.op(Op::RETURN);
                   },
                   kAccPublic));
    // bootstrap：LambdaMetafactory.metafactory（MethodType/Handle/MethodType 实参）。
    b.addBootstrapMethod("java/lang/invoke/LambdaMetafactory",
                         "metafactory",
                         "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                         "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/"
                         "invoke/MethodHandle;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/"
                         "MethodType;)Ljava/lang/invoke/CallSite;",
                         {BsmArg{'M', "()V"},
                          BsmArg{'H', "", 6, "cache/C", "stat", "()V"},
                          BsmArg{'M', "()V"}});
    return b.assemble(ms);
}

ResourceContainer buildRichFixture()
{
    ResourceContainer container;
    container.addResource("cache/Iface.class",
                          std::make_unique<ClassResource>("cache/Iface.class",
                                                          buildRichIfaceClass()));
    container.addResource("cache/P.class",
                          std::make_unique<ClassResource>("cache/P.class", buildRichBaseClass()));
    container.addResource("cache/C.class",
                          std::make_unique<ClassResource>("cache/C.class", buildRichCallerClass()));
    return container;
}

/// 读文件全部字节（写出→读回→内存加载三步的共用件）。
std::vector<ca::u8> readAllBytes(const std::string& path)
{
    std::ifstream  in(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in.is_open());
    const auto     size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<ca::u8> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    in.close();
    std::remove(path.c_str());
    return bytes;
}

const std::string kFingerprint =
    "db9003cc60cfd83ff6a5cef226400b54acfe5fe7e167957a1fd0cae349f0c031";

// ── 六主表逐条逐字段对拍 ────────────────────────────────────────

void expectSameSite(const ReferenceSite& a, const ReferenceSite& b)
{
    EXPECT_EQ(a.sourceClass, b.sourceClass);
    EXPECT_EQ(a.sourceMethod, b.sourceMethod);
    EXPECT_EQ(a.sourceDesc, b.sourceDesc);
    EXPECT_EQ(a.opcode, b.opcode);
    EXPECT_EQ(a.insnIndex, b.insnIndex);
}

void expectSameClassDecls(const ReferenceIndex& original, const ReferenceIndex& restored)
{
    ASSERT_EQ(original.classDeclarations().size(), restored.classDeclarations().size());
    for (ca::usize i = 0; i < original.classDeclarations().size(); ++i) {
        const ClassDeclaration& a = original.classDeclarations()[i];
        const ClassDeclaration& b = restored.classDeclarations()[i];
        EXPECT_EQ(a.name, b.name) << "classDecls[" << i << "].name";
        EXPECT_EQ(a.superName, b.superName) << "classDecls[" << i << "].superName";
        ASSERT_EQ(a.interfaces.size(), b.interfaces.size()) << "classDecls[" << i << "]";
        for (ca::usize j = 0; j < a.interfaces.size(); ++j) {
            EXPECT_EQ(a.interfaces[j], b.interfaces[j]) << "classDecls[" << i << "].iface[" << j
                                                        << "]";
        }
        EXPECT_EQ(a.access, b.access) << "classDecls[" << i << "].access";
    }
}

void expectSameMethodDecls(const ReferenceIndex& original, const ReferenceIndex& restored)
{
    ASSERT_EQ(original.methodDeclarations().size(), restored.methodDeclarations().size());
    for (ca::usize i = 0; i < original.methodDeclarations().size(); ++i) {
        const MethodDeclaration& a = original.methodDeclarations()[i];
        const MethodDeclaration& b = restored.methodDeclarations()[i];
        EXPECT_EQ(a.method.owner, b.method.owner) << "methodDecls[" << i << "]";
        EXPECT_EQ(a.method.name, b.method.name) << "methodDecls[" << i << "]";
        EXPECT_EQ(a.method.desc, b.method.desc) << "methodDecls[" << i << "]";
        EXPECT_EQ(a.access, b.access) << "methodDecls[" << i << "].access";
    }
}

void expectSameFieldDecls(const ReferenceIndex& original, const ReferenceIndex& restored)
{
    ASSERT_EQ(original.fieldDeclarations().size(), restored.fieldDeclarations().size());
    for (ca::usize i = 0; i < original.fieldDeclarations().size(); ++i) {
        const FieldDeclaration& a = original.fieldDeclarations()[i];
        const FieldDeclaration& b = restored.fieldDeclarations()[i];
        EXPECT_EQ(a.field.owner, b.field.owner) << "fieldDecls[" << i << "]";
        EXPECT_EQ(a.field.name, b.field.name) << "fieldDecls[" << i << "]";
        EXPECT_EQ(a.field.desc, b.field.desc) << "fieldDecls[" << i << "]";
        EXPECT_EQ(a.access, b.access) << "fieldDecls[" << i << "].access";
    }
}

void expectSameMethodRefs(const ReferenceIndex& original, const ReferenceIndex& restored)
{
    ASSERT_EQ(original.methodReferences().size(), restored.methodReferences().size());
    for (ca::usize i = 0; i < original.methodReferences().size(); ++i) {
        const MethodReference& a = original.methodReferences()[i];
        const MethodReference& b = restored.methodReferences()[i];
        expectSameSite(a.site, b.site);
        EXPECT_EQ(a.target.owner, b.target.owner) << "methodRefs[" << i << "]";
        EXPECT_EQ(a.target.name, b.target.name) << "methodRefs[" << i << "]";
        EXPECT_EQ(a.target.desc, b.target.desc) << "methodRefs[" << i << "]";
        EXPECT_EQ(a.reflectionArgDesc, b.reflectionArgDesc) << "methodRefs[" << i << "]";
        EXPECT_EQ(a.isInterface, b.isInterface) << "methodRefs[" << i << "]";
        EXPECT_EQ(a.kind, b.kind) << "methodRefs[" << i << "].kind";
    }
}

void expectSameFieldRefs(const ReferenceIndex& original, const ReferenceIndex& restored)
{
    ASSERT_EQ(original.fieldReferences().size(), restored.fieldReferences().size());
    for (ca::usize i = 0; i < original.fieldReferences().size(); ++i) {
        const FieldReference& a = original.fieldReferences()[i];
        const FieldReference& b = restored.fieldReferences()[i];
        expectSameSite(a.site, b.site);
        EXPECT_EQ(a.target.owner, b.target.owner) << "fieldRefs[" << i << "]";
        EXPECT_EQ(a.target.name, b.target.name) << "fieldRefs[" << i << "]";
        EXPECT_EQ(a.target.desc, b.target.desc) << "fieldRefs[" << i << "]";
        EXPECT_EQ(a.kind, b.kind) << "fieldRefs[" << i << "].kind";
    }
}

void expectSameTypeRefs(const ReferenceIndex& original, const ReferenceIndex& restored)
{
    ASSERT_EQ(original.typeReferences().size(), restored.typeReferences().size());
    for (ca::usize i = 0; i < original.typeReferences().size(); ++i) {
        const TypeReference& a = original.typeReferences()[i];
        const TypeReference& b = restored.typeReferences()[i];
        expectSameSite(a.site, b.site);
        EXPECT_EQ(a.targetType, b.targetType) << "typeRefs[" << i << "]";
        EXPECT_EQ(a.kind, b.kind) << "typeRefs[" << i << "].kind";
    }
}

/// 方法/字段引用 kind 分布断言（全 kind 覆盖夹具的覆盖面回读，防夹具静默缩水）。
void expectMethodKindPresent(const ReferenceIndex& index, MethodReferenceKind kind,
                             std::string_view targetName)
{
    for (const MethodReference& ref : index.methodReferences()) {
        if (ref.kind == kind && ref.target.name == targetName) return;
    }
    ADD_FAILURE() << "expected method kind " << static_cast<int>(kind) << " on " << targetName;
}

void expectFieldKindPresent(const ReferenceIndex& index, FieldReferenceKind kind,
                            std::string_view targetName)
{
    for (const FieldReference& ref : index.fieldReferences()) {
        if (ref.kind == kind && ref.target.name == targetName) return;
    }
    ADD_FAILURE() << "expected field kind " << static_cast<int>(kind) << " on " << targetName;
}

}   // namespace

TEST(MjReferenceCacheTest, RoundTripPreservesAllTables)
{
    ResourceContainer container;
    ReferenceIndex    original;
    original.build(buildRichFixture());

    // 夹具覆盖面回读：全 kind 至少一条（kind 字节写入端契约的输入保障）。
    expectMethodKindPresent(original, MethodReferenceKind::Direct, "stat");
    expectMethodKindPresent(original, MethodReferenceKind::MethodHandle, "stat");
    expectMethodKindPresent(original, MethodReferenceKind::Lambda, "stat");
    expectMethodKindPresent(original, MethodReferenceKind::Reflection, "stat");
    expectMethodKindPresent(original, MethodReferenceKind::ReflectionExecution, "stat");
    expectFieldKindPresent(original, FieldReferenceKind::Direct, "counter");
    expectFieldKindPresent(original, FieldReferenceKind::ReflectionAccess, "counter");
    ASSERT_GE(original.typeReferenceCount(), ca::usize{6});

    const std::string path =
        (test::getTempOutPath("reference_index_round_trip.ridx")).string();
    ASSERT_TRUE(writeReferenceIndexCache(path.c_str(), original, kFingerprint));

    const std::vector<ca::u8> bytes = readAllBytes(path);

    // ── 字节级头布局对拍（持久化工件契约：88 字节 pack(1) 头 + 6 段计数）──
    ASSERT_GE(bytes.size(), sizeof(mjbase::reference::ridx::Header));
    mjbase::reference::ridx::Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    EXPECT_EQ(header.magic, mjbase::reference::ridx::kMagic);
    EXPECT_EQ(header.version, mjbase::reference::ridx::kVersion);
    // 指纹以 hex 编码头字节落盘：取 kFingerprint 前两字符 "db" → 0xdb。
    EXPECT_EQ(header.fingerprintSha256[0], 0xdb);

    ReferenceIndex restored;
    std::string    loadedFingerprint;
    ASSERT_TRUE(
        loadReferenceIndexCache(restored, loadedFingerprint, bytes.data(), bytes.size()));
    EXPECT_EQ(loadedFingerprint, kFingerprint);

    // 头部计数与主表一致
    EXPECT_EQ(header.classDeclsCount, restored.classDeclarationCount());
    EXPECT_EQ(header.methodDeclsCount, restored.methodDeclarationCount());
    EXPECT_EQ(header.fieldDeclsCount, restored.fieldDeclarationCount());
    EXPECT_EQ(header.methodRefsCount, restored.methodReferenceCount());
    EXPECT_EQ(header.fieldRefsCount, restored.fieldReferenceCount());
    EXPECT_EQ(header.typeRefsCount, restored.typeReferenceCount());

    // ── 逐段对拍：六主表逐条逐字段 ──
    expectSameClassDecls(original, restored);
    expectSameMethodDecls(original, restored);
    expectSameFieldDecls(original, restored);
    expectSameMethodRefs(original, restored);
    expectSameFieldRefs(original, restored);
    expectSameTypeRefs(original, restored);

    // 派生 lookup 也能用：method refs 到 C.stat 应当能查到（含 kind 区分）。
    ca::usize directHits    = 0;
    ca::usize execHits      = 0;
    for (const MethodReference& ref : restored.methodReferencesTo("cache/C", "stat", "()V")) {
        if (ref.kind == MethodReferenceKind::Direct) ++directHits;
        if (ref.kind == MethodReferenceKind::ReflectionExecution) ++execHits;
    }
    EXPECT_GE(directHits, ca::usize{1});
    EXPECT_GE(execHits, ca::usize{1});
    // field refs 到 C.counter
    ASSERT_FALSE(restored.fieldReferencesTo("cache/C", "counter", "I").empty());
    EXPECT_EQ(restored.fieldReferencesTo("cache/C", "counter", "I").front().site.sourceClass,
              "cache/C");

    // 声明查找
    EXPECT_NE(restored.declaredClass("cache/C"), nullptr);
    EXPECT_NE(restored.declaredMethod("cache/C", "stat", "()V"), nullptr);
    EXPECT_NE(restored.declaredField("cache/C", "counter", "I"), nullptr);

    // ── 字节级重写对拍：restore 后再写出，与首写逐字节等价 ──
    const std::string path2 =
        (test::getTempOutPath("reference_index_round_trip_rewrite.ridx")).string();
    ASSERT_TRUE(writeReferenceIndexCache(path2.c_str(), restored, kFingerprint));
    const std::vector<ca::u8> bytes2 = readAllBytes(path2);
    ASSERT_EQ(bytes.size(), bytes2.size());
    EXPECT_EQ(0, std::memcmp(bytes.data(), bytes2.data(), bytes.size()))
        << "re-serialized cache must be byte-identical";
}

TEST(MjReferenceCacheTest, LoadRejectsWrongMagic)
{
    ResourceContainer container;
    ReferenceIndex    original = buildFixtureIndex(container);
    const std::string path     = test::getTempOutPath("bad_magic.ridx").string();
    ASSERT_TRUE(writeReferenceIndexCache(path.c_str(), original, kFingerprint));

    const std::vector<ca::u8> bytes = readAllBytes(path);

    // 改坏 magic
    std::vector<ca::u8> corrupt = bytes;
    corrupt[0]                  = 0x00;

    ReferenceIndex restored;
    std::string    fp;
    EXPECT_FALSE(
        loadReferenceIndexCache(restored, fp, corrupt.data(), corrupt.size()));
}

TEST(MjReferenceCacheTest, LoadRejectsWrongVersion)
{
    ResourceContainer container;
    ReferenceIndex    original = buildFixtureIndex(container);
    const std::string path     = test::getTempOutPath("bad_version.ridx").string();
    ASSERT_TRUE(writeReferenceIndexCache(path.c_str(), original, kFingerprint));

    const std::vector<ca::u8> bytes = readAllBytes(path);

    // 改坏 version（Header 偏移 4）
    std::vector<ca::u8> corrupt = bytes;
    corrupt[4]                  = 0xff;

    ReferenceIndex restored;
    std::string    fp;
    EXPECT_FALSE(
        loadReferenceIndexCache(restored, fp, corrupt.data(), corrupt.size()));
}

TEST(MjReferenceCacheTest, LoadRejectsTruncatedData)
{
    ResourceContainer container;
    ReferenceIndex    original = buildFixtureIndex(container);
    const std::string path     = test::getTempOutPath("truncated.ridx").string();
    ASSERT_TRUE(writeReferenceIndexCache(path.c_str(), original, kFingerprint));

    const std::vector<ca::u8> bytes = readAllBytes(path);

    // 截断到 Header 之后一点点
    ReferenceIndex restored;
    std::string    fp;
    EXPECT_FALSE(loadReferenceIndexCache(restored,
                                         fp,
                                         bytes.data(),
                                         sizeof(mjbase::reference::ridx::Header) + 1));
    EXPECT_FALSE(loadReferenceIndexCache(restored, fp, bytes.data(), 0));
    EXPECT_FALSE(loadReferenceIndexCache(restored, fp, nullptr, 100));
}

TEST(MjReferenceCacheTest, WriterRejectsBadFingerprint)
{
    ResourceContainer container;
    ReferenceIndex    original = buildFixtureIndex(container);
    const std::string path     = test::getTempOutPath("bad_fp.ridx").string();
    EXPECT_FALSE(writeReferenceIndexCache(path.c_str(), original, "short"));
    EXPECT_FALSE(writeReferenceIndexCache(path.c_str(),
                                          original,
                                          "zzzz3cc60cfd83ff6a5cef226400b54acfe5fe7e167957a1fd0"
                                          "cae349f0c031"));   // 含非 hex 字符
    std::remove(path.c_str());
}
