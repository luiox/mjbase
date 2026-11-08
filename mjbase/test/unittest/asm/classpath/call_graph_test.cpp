/// @file        call_graph_test.cpp
/// @brief       asm/classpath 调用图测试：调用点提取、正反向索引、调用点计数、
///              虚调用未解析标记、可达闭包与库类过滤、外部目标丢弃、会话喂入、
///              读段/升格/操作数三段错误路径。
///
/// 夹具纪律：类字节一律 jasm 文本装配（JvmJasmService）。默认帧less 49
///（shared/jasm_fixture.hpp 版本口径）——调用图夹具只承载 invoke 序列结构，
/// 静态 void 调用链栈面平凡成立。invokedynamic 站点走 61 装配（BSM 面的
/// 实证径，indy_site_inliner_jvm_test 同径），61 装配期做帧分析，接收者以
/// aconst_null 垫出、结果以 pop 清栈自洽——垫栈指令不产生调用点。
///
/// 例外①（装配器无法直发的非法形态：改写前 expectValidateOk 自检 + 申报，
/// 以被测 build() 实跑为最终证明）：
/// - PromoteFailureFailsBuild：合法 goto 基座经 retargetFirstGoto 单点把
///   目标改指 pc1（落在自身操作数中间）——装配器只发合法跳转；
/// - InvokeOperandMismatchFailsBuild：invokestatic 的 CP 操作数经 DOM 单点
///   改指 Utf8 条目——装配器只发 tag 相符的 Methodref；
/// - BadBytesFailAtReadStage：截断字节是读段语料（格式即坏），字节级保留。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/bytes.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"

#include "shared/jasm_fixture.hpp"

