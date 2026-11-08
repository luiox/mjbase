/// @file        indy_test.cpp
/// @brief       asm/classpath indy 解析面测试：BSM 分族、lambda 展开产物、
///              altMetafactory 桥类型、已知无目标族、未知族 fail-closed、
///              VarHandle 访问读写分类，以及 CallGraph 的展开边织入与外部目标过滤。
///
/// 夹具纪律：类字节一律 jasm 文本装配（JvmJasmService）。indy 需
/// BootstrapMethods 面，61 装配（indy_site_inliner_jvm_test 同径）；61 装配期
/// 做帧分析，原字节夹具的裸 invoke 序列以 aload/iload 垫出实参与接收者，
/// 垫栈不产生 indy 站点。BSM 静态参数字面形：MethodType 用裸描述符、句柄用
/// { invokestatic|invokevirtual, Owner.name, desc }、类用 { internalName }、
/// 字段句柄用 { getfield, Owner.field, desc }（后三者为既有 fixture 未实证
/// 形，以 CI 装配为准）。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/bytes.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/asm/classpath/indy.hpp"

#include "shared/jasm_fixture.hpp"

namespace {

using Bytes = std::vector<ca::u8>;
using mjbase::asm_classpath::BootstrapFamily;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::asm_classpath::CallGraphOptions;
using mjbase::asm_classpath::CallSite;
using mjbase::asm_classpath::HandleDispatch;
using mjbase::asm_classpath::IndyIndex;
using mjbase::asm_classpath::IndyIndexBuilder;
using mjbase::asm_classpath::IndyResolution;
using mjbase::asm_classpath::InvokeKind;
using mjbase::asm_classpath::VarHandleAccess;
using mjbase::MemberKey;

/// lambda 宿主夹具（sample/LambdaHost）：7 个 BSM（metafactory ×3 /
/// altMetafactory / 未知族 / 字符串拼接族 / record ObjectMethods）× 各 indy
/// 站点 + 4 个 VarHandle invokevirtual 站点。61 装配（indy 需 BSM 面），方法
/// 体以 aload/iload 垫出实参保证帧自洽，垫栈不产生 indy 站点。
const Bytes& lambdaHostBytes()
{
    static const Bytes cls = mjbase::test::assembleJasm(R"JASM(.super java/lang/Object
.class public super sample/LambdaHost {
  .method public static run (Lsample/Ctx;)V {
    code: {
      aload 0
      invokedynamic apply (Lsample/Ctx;)Ljava/util/function/Function; { invokestatic, java/lang/invoke/LambdaMetafactory.metafactory, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { (Ljava/lang/Object;)Ljava/lang/Object;, { invokestatic, sample/LambdaHost.lambda$run$0, (Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object; }, (Ljava/lang/Object;)Ljava/lang/Object; }
      pop
      return
    }
  }
  .method private static lambda$run$0 (Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object; {
    code: {
      aconst_null
      areturn
    }
  }
  .method public static ref ()Ljava/util/function/Function; {
    code: {
      invokedynamic apply ()Ljava/util/function/Function; { invokestatic, java/lang/invoke/LambdaMetafactory.metafactory, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { (Ljava/lang/Object;)Ljava/lang/Object;, { invokestatic, sample/Target.impl, (Ljava/lang/Object;)Ljava/lang/Object; }, (Ljava/lang/Object;)Ljava/lang/Object; }
      areturn
    }
  }
  .method public static vref (Lsample/Target;)Ljava/util/function/Function; {
    code: {
      aload 0
      invokedynamic apply (Lsample/Target;)Ljava/util/function/Function; { invokestatic, java/lang/invoke/LambdaMetafactory.metafactory, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { (Ljava/lang/Object;)Ljava/lang/Object;, { invokevirtual, sample/Target.virt, (Ljava/lang/Object;)Ljava/lang/Object; }, (Ljava/lang/Object;)Ljava/lang/Object; }
      pop
      return
    }
  }
  .method public static altRef ()Ljava/util/function/Function; {
    code: {
      invokedynamic apply ()Ljava/util/function/Function; { invokestatic, java/lang/invoke/LambdaMetafactory.altMetafactory, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite; } { (Ljava/lang/Object;)Ljava/lang/Object;, { invokestatic, sample/Target.impl, (Ljava/lang/Object;)Ljava/lang/Object; }, (Ljava/lang/Object;)Ljava/lang/Object;, 5, 1, (Ljava/lang/String;)Ljava/lang/Object; }
      areturn
    }
  }
  .method public static mystery ()V {
    code: {
      invokedynamic weird ()V { invokestatic, com/example/Mystery.bootstrap, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { }
      return
    }
  }
  .method public static concat (Ljava/lang/String;)Ljava/lang/String; {
    code: {
      aload 0
      invokedynamic makeConcatWithConstants (Ljava/lang/String;)Ljava/lang/String; { invokestatic, java/lang/invoke/StringConcatFactory.makeConcatWithConstants, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite; } { "\u0001!" }
      areturn
    }
  }
  .method public static recordToString (Lsample/Point;)Ljava/lang/String; {
    code: {
      aload 0
      invokedynamic toString (Lsample/Point;)Ljava/lang/String; { invokestatic, java/lang/runtime/ObjectMethods.bootstrap, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/TypeDescriptor;Ljava/lang/Class;Ljava/lang/String;[Ljava/lang/invoke/MethodHandle;)Ljava/lang/Object; } { { sample/Point }, "x;y", { getfield, sample/Point.x, I }, { getfield, sample/Point.y, I } }
      areturn
    }
  }
  .method public static vhRead (Ljava/lang/invoke/VarHandle;Ljava/lang/Object;)I {
    code: {
      aload 0
      aload 1
      invokevirtual java/lang/invoke/VarHandle.get (Ljava/lang/Object;)I
      ireturn
    }
  }
  .method public static vhWrite (Ljava/lang/invoke/VarHandle;Ljava/lang/Object;I)V {
    code: {
      aload 0
      aload 1
      iload 2
      invokevirtual java/lang/invoke/VarHandle.set (Ljava/lang/Object;I)V
      return
    }
  }
  .method public static vhUpdate (Ljava/lang/invoke/VarHandle;Ljava/lang/Object;II)Z {
    code: {
      aload 0
      aload 1
      iload 2
      iload 3
      invokevirtual java/lang/invoke/VarHandle.compareAndSet (Ljava/lang/Object;II)Z
      ireturn
    }
  }
  .method public static vhMeta (Ljava/lang/invoke/VarHandle;Ljava/lang/invoke/VarHandle$AccessMode;)Ljava/lang/invoke/MethodType; {
    code: {
      aload 0
      aload 1
      invokevirtual java/lang/invoke/VarHandle.accessModeType (Ljava/lang/invoke/VarHandle$AccessMode;)Ljava/lang/invoke/MethodType;
      areturn
    }
  }
}
)JASM", 61);
    return cls;
}

/// 展开目标夹具（sample/Target）：impl（private static）与 virt（public 实例）。
const Bytes& targetBytes()
{
    static const Bytes cls = mjbase::test::assembleJasm(R"JASM(.super java/lang/Object
.class public super sample/Target {
  .method private static impl (Ljava/lang/Object;)Ljava/lang/Object; {
    code: {
      aconst_null
      areturn
    }
  }
  .method public virt (Ljava/lang/Object;)Ljava/lang/Object; {
    code: {
      aconst_null
      areturn
    }
  }
}
)JASM", 61);
    return cls;
}

jbc::core::ByteView byteView(const Bytes& bytes)
{
    return jbc::core::ByteView{bytes.data(), bytes.size()};
}

bool hasMember(const std::vector<MemberKey>& keys, const mjbase::KeyPool& pool,
               std::string_view owner, std::string_view name, std::string_view desc)
{
    for (const MemberKey key : keys) {
        if (pool.str(key.cls) == owner && pool.str(key.name) == name &&
            pool.str(key.desc) == desc) {
            return true;
        }
    }
    return false;
}

IndyIndex fixtureIndex()
{
    const Bytes host   = lambdaHostBytes();
    const Bytes target = targetBytes();

    IndyIndexBuilder builder;
    EXPECT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    EXPECT_TRUE(builder.addClassBytes(byteView(target)).is_ok());
    auto built = builder.build();
    EXPECT_TRUE(built.is_ok());
    if (!built.is_ok()) {
        return IndyIndex{};
    }
    return std::move(built).unwrap();
}

}   // namespace

// ---------------------------------------------------------------------------
// BSM 分族与展开产物
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, IndyLambdaMetafactoryExposesTargetAndSignature)
{
    const IndyIndex index_ = fixtureIndex();
    const std::vector<const IndyResolution*> found =
        index_.resolutionsOf("sample/LambdaHost", "run", "(Lsample/Ctx;)V");
    ASSERT_EQ(found.size(), 1u);
    const IndyResolution& res = *found[0];

    EXPECT_EQ(res.family, BootstrapFamily::LambdaMetafactory);
    EXPECT_TRUE(res.known());
    EXPECT_TRUE(res.resolved());
    EXPECT_EQ(res.name, "apply");
    EXPECT_EQ(res.desc, "(Lsample/Ctx;)Ljava/util/function/Function;");
    EXPECT_EQ(res.functionalInterface, "java/util/function/Function");
    EXPECT_EQ(res.samMethodType, "(Ljava/lang/Object;)Ljava/lang/Object;");
    EXPECT_EQ(res.instantiatedMethodType, "(Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_EQ(res.capturedTypes.size(), 1u);
    EXPECT_EQ(res.capturedTypes[0], "Lsample/Ctx;");

    // BSM 自身句柄。
    EXPECT_EQ(res.bootstrap.kind, 6u);
    EXPECT_EQ(res.bootstrap.owner, "java/lang/invoke/LambdaMetafactory");
    EXPECT_EQ(res.bootstrap.name, "metafactory");

    ASSERT_EQ(res.targets.size(), 1u);
    const auto& target = res.targets[0];
    EXPECT_EQ(target.kind, 6u);
    EXPECT_EQ(target.dispatch(), HandleDispatch::Static);
    EXPECT_EQ(target.owner, "sample/LambdaHost");
    EXPECT_EQ(target.name, "lambda$run$0");
    EXPECT_EQ(target.desc, "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;");
}

TEST(MjAsmClasspathTest, IndyAltMetafactoryExtractsBridgesAndKeepsImplTarget)
{
    const IndyIndex index_ = fixtureIndex();
    const std::vector<const IndyResolution*> found =
        index_.resolutionsOf("sample/LambdaHost", "altRef",
                             "()Ljava/util/function/Function;");
    ASSERT_EQ(found.size(), 1u);
    const IndyResolution& res = *found[0];

    EXPECT_EQ(res.family, BootstrapFamily::LambdaMetafactory);
    ASSERT_EQ(res.targets.size(), 1u);
    EXPECT_EQ(res.targets[0].owner, "sample/Target");
    EXPECT_EQ(res.targets[0].name, "impl");
    ASSERT_EQ(res.bridgeMethodTypes.size(), 1u);
    EXPECT_EQ(res.bridgeMethodTypes[0], "(Ljava/lang/String;)Ljava/lang/Object;");
}

TEST(MjAsmClasspathTest, IndyKnownFamiliesWithoutCallTargetsStayKnown)
{
    const IndyIndex index_ = fixtureIndex();
    const auto concat = index_.resolutionsOf("sample/LambdaHost", "concat",
                                             "(Ljava/lang/String;)Ljava/lang/String;");
    ASSERT_EQ(concat.size(), 1u);
    EXPECT_EQ(concat[0]->family, BootstrapFamily::StringConcatFactory);
    EXPECT_TRUE(concat[0]->known());
    EXPECT_TRUE(concat[0]->targets.empty());
    ASSERT_EQ(concat[0]->capturedTypes.size(), 1u);
    EXPECT_EQ(concat[0]->capturedTypes[0], "Ljava/lang/String;");

    const auto record = index_.resolutionsOf(
        "sample/LambdaHost", "recordToString", "(Lsample/Point;)Ljava/lang/String;");
    ASSERT_EQ(record.size(), 1u);
    EXPECT_EQ(record[0]->family, BootstrapFamily::ObjectMethods);
    ASSERT_EQ(record[0]->targets.size(), 2u);
    EXPECT_EQ(record[0]->targets[0].dispatch(), HandleDispatch::Field);
    EXPECT_EQ(record[0]->targets[0].name, "x");
    EXPECT_EQ(record[0]->targets[1].name, "y");
}

TEST(MjAsmClasspathTest, IndyUnknownBootstrapFailsClosed)
{
    const IndyIndex index_ = fixtureIndex();
    const auto found =
        index_.resolutionsOf("sample/LambdaHost", "mystery", "()V");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0]->family, BootstrapFamily::Unknown);
    EXPECT_FALSE(found[0]->known());
    EXPECT_TRUE(found[0]->targets.empty());
    EXPECT_TRUE(index_.hasUnknownFamilies());
}

// ---------------------------------------------------------------------------
// VarHandle 访问模式
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, VarHandleAccessModesClassifyReadWrite)
{
    const IndyIndex index_ = fixtureIndex();
    const auto read =
        index_.varHandleAccessesOf("sample/LambdaHost", "vhRead",
                                   "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;)I");
    ASSERT_EQ(read.size(), 1u);
    EXPECT_EQ(read[0]->mode, "get");
    EXPECT_TRUE(read[0]->read);
    EXPECT_FALSE(read[0]->write);

    const auto write = index_.varHandleAccessesOf(
        "sample/LambdaHost", "vhWrite",
        "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;I)V");
    ASSERT_EQ(write.size(), 1u);
    EXPECT_EQ(write[0]->mode, "set");
    EXPECT_FALSE(write[0]->read);
    EXPECT_TRUE(write[0]->write);

    const auto update = index_.varHandleAccessesOf(
        "sample/LambdaHost", "vhUpdate",
        "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;II)Z");
    ASSERT_EQ(update.size(), 1u);
    EXPECT_EQ(update[0]->mode, "compareAndSet");
    EXPECT_TRUE(update[0]->read);
    EXPECT_TRUE(update[0]->write);

    // accessModeType 是普通方法，不是访问模式。
    EXPECT_TRUE(index_
                    .varHandleAccessesOf("sample/LambdaHost", "vhMeta",
                                         "(Ljava/lang/invoke/VarHandle;"
                                         "Ljava/lang/invoke/VarHandle$AccessMode;)"
                                         "Ljava/lang/invoke/MethodType;")
                    .empty());
}

// ---------------------------------------------------------------------------
// CallGraph 织入
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, CallGraphExpandsLambdaIndyToRealEdge)
{
    const Bytes host   = lambdaHostBytes();
    const Bytes target = targetBytes();
    CallGraphBuilder builder;
    ASSERT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    ASSERT_TRUE(builder.addClassBytes(byteView(target)).is_ok());
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    const MemberKey run =
        mjbase::memberKeyOf(graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"),
                            "run", "(Lsample/Ctx;)V");
    const MemberKey targetImpl =
        mjbase::memberKeyOf(graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/Target"),
                            "impl", "(Ljava/lang/Object;)Ljava/lang/Object;");
    const MemberKey leaf = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "lambda$run$0",
        "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;");

    const std::vector<CallSite> sites = graph.outgoingSitesOf(run);
    ASSERT_EQ(sites.size(), 2u);

    // 原始 indy 保守边在前，展开边在后，共用同一条 INVOKEDYNAMIC 指令。
    EXPECT_EQ(sites[0].kind, InvokeKind::Dynamic);
    EXPECT_FALSE(sites[0].indyExpanded);
    EXPECT_EQ(sites[0].indyFamily, BootstrapFamily::LambdaMetafactory);
    EXPECT_EQ(sites[1].callee, leaf);
    EXPECT_EQ(sites[1].kind, InvokeKind::Static);
    EXPECT_TRUE(sites[1].indyExpanded);
    EXPECT_EQ(sites[1].indyFamily, BootstrapFamily::LambdaMetafactory);
    EXPECT_EQ(sites[1].insn, sites[0].insn);

    EXPECT_TRUE(graph.isDeclared(leaf));
    EXPECT_TRUE(hasMember(graph.calleesOf(run), graph.pool(), "sample/LambdaHost",
                          "lambda$run$0",
                          "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;"));

    const std::vector<MemberKey> closure = graph.reachableClosure({run});
    EXPECT_TRUE(hasMember(closure, graph.pool(), "sample/LambdaHost", "lambda$run$0",
                          "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;"));

    // ref 与 altRef 的 BSM impl 都是 Target.impl，两处各展开一条边。
    EXPECT_EQ(graph.callSiteCountOf(targetImpl), 2u);
}

TEST(MjAsmClasspathTest, CallGraphFiltersIndyTargetsIndependently)
{
    const Bytes host = lambdaHostBytes();
    CallGraphBuilder builder(CallGraphOptions{/*includeExternalTargets=*/false});
    ASSERT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    const MemberKey run =
        mjbase::memberKeyOf(graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"),
                            "run", "(Lsample/Ctx;)V");
    const MemberKey leaf = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "lambda$run$0",
        "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;");

