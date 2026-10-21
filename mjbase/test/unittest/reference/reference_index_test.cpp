/// @file        reference_index_test.cpp
/// @brief       ReferenceIndex 全量用例。
///
/// 类字节由 class_bytes_builder.hpp 手拼；方法边断言按同一过滤规则落在索引
/// 事实上（indexHasMethodEdge，见函数注）。
#include <gtest/gtest.h>

#include <functional>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

using jbc::core::Op;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::ClassDeclaration;
using mjbase::reference::FieldReference;
using mjbase::reference::FieldReferenceKind;
using mjbase::reference::MethodDeclaration;
using mjbase::reference::MethodReference;
using mjbase::reference::MethodReferenceKind;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::ReferenceIndexAnalysis;
using mjbase::reference::TypeReferenceKind;
using mjbase::spi::GlobalAnalysisManager;
using mjbase::reference::test_support::AnnotationSpec;
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
constexpr ca::u16 kAccModule    = 0x8000;

constexpr const char* kGetDeclaredMethodDesc =
    "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;";
constexpr const char* kGetDeclaredConstructorDesc =
    "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;";
constexpr const char* kGetDeclaredFieldDesc =
    "(Ljava/lang/String;)Ljava/lang/reflect/Field;";
constexpr const char* kMethodInvokeDesc =
    "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;";
constexpr const char* kCtorNewInstanceDesc  = "([Ljava/lang/Object;)Ljava/lang/Object;";
constexpr const char* kClassNewInstanceDesc = "()Ljava/lang/Object;";
constexpr const char* kForNameDesc          = "(Ljava/lang/String;)Ljava/lang/Class;";
constexpr const char* kGetIntDesc           = "(Ljava/lang/Object;)I";
constexpr const char* kSetIntDesc           = "(Ljava/lang/Object;I)V";
constexpr const char* kBsmDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;"
    "Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;"
    ")Ljava/lang/invoke/CallSite;";

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

/// 带 Signature/注解元数据的方法规格。
MethodSpec mMeta(std::string_view name, std::string_view desc, std::string_view signature,
                 std::vector<AnnotationSpec> annotations,
                 std::function<void(CodeEmit&)> emit)
{
    MethodSpec out;
    out.access      = kAccPublic | kAccStatic;
    out.name        = name;
    out.desc        = desc;
    out.signature   = signature;
    out.maxStack    = 0;
    out.maxLocals   = 0;
    out.emit        = std::move(emit);
    out.annotations = std::move(annotations);
    return out;
}

void addClassBytes(ResourceContainer& container, std::string_view name, const Bytes& bytes)
{
    container.addResource(std::string(name) + ".class",
                          std::make_unique<ClassResource>(std::string(name) + ".class", bytes));
}

/// 成边规则（Direct/MethodHandle/Lambda/
/// ReflectionExecution(非空 desc) 成边，Reflection 查找不成边）按索引事实
/// 断言。mjbase CallGraph 面向自建会话、不消费索引（call_graph 批语义），
/// 边断言统一走本过滤规则，断言强度不变。
bool indexHasMethodEdge(const ReferenceIndex& index, std::string_view sourceOwner,
                        std::string_view sourceName, std::string_view sourceDesc,
                        std::string_view targetOwner, std::string_view targetName,
                        std::string_view targetDesc)
{
    for (const MethodReference& ref : index.methodReferences()) {
        if (ref.kind == MethodReferenceKind::Reflection) continue;
        if (ref.kind == MethodReferenceKind::ReflectionExecution && ref.target.desc.empty()) {
            continue;
        }
        if (ref.site.sourceClass == sourceOwner && ref.site.sourceMethod == sourceName &&
            ref.site.sourceDesc == sourceDesc && ref.target.owner == targetOwner &&
            ref.target.name == targetName && ref.target.desc == targetDesc) {
            return true;
        }
    }
    return false;
}

// ── 基础夹具（A 调 B）────────────────────────────────────────────