namespace {

using Bytes = std::vector<ca::u8>;
using jbc::core::Op;
using mjbase::asm_classpath::CallChain;
using mjbase::asm_classpath::CallChainOptions;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::asm_classpath::CallGraphOptions;
using mjbase::asm_classpath::CallSite;
using mjbase::asm_classpath::CallTree;
using mjbase::asm_classpath::CallTreeOptions;
using mjbase::asm_classpath::InvokeKind;
using mjbase::asm_classpath::kNoCallTreeParent;

/// CallSpec → jasm 助记符（invoke 家族一一对应）。
const char* invokeMnemonic(Op op)
{
    switch (op) {
    case Op::INVOKEVIRTUAL: return "invokevirtual";
    case Op::INVOKESPECIAL: return "invokespecial";
    case Op::INVOKESTATIC: return "invokestatic";
    case Op::INVOKEINTERFACE: return "invokeinterface";
    case Op::INVOKEDYNAMIC: return "invokedynamic";
    default: return "invokestatic";
    }
}

/// 单条 invoke 指令规格（调用点提取只看 invoke 指令本身）。
struct CallSpec
{
    Op op = Op::INVOKESTATIC;
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

/// 方法规格：abstract 时无方法体（calls 忽略，出边为空）。
struct MethodSpec
{
    std::string_view name;
    std::string_view desc;
    std::vector<CallSpec> calls;
    bool abstract = false;
};

/// jasm 装配最小 classfile（帧less 49：public super /
/// super=java/lang/Object / 无接口无字段无类属性），方法各带一个 Code 属性，
/// 指令序列 = invokes + return。
Bytes makeClass(std::string_view thisClass, const std::vector<MethodSpec>& methods)
{
    std::string memberText;
    for (const MethodSpec& m : methods) {
        if (m.abstract) {
            memberText += "  .method public abstract ";
            memberText += m.name;
            memberText += ' ';
            memberText += m.desc;
            memberText += " { }\n";
            continue;
        }
        memberText += "  .method public static ";
        memberText += m.name;
        memberText += ' ';
        memberText += m.desc;
        memberText += " {\n    code: {\n";
        for (const CallSpec& c : m.calls) {
            memberText += "      ";
            memberText += invokeMnemonic(c.op);
            memberText += ' ';
            if (c.op != Op::INVOKEDYNAMIC) {
                memberText += c.owner;
                memberText += '.';
            }
            memberText += c.name;
            memberText += ' ';
            memberText += c.desc;
            memberText += '\n';
        }
        memberText += "      return\n    }\n  }\n";
    }
    const std::string src = ".super java/lang/Object\n.class public super "
        + std::string(thisClass) + " {\n" + memberText + "}\n";
    return mjbase::test::assembleJasm(src);
}

/// 五类 invoke 站点夹具（K.m）：invokedynamic 需 BootstrapMethods 面，
/// 61 装配（indy_site_inliner_jvm_test 同径）；61 装配期做帧分析，接收者用
/// aconst_null 垫出、结果以 pop 清栈——垫栈不产生调用点，站点 kind 次序
/// 与原字节夹具一致（virtual/special/static/interface/dynamic）。
const Bytes& invokeKindsClassBytes()
{
    static const Bytes cls = mjbase::test::assembleJasm(R"JASM(.super java/lang/Object
.class public super K {
  .method public static m ()V {
    code: {
      aconst_null
      invokevirtual java/lang/Object.toString ()Ljava/lang/String;
      pop
      aconst_null
      invokespecial K.self ()V
      invokestatic K.helper ()V
      aconst_null
      invokeinterface java/util/List.size ()I
      pop
      invokedynamic apply ()Ljava/lang/Object; { invokestatic, Boot.boot, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { }
      pop
      return
    }
  }
}
)JASM", 61);
    return cls;
}

/// 例外①载面（invoke 操作数改锚）：首个方法 Code 内第一条 invokestatic
///（0xB8）的 CP 操作数单点改指本方法名的 Utf8 槽位（tag 与 invoke 不符）。
Bytes pointInvokeOperandAtUtf8(const Bytes& clsBytes)
{
    return mjbase::test::mutateClassFile(
        clsBytes, "invoke-operand-utf8", [](jbc::core::ClassFile& cf) {
            ASSERT_FALSE(cf.methods.empty());
            if (cf.methods.empty()) return;
            const ca::u32 utf8Slot = cf.methods[0].nameCp;
            for (jbc::core::AttrSlot& slot : cf.methods[0].attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
                if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                    ADD_FAILURE() << "invoke-operand-utf8: Code 解码失败";
                    return;
                }
                auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
                if (code == nullptr) {
                    ADD_FAILURE() << "invoke-operand-utf8: Code 槽类型不符";
                    return;
                }
                for (usize i = 0; i + 2 < code->code.size(); ++i) {
                    if (code->code[i] != 0xB8) continue;   // INVOKESTATIC
                    code->code[i + 1] = static_cast<ca::u8>(utf8Slot >> 8);
                    code->code[i + 2] = static_cast<ca::u8>(utf8Slot);
                    return;
                }
                ADD_FAILURE() << "invoke-operand-utf8: 未找到 INVOKESTATIC";
                return;
            }
            ADD_FAILURE() << "invoke-operand-utf8: 无 Code 属性";
        });
}

mjbase::MemberKey keyOf(CallGraph& g, std::string_view cls, std::string_view name,
                        std::string_view desc)
{
    return mjbase::memberKeyOf(g.pool(), mjbase::classKeyOf(g.pool(), cls), name, desc);
}

mjbase::MemberKey missingKey(CallGraph& g)
{
    return mjbase::memberKeyOf(g.pool(), mjbase::classKeyOf(g.pool(), "<missing>"),
                               "none", "()V");
}

bool containsKey(const std::vector<mjbase::MemberKey>& keys, const mjbase::KeyPool& pool,
                 std::string_view cls, std::string_view name, std::string_view desc)
{
    for (const mjbase::MemberKey k : keys) {
        if (pool.str(k.cls) == cls && pool.str(k.name) == name && pool.str(k.desc) == desc) {
            return true;
        }
    }
    return false;
}

CallGraph buildGraphOf(const std::vector<Bytes>& classes, CallGraphOptions options = {})
{
    CallGraphBuilder b(options);
    for (const Bytes& c : classes) {
        auto add = b.addClassBytes(jbc::core::ByteView{c.data(), c.size()});
        EXPECT_TRUE(add.is_ok());
    }
    auto r = b.build();
    EXPECT_TRUE(r.is_ok());
    return std::move(r).unwrap();
}

/// 两类基础夹具：App.entry → Lib.callee + java/lang/Integer.parseInt。
const Bytes& appClassBytes()
{
    static const Bytes cls = makeClass("App", {{"entry", "()V", {
        {Op::INVOKESTATIC, "Lib", "callee", "()V"},
        {Op::INVOKESTATIC, "java/lang/Integer", "parseInt", "(Ljava/lang/String;)I"},
    }}});
    return cls;
}

const Bytes& libClassBytes()
{
    static const Bytes cls =
        makeClass("Lib", {{"callee", "()V", {}}, {"abs", "()V", {}, true}});
    return cls;
}

}   // namespace

// ---------------------------------------------------------------------------
// 构建与正反向查询（A 调 B 的对拍主用例）
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, BuildsDeclaredAndExternalEdgesFromBytes)
{
    CallGraph g = buildGraphOf({appClassBytes(), libClassBytes()});

    const mjbase::MemberKey entry = keyOf(g, "App", "entry", "()V");
    const mjbase::MemberKey callee = keyOf(g, "Lib", "callee", "()V");
    const mjbase::MemberKey abs = keyOf(g, "Lib", "abs", "()V");
    const mjbase::MemberKey parseInt =
        keyOf(g, "java/lang/Integer", "parseInt", "(Ljava/lang/String;)I");

    ASSERT_TRUE(g.contains(entry));
    ASSERT_TRUE(g.contains(callee));
    ASSERT_TRUE(g.contains(abs));
    ASSERT_TRUE(g.contains(parseInt));
    EXPECT_EQ(g.methodCount(), 4u);
    EXPECT_EQ(g.siteCount(), 2u);

    EXPECT_TRUE(g.isDeclared(entry));
    EXPECT_TRUE(g.isDeclared(callee));
    EXPECT_TRUE(g.isDeclared(abs));   // abstract 方法也建节点
    EXPECT_FALSE(g.isDeclared(parseInt));
    EXPECT_EQ(g.accessOf(entry), 0x0009u);
    EXPECT_EQ(g.accessOf(abs), 0x0401u);

    // 出边：entry 的两个调用点，callee/parseInt 各一。
    const std::vector<CallSite> out = g.outgoingSitesOf(entry);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].callee, callee);
    EXPECT_EQ(out[1].callee, parseInt);
    EXPECT_EQ(out[0].caller, entry);
    EXPECT_TRUE(out[0].isDirect());
    EXPECT_EQ(out[0].kind, InvokeKind::Static);

    // 入边（反向索引）：callee 与 parseInt 都被 entry 调。
    const std::vector<CallSite> in = g.incomingSitesOf(callee);
    ASSERT_EQ(in.size(), 1u);
    EXPECT_EQ(in[0].caller, entry);
    EXPECT_EQ(in[0].callee, callee);
    ASSERT_EQ(g.incomingSitesOf(parseInt).size(), 1u);

    // abstract 方法无调用点；未知方法查空不炸。
    EXPECT_TRUE(g.outgoingSitesOf(abs).empty());
    EXPECT_TRUE(g.outgoingSitesOf(missingKey(g)).empty());
    EXPECT_TRUE(g.incomingSitesOf(missingKey(g)).empty());

    // 去重集合（旧 outgoingCallsOf/incomingCallsOf 的对应面）。
    EXPECT_EQ(g.calleesOf(entry).size(), 2u);
    EXPECT_TRUE(containsKey(g.calleesOf(entry), g.pool(), "java/lang/Integer", "parseInt",
                            "(Ljava/lang/String;)I"));
    EXPECT_TRUE(containsKey(g.callersOf(callee), g.pool(), "App", "entry", "()V"));
}