    // 保守伪 callee 未声明被丢；展开目标是本类声明方法，保留。
    const std::vector<CallSite> sites = graph.outgoingSitesOf(run);
    ASSERT_EQ(sites.size(), 1u);
    EXPECT_TRUE(sites[0].indyExpanded);
    EXPECT_EQ(sites[0].callee, leaf);

    // ref 的展开目标是外部类 Target.impl，与保守边一起丢弃。
    const MemberKey ref = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "ref",
        "()Ljava/util/function/Function;");
    EXPECT_TRUE(graph.outgoingSitesOf(ref).empty());
}

TEST(MjAsmClasspathTest, CallGraphMarksVirtualHandleAsVirtualDispatch)
{
    const Bytes host   = lambdaHostBytes();
    const Bytes target = targetBytes();
    CallGraphBuilder builder;
    ASSERT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    ASSERT_TRUE(builder.addClassBytes(byteView(target)).is_ok());
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    const MemberKey vref = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "vref",
        "(Lsample/Target;)Ljava/util/function/Function;");

    bool sawExpanded = false;
    for (const CallSite& site : graph.outgoingSitesOf(vref)) {
        if (!site.indyExpanded) {
            continue;
        }
        sawExpanded = true;
        EXPECT_EQ(site.kind, InvokeKind::Virtual);
        EXPECT_TRUE(site.isVirtualDispatch());
        EXPECT_EQ(graph.pool().str(site.callee.name), "virt");
    }
    EXPECT_TRUE(sawExpanded);
}

