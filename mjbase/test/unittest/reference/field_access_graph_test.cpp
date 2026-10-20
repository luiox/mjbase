/// @file        field_access_graph_test.cpp
/// @brief       全程序字段访问图测试：直接读写事实、调用边传递、只写/未触碰
///              字段、虚分派与外部目标的 unknown 标记。
///
/// 夹具用 reference 测试的手拼 class 汇编器；索引与调用图喂入同一批类字节，
/// 保证两侧方法身份一致。
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/field_access_graph.hpp"
#include "mjbase/reference/reference_index.hpp"

namespace {

using jbc::core::Op;
using mjbase::MemberKey;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::FieldAccessGraph;
using mjbase::reference::FieldAccessSummary;
using mjbase::reference::MethodFieldSummary;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::FieldSpec;
using mjbase::reference::test_support::MethodSpec;

constexpr ca::u16 kPublic = 0x0001;
constexpr ca::u16 kPrivate = 0x0002;
constexpr ca::u16 kStatic = 0x0008;

MethodSpec m(std::string_view name, std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals,
             std::function<void(CodeEmit&)> emit, ca::u16 access = kPublic | kStatic)
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

/// 同一批类字节同时登记进容器（索引扫描）与调用图 builder。
struct ScanSet
{
    ResourceContainer  container;
    std::vector<Bytes> bytes;

    void add(std::string_view name, Bytes b)
    {
        bytes.push_back(std::move(b));
        container.addResource(std::string(name) + ".class",
                              std::make_unique<ClassResource>(std::string(name) + ".class",
                                                              bytes.back()));
    }
};

bool isMember(const FieldAccessGraph& graph, MemberKey key, std::string_view owner,
              std::string_view name, std::string_view desc)
{
    return graph.pool().str(key.cls) == owner && graph.pool().str(key.name) == name &&
           graph.pool().str(key.desc) == desc;
}

bool hasMember(const FieldAccessGraph& graph, const std::vector<MemberKey>& keys,
               std::string_view owner, std::string_view name, std::string_view desc)
{
    for (const MemberKey key : keys) {
        if (isMember(graph, key, owner, name, desc)) {
            return true;
        }
    }
    return false;
}

ScanSet buildScanSet()
{
    ScanSet scan;

    ClassBytesBuilder data("Data");
    data.addField(FieldSpec{static_cast<ca::u16>(kPublic | kStatic), "counter", "I"});
    data.addField(FieldSpec{kPrivate, "value", "I"});
    data.addField(FieldSpec{kPrivate, "writtenOnly", "I"});
    data.addField(FieldSpec{kPrivate, "never", "I"});

    std::vector<MethodSpec> methods;
    methods.push_back(m("readStatic", "()I", 1, 0, [](CodeEmit& e) {
        e.fieldInsn(Op::GETSTATIC, "Data", "counter", "I");
        e.op(Op::IRETURN);
    }));
    methods.push_back(m("writeStatic", "()V", 1, 0, [](CodeEmit& e) {
        e.i32const(1);
        e.fieldInsn(Op::PUTSTATIC, "Data", "counter", "I");
        e.op(Op::RETURN);
    }));
    methods.push_back(m("readValue", "(LData;)I", 1, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.fieldInsn(Op::GETFIELD, "Data", "value", "I");
        e.op(Op::IRETURN);
    }));
    methods.push_back(m("writeValue", "(LData;)V", 1, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.i32const(2);
        e.fieldInsn(Op::PUTFIELD, "Data", "value", "I");
        e.op(Op::RETURN);
    }));
    methods.push_back(m("writeOnly", "(LData;)V", 1, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.i32const(3);
        e.fieldInsn(Op::PUTFIELD, "Data", "writtenOnly", "I");
        e.op(Op::RETURN);
    }));
    methods.push_back(m("readViaCall", "()I", 1, 0, [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "Data", "readStatic", "()I");
        e.op(Op::IRETURN);
    }));
    methods.push_back(m("virt", "()V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); }, kPublic));
    methods.push_back(m("virtCall", "(LData;)V", 1, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.invoke(Op::INVOKEVIRTUAL, "Data", "virt", "()V");
        e.op(Op::RETURN);
    }));
    methods.push_back(m("extCall", "()V", 2, 0, [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "java/lang/System", "currentTimeMillis", "()J");
        e.op(Op::POP2);
        e.op(Op::RETURN);
    }));
    methods.push_back(m("noAccess", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    scan.add("Data", data.assemble(methods));

    ClassBytesBuilder user("User");
    scan.add("User", user.assemble({m("main", "()V", 1, 0, [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "Data", "readViaCall", "()I");
        e.op(Op::POP);
        e.invoke(Op::INVOKESTATIC, "Data", "writeStatic", "()V");
        e.op(Op::RETURN);
    })}));

    return scan;
}

CallGraph buildGraph(const std::vector<Bytes>& classFiles)
{
    CallGraphBuilder builder;
    for (const Bytes& bytes : classFiles) {
        EXPECT_TRUE(builder.addClassBytes(jbc::core::ByteView{bytes.data(), bytes.size()})
                        .is_ok());
    }
    auto built = builder.build();
    EXPECT_TRUE(built.is_ok());
    return std::move(built).unwrap();
}

struct GraphFixture
{
    ScanSet           scan;
    ReferenceIndex    index;
    CallGraph         callGraph;
    FieldAccessGraph  graph;

    GraphFixture()
        : scan(buildScanSet())
        , callGraph(buildGraph(scan.bytes))
    {
        index.build(scan.container);
        graph.build(index, callGraph);
    }
};

}   // namespace