TEST(MjAsmClasspathTest, CallSitesResolveInsnInKeptAliveSession)
{
    CallGraph g = buildGraphOf({appClassBytes(), libClassBytes()});
    const mjbase::MemberKey entry = keyOf(g, "App", "entry", "()V");

    const std::vector<CallSite> out = g.outgoingSitesOf(entry);
    ASSERT_EQ(out.size(), 2u);

    // 调用点的会话保活：sessionOf 与 sessionOfClass(App) 交回同一会话。
    const std::shared_ptr<const jbc::ir::ClassSession> sApp =
        g.sessionOfClass(mjbase::classKeyOf(g.pool(), "App"));
    ASSERT_NE(sApp, nullptr);
    EXPECT_EQ(g.sessionOf(out[0]).get(), sApp.get());
    EXPECT_EQ(g.sessionOf(out[1]).get(), sApp.get());

    // 调用点指令可在该会话的 CodeDom 内解析：op 与源 pc 逐条对上。
    const jbc::ir::CodeUnit* code = sApp->unit().methods[0].code.get();
    ASSERT_NE(code, nullptr);
    for (const CallSite& site : out) {
        const jbc::ir::Insn* insn = code->dom.peek(site.insn);
        ASSERT_NE(insn, nullptr);
        EXPECT_EQ(insn->op, Op::INVOKESTATIC);
    }
    EXPECT_EQ(code->dom.peek(out[0].insn)->srcPc, 0u);
    EXPECT_EQ(code->dom.peek(out[1].insn)->srcPc, 3u);   // invokestatic 各 3 字节
    EXPECT_NE(out[0].insn, out[1].insn);

    // 只被引用的类（未喂字节）没有会话。
    EXPECT_EQ(g.sessionOfClass(mjbase::classKeyOf(g.pool(), "java/lang/Integer")), nullptr);
}

