/// @file        call_graph_test.cpp
/// @brief       asm/classpath 调用图测试：调用点提取、正反向索引、调用点计数、
///              虚调用未解析标记、可达闭包与库类过滤、外部目标丢弃、会话喂入、
///              读段/升格/操作数三段错误路径。
///
/// 类字节全部手工拼装（utf8/class/name_and_type/methodref 条目写法参考
/// libs/libjbc/demo/control_flow.cpp）：每个字节受控，不依赖本地语料。
#include <gtest/gtest.h>
#include <cstdio>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/bytes.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"

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

void putU8(Bytes& b, ca::u8 v) { b.push_back(v); }

void putU16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

void putU32(Bytes& b, ca::u32 v)
{
    b.push_back(static_cast<ca::u8>(v >> 24));
    b.push_back(static_cast<ca::u8>(v >> 16));
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

void putUtf8Raw(Bytes& b, std::string_view s)
{
    for (const char c : s) {
        b.push_back(static_cast<ca::u8>(c));
    }
}

/// 单条 invoke 指令规格。broken = true 时操作数故意指向 Utf8 条目
/// （tag 与 invoke 不符），用于触发 BuildError::Stage::Operand。
struct CallSpec
{
    Op op = Op::INVOKESTATIC;
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
    bool broken = false;
};

/// 方法规格：abstract 时无 Code 属性（calls 忽略）。
struct MethodSpec
{
    std::string_view name;
    std::string_view desc;
    std::vector<CallSpec> calls;
    bool abstract = false;
};

/// 手工拼最小 classfile（V1_8 / public super / super=java/lang/Object /
/// 无接口无字段无类属性），方法各带一个 Code 属性，指令序列 = invokes + return。
///
/// 组装分两遍：先把全部 CP 条目物化（cpCount 才算得对），再写字节；
/// 第二遍里 utf8/cls/nat/ref 全部命中缓存，不再追加。
class ClassBytesBuilder
{
public:
    explicit ClassBytesBuilder(std::string_view thisClass)
    {
        thisIdx_ = cls(thisClass);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
    }

    /// 物化一个方法的 CP 条目（名字/描述符/调用引用）。
    void declare(const MethodSpec& m)
    {
        utf8(m.name);
        utf8(m.desc);
        for (const CallSpec& c : m.calls) {
            if (c.broken) {
                utf8(c.name);   // 故意只留 Utf8 条目
            } else if (c.op == Op::INVOKEDYNAMIC) {
                ref(0x12, "", c.name, c.desc);
            } else if (c.op == Op::INVOKEINTERFACE) {
                ref(0x0B, c.owner, c.name, c.desc);
            } else {
                ref(0x0A, c.owner, c.name, c.desc);
            }
        }
    }

    Bytes assemble(std::string_view thisClass, const std::vector<MethodSpec>& methods)
    {
        Bytes out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);   // V1_8
        putU16(out, next_);   // cpCount（slot 0 无效，无 long/double 双槽）
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, 0x0021);   // public super
        putU16(out, thisIdx_);
        putU16(out, superIdx_);
        putU16(out, 0);   // interfaces
        putU16(out, 0);   // fields
        putU16(out, static_cast<ca::u16>(methods.size()));

        for (const MethodSpec& m : methods) {
            putU16(out, m.abstract ? 0x0401 : 0x0009);   // public [static|abstract]
            putU16(out, utf8(m.name));
            putU16(out, utf8(m.desc));
            if (m.abstract) {
                putU16(out, 0);
                continue;
            }
            const Bytes code = codeOf(m);
            Bytes attr;
            putU16(attr, 4);   // maxStack
            putU16(attr, 1);   // maxLocals
            putU32(attr, static_cast<ca::u32>(code.size()));
            attr.insert(attr.end(), code.begin(), code.end());
            putU16(attr, 0);   // exc table
            putU16(attr, 0);   // nested attrs
            putU16(out, 1);    // method attr count
            putU16(out, codeName_);
            putU32(out, static_cast<ca::u32>(attr.size()));
            out.insert(out.end(), attr.begin(), attr.end());
        }
        putU16(out, 0);   // class attrs
        return out;
    }

