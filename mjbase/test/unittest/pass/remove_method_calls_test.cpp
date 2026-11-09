// MethodCallRemover（调用点收尾件，opt pass 波次）单测：点名删调用点的栈
// 等价（四族 invoke / cat2 吞参 / 死局部 store / void 吞参）、放弃路径
// （dup 前缀 / 方法句柄钉死 / 引用锚定 / 返回值被消费 / 容器不可读）、
// 调用图边同步（none() 失效驱动重建）、默认 no-op 与无用结果白名单
// 可配置面。
//
// 夹具纪律：方法体一律 jasm 文本装配（JvmJasmService），原
// structural_pass_util 手拼载体退役；方法体断言仍经 libjbc 会话 promote
// 后的 CodeDom 指令序完成（不依赖字节偏移）。三处按语义合法重表达：
// - 方法句柄钉死面：condy ldc 表达——BSM 引用位 { invokestatic,
//   Used.pick, ()I } 落 CP MethodHandle 条目（句柄扫描逐条目覆盖），
//   钉死三元组与原 ldc 直载柄同实；
// - 分支锚定面：原 goto 直落调用点（栈形断裂不可装配）以异常区间起点锚
//   调用点同位重表达（labelsAt 对分支目标与异常边界同源留锚）；
// - dup2 前缀面：原 return 欠清栈不可装配，窗尾补 pop2 清栈——放弃判定
//   只看调用点前一条为 DUP2，面同实。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/remove_method_calls.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

using mjbase::pass::MethodCallRemovalConfig;
using mjbase::pass::MethodCallRemover;
using mjbase::pass::MethodCallSpec;

// ── 夹具构造 ─────────────────────────────────────────────────────────────────

/// 类便捷装配：cls 类挂 methodsText（若干 `.method` 声明文本）。
Bytes callerClass(const std::string& cls, std::string_view methodsText)
{
    const std::string src = R"JASM(.super java/lang/Object
.class public super )JASM" + cls + R"JASM( {
)JASM" + std::string(methodsText) + R"JASM(}
)JASM";
    // major 61：帧面产物（分支体改写走 Strict 帧再生，再生后 SMT 在场）。
    return assembleJasm(src, 61);
}

/// 被调用方类：pick()I（返回 1）+ keep()V。调用点删除不得改写它。
Bytes buildCalleeClass(const std::string& name)
{
    return callerClass(name, R"JASM(  .method public static pick ()I {
    code: {
      iconst_1
      ireturn
    }
  }
  .method public static keep ()V {
    code: {
      return
    }
  }
)JASM");
}

/// 点名 include 规格。
MethodCallSpec spec(const char* owner, const char* name, const char* desc)
{
    MethodCallSpec s;
    s.owner = ca::str::Utf8String::from_cstr(owner);
    s.name  = ca::str::Utf8String::from_cstr(name);
    s.desc  = ca::str::Utf8String::from_cstr(desc);
    return s;
}

/// Used.pick()I 的 include 规格（本文件最常用点名）。
MethodCallSpec pickSpec() { return spec("Used", "pick", "()I"); }

/// 读回 class 字节中某方法 promote 后的指令序（按方法名取第一个命中）。
std::vector<jbc::core::Op> methodOps(const Bytes& bytes, const char* methodName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok()) << "session open failed: " << methodName;
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (unit.symbols().str(unit.methods[idx].name) != methodName) continue;
        if (!jbc::ir::hasCode(*session, idx)) return {};
        auto promoted = jbc::ir::promoteMethod(*session, idx);
        EXPECT_TRUE(promoted.is_ok())
            << "promote failed: " << methodName << ": "
            << (promoted.is_err() ? jbc::ir::to_string(promoted.unwrap_err()) : "");
        if (promoted.is_err()) return {};
        auto                       cuRef = std::move(promoted).unwrap();
        std::vector<jbc::core::Op> ops;
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            EXPECT_NE(in, nullptr) << "dead insn slot: " << methodName;
            if (in == nullptr) return {};
            ops.push_back(in->op);
        }
        return ops;
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return {};
}