TEST(MjAsmClasspathTest, InvokeKindsMarkVirtualUnresolved)
{
    CallGraph g = buildGraphOf({invokeKindsClassBytes()});
    const mjbase::MemberKey m = keyOf(g, "K", "m", "()V");

    const std::vector<CallSite> out = g.outgoingSitesOf(m);
    ASSERT_EQ(out.size(), 5u);

    EXPECT_EQ(out[0].kind, InvokeKind::Virtual);
    EXPECT_EQ(out[1].kind, InvokeKind::Special);
    EXPECT_EQ(out[2].kind, InvokeKind::Static);
    EXPECT_EQ(out[3].kind, InvokeKind::Interface);
    EXPECT_EQ(out[4].kind, InvokeKind::Dynamic);

    // 虚调用（virtual/interface）标记 unresolved：目标待层次批重写。
    EXPECT_TRUE(out[0].isVirtualDispatch());
    EXPECT_TRUE(out[3].isVirtualDispatch());
    EXPECT_FALSE(out[0].isDirect());
    EXPECT_FALSE(out[3].isDirect());

    // special/static/dynamic 记直边。
    EXPECT_TRUE(out[1].isDirect());
    EXPECT_TRUE(out[2].isDirect());
    EXPECT_TRUE(out[4].isDirect());

    // indy 无 owner：callee 键的 owner 段驻留空串，name/desc 来自 NaT。
    EXPECT_TRUE(g.pool().str(out[4].callee.cls).empty());
    EXPECT_EQ(g.pool().str(out[4].callee.name), "apply");
    EXPECT_EQ(g.pool().str(out[4].callee.desc), "()Ljava/lang/Object;");

    // 谓词分类表。
    InvokeKind kind = InvokeKind::Static;
    EXPECT_FALSE(mjbase::asm_classpath::invokeKindOf(Op::NOP, kind));
    EXPECT_TRUE(mjbase::asm_classpath::invokeKindOf(Op::INVOKEVIRTUAL, kind));
    EXPECT_EQ(kind, InvokeKind::Virtual);
}

TEST(MjAsmClasspathTest, CallSiteCountCountsRepeatedSitesFromSameCaller)
{
    const Bytes caller = makeClass("Caller", {{"twice", "()V", {
        {Op::INVOKESTATIC, "Lib", "callee", "()V"},
        {Op::INVOKESTATIC, "Lib", "callee", "()V"},
    }}});

    CallGraph g = buildGraphOf({caller, libClassBytes()});
    const mjbase::MemberKey twice = keyOf(g, "Caller", "twice", "()V");
    const mjbase::MemberKey callee = keyOf(g, "Lib", "callee", "()V");

    // 同一调用方多处调用分别计数，与去重调用方集合互补（旧版验收语义）。
    const std::vector<CallSite> out = g.outgoingSitesOf(twice);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].callee, callee);
    EXPECT_EQ(out[1].callee, callee);
    EXPECT_NE(out[0].insn, out[1].insn);
    EXPECT_EQ(g.callSiteCountOf(callee), 2u);
    EXPECT_EQ(g.callersOf(callee).size(), 1u);

    // 未被调用 / 不存在的方法计数为 0。
    EXPECT_EQ(g.callSiteCountOf(twice), 0u);
    EXPECT_EQ(g.callSiteCountOf(missingKey(g)), 0u);
}

// ---------------------------------------------------------------------------
// 可达闭包与库类过滤
// ---------------------------------------------------------------------------

namespace {

/// 三类链夹具：Root.entry → {Mid.step, java/lang/Integer.parseInt}；
/// Mid.step → Leaf.done；Leaf.done 无出边。
struct ChainFixture
{
    Bytes root = makeClass("Root", {{"entry", "()V", {
        {Op::INVOKESTATIC, "Mid", "step", "()V"},
        {Op::INVOKESTATIC, "java/lang/Integer", "parseInt", "(Ljava/lang/String;)I"},
    }}});
    Bytes mid =
        makeClass("Mid", {{"step", "()V", {{Op::INVOKESTATIC, "Leaf", "done", "()V"}}}});
    Bytes leaf = makeClass("Leaf", {{"done", "()V", {}}});
};

}   // namespace