TEST(MjReferenceFieldAccessTest, DirectFactsAndInvertedIndex)
{
    GraphFixture fx;

    const FieldAccessSummary* counter = fx.graph.fieldSummary("Data", "counter", "I");
    ASSERT_NE(counter, nullptr);
    EXPECT_TRUE(counter->declared);
    EXPECT_TRUE(counter->isRead());
    EXPECT_TRUE(counter->isWritten());
    EXPECT_TRUE(hasMember(fx.graph, counter->readers, "Data", "readStatic", "()I"));
    EXPECT_TRUE(hasMember(fx.graph, counter->writers, "Data", "writeStatic", "()V"));

    const FieldAccessSummary* value = fx.graph.fieldSummary("Data", "value", "I");
    ASSERT_NE(value, nullptr);
    EXPECT_TRUE(hasMember(fx.graph, fx.graph.readersOf("Data", "value", "I"), "Data",
                          "readValue", "(LData;)I"));
    EXPECT_TRUE(hasMember(fx.graph, fx.graph.writersOf("Data", "value", "I"), "Data",
                          "writeValue", "(LData;)V"));
    EXPECT_FALSE(value->isOnlyWritten());
}

TEST(MjReferenceFieldAccessTest, TransitiveSummariesFollowCallEdges)
{
    GraphFixture fx;

    const MethodFieldSummary* viaCall = fx.graph.methodSummary("Data", "readViaCall", "()I");
    ASSERT_NE(viaCall, nullptr);
    ASSERT_EQ(viaCall->fieldsRead.size(), 1u);
    EXPECT_TRUE(isMember(fx.graph, viaCall->fieldsRead[0], "Data", "counter", "I"));
    EXPECT_TRUE(viaCall->fieldsWritten.empty());
    EXPECT_FALSE(viaCall->hasUnknownCallees);

    const MethodFieldSummary* main = fx.graph.methodSummary("User", "main", "()V");
    ASSERT_NE(main, nullptr);
    EXPECT_TRUE(hasMember(fx.graph, main->fieldsRead, "Data", "counter", "I"));
    EXPECT_TRUE(hasMember(fx.graph, main->fieldsWritten, "Data", "counter", "I"));
    EXPECT_FALSE(main->hasUnknownCallees);

    // 倒排索引也含传递读写方。
    EXPECT_TRUE(hasMember(fx.graph, fx.graph.readersOf("Data", "counter", "I"), "User", "main",
                          "()V"));
    EXPECT_TRUE(hasMember(fx.graph, fx.graph.writersOf("Data", "counter", "I"), "User",
                          "main", "()V"));
}

TEST(MjReferenceFieldAccessTest, UnreadAndUntouchedDeclaredFields)
{
    GraphFixture fx;

    const FieldAccessSummary* writtenOnly =
        fx.graph.fieldSummary("Data", "writtenOnly", "I");
    ASSERT_NE(writtenOnly, nullptr);
    EXPECT_TRUE(writtenOnly->isWritten());
    EXPECT_FALSE(writtenOnly->isRead());
    EXPECT_TRUE(writtenOnly->isOnlyWritten());

    const std::vector<MemberKey> unread = fx.graph.unreadDeclaredFields();
    EXPECT_TRUE(hasMember(fx.graph, unread, "Data", "writtenOnly", "I"));
    EXPECT_TRUE(hasMember(fx.graph, unread, "Data", "never", "I"));
    EXPECT_FALSE(hasMember(fx.graph, unread, "Data", "counter", "I"));
    EXPECT_FALSE(hasMember(fx.graph, unread, "Data", "value", "I"));

    const std::vector<MemberKey> untouched = fx.graph.untouchedDeclaredFields();
    ASSERT_EQ(untouched.size(), 1u);
    EXPECT_TRUE(isMember(fx.graph, untouched[0], "Data", "never", "I"));
}

TEST(MjReferenceFieldAccessTest, VirtualDispatchAndExternalCallsMarkUnknown)
{
    GraphFixture fx;

    const MethodFieldSummary* virtCall = fx.graph.methodSummary("Data", "virtCall", "(LData;)V");
    ASSERT_NE(virtCall, nullptr);
    EXPECT_TRUE(virtCall->hasUnknownCallees);

    const MethodFieldSummary* extCall = fx.graph.methodSummary("Data", "extCall", "()V");
    ASSERT_NE(extCall, nullptr);
    EXPECT_TRUE(extCall->hasUnknownCallees);

    const MethodFieldSummary* virt = fx.graph.methodSummary("Data", "virt", "()V");
    ASSERT_NE(virt, nullptr);
    EXPECT_FALSE(virt->hasUnknownCallees);

    const MethodFieldSummary* noAccess = fx.graph.methodSummary("Data", "noAccess", "()V");
    ASSERT_NE(noAccess, nullptr);
    EXPECT_TRUE(noAccess->fieldsRead.empty());
    EXPECT_TRUE(noAccess->fieldsWritten.empty());
    EXPECT_FALSE(noAccess->hasUnknownCallees);

    EXPECT_TRUE(fx.graph.hasUnknownEffects());
    EXPECT_FALSE(fx.graph.hasUnresolvedFieldRefs());
}

TEST(MjReferenceFieldAccessTest, ClearDropsAllResults)
{
    GraphFixture fx;
    fx.graph.clear();
    EXPECT_TRUE(fx.graph.methodSummaries().empty());
    EXPECT_TRUE(fx.graph.fieldSummaries().empty());
    EXPECT_FALSE(fx.graph.hasUnknownEffects());
    EXPECT_EQ(fx.graph.fieldSummary("Data", "counter", "I"), nullptr);
}