/// 方法体是否仍含 invoke 家族指令。
bool hasInvokeOp(const Bytes& bytes, const char* methodName)
{
    for (const jbc::core::Op op : methodOps(bytes, methodName)) {
        if (op == jbc::core::Op::INVOKEVIRTUAL || op == jbc::core::Op::INVOKESPECIAL ||
            op == jbc::core::Op::INVOKESTATIC || op == jbc::core::Op::INVOKEINTERFACE) {
            return true;
        }
    }
    return false;
}

const mjbase::model::ClassResource* classResource(mjbase::model::ResourceContainer& container,
                                                  const std::string&                uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

/// 跑一次 pass 的便利壳（AM 仅作管线上下文，本 pass 不消费分析）。
mjbase::spi::PreservedAnalyses runPass(MethodCallRemover&                 pass,
                                       mjbase::model::ResourceContainer&  container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    return pass.run(container, gam);
}

}   // namespace

// ── 默认行为与点名删调用点 ───────────────────────────────────────────────────

TEST(MethodCallRemover, NoConfigIsCompleteNoOp)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      invokestatic Used.pick ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    const auto* before = classResource(container, "Caller.class");
    ASSERT_NE(before, nullptr);
    const Bytes original = before->get();

    MethodCallRemover pass;   // 默认配置：两面全关
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    const auto* after = classResource(container, "Caller.class");
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->get(), original);
}

TEST(MethodCallRemover, RemovesStaticCallWithSwallowedCat1Result)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      invokestatic Used.pick ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes callerBefore = classResource(container, "Caller.class")->get();
    const Bytes usedBefore   = classResource(container, "Used.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    const auto* used   = classResource(container, "Used.class");
    ASSERT_NE(caller, nullptr);
    ASSERT_NE(used, nullptr);
    EXPECT_EQ(used->get(), usedBefore) << "被点名成员所在类不得改写";
    const auto ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], jbc::core::Op::RETURN) << "invokestatic + pop 应整窗消失";
    EXPECT_NE(caller->get(), callerBefore);
}

TEST(MethodCallRemover, RemovesAllFourInvokeFamilies)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "Caller.class",
        callerClass("Caller",
                    R"JASM(  .method public static mStatic ()V {
    code: {
      invokestatic Used.keep ()V
      return
    }
  }
  .method public static mVirtual ()V {
    code: {
      aconst_null
      invokevirtual Used.virt ()V
      return
    }
  }
  .method public static mSpecial ()V {
    code: {
      aconst_null
      invokespecial Used.priv ()V
      return
    }
  }
  .method public static mIface ()V {
    code: {
      aconst_null
      invokeinterface Itf.f ()V
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "keep", "()V"));
    config.includeCalls.push_back(spec("Used", "virt", "()V"));
    config.includeCalls.push_back(spec("Used", "priv", "()V"));
    config.includeCalls.push_back(spec("Itf", "f", "()V"));
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    // static：无参无返回 → 纯 [RETURN]。
    EXPECT_EQ(methodOps(caller->get(), "mStatic"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
    // virtual/special/interface：aconst_null 生产者保留，接收者由补入的
    // POP 吞掉（原位插入在调用点之前）。
    for (const char* m : {"mVirtual", "mSpecial", "mIface"}) {
        EXPECT_EQ(methodOps(caller->get(), m),
                  (std::vector<jbc::core::Op>{jbc::core::Op::ACONST_NULL, jbc::core::Op::POP,
                                              jbc::core::Op::RETURN}))
            << m;
    }
}

TEST(MethodCallRemover, SwallowsCat2ResultAndCat2Args)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "Caller.class",
        callerClass("Caller",
                    R"JASM(  .method public static mLongRet ()V {
    code: {
      invokestatic Used.jl ()J
      pop2
      return
    }
  }
  .method public static mLongArg ()V {
    code: {
      lconst_1
      invokestatic Used.pl (J)I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "jl", "()J"));
    config.includeCalls.push_back(spec("Used", "pl", "(J)I"));
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(methodOps(caller->get(), "mLongRet"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
    // cat2 实参吞 POP2；返回值消费面（POP）随调用一并消失，生产者保留。
    EXPECT_EQ(methodOps(caller->get(), "mLongArg"),
              (std::vector<jbc::core::Op>{jbc::core::Op::LCONST_1, jbc::core::Op::POP2,
                                          jbc::core::Op::RETURN}));
}

TEST(MethodCallRemover, VoidCallArgsSwallowedByPops)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      iconst_1
      lconst_1
      ldc "x"
      invokestatic Used.takes (IJLjava/lang/String;)V
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "takes", "(IJLjava/lang/String;)V"));
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    // iconst_1 + lconst_1 + ldc 生产者保留；吞参序 POP(I) POP2(J) POP(ref)。
    const auto ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[3], jbc::core::Op::POP);
    EXPECT_EQ(ops[4], jbc::core::Op::POP2);
    EXPECT_EQ(ops[5], jbc::core::Op::POP);
    EXPECT_EQ(ops[6], jbc::core::Op::RETURN);
}