TEST(MjAsmClasspathTest, ReachableClosureWalksChainAndFiltersLibrary)
{
    const ChainFixture fx;
    CallGraph g = buildGraphOf({fx.root, fx.mid, fx.leaf});

    const mjbase::MemberKey entry = keyOf(g, "Root", "entry", "()V");
    const mjbase::MemberKey step = keyOf(g, "Mid", "step", "()V");

    // 闭包包含入口自身 + 沿出边可达集（含库类外部节点）。
    std::vector<mjbase::MemberKey> closure = g.reachableClosure({entry});
    ASSERT_EQ(closure.size(), 4u);
    EXPECT_TRUE(containsKey(closure, g.pool(), "Root", "entry", "()V"));
    EXPECT_TRUE(containsKey(closure, g.pool(), "Mid", "step", "()V"));
    EXPECT_TRUE(containsKey(closure, g.pool(), "Leaf", "done", "()V"));
    EXPECT_TRUE(containsKey(closure, g.pool(), "java/lang/Integer", "parseInt",
                            "(Ljava/lang/String;)I"));

    // 过滤库类：指向 java/* 的边不再传播。
    closure = g.reachableClosure({entry}, /*skipLibraryClasses=*/true);
    ASSERT_EQ(closure.size(), 3u);
    EXPECT_FALSE(containsKey(closure, g.pool(), "java/lang/Integer", "parseInt",
                             "(Ljava/lang/String;)I"));

    // 从中间出发的子闭包；未知入口与空入口集。
    closure = g.reachableClosure({step});
    ASSERT_EQ(closure.size(), 2u);
    EXPECT_TRUE(containsKey(closure, g.pool(), "Mid", "step", "()V"));
    EXPECT_TRUE(containsKey(closure, g.pool(), "Leaf", "done", "()V"));
    EXPECT_TRUE(g.reachableClosure({missingKey(g)}).empty());
    EXPECT_TRUE(g.reachableClosure({}).empty());

    // 多入口并集（两个叶节点互不可达）。
    const mjbase::MemberKey done = keyOf(g, "Leaf", "done", "()V");
    const mjbase::MemberKey parseInt =
        keyOf(g, "java/lang/Integer", "parseInt", "(Ljava/lang/String;)I");
    closure = g.reachableClosure({done, parseInt});
    ASSERT_EQ(closure.size(), 2u);
}

TEST(MjAsmClasspathTest, ExternalTargetsCanBeDropped)
{
    const ChainFixture fx;
    CallGraph g = buildGraphOf({fx.root, fx.mid, fx.leaf},
                               CallGraphOptions{/*includeExternalTargets=*/false});

    const mjbase::MemberKey entry = keyOf(g, "Root", "entry", "()V");
    const mjbase::MemberKey parseInt =
        keyOf(g, "java/lang/Integer", "parseInt", "(Ljava/lang/String;)I");

    EXPECT_FALSE(g.contains(parseInt));   // 外部节点不落
    EXPECT_EQ(g.methodCount(), 3u);
    EXPECT_EQ(g.siteCount(), 2u);   // entry→step、step→done；entry→parseInt 丢弃

    const std::vector<CallSite> out = g.outgoingSitesOf(entry);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(g.pool().str(out[0].callee.cls), "Mid");
    EXPECT_EQ(g.callSiteCountOf(parseInt), 0u);

    const std::vector<mjbase::MemberKey> closure = g.reachableClosure({entry});
    ASSERT_EQ(closure.size(), 3u);
    EXPECT_FALSE(containsKey(closure, g.pool(), "java/lang/Integer", "parseInt",
                             "(Ljava/lang/String;)I"));
}