Bytes buildTargetClass()
{
    ClassBytesBuilder b("B");
    b.addField(FieldSpec{kAccPublic | kAccStatic, "value", "I"});
    b.addField(FieldSpec{kAccPublic, "child", "LB;"});
    std::vector<MethodSpec> ms;
    ms.push_back(m("<init>", "()V", 1, 1,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
                       e.op(Op::RETURN);
                   },
                   kAccPublic));
    ms.push_back(m("callee", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(m("consume", "(Ljava/lang/String;)LB;", 1, 0, [](CodeEmit& e) {
        e.op(Op::ACONST_NULL);
        e.op(Op::ARETURN);
    }));
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
        e.ldcClass("B");
        e.op(Op::POP);
        e.ldcString("text");
        e.invoke(Op::INVOKESTATIC, "B", "consume", "(Ljava/lang/String;)LB;");
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

ResourceContainer buildReferenceFixture()
{
    ResourceContainer container;
    addClassBytes(container, "A", buildCallerClass());
    addClassBytes(container, "B", buildTargetClass());
    return container;
}

// ── 声明类型夹具（Iface/Parent/Child）───────────────────────────

Bytes buildSimpleClass(std::string_view name, ca::u16 access = kAccPublic)
{
    ClassBytesBuilder b(name);
    b.setAccess(access);
    return b.assemble({});
}

Bytes buildDerivedClass()
{
    ClassBytesBuilder b("Child");
    b.setSuper("Parent");
    b.setInterfaces({"Iface"});
    b.addField(FieldSpec{kAccPrivate, "iface", "LIface;"});
    return b.assemble({});
}

ResourceContainer buildDeclarationTypeFixture()
{
    ResourceContainer container;
    addClassBytes(container, "Iface",
                  buildSimpleClass("Iface", kAccPublic | kAccInterface | kAccAbstract));
    addClassBytes(container, "Parent", buildSimpleClass("Parent"));
    addClassBytes(container, "Child", buildDerivedClass());
    return container;
}

// ── 元数据夹具（Signature/注解）─────────────────────────────────

Bytes buildMetadataReferenceClass()
{
    ClassBytesBuilder b("MetaUser");
    b.setClassSignature(
        "Ljava/lang/Object;Lcom/example/GenericIface<Lcom/example/GenericArg;>;");
    b.addClassAnnotation({"Lcom/example/Anno;",
                          {{"target", {'c', "com/example/AnnotationValue"}}}});
    b.addField(FieldSpec{kAccPrivate, "items", "Ljava/util/List;",
                         "Ljava/util/List<Lcom/example/FieldType;>;",
                         {{"Lcom/example/FieldAnno;", {}}}});
    std::vector<MethodSpec> ms;
    ms.push_back(mMeta("use", "()V", "()Lcom/example/MethodReturn;",
                       {{"Lcom/example/MethodAnno;", {}}},
                       [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

// ── 结构元数据夹具（EnclosingMethod/Nest/Permitted/InnerClasses/Record）──

Bytes buildStructuralMetadataReferenceClass()
{
    ClassBytesBuilder b("StructuralUser");
    b.setEnclosingMethod("com/example/Outer", "ownerMethod", "()V");
    b.setNestHost("com/example/NestHost");
    b.addNestMember("com/example/NestMember");
    b.addPermittedSubclass("com/example/Permitted");
    b.addInnerClass("com/example/Outer$Inner", "com/example/Outer", "Inner", kAccPublic);
    b.addRecordComponent({"recordComponent",
                          "Lcom/example/RecordType;",
                          "Lcom/example/RecordGeneric;",
                          {{"Lcom/example/RecordAnno;", {}}}});
    return b.assemble({});
}

// ── Module 元数据夹具 ───────────────────────────────────────────

Bytes buildModuleMetadataReferenceClass()
{
    ClassBytesBuilder b("module-info");
    b.setAccess(kAccModule);
    b.setNoSuper();
    b.setModule("example.module",
                "example/Main",
                {"example/Service"},
                {{"example/Service", {"example/Impl"}}});
    return b.assemble({});
}

// ── 反射字符串/MethodHandle/Lambda 夹具 ─────────────────────────

Bytes buildIndirectBytecodeReferenceClass()
{
    ClassBytesBuilder b("IndirectUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("lambdaTarget", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(m("use", "()V", 1, 0, [](CodeEmit& e) {
        e.ldcString("com.example.Reflected");
        e.invoke(Op::INVOKESTATIC, "java/lang/Class", "forName", kForNameDesc);
        e.op(Op::POP);
        e.ldcMethodHandle(6, "IndirectUser", "lambdaTarget", "()V");   // H_INVOKESTATIC
        e.op(Op::POP);
        e.invokedynamic(0, "run", "()Ljava/lang/Runnable;");
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    // bootstrap：LambdaMetafactory.metafactory（MethodType/Handle/MethodType 实参）。
    b.addBootstrapMethod("java/lang/invoke/LambdaMetafactory",
                         "metafactory",
                         kBsmDesc,
                         {BsmArg{'M', "()V"},
                          BsmArg{'H', "", 6, "IndirectUser", "lambdaTarget", "()V"},
                          BsmArg{'M', "()V"}});
    return b.assemble(ms);
}

// ── 常量反射成员查找夹具 ────────────────────────────────────────

/// ReflectTarget：secret 字段 + <init> + reflectTarget/otherTarget 静态方法。
Bytes buildReflectionMemberTargetClass()
{
    ClassBytesBuilder b("com/example/ReflectTarget");
    b.addField(FieldSpec{kAccPrivate, "secret", "I"});
    std::vector<MethodSpec> ms;
    ms.push_back(m("<init>", "()V", 1, 1,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
                       e.op(Op::RETURN);
                   },
                   kAccPublic));
    ms.push_back(m("reflectTarget", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(m("otherTarget", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

/// ReflectUser：getDeclaredMethod 常量查找 + forName 后 getDeclaredField。
Bytes buildReflectionMemberReferenceClass()
{
    ClassBytesBuilder b("ReflectUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("use", "()V", 3, 0, [](CodeEmit& e) {
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("reflectTarget");
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.op(Op::POP);
        e.ldcString("com.example.ReflectTarget");
        e.invoke(Op::INVOKESTATIC, "java/lang/Class", "forName", kForNameDesc);
        e.ldcString("secret");
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredField",
                 kGetDeclaredFieldDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 反射执行夹具：Method.invoke / Constructor.newInstance / Class.newInstance /
/// Field.get+set 全形态（局部别名传播）。
Bytes buildReflectionExecutionReferenceClass()
{
    ClassBytesBuilder b("ReflectExecutionUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("use", "()V", 3, 4, [](CodeEmit& e) {
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("reflectTarget");
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.varInsn(Op::ASTORE, 0);
        e.varInsn(Op::ALOAD, 0);
        e.op(Op::ACONST_NULL);
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Method", "invoke", kMethodInvokeDesc);
        e.op(Op::POP);

        e.ldcClass("com/example/ReflectTarget");
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredConstructor",
                 kGetDeclaredConstructorDesc);
        e.varInsn(Op::ASTORE, 1);
        e.varInsn(Op::ALOAD, 1);
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Constructor", "newInstance",
                 kCtorNewInstanceDesc);
        e.op(Op::POP);

        e.ldcString("com.example.ReflectTarget");
        e.invoke(Op::INVOKESTATIC, "java/lang/Class", "forName", kForNameDesc);
        e.varInsn(Op::ASTORE, 3);
        e.varInsn(Op::ALOAD, 3);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "newInstance", kClassNewInstanceDesc);
        e.op(Op::POP);

        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("secret");
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredField",
                 kGetDeclaredFieldDesc);
        e.varInsn(Op::ASTORE, 2);
        e.varInsn(Op::ALOAD, 2);
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Field", "getInt", kGetIntDesc);
        e.op(Op::POP);
        e.varInsn(Op::ALOAD, 2);
        e.op(Op::ACONST_NULL);
        e.op(Op::ICONST_1);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Field", "setInt", kSetIntDesc);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 控制流合并冲突夹具：两分支各 store 一个不同目标的 Method 对象到局部 1。
Bytes buildAmbiguousReflectionExecutionClass()
{
    ClassBytesBuilder b("ReflectMergeUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("use", "(Z)V", 3, 2, [](CodeEmit& e) {
        const std::size_t elseLabel = e.newLabel();
        const std::size_t joinLabel = e.newLabel();
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, elseLabel);
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("reflectTarget");
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.varInsn(Op::ASTORE, 1);
        e.branch(Op::GOTO, joinLabel);
        e.bindLabel(elseLabel);
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("otherTarget");
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.varInsn(Op::ASTORE, 1);
        e.bindLabel(joinLabel);
        e.varInsn(Op::ALOAD, 1);
        e.op(Op::ACONST_NULL);
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Method", "invoke", kMethodInvokeDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 重载目标：reflectTarget 的 4 个重载（无参/int/String/String+int）。
Bytes buildOverloadedReflectionMemberTargetClass()
{
    ClassBytesBuilder b("com/example/ReflectTarget");
    std::vector<MethodSpec> ms;
    ms.push_back(m("reflectTarget", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(
        m("reflectTarget", "(I)V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(m("reflectTarget", "(Ljava/lang/String;)V", 0, 1,
                   [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(m("reflectTarget", "(Ljava/lang/String;I)V", 0, 2,
                   [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

/// 内联 DUP/AASTORE 构造的 Class[] 实参（String.class 元素）。
Bytes buildReflectionMemberReferenceWithClassArray()
{
    ClassBytesBuilder b("ReflectUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("use", "()V", 6, 0, [](CodeEmit& e) {
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("reflectTarget");
        e.op(Op::ICONST_1);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
        e.op(Op::DUP);
        e.op(Op::ICONST_0);
        e.ldcClass("java/lang/String");
        e.op(Op::AASTORE);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 内联 Class[] 实参，元素为 Integer.TYPE（GETSTATIC 基本类型字段）。
Bytes buildReflectionMemberReferenceWithPrimitiveTypeArray()
{
    ClassBytesBuilder b("ReflectUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("use", "()V", 6, 0, [](CodeEmit& e) {
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("reflectTarget");
        e.op(Op::ICONST_1);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
        e.op(Op::DUP);
        e.op(Op::ICONST_0);
        e.fieldInsn(Op::GETSTATIC, "java/lang/Integer", "TYPE", "Ljava/lang/Class;");
        e.op(Op::AASTORE);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 局部数组形态：ANEWARRAY → ASTORE → 常量 ALOAD/AASTORE 填充。
/// complete=false 时索引 1 缺席（不完整填充，保持未解析）。
Bytes buildReflectionMemberReferenceWithLocalClassArray(bool complete)
{
    ClassBytesBuilder b("ReflectUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("use", "()V", 5, 1, [&](CodeEmit& e) {
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("reflectTarget");
        e.op(Op::ICONST_2);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
        e.varInsn(Op::ASTORE, 0);
        if (complete) {
            e.varInsn(Op::ALOAD, 0);
            e.op(Op::ICONST_1);
            e.fieldInsn(Op::GETSTATIC, "java/lang/Integer", "TYPE", "Ljava/lang/Class;");
            e.op(Op::AASTORE);
        }
        e.varInsn(Op::ALOAD, 0);
        e.op(Op::ICONST_0);
        e.ldcClass("java/lang/String");
        e.op(Op::AASTORE);
        e.varInsn(Op::ALOAD, 0);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 重载构造目标：<init>()V / (String) / (String,int)。
Bytes buildOverloadedReflectionConstructorTargetClass()
{
    ClassBytesBuilder b("com/example/ReflectTarget");
    std::vector<MethodSpec> ms;
    const auto writeCtor = [&ms](const char* desc, ca::u16 maxLocals) {
        ms.push_back(m("<init>", desc, 1, maxLocals,
                       [](CodeEmit& e) {
                           e.varInsn(Op::ALOAD, 0);
                           e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
                           e.op(Op::RETURN);
                       },
                       kAccPublic));
    };
    writeCtor("()V", 1);
    writeCtor("(Ljava/lang/String;)V", 2);
    writeCtor("(Ljava/lang/String;I)V", 3);
    return b.assemble(ms);
}

/// 内联 Class[] 实参的 getDeclaredConstructor（String.class 元素）。
Bytes buildReflectionConstructorReferenceWithClassArray()
{
    ClassBytesBuilder b("ReflectUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("use", "()V", 5, 0, [](CodeEmit& e) {
        e.ldcClass("com/example/ReflectTarget");
        e.op(Op::ICONST_1);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
        e.op(Op::DUP);
        e.op(Op::ICONST_0);
        e.ldcClass("java/lang/String");
        e.op(Op::AASTORE);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredConstructor",
                 kGetDeclaredConstructorDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 局部数组形态的 getDeclaredConstructor（索引 0 String.class、1 Integer.TYPE）。
Bytes buildReflectionConstructorReferenceWithLocalClassArray()
{
    ClassBytesBuilder b("ReflectUser");
    std::vector<MethodSpec> ms;
    ms.push_back(m("use", "()V", 4, 1, [](CodeEmit& e) {
        e.ldcClass("com/example/ReflectTarget");
        e.op(Op::ICONST_2);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
        e.varInsn(Op::ASTORE, 0);
        e.varInsn(Op::ALOAD, 0);
        e.op(Op::ICONST_0);
        e.ldcClass("java/lang/String");
        e.op(Op::AASTORE);
        e.varInsn(Op::ALOAD, 0);
        e.op(Op::ICONST_1);
        e.fieldInsn(Op::GETSTATIC, "java/lang/Integer", "TYPE", "Ljava/lang/Class;");
        e.op(Op::AASTORE);
        e.varInsn(Op::ALOAD, 0);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredConstructor",
                 kGetDeclaredConstructorDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// JSR/RET fail-open 夹具（#975 评审跟进卡）：两个方法同款「常量
/// getDeclaredMethod → Method.invoke」反射执行模式，jsrReflect 额外含
/// JSR/RET 子例程。新引擎对 JSR 保守化（快照拒绝、fail-open），
/// 预期：直接/lookup 事实保留、数据流证明的 ReflectionExecution 不产出。
Bytes buildJsrReflectionUserClass()
{
    ClassBytesBuilder b("JsrReflectUser");
    std::vector<MethodSpec> ms;
    // 无 JSR 对照方法：反射执行可被数据流证明。
    ms.push_back(m("plainReflect", "()V", 3, 1, [](CodeEmit& e) {
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("reflectTarget");
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.varInsn(Op::ASTORE, 0);
        e.varInsn(Op::ALOAD, 0);
        e.op(Op::ACONST_NULL);
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Method", "invoke", kMethodInvokeDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    // JSR 方法：同款反射模式 + JSR/RET 子例程。
    ms.push_back(m("jsrReflect", "()V", 3, 2, [](CodeEmit& e) {
        const std::size_t sub = e.newLabel();
        e.ldcClass("com/example/ReflectTarget");
        e.ldcString("reflectTarget");
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 kGetDeclaredMethodDesc);
        e.varInsn(Op::ASTORE, 0);
        e.varInsn(Op::ALOAD, 0);
        e.op(Op::ACONST_NULL);
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Method", "invoke", kMethodInvokeDesc);
        e.op(Op::POP);
        e.op(Op::ICONST_0);
        e.branch(Op::JSR, sub);
        e.op(Op::RETURN);
        e.bindLabel(sub);
        e.varInsn(Op::ASTORE, 1);
        e.varInsn(Op::RET, 1);
    }));
    return b.assemble(ms);
}

}   // namespace

// ── 1. 直接方法/字段/类型引用 ───────────────────────────────────

TEST(MjReferenceTest, indexesDirectMethodFieldAndTypeReferences)
{
    ResourceContainer container = buildReferenceFixture();

    ReferenceIndex index;
    index.build(container);

    const std::vector<MethodReference>& calleeRefs =
        index.methodReferencesTo("B", "callee", "()V");
    ASSERT_EQ(calleeRefs.size(), 1u);
    EXPECT_EQ(calleeRefs[0].site.sourceClass, "A");
    EXPECT_EQ(calleeRefs[0].site.sourceMethod, "caller");
    EXPECT_EQ(calleeRefs[0].target.name, "callee");
    EXPECT_EQ(calleeRefs[0].site.opcode, static_cast<ca::i32>(Op::INVOKESTATIC));

    const std::vector<FieldReference>& fieldRefs = index.fieldReferencesTo("B", "value", "I");
    ASSERT_EQ(fieldRefs.size(), 1u);
    EXPECT_EQ(fieldRefs[0].site.sourceClass, "A");
    EXPECT_EQ(fieldRefs[0].site.opcode, static_cast<ca::i32>(Op::GETSTATIC));

    const std::vector<mjbase::reference::TypeReference>& typeRefs =
        index.typeReferencesTo("B");
    EXPECT_GE(typeRefs.size(), 3u);
}

// ── 2. 声明与源方法视图 ─────────────────────────────────────────

TEST(MjReferenceTest, indexesDeclarationsAndSourceViews)
{
    ResourceContainer container = buildReferenceFixture();

    ReferenceIndex index;
    index.build(container);

    const MethodDeclaration* caller = index.declaredMethod("A", "caller", "()V");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(caller->access & kAccStatic, kAccStatic);
    EXPECT_EQ(index.declaredMethod("Missing", "m", "()V"), nullptr);

    const ClassDeclaration* classA = index.declaredClass("A");
    ASSERT_NE(classA, nullptr);
    EXPECT_EQ(classA->access & kAccPublic, kAccPublic);
    EXPECT_EQ(index.declaredClass("Missing"), nullptr);
    ASSERT_EQ(index.classDeclarations().size(), 2u);
    EXPECT_EQ(index.classDeclarationCount(), 2u);

    const mjbase::reference::FieldDeclaration* child =
        index.declaredField("B", "child", "LB;");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->field.owner, "B");

    const std::vector<MethodReference>& callsFromCaller =
        index.methodReferencesFrom("A", "caller", "()V");
    ASSERT_EQ(callsFromCaller.size(), 3u);
    EXPECT_EQ(callsFromCaller[0].target.name, "<init>");
    EXPECT_EQ(callsFromCaller[1].target.name, "callee");
    EXPECT_EQ(callsFromCaller[2].target.name, "consume");

    const std::vector<FieldReference>& fieldsFromCaller =
        index.fieldReferencesFrom("A", "caller", "()V");
    ASSERT_EQ(fieldsFromCaller.size(), 1u);
    EXPECT_EQ(fieldsFromCaller[0].target.name, "value");

    const std::vector<mjbase::reference::TypeReference>& typeRefsFromCaller =
        index.typeReferencesFrom("A", "caller", "()V");
    bool sawClassConstant    = false;
    bool sawStringDescriptor = false;
    bool sawReturnDescriptor = false;
    for (const auto& ref : typeRefsFromCaller) {
        if (ref.kind == TypeReferenceKind::ClassConstant && ref.targetType == "B") {
            sawClassConstant = true;
        }
        if (ref.kind == TypeReferenceKind::MethodDescriptor &&
            ref.targetType == "java/lang/String") {
            sawStringDescriptor = true;
        }
        if (ref.kind == TypeReferenceKind::MethodDescriptor && ref.targetType == "B") {
            sawReturnDescriptor = true;
        }
    }
    EXPECT_TRUE(sawClassConstant);
    EXPECT_TRUE(sawStringDescriptor);
    EXPECT_TRUE(sawReturnDescriptor);
}

// ── 3. owner/类级视图 ───────────────────────────────────────────

TEST(MjReferenceTest, indexesOwnerAndClassLevelViews)
{
    ResourceContainer container = buildReferenceFixture();

    ReferenceIndex index;
    index.build(container);

    const std::vector<MethodDeclaration>& declaredMethods = index.declaredMethodsIn("B");
    ASSERT_EQ(declaredMethods.size(), 3u);
    EXPECT_EQ(declaredMethods[0].method.name, "<init>");
    EXPECT_EQ(declaredMethods[1].method.name, "callee");
    EXPECT_EQ(declaredMethods[2].method.name, "consume");

    const std::vector<mjbase::reference::FieldDeclaration>& declaredFields =
        index.declaredFieldsIn("B");
    ASSERT_EQ(declaredFields.size(), 2u);
    EXPECT_EQ(declaredFields[0].field.name, "value");
    EXPECT_EQ(declaredFields[1].field.name, "child");

    const std::vector<MethodReference>& callsFromClass =
        index.methodReferencesFromClass("A");
    ASSERT_EQ(callsFromClass.size(), 3u);
    EXPECT_EQ(callsFromClass[0].target.name, "<init>");
    EXPECT_EQ(callsFromClass[1].target.name, "callee");
    EXPECT_EQ(callsFromClass[2].target.name, "consume");

    const std::vector<FieldReference>& fieldsFromClass =
        index.fieldReferencesFromClass("A");
    ASSERT_EQ(fieldsFromClass.size(), 1u);
    EXPECT_EQ(fieldsFromClass[0].target.name, "value");

    const std::vector<mjbase::reference::TypeReference>& typesFromClass =
        index.typeReferencesFromClass("A");
    EXPECT_GE(typesFromClass.size(), 6u);

    const std::vector<MethodReference>& callsToOwner = index.methodReferencesToOwner("B");
    ASSERT_EQ(callsToOwner.size(), 3u);
    EXPECT_EQ(callsToOwner[0].site.sourceClass, "A");

    const std::vector<FieldReference>& fieldsToOwner = index.fieldReferencesToOwner("B");
    ASSERT_EQ(fieldsToOwner.size(), 1u);
    EXPECT_EQ(fieldsToOwner[0].site.sourceClass, "A");

    EXPECT_TRUE(index.declaredMethodsIn("Missing").empty());
    EXPECT_TRUE(index.methodReferencesFromClass("Missing").empty());
    EXPECT_TRUE(index.methodReferencesToOwner("Missing").empty());
}

// ── 4. 类头与字段声明类型引用 ───────────────────────────────────

TEST(MjReferenceTest, indexesClassHeaderAndFieldDeclarationTypeReferences)
{
    ResourceContainer container = buildDeclarationTypeFixture();

    ReferenceIndex index;
    index.build(container);

    const ClassDeclaration* child = index.declaredClass("Child");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->superName, "Parent");
    ASSERT_EQ(child->interfaces.size(), 1u);
    EXPECT_EQ(child->interfaces[0], "Iface");

    bool sawParentSuper = false;
    for (const auto& ref : index.typeReferencesTo("Parent")) {
        if (ref.kind == TypeReferenceKind::ClassSuper && ref.site.sourceClass == "Child") {
            EXPECT_EQ(ref.site.sourceMethod, "");
            EXPECT_EQ(ref.site.insnIndex, -1);
            sawParentSuper = true;
        }
    }
    EXPECT_TRUE(sawParentSuper);

    bool sawIfaceHeader          = false;
    bool sawIfaceFieldDescriptor = false;
    for (const auto& ref : index.typeReferencesTo("Iface")) {
        if (ref.kind == TypeReferenceKind::ClassInterface && ref.site.sourceClass == "Child") {
            sawIfaceHeader = true;
        }
        if (ref.kind == TypeReferenceKind::FieldDeclarationDescriptor &&
            ref.site.sourceClass == "Child") {
            sawIfaceFieldDescriptor = true;
        }
    }
    EXPECT_TRUE(sawIfaceHeader);
    EXPECT_TRUE(sawIfaceFieldDescriptor);

    const std::vector<mjbase::reference::TypeReference>& childTypes =
        index.typeReferencesFromClass("Child");
    bool sawClassViewSuper           = false;
    bool sawClassViewFieldDescriptor = false;
    for (const auto& ref : childTypes) {
        if (ref.kind == TypeReferenceKind::ClassSuper && ref.targetType == "Parent") {
            sawClassViewSuper = true;
        }
        if (ref.kind == TypeReferenceKind::FieldDeclarationDescriptor &&
            ref.targetType == "Iface") {
            sawClassViewFieldDescriptor = true;
        }
    }
    EXPECT_TRUE(sawClassViewSuper);
    EXPECT_TRUE(sawClassViewFieldDescriptor);
    EXPECT_TRUE(index.typeReferencesFrom("Child", "", "").empty());
}

// ── 5. GlobalAnalysisManager 集成 ───────────────────────────────

TEST(MjReferenceTest, globalAnalysisManagerIntegration)
{
    ResourceContainer container = buildReferenceFixture();

    ca::str::Utf8StringArena arena;
    GlobalAnalysisManager    gam(arena);
    gam.registerAnalysis<ReferenceIndexAnalysis>();
    auto& index = gam.getResult<ReferenceIndexAnalysis>(container);

    EXPECT_EQ(index.fieldReferencesTo("B", "value", "I").size(), 1u);
    EXPECT_EQ(index.methodReferencesTo("B", "callee", "()V").size(), 1u);
}

// ── 6. 注解与签名类型引用 ───────────────────────────────────────

TEST(MjReferenceTest, indexesAnnotationAndSignatureTypeReferences)
{
    ResourceContainer container;
    addClassBytes(container, "MetaUser", buildMetadataReferenceClass());

    ReferenceIndex index;
    index.build(container);

    auto hasTypeRef = [&](std::string_view type, TypeReferenceKind kind) {
        for (const auto& ref : index.typeReferencesTo(type)) {
            if (ref.kind == kind && ref.site.sourceClass == "MetaUser") return true;
        }
        return false;
    };

    EXPECT_TRUE(hasTypeRef("com/example/Anno", TypeReferenceKind::Annotation));
    EXPECT_TRUE(hasTypeRef("com/example/AnnotationValue", TypeReferenceKind::Annotation));
    EXPECT_TRUE(hasTypeRef("com/example/FieldAnno", TypeReferenceKind::Annotation));
    EXPECT_TRUE(hasTypeRef("com/example/MethodAnno", TypeReferenceKind::Annotation));
    EXPECT_TRUE(hasTypeRef("com/example/GenericIface", TypeReferenceKind::Signature));
    EXPECT_TRUE(hasTypeRef("com/example/GenericArg", TypeReferenceKind::Signature));
    EXPECT_TRUE(hasTypeRef("com/example/FieldType", TypeReferenceKind::Signature));
    EXPECT_TRUE(hasTypeRef("com/example/MethodReturn", TypeReferenceKind::Signature));
}

// ── 7. 结构元数据类型引用 ───────────────────────────────────────

TEST(MjReferenceTest, indexesStructuralMetadataTypeReferences)
{
    ResourceContainer container;
    addClassBytes(container, "StructuralUser", buildStructuralMetadataReferenceClass());

    ReferenceIndex index;
    index.build(container);

    auto hasTypeRef = [&](std::string_view type, TypeReferenceKind kind) {
        for (const auto& ref : index.typeReferencesTo(type)) {
            if (ref.kind == kind && ref.site.sourceClass == "StructuralUser" &&
                ref.site.sourceMethod.empty() && ref.site.insnIndex == -1) {
                return true;
            }
        }
        return false;
    };

    EXPECT_TRUE(hasTypeRef("com/example/Outer", TypeReferenceKind::StructuralMetadata));
    EXPECT_TRUE(hasTypeRef("com/example/NestHost", TypeReferenceKind::StructuralMetadata));
    EXPECT_TRUE(hasTypeRef("com/example/NestMember", TypeReferenceKind::StructuralMetadata));
    EXPECT_TRUE(hasTypeRef("com/example/Permitted", TypeReferenceKind::StructuralMetadata));
    EXPECT_TRUE(hasTypeRef("com/example/Outer$Inner", TypeReferenceKind::StructuralMetadata));
    EXPECT_TRUE(hasTypeRef("com/example/RecordType", TypeReferenceKind::StructuralMetadata));
    EXPECT_TRUE(hasTypeRef("com/example/RecordGeneric", TypeReferenceKind::Signature));
    EXPECT_TRUE(hasTypeRef("com/example/RecordAnno", TypeReferenceKind::Annotation));

    bool sawClassViewRecordType = false;
    for (const auto& ref : index.typeReferencesFromClass("StructuralUser")) {
        if (ref.kind == TypeReferenceKind::StructuralMetadata &&
            ref.targetType == "com/example/RecordType") {
            sawClassViewRecordType = true;
        }
    }
    EXPECT_TRUE(sawClassViewRecordType);
}

// ── 8. Module 元数据类型引用 ────────────────────────────────────

TEST(MjReferenceTest, indexesModuleMetadataTypeReferences)
{
    ResourceContainer container;
    addClassBytes(container, "module-info", buildModuleMetadataReferenceClass());

    ReferenceIndex index;
    index.build(container);

    auto hasModuleTypeRef = [&](std::string_view type) {
        for (const auto& ref : index.typeReferencesTo(type)) {
            if (ref.kind == TypeReferenceKind::StructuralMetadata &&
                ref.site.sourceClass == "module-info" && ref.site.sourceMethod.empty() &&
                ref.site.insnIndex == -1) {
                return true;
            }
        }
        return false;
    };

    EXPECT_TRUE(hasModuleTypeRef("example/Main"));
    EXPECT_TRUE(hasModuleTypeRef("example/Service"));
    EXPECT_TRUE(hasModuleTypeRef("example/Impl"));
}

// ── 9. 反射字符串/MethodHandle/Lambda 目标 ──────────────────────

TEST(MjReferenceTest, indexesReflectionStringsMethodHandlesAndLambdaTargets)
{
    ResourceContainer container;
    addClassBytes(container, "IndirectUser", buildIndirectBytecodeReferenceClass());

    ReferenceIndex index;
    index.build(container);

    bool sawReflectionString = false;
    for (const auto& ref : index.typeReferencesTo("com/example/Reflected")) {
        if (ref.kind == TypeReferenceKind::ReflectionString &&
            ref.site.sourceClass == "IndirectUser") {
            sawReflectionString = true;
        }
    }
    EXPECT_TRUE(sawReflectionString);

    bool sawMethodHandle = false;
    bool sawLambda       = false;
    for (const auto& ref : index.methodReferencesTo("IndirectUser", "lambdaTarget", "()V")) {
        if (ref.kind == MethodReferenceKind::MethodHandle) sawMethodHandle = true;
        if (ref.kind == MethodReferenceKind::Lambda) sawLambda = true;
    }
    EXPECT_TRUE(sawMethodHandle);
    EXPECT_TRUE(sawLambda);

    // Lambda 事实按同一成边规则在索引事实上断言
    // （见 indexHasMethodEdge 函数注）。
    EXPECT_TRUE(
        indexHasMethodEdge(index, "IndirectUser", "use", "()V", "IndirectUser", "lambdaTarget",
                           "()V"));
}

// ── 10. 常量反射成员查找 ────────────────────────────────────────

TEST(MjReferenceTest, indexesConstantReflectionMemberLookups)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectUser", buildReflectionMemberReferenceClass());
    addClassBytes(container, "com/example/ReflectTarget", buildReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawMethodLookup = false;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "()V")) {
        if (ref.kind == MethodReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawMethodLookup = true;
        }
    }
    EXPECT_TRUE(sawMethodLookup);
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "").empty());

    bool sawFieldLookup = false;
    for (const auto& ref : index.fieldReferencesTo("com/example/ReflectTarget", "secret", "I")) {
        if (ref.kind == FieldReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawFieldLookup = true;
        }
    }
    EXPECT_TRUE(sawFieldLookup);
    EXPECT_TRUE(index.fieldReferencesTo("com/example/ReflectTarget", "secret", "").empty());

    // 查找事实不成边；目标方法仍在索引声明可见。
    EXPECT_NE(index.declaredMethod("com/example/ReflectTarget", "reflectTarget", "()V"),
              nullptr);
    EXPECT_FALSE(indexHasMethodEdge(index, "ReflectUser", "use", "()V",
                                    "com/example/ReflectTarget", "reflectTarget", "()V"));
}

// ── 11. 局部别名的反射执行 ──────────────────────────────────────

TEST(MjReferenceTest, indexesReflectionExecutionThroughLocalAliases)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectExecutionUser", buildReflectionExecutionReferenceClass());
    addClassBytes(container, "com/example/ReflectTarget", buildReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawMethodExecution = false;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "()V")) {
        if (ref.kind == MethodReferenceKind::ReflectionExecution &&
            ref.site.sourceClass == "ReflectExecutionUser") {
            sawMethodExecution = true;
            EXPECT_EQ(ref.site.opcode, static_cast<ca::i32>(Op::INVOKEVIRTUAL));
        }
    }
    EXPECT_TRUE(sawMethodExecution);

    ca::i32 constructorExecutionCount = 0;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "<init>", "()V")) {
        if (ref.kind == MethodReferenceKind::ReflectionExecution &&
            ref.site.sourceClass == "ReflectExecutionUser") {
            ++constructorExecutionCount;
        }
    }
    EXPECT_EQ(constructorExecutionCount, 2);

    ca::i32 fieldAccessCount = 0;
    for (const auto& ref : index.fieldReferencesTo("com/example/ReflectTarget", "secret", "I")) {
        if (ref.kind == FieldReferenceKind::ReflectionAccess &&
            ref.site.sourceClass == "ReflectExecutionUser") {
            ++fieldAccessCount;
        }
    }
    EXPECT_EQ(fieldAccessCount, 2);

    EXPECT_TRUE(indexHasMethodEdge(index, "ReflectExecutionUser", "use", "()V",
                                   "com/example/ReflectTarget", "reflectTarget", "()V"));
    EXPECT_TRUE(indexHasMethodEdge(index, "ReflectExecutionUser", "use", "()V",
                                   "com/example/ReflectTarget", "<init>", "()V"));
}

// ── 12. 控制流合并的冲突别名丢弃 ────────────────────────────────

TEST(MjReferenceTest, dropsConflictingReflectionAliasesAtControlFlowMerge)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectMergeUser", buildAmbiguousReflectionExecutionClass());
    addClassBytes(container, "com/example/ReflectTarget", buildReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    auto hasExecution = [&](std::string_view name) {
        for (const auto& ref :
             index.methodReferencesTo("com/example/ReflectTarget", name, "()V")) {
            if (ref.kind == MethodReferenceKind::ReflectionExecution &&
                ref.site.sourceClass == "ReflectMergeUser") {
                return true;
            }
        }
        return false;
    };
    EXPECT_FALSE(hasExecution("reflectTarget"));
    EXPECT_FALSE(hasExecution("otherTarget"));

    EXPECT_FALSE(indexHasMethodEdge(index, "ReflectMergeUser", "use", "(Z)V",
                                    "com/example/ReflectTarget", "reflectTarget", "()V"));
    EXPECT_FALSE(indexHasMethodEdge(index, "ReflectMergeUser", "use", "(Z)V",
                                    "com/example/ReflectTarget", "otherTarget", "()V"));
}

// ── 13. 重载歧义保持未解析 ──────────────────────────────────────

TEST(MjReferenceTest, keepsAmbiguousReflectionMethodLookupUnresolved)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectUser", buildReflectionMemberReferenceClass());
    addClassBytes(container, "com/example/ReflectTarget",
                  buildOverloadedReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawUnresolvedMethodLookup = false;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "")) {
        if (ref.kind == MethodReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawUnresolvedMethodLookup = true;
        }
    }
    EXPECT_TRUE(sawUnresolvedMethodLookup);
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "()V").empty());
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "(I)V").empty());

    EXPECT_FALSE(indexHasMethodEdge(index, "ReflectUser", "use", "()V",
                                    "com/example/ReflectTarget", "reflectTarget", "()V"));
}

// ── 14. Class[] 实参落解 String 重载 ────────────────────────────

TEST(MjReferenceTest, resolvesOverloadedReflectionMethodFromClassArray)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectUser", buildReflectionMemberReferenceWithClassArray());
    addClassBytes(container, "com/example/ReflectTarget",
                  buildOverloadedReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawStringOverload = false;
    for (const auto& ref : index.methodReferencesTo(
             "com/example/ReflectTarget", "reflectTarget", "(Ljava/lang/String;)V")) {
        if (ref.kind == MethodReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawStringOverload = true;
            EXPECT_EQ(ref.reflectionArgDesc, "(Ljava/lang/String;)");
        }
    }
    EXPECT_TRUE(sawStringOverload);
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "").empty());
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "()V").empty());
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "(I)V").empty());

    EXPECT_FALSE(indexHasMethodEdge(index,
                                    "ReflectUser",
                                    "use",
                                    "()V",
                                    "com/example/ReflectTarget",
                                    "reflectTarget",
                                    "(Ljava/lang/String;)V"));
}

// ── 15. Class[] 实参落解 int 重载（TYPE 基本类型字段）──────────

TEST(MjReferenceTest, resolvesOverloadedReflectionMethodFromPrimitiveTypeArray)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectUser",
                  buildReflectionMemberReferenceWithPrimitiveTypeArray());
    addClassBytes(container, "com/example/ReflectTarget",
                  buildOverloadedReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawIntOverload = false;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "(I)V")) {
        if (ref.kind == MethodReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawIntOverload = true;
            EXPECT_EQ(ref.reflectionArgDesc, "(I)");
        }
    }
    EXPECT_TRUE(sawIntOverload);
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "").empty());
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "()V").empty());
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "(Ljava/lang/String;)V")
            .empty());

    EXPECT_FALSE(indexHasMethodEdge(index, "ReflectUser", "use", "()V",
                                    "com/example/ReflectTarget", "reflectTarget", "(I)V"));
}

// ── 16. 局部数组落解双参重载 ────────────────────────────────────

TEST(MjReferenceTest, resolvesOverloadedReflectionMethodFromLocalClassArray)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectUser",
                  buildReflectionMemberReferenceWithLocalClassArray(true));
    addClassBytes(container, "com/example/ReflectTarget",
                  buildOverloadedReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawTwoArgumentOverload = false;
    for (const auto& ref : index.methodReferencesTo(
             "com/example/ReflectTarget", "reflectTarget", "(Ljava/lang/String;I)V")) {
        if (ref.kind == MethodReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawTwoArgumentOverload = true;
            EXPECT_EQ(ref.reflectionArgDesc, "(Ljava/lang/String;I)");
        }
    }
    EXPECT_TRUE(sawTwoArgumentOverload);
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "").empty());
}