TEST(MethodCallRemover, DeadLocalStoreResultRemovedLiveStoreKept)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "Caller.class",
        callerClass("Caller",
                    R"JASM(  .method public static deadStore ()V {
    code: {
      invokestatic Used.pick ()I
      istore 1
      return
    }
  }
  .method public static liveStore ()V {
    code: {
      invokestatic Used.pick ()I
      istore 1
      iload 1
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(methodOps(caller->get(), "deadStore"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
    EXPECT_TRUE(hasInvokeOp(caller->get(), "liveStore")) << "活局部 store 的调用必须保留";
}

// ── 放弃路径（宁可留调用，不可产生非法栈）────────────────────────────────────

TEST(MethodCallRemover, DupPrefixConstructorSiteAbandoned)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static make ()V {
    code: {
      new Used
      dup
      invokespecial Used.<init> ()V
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "<init>", "()V"));
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved()) << "dup 前缀调用点保守放弃";
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, Dup2PrefixStaticSiteAbandoned)
{
    // 窗尾补 pop2 清栈（放弃判定只看调用点前一条为 DUP2，面同实）。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      lconst_1
      dup2
      invokestatic Used.pl (J)V
      pop2
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "pl", "(J)V"));
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, MethodHandlePinnedMemberAbandoned)
{
    // condy ldc 的 BSM 引用位 { invokestatic, Used.pick, ()I } 落 CP
    // MethodHandle(6, Used.pick()I)——钉死三元组与原 ldc 直载柄同实。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      ldc { mh, Ljava/lang/invoke/MethodHandle;, { invokestatic, Used.pick, ()I }, { } }
      pop
      invokestatic Used.pick ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved())
        << "句柄钉死的成员整体放弃：删调用与删除意图矛盾";
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, ExcludeCancelsInclude)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      invokestatic Used.pick ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    config.excludeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, UnreadableClassAbortsWholePass)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      invokestatic Used.pick ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    // 畸形 class 资源（刻意截断样本，非手拼类）：句柄钉死证明需要全容器
    // 可读，任一不可读整 pass 放弃。
    container.addResource("broken.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "broken.class", Bytes{0xCA, 0xFE, 0xBA, 0xBE, 0x00}));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, BranchAnchoredSiteKept)
{
    // 异常区间起点锚调用点（labelsAt 对分支目标与异常边界同源留锚，
    // handler 裸 athrow 吃掉异常栈）→ 调用点被锚定，放弃。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    exceptions: {
      { Lb, Lr, Lr, * }
    },
    code: {
      Lb:
      invokestatic Used.pick ()I
      pop
      return
      Lr:
      athrow
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved()) << "分支锚定的调用点保守放弃";
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, ConsumedResultSiteAbandoned)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      invokestatic Used.pick ()I
      iconst_1
      iadd
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