// ---------------------------------------------------------------------------
// 会话喂入与幂等
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, BuilderAcceptsAlreadyOpenSessions)
{
    auto r = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{libClassBytes().data(), libClassBytes().size()});
    ASSERT_TRUE(r.is_ok());
    std::shared_ptr<jbc::ir::ClassSession> lib = std::move(r).unwrap();

    const Bytes caller = makeClass("Caller", {{"once", "()V", {
        {Op::INVOKESTATIC, "Lib", "callee", "()V"},
    }}});

    CallGraphBuilder b;
    b.addSession(lib);
    b.addSession(lib);   // 同一会话重复喂入幂等
    ASSERT_TRUE(
        b.addClassBytes(jbc::core::ByteView{caller.data(), caller.size()}).is_ok());

    auto built = b.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph g = std::move(built).unwrap();

    const mjbase::MemberKey callee = keyOf(g, "Lib", "callee", "()V");
    ASSERT_EQ(g.callSiteCountOf(callee), 1u);   // 幂等：没有重复边

    // sessionOfClass 交回的就是喂入的那个会话（保活副本指针相等）；
    // 调用点指令所在的会话是调用方类（Caller，经字节喂入）的会话。
    EXPECT_EQ(g.sessionOfClass(mjbase::classKeyOf(g.pool(), "Lib")).get(), lib.get());
    const std::vector<CallSite> in = g.incomingSitesOf(callee);
    ASSERT_EQ(in.size(), 1u);
    EXPECT_EQ(g.sessionOf(in[0]).get(),
              g.sessionOfClass(mjbase::classKeyOf(g.pool(), "Caller")).get());
    EXPECT_NE(g.sessionOf(in[0]).get(), nullptr);
    // 喂入会话的方法已被 build 升格（InsnId 可解析的前提）。
    EXPECT_NE(lib->unit().methods[0].code.get(), nullptr);

    // build 可重复调用（喂入集合不变 → 同样的图）。
    auto again = b.build();
    ASSERT_TRUE(again.is_ok());
    CallGraph g2 = std::move(again).unwrap();
    EXPECT_EQ(g2.callSiteCountOf(keyOf(g2, "Lib", "callee", "()V")), 1u);
}

TEST(MjAsmClasspathTest, EmptyBuilderYieldsEmptyGraph)
{
    CallGraphBuilder b;
    auto r = b.build();
    ASSERT_TRUE(r.is_ok());
    CallGraph g = std::move(r).unwrap();
    EXPECT_EQ(g.methodCount(), 0u);
    EXPECT_EQ(g.siteCount(), 0u);
}

// ---------------------------------------------------------------------------
// 调用链 / 调用树视图
// ---------------------------------------------------------------------------

/// a → {b,c}，b/c → d，d → a（回边），leaf 无出边，dup 重复调用 b 两次。
const Bytes& chainClassBytes()
{
    static const Bytes cls = makeClass("Chain", {
        {"a", "()V", {{Op::INVOKESTATIC, "Chain", "b", "()V"},
                      {Op::INVOKESTATIC, "Chain", "c", "()V"}}},
        {"b", "()V", {{Op::INVOKESTATIC, "Chain", "d", "()V"}}},
        {"c", "()V", {{Op::INVOKESTATIC, "Chain", "d", "()V"}}},
        {"d", "()V", {{Op::INVOKESTATIC, "Chain", "a", "()V"}}},
        {"leaf", "()V", {}},
        {"dup", "()V", {{Op::INVOKESTATIC, "Chain", "b", "()V"},
                        {Op::INVOKESTATIC, "Chain", "b", "()V"}}},
    });
    return cls;
}

std::vector<std::string> chainNames(const CallGraph& g, const CallChain& chain)
{
    std::vector<std::string> names;
    names.reserve(chain.methods.size());
    for (const mjbase::MemberKey m : chain.methods) {
        names.emplace_back(g.pool().str(m.name));
    }
    return names;
}

using NameChain = std::vector<std::string>;

TEST(MjAsmClasspathTest, EnumerateChainsYieldsSimplePathsInDfsOrder)
{
    CallGraph g = buildGraphOf({chainClassBytes()});
    const mjbase::MemberKey a = keyOf(g, "Chain", "a", "()V");

    auto cursor = g.enumerateChains(a);
    CallChain chain;
    ASSERT_TRUE(cursor.next(chain));
    EXPECT_EQ(chainNames(g, chain), (NameChain{"a", "b", "d"}));
    ASSERT_TRUE(cursor.next(chain));
    EXPECT_EQ(chainNames(g, chain), (NameChain{"a", "c", "d"}));

    // 结束：返回 false 且 out 不变。
    chain.methods.assign(3, missingKey(g));
    EXPECT_FALSE(cursor.next(chain));
    EXPECT_EQ(chain.methods.size(), 3u);
}

TEST(MjAsmClasspathTest, EnumerateChainsTruncatesAtDepthLimit)
{
    CallGraph g = buildGraphOf({chainClassBytes()});
    const mjbase::MemberKey a = keyOf(g, "Chain", "a", "()V");

    CallChainOptions options;
    options.maxDepth = 1;
    auto cursor = g.enumerateChains(a, options);
    CallChain chain;
    ASSERT_TRUE(cursor.next(chain));
    EXPECT_EQ(chainNames(g, chain), (NameChain{"a", "b"}));
    ASSERT_TRUE(cursor.next(chain));
    EXPECT_EQ(chainNames(g, chain), (NameChain{"a", "c"}));
    EXPECT_FALSE(cursor.next(chain));

    // 深度上限 = 2 时环仍在路径内截断，两条链回到完整形态。
    options.maxDepth = 2;
    auto cursor2 = g.enumerateChains(a, options);
    ASSERT_TRUE(cursor2.next(chain));
    EXPECT_EQ(chainNames(g, chain), (NameChain{"a", "b", "d"}));
    ASSERT_TRUE(cursor2.next(chain));
    EXPECT_EQ(chainNames(g, chain), (NameChain{"a", "c", "d"}));
    EXPECT_FALSE(cursor2.next(chain));
}