TEST(MjAsmClasspathTest, CallGraphKeepsUnknownBsmConservative)
{
    const Bytes host   = lambdaHostBytes();
    const Bytes target = targetBytes();
    CallGraphBuilder builder;
    ASSERT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    ASSERT_TRUE(builder.addClassBytes(byteView(target)).is_ok());
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    const MemberKey mystery = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "mystery", "()V");

    const std::vector<CallSite> sites = graph.outgoingSitesOf(mystery);
    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].kind, InvokeKind::Dynamic);
    EXPECT_FALSE(sites[0].indyExpanded);
    EXPECT_EQ(sites[0].indyFamily, BootstrapFamily::Unknown);

    // record getter 是字段句柄，不产调用边；字符串拼接族无目标。
    for (const char* name : {"recordToString", "concat"}) {
        const MemberKey key = mjbase::memberKeyOf(
            graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), name,
            name == std::string_view("concat")
                ? "(Ljava/lang/String;)Ljava/lang/String;"
                : "(Lsample/Point;)Ljava/lang/String;");
        const std::vector<CallSite> methodSites = graph.outgoingSitesOf(key);
        ASSERT_EQ(methodSites.size(), 1u);
        EXPECT_EQ(methodSites[0].kind, InvokeKind::Dynamic);
        EXPECT_FALSE(methodSites[0].indyExpanded);
    }
}