// ── 调用图边同步 ─────────────────────────────────────────────────────────────

TEST(MethodCallRemover, CallGraphEdgesResyncAfterRemoval)
{
    namespace ref = mjbase::reference;
    mjbase::model::ResourceContainer container;
    addClassResource(container, "App.class",
                     callerClass("App", R"JASM(  .method public static main ()V {
    code: {
      invokestatic Used.pick ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    gam.registerAnalysis<ref::ReferenceIndexAnalysis>();
    gam.registerAnalysis<ref::CallGraphAnalysis>();

    auto keepKey = [](mjbase::asm_classpath::CallGraph& graph) {
        return mjbase::memberKeyOf(graph.pool(), mjbase::classKeyOf(graph.pool(), "Used"),
                                   "pick", "()I");
    };

    auto& graphBefore = gam.getResult<ref::CallGraphAnalysis>(container);
    ASSERT_EQ(graphBefore.incomingSitesOf(keepKey(graphBefore)).size(), 1u);

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    // 经 PassManager 跑：失效由编排器按 none() 驱动——后续消费者重建出的
    // 调用图不再有被删调用边。
    mjbase::spi::GlobalPassManager mgr;
    mgr.addPass(std::make_unique<MethodCallRemover>(std::move(config)));
    EXPECT_FALSE(mgr.run(container, gam).areAllPreserved());

    auto& graphAfter = gam.getResult<ref::CallGraphAnalysis>(container);
    EXPECT_TRUE(graphAfter.incomingSitesOf(keepKey(graphAfter)).empty());
    EXPECT_EQ(graphAfter.callSiteCountOf(keepKey(graphAfter)), 0u);
}

TEST(MethodCallRemover, BranchyMethodRewrittenWithFrameRewire)
{
    // 调用点在分支方法头部（分支目标都不落在删除区间内）：
    //   invokestatic pick()I; pop; iload 0; ifeq L1; iconst_0; pop;
    //   goto L2; L1: iconst_1; pop; L2: return
    // 方法带分支 → 写回走 Strict 帧再生链（V61 装配产物，再生后 SMT 在场）。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go (I)V {
    code: {
      invokestatic Used.pick ()I
      pop
      iload 0
      ifeq L1
      iconst_0
      pop
      goto L2
      L1:
      iconst_1
      pop
      L2:
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    // 改写类必须仍可解析（会话重开 + promote 即解析门），调用点消失：
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(methodOps(caller->get(), "go"),
              (std::vector<jbc::core::Op>{jbc::core::Op::ILOAD_0, jbc::core::Op::IFEQ,
                                          jbc::core::Op::ICONST_0, jbc::core::Op::POP,
                                          jbc::core::Op::GOTO, jbc::core::Op::ICONST_1,
                                          jbc::core::Op::POP, jbc::core::Op::RETURN}));
    // 帧再生落地：Code 属性带 StackMapTable（Strict 重算产物）。
    auto read = jbc::core::readClass(jbc::core::ByteView(caller->get()));
    ASSERT_TRUE(read.is_ok());
    auto cf = std::move(read).unwrap().file;
    const jbc::core::CodeAttr* codeAttr = nullptr;
    for (auto& member : cf->methods) {
        if (cf->nameOf(member) != "go") continue;
        for (auto& slot : member.attrs) {
            if (!cf->cp.utf8Equals(slot.nameCp, "Code")) continue;
            auto decoded = cf->decodeAttr(slot);
            ASSERT_TRUE(decoded.is_ok()) << "Code attr decode failed";
            codeAttr = ca::core::tag_cast::dyn_cast<jbc::core::CodeAttr>(decoded.unwrap());
        }
    }
    ASSERT_NE(codeAttr, nullptr);
    bool hasSmt = false;
    for (const auto& slot : codeAttr->attrs) {
        if (cf->cp.utf8Equals(slot.nameCp, "StackMapTable")) hasSmt = true;
    }
    EXPECT_TRUE(hasSmt) << "分支体改写后必须再生 StackMapTable";
}

TEST(MethodCallRemover, SecondRunIsStrictNoOp)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      invokestatic Used.pick ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());
    const Bytes afterFirst = classResource(container, "Caller.class")->get();

    EXPECT_TRUE(runPass(pass, container).areAllPreserved()) << "固定点：第二跑严格 no-op";
    EXPECT_EQ(classResource(container, "Caller.class")->get(), afterFirst);
}

// ── 无用结果白名单面 ─────────────────────────────────────────────────────────

TEST(MethodCallRemover, WhitelistStringWindowRemoved)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      ldc "hello"
      invokevirtual java/lang/String.length ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;   // 内建默认白名单
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(methodOps(classResource(container, "Caller.class")->get(), "go"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
}

TEST(MethodCallRemover, WhitelistStringNonConstReceiverKept)
{
    // aload 0（实参，非常量接收者，可能 NPE）→ 保守保留。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go (Ljava/lang/String;)V {
    code: {
      aload 0
      invokevirtual java/lang/String.length ()I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, WhitelistMathWindowRemovedNonConstArgKept)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "Caller.class",
        callerClass("Caller",
                    R"JASM(  .method public static constArg ()V {
    code: {
      iconst_5
      invokestatic java/lang/Math.abs (I)I
      pop
      return
    }
  }
  .method public static varArg (I)V {
    code: {
      iload 0
      invokestatic java/lang/Math.abs (I)I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(methodOps(caller->get(), "constArg"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
    EXPECT_NE(caller->get(), before) << "varArg 保留，类应有差异（constArg 被删）";
    EXPECT_TRUE(hasInvokeOp(caller->get(), "varArg"));
}

TEST(MethodCallRemover, WhitelistCat2JdkCallRemoved)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      invokestatic java/lang/System.nanoTime ()J
      pop2
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(methodOps(classResource(container, "Caller.class")->get(), "go"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
}

TEST(MethodCallRemover, WhitelistUserDiscardableEntrySwallowsArgs)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Caller.class",
                     callerClass("Caller", R"JASM(  .method public static go ()V {
    code: {
      ldc "TAG"
      ldc "msg"
      invokestatic com/example/Log.d (Ljava/lang/String;Ljava/lang/String;)I
      pop
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;
    config.useDefaultWhitelist         = false;   // 只用用户追加名单
    config.discardableCalls.push_back(
        spec("com/example/Log", "d", "(Ljava/lang/String;Ljava/lang/String;)I"));
    MethodCallRemover pass(std::move(config));
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    // 调用与结果 pop 消失；实参生产者保留、由吞参 pop 收走。
    const auto ops = methodOps(classResource(container, "Caller.class")->get(), "go");
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], jbc::core::Op::LDC);
    EXPECT_EQ(ops[1], jbc::core::Op::LDC);
    EXPECT_EQ(ops[2], jbc::core::Op::POP);
    EXPECT_EQ(ops[3], jbc::core::Op::POP);
    EXPECT_EQ(ops[4], jbc::core::Op::RETURN);
}

TEST(MethodCallRemover, WhitelistFacetOffByDefault)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "Caller.class",
        callerClass("Caller",
                    R"JASM(  .method public static s ()V {
    code: {
      ldc "hello"
      invokevirtual java/lang/String.length ()I
      pop
      return
    }
  }
  .method public static t ()V {
    code: {
      invokestatic java/lang/System.nanoTime ()J
      pop2
      return
    }
  }
)JASM"));
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;   // 白名单面默认关
    MethodCallRemover pass(std::move(config));
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}