TEST(MjAsmClasspathTest, EnumerateChainsLeafMissingRootAndDedup)
{
    CallGraph g = buildGraphOf({chainClassBytes()});

    // 无出边的方法：单节点链。
    auto leaf = g.enumerateChains(keyOf(g, "Chain", "leaf", "()V"));
    CallChain chain;
    ASSERT_TRUE(leaf.next(chain));
    EXPECT_EQ(chainNames(g, chain), (NameChain{"leaf"}));
    EXPECT_FALSE(leaf.next(chain));

    // 根不在图中：立即耗尽，不产出空链。
    auto missing = g.enumerateChains(missingKey(g));
    CallChain untouched;
    EXPECT_FALSE(missing.next(untouched));
    EXPECT_TRUE(untouched.methods.empty());

    // 重复调用点合并为一条边：dup 两次调 b 只产出一条链；b→d→a 继续沿
    // 未在路径上的 c 展开，(a,b) 回边被截断。
    auto dup = g.enumerateChains(keyOf(g, "Chain", "dup", "()V"));
    ASSERT_TRUE(dup.next(chain));
    EXPECT_EQ(chainNames(g, chain), (NameChain{"dup", "b", "d", "a", "c"}));
    EXPECT_FALSE(dup.next(chain));
}

TEST(MjAsmClasspathTest, CallTreeKeepsReentryAndMarksCycleLeaves)
{
    CallGraph g = buildGraphOf({chainClassBytes()});
    const mjbase::MemberKey a = keyOf(g, "Chain", "a", "()V");

    const CallTree tree = g.buildCallTree(a);
    ASSERT_EQ(tree.nodes.size(), 7u);
    EXPECT_FALSE(tree.truncated);

    // 先序下标：a, b, d, a(cycle), c, d, a(cycle)。
    EXPECT_EQ(tree.nodes[0].method, a);
    EXPECT_EQ(tree.nodes[0].parent, kNoCallTreeParent);
    EXPECT_EQ(tree.nodes[0].depth, 0u);
    EXPECT_EQ(tree.nodes[0].methodId, 0u);
    EXPECT_EQ(tree.nodes[0].children, (std::vector<ca::u32>{1, 4}));

    EXPECT_EQ(g.pool().str(tree.nodes[1].method.name), "b");
    EXPECT_EQ(tree.nodes[1].parent, 0u);
    EXPECT_EQ(g.pool().str(tree.nodes[2].method.name), "d");
    EXPECT_EQ(tree.nodes[2].depth, 2u);

    // 回边：保留为 cycle 叶节点，methodId 与首次出现的 a 相同。
    EXPECT_EQ(tree.nodes[3].method, a);
    EXPECT_EQ(tree.nodes[3].methodId, tree.nodes[0].methodId);
    EXPECT_EQ(tree.nodes[3].depth, 3u);
    EXPECT_TRUE(tree.nodes[3].cycle);
    EXPECT_TRUE(tree.nodes[3].children.empty());

    // 重入方法在另一分支是独立节点，但共享 methodId。
    EXPECT_EQ(g.pool().str(tree.nodes[5].method.name), "d");
    EXPECT_EQ(tree.nodes[5].methodId, tree.nodes[2].methodId);
    EXPECT_FALSE(tree.nodes[5].cycle);
}