// ── 17. 不完整局部数组保持未解析 ────────────────────────────────

TEST(MjReferenceTest, keepsIncompleteLocalClassArrayUnresolved)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectUser",
                  buildReflectionMemberReferenceWithLocalClassArray(false));
    addClassBytes(container, "com/example/ReflectTarget",
                  buildOverloadedReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawUnresolvedLookup = false;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "")) {
        if (ref.kind == MethodReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawUnresolvedLookup = true;
            EXPECT_TRUE(ref.reflectionArgDesc.empty());
        }
    }
    EXPECT_TRUE(sawUnresolvedLookup);
    EXPECT_TRUE(
        index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "(Ljava/lang/String;I)V")
            .empty());
}

// ── 18. Class[] 实参落解构造重载 ────────────────────────────────

TEST(MjReferenceTest, resolvesOverloadedReflectionConstructorFromClassArray)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectUser", buildReflectionConstructorReferenceWithClassArray());
    addClassBytes(container, "com/example/ReflectTarget",
                  buildOverloadedReflectionConstructorTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawStringCtor = false;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "<init>", "(Ljava/lang/String;)V")) {
        if (ref.kind == MethodReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawStringCtor = true;
            EXPECT_EQ(ref.reflectionArgDesc, "(Ljava/lang/String;)");
        }
    }
    EXPECT_TRUE(sawStringCtor);
    EXPECT_TRUE(index.methodReferencesTo("com/example/ReflectTarget", "<init>", "").empty());
    EXPECT_TRUE(index.methodReferencesTo("com/example/ReflectTarget", "<init>", "()V").empty());

    EXPECT_FALSE(indexHasMethodEdge(index,
                                    "ReflectUser",
                                    "use",
                                    "()V",
                                    "com/example/ReflectTarget",
                                    "<init>",
                                    "(Ljava/lang/String;)V"));
}