private:
    ca::u16 utf8(std::string_view s)
    {
        std::string key(s);
        const auto it = utf8s_.find(key);
        if (it != utf8s_.end()) {
            return it->second;
        }
        putU8(cp_, 0x01);
        putU16(cp_, static_cast<ca::u16>(s.size()));
        putUtf8Raw(cp_, s);
        const ca::u16 idx = next_++;
        utf8s_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 cls(std::string_view n)
    {
        std::string key(n);
        const auto it = classes_.find(key);
        if (it != classes_.end()) {
            return it->second;
        }
        const ca::u16 u = utf8(n);
        putU8(cp_, 0x07);
        putU16(cp_, u);
        const ca::u16 idx = next_++;
        classes_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 nat(std::string_view name, std::string_view desc)
    {
        std::string key = std::string(name) + "." + std::string(desc);
        const auto it = nats_.find(key);
        if (it != nats_.end()) {
            return it->second;
        }
        const ca::u16 un = utf8(name);
        const ca::u16 ud = utf8(desc);
        putU8(cp_, 0x0C);
        putU16(cp_, un);
        putU16(cp_, ud);
        const ca::u16 idx = next_++;
        nats_.emplace(std::move(key), idx);
        return idx;
    }

    /// tag：0x0A Methodref / 0x0B InterfaceMethodref / 0x12 InvokeDynamic。
    ca::u16 ref(ca::u8 tag, std::string_view owner, std::string_view name,
                std::string_view desc)
    {
        std::string key = std::string(1, static_cast<char>(tag)) + "." +
                          std::string(owner) + "." + std::string(name) + "." +
                          std::string(desc);
        const auto it = refs_.find(key);
        if (it != refs_.end()) {
            return it->second;
        }
        // 先物化全部依赖条目再写字节：cls() 内部会向 cp_ 追加 utf8/Class，
        // 若在 putU8(tag) 之后作为实参求值会把本条目结构冲乱。
        const ca::u16 uc = tag == 0x12 ? 0 : cls(owner);
        const ca::u16 un = nat(name, desc);
        putU8(cp_, tag);
        if (tag == 0x12) {
            putU16(cp_, 0);   // bootstrap_method_attr_index：memberOf 不校验
            putU16(cp_, un);
        } else {
            putU16(cp_, uc);
            putU16(cp_, un);
        }
        const ca::u16 idx = next_++;
        refs_.emplace(std::move(key), idx);
        return idx;
    }

    Bytes codeOf(const MethodSpec& m)
    {
        Bytes code;
        for (const CallSpec& c : m.calls) {
            ca::u16 cpIdx = 0;
            if (c.broken) {
                cpIdx = utf8(c.name);   // 命中 declare 物化过的 Utf8 条目
            } else if (c.op == Op::INVOKEDYNAMIC) {
                cpIdx = ref(0x12, "", c.name, c.desc);
            } else if (c.op == Op::INVOKEINTERFACE) {
                cpIdx = ref(0x0B, c.owner, c.name, c.desc);
            } else {
                cpIdx = ref(0x0A, c.owner, c.name, c.desc);
            }
            putU8(code, static_cast<ca::u8>(c.op));
            putU16(code, cpIdx);
            if (c.op == Op::INVOKEINTERFACE) {
                putU8(code, 1);   // count（读段不校验）
                putU8(code, 0);
            } else if (c.op == Op::INVOKEDYNAMIC) {
                putU8(code, 0);
                putU8(code, 0);
            }
        }
        putU8(code, 0xB1);   // return
        return code;
    }

    Bytes cp_;
    ca::u16 next_ = 1;   // slot 0 无效
    std::map<std::string, ca::u16> utf8s_;
    std::map<std::string, ca::u16> classes_;
    std::map<std::string, ca::u16> nats_;
    std::map<std::string, ca::u16> refs_;
    ca::u16 thisIdx_ = 0;
    ca::u16 superIdx_ = 0;
    ca::u16 codeName_ = 0;
};

Bytes makeClass(std::string_view thisClass, const std::vector<MethodSpec>& methods)
{
    ClassBytesBuilder b(thisClass);
    for (const MethodSpec& m : methods) {
        b.declare(m);
    }
    return b.assemble(thisClass, methods);
}

/// 单方法 + 直接给定 code 字节（畸形指令序列用，不走 invoke 拼装）。
Bytes makeClassWithRawCode(std::string_view thisClass, std::string_view name,
                           std::string_view desc, const Bytes& code)
{
    Bytes cp;
    ca::u16 next = 1;
    const auto utf8 = [&](std::string_view s) {
        putU8(cp, 0x01);
        putU16(cp, static_cast<ca::u16>(s.size()));
        putUtf8Raw(cp, s);
        return next++;
    };
    const ca::u16 uThis = utf8(thisClass);
    putU8(cp, 0x07);
    putU16(cp, uThis);
    const ca::u16 cThis = next++;
    const ca::u16 uObj = utf8("java/lang/Object");
    putU8(cp, 0x07);
    putU16(cp, uObj);
    const ca::u16 cObj = next++;
    const ca::u16 uName = utf8(name);
    const ca::u16 uDesc = utf8(desc);
    const ca::u16 uCode = utf8("Code");

    Bytes out;
    putU32(out, 0xCAFEBABEu);
    putU16(out, 0x00);
    putU16(out, 0x34);
    putU16(out, next);
    out.insert(out.end(), cp.begin(), cp.end());
    putU16(out, 0x0021);
    putU16(out, cThis);
    putU16(out, cObj);
    putU16(out, 0);
    putU16(out, 0);
    putU16(out, 1);
    putU16(out, 0x0009);
    putU16(out, uName);
    putU16(out, uDesc);
    putU16(out, 1);
    Bytes attr;
    putU16(attr, 4);
    putU16(attr, 1);
    putU32(attr, static_cast<ca::u32>(code.size()));
    attr.insert(attr.end(), code.begin(), code.end());
    putU16(attr, 0);
    putU16(attr, 0);
    putU16(out, uCode);
    putU32(out, static_cast<ca::u32>(attr.size()));
    out.insert(out.end(), attr.begin(), attr.end());
    putU16(out, 0);
    return out;
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

    // 去重集合查询（calleesOf / callersOf）。
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
    const Bytes cls = makeClass("K", {{"m", "()V", {
        {Op::INVOKEVIRTUAL, "java/lang/Object", "toString", "()Ljava/lang/String;"},
        {Op::INVOKESPECIAL, "K", "<init>", "()V"},
        {Op::INVOKESTATIC, "K", "helper", "()V"},
        {Op::INVOKEINTERFACE, "java/util/List", "size", "()I"},
        {Op::INVOKEDYNAMIC, "", "apply", "()Ljava/lang/Object;"},
    }}});

    CallGraph g = buildGraphOf({cls});
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

    // 同一调用方多处调用分别计数，与去重调用方集合互补。
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
    // goto +1 落在自身操作数中间：Code 属性在、指令导入必败（BadTarget）。
    const Bytes broken = makeClassWithRawCode("Bad", "m", "()V", {0xA7, 0x00, 0x01, 0xB1});

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
    // invokestatic 的 CP 操作数指向 Utf8 条目（tag 不符）→ memberOf 失败。
    const Bytes bad = makeClass("Bad2", {{"m", "()V", {
        {Op::INVOKESTATIC, "X", "whatever", "()V", /*broken=*/true},
    }}});

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