TEST(MjAsmClasspathTest, CallTreeRespectsDepthAndNodeCaps)
{
    CallGraph g = buildGraphOf({chainClassBytes()});
    const mjbase::MemberKey a = keyOf(g, "Chain", "a", "()V");

    CallTreeOptions options;
    options.maxDepth = 2;
    CallTree depthCapped = g.buildCallTree(a, options);
    ASSERT_EQ(depthCapped.nodes.size(), 5u);
    EXPECT_FALSE(depthCapped.truncated);
    ASSERT_EQ(depthCapped.nodes[0].children.size(), 2u);
    // 深度 2 的 d 按叶处理：没有 cycle 子节点。
    EXPECT_TRUE(depthCapped.nodes[2].children.empty());
    EXPECT_TRUE(depthCapped.nodes[4].children.empty());

    options = CallTreeOptions{};
    options.maxNodes = 3;
    CallTree nodeCapped = g.buildCallTree(a, options);
    ASSERT_EQ(nodeCapped.nodes.size(), 3u);
    EXPECT_TRUE(nodeCapped.truncated);

    // 上限恰好等于完整树规模：不截断。
    options.maxNodes = 7;
    CallTree exact = g.buildCallTree(a, options);
    EXPECT_EQ(exact.nodes.size(), 7u);
    EXPECT_FALSE(exact.truncated);

    // 根不在图中：空树；叶根：单节点。
    EXPECT_TRUE(g.buildCallTree(missingKey(g)).nodes.empty());
    const CallTree leaf = g.buildCallTree(keyOf(g, "Chain", "leaf", "()V"));
    ASSERT_EQ(leaf.nodes.size(), 1u);
    EXPECT_EQ(leaf.nodes[0].depth, 0u);
    EXPECT_TRUE(leaf.nodes[0].children.empty());
    EXPECT_FALSE(leaf.truncated);
}

// ---------------------------------------------------------------------------
// 错误路径：读段 / 升格 / 操作数
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, BadBytesFailAtReadStage)
{
    CallGraphBuilder b;
    const Bytes truncated = {0xCA, 0xFE, 0xBA};
    const auto add =
        b.addClassBytes(jbc::core::ByteView{truncated.data(), truncated.size()});
    ASSERT_TRUE(add.is_err());
    EXPECT_EQ(add.unwrap_err().stage, jbc::ir::SessionError::Stage::Read);
}

TEST(MjAsmClasspathTest, PromoteFailureFailsBuild)
{
    // goto 重锚例外①：合法基座 goto 直落下一指令，retargetFirstGoto 单点把
    // 目标改指 pc1（落在自身操作数中间）——装配器只发合法跳转，此形无法从
    // 源码直达；改写后指令导入必败（BadTarget → BadInstructions）。
    const Bytes base = mjbase::test::assembleMethodClass(
        "m", "()V", "      goto L0\n      L0:\n      return\n", "Bad");
    ASSERT_FALSE(base.empty());
    expectValidateOk(base);
    const Bytes broken = mjbase::test::retargetFirstGoto(base, 1);
    ASSERT_FALSE(broken.empty());

    CallGraphBuilder b;
    ASSERT_TRUE(
        b.addClassBytes(jbc::core::ByteView{broken.data(), broken.size()}).is_ok());
    auto r = b.build();
    ASSERT_TRUE(r.is_err());
    const mjbase::asm_classpath::BuildError e = r.unwrap_err();
    EXPECT_EQ(e.stage, mjbase::asm_classpath::BuildError::Stage::Promote);
    EXPECT_EQ(e.error, jbc::ir::PromoteError::BadInstructions);
    EXPECT_EQ(e.className, "Bad");
    EXPECT_EQ(e.methodName, "m");
    EXPECT_EQ(e.methodDesc, "()V");
}

TEST(MjAsmClasspathTest, InvokeOperandMismatchFailsBuild)
{
    // invoke 操作数改锚例外①：invokestatic 的 CP 操作数单点改指 Utf8 条目
    //（本方法名槽位）——装配器只发 tag 相符的 Methodref，此形无法从源码
    // 直达；改写后 memberOf 失败由 build() 实跑证明（Operand 段）。
    const Bytes base = makeClass("Bad2", {{"m", "()V", {
        {Op::INVOKESTATIC, "X", "whatever", "()V"},
    }}});
    ASSERT_FALSE(base.empty());
    expectValidateOk(base);
    const Bytes bad = pointInvokeOperandAtUtf8(base);
    ASSERT_FALSE(bad.empty());

    CallGraphBuilder b;
    ASSERT_TRUE(
        b.addClassBytes(jbc::core::ByteView{bad.data(), bad.size()}).is_ok());
    auto r = b.build();
    ASSERT_TRUE(r.is_err());
    const mjbase::asm_classpath::BuildError e = r.unwrap_err();
    EXPECT_EQ(e.stage, mjbase::asm_classpath::BuildError::Stage::Operand);
    EXPECT_EQ(e.className, "Bad2");
    EXPECT_EQ(e.methodName, "m");
    EXPECT_EQ(e.methodDesc, "()V");
    EXPECT_EQ(e.insnSlot, 0u);   // 方法内第一条指令
}