// ── 19. 局部数组落解构造重载 ────────────────────────────────────

TEST(MjReferenceTest, resolvesOverloadedReflectionConstructorFromLocalClassArray)
{
    ResourceContainer container;
    addClassBytes(container, "ReflectUser",
                  buildReflectionConstructorReferenceWithLocalClassArray());
    addClassBytes(container, "com/example/ReflectTarget",
                  buildOverloadedReflectionConstructorTargetClass());

    ReferenceIndex index;
    index.build(container);

    bool sawTwoArgumentConstructor = false;
    for (const auto& ref : index.methodReferencesTo(
             "com/example/ReflectTarget", "<init>", "(Ljava/lang/String;I)V")) {
        if (ref.kind == MethodReferenceKind::Reflection && ref.site.sourceClass == "ReflectUser") {
            sawTwoArgumentConstructor = true;
            EXPECT_EQ(ref.reflectionArgDesc, "(Ljava/lang/String;I)");
        }
    }
    EXPECT_TRUE(sawTwoArgumentConstructor);
    EXPECT_TRUE(index.methodReferencesTo("com/example/ReflectTarget", "<init>", "").empty());
}

// ── 20. JSR/RET fail-open（#975）───────────────────────────────
//
// 含 JSR/RET 的方法走 ReferenceIndex 全流程——不崩、fail-open 跳过：直接引用
// 与常量 lookup 事实保留，数据流证明的 ReflectionExecution 不产出；相邻无
// JSR 的同款方法（对照组）不受扰、照常产出 ReflectionExecution。
TEST(MjReferenceTest, failsOpenOnJsrKeepsLookupAndDropsExecution)
{
    ResourceContainer container;
    addClassBytes(container, "JsrReflectUser", buildJsrReflectionUserClass());
    addClassBytes(container, "com/example/ReflectTarget", buildReflectionMemberTargetClass());

    ReferenceIndex index;
    index.build(container);

    // fail-open 主断言：JSR 方法的反射执行事实不产出。
    ca::i32 jsrExecutionCount = 0;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "()V")) {
        if (ref.kind == MethodReferenceKind::ReflectionExecution &&
            ref.site.sourceClass == "JsrReflectUser" && ref.site.sourceMethod == "jsrReflect") {
            ++jsrExecutionCount;
        }
    }
    EXPECT_EQ(jsrExecutionCount, 0);

    // 直接事实保留：JSR 方法体内 Method.invoke 的常量池引用仍在索引。
    bool jsrKeepsDirectInvoke = false;
    for (const auto& ref : index.methodReferencesFrom("JsrReflectUser", "jsrReflect", "()V")) {
        if (ref.kind == MethodReferenceKind::Direct &&
            ref.target.owner == "java/lang/reflect/Method" && ref.target.name == "invoke") {
            jsrKeepsDirectInvoke = true;
        }
    }
    EXPECT_TRUE(jsrKeepsDirectInvoke);

    // 相邻正常方法（对照组）：同款模式照常证明，产出 ReflectionExecution。
    ca::i32 plainExecutionCount = 0;
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "()V")) {
        if (ref.kind == MethodReferenceKind::ReflectionExecution &&
            ref.site.sourceClass == "JsrReflectUser" && ref.site.sourceMethod == "plainReflect") {
            ++plainExecutionCount;
        }
    }
    EXPECT_EQ(plainExecutionCount, 1);

    // JSR 方法不得扰动对照方法的事实归属。
    for (const auto& ref :
         index.methodReferencesTo("com/example/ReflectTarget", "reflectTarget", "()V")) {
        if (ref.kind == MethodReferenceKind::ReflectionExecution &&
            ref.site.sourceClass == "JsrReflectUser") {
            EXPECT_EQ(ref.site.sourceMethod, "plainReflect");
        }
    }
}
