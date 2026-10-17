/// @file        reference_index_changeset_test.cpp
/// @brief       E2 ChangeSet 局部重建等价性测试（libjbc design.md §35.4 硬验收）。
///
/// 清单行点名的缺失证明：同一批变换后，「按 ChangeSet 局部重建的
/// ReferenceIndex」与「变换后容器全量重扫的 ReferenceIndex」六张主表逐项
/// 一致。三层钉死：
///   - 成员级混合批次（体改写/改名/增删成员，方法粒度精确补丁 + 字段粒度
///     类级事实面重扫）等价；
///   - 类级变更（ClassMetadata）整类重扫等价；
///   - 真实 pass 流水（GlobalToClassPassAdaptor + NopRemover，经 changeSink
///     逐类折叠）等价。
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"
#include "libca/core/tag_cast.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

using jbc::core::Op;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::FieldSpec;
using mjbase::reference::test_support::MethodSpec;

namespace {

constexpr ca::u16 kAccPublic = 0x0001;
constexpr ca::u16 kAccStatic = 0x0008;

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

void addClassBytes(ResourceContainer& container, std::string_view name, const Bytes& bytes)
{
    container.addResource(std::string(name) + ".class",
                          std::make_unique<ClassResource>(std::string(name) + ".class", bytes));
}

/// 被变换类 W：引用出入口齐全（方法调用/字段访问/类型指令），供成员级批次
/// 与类级批次共用。
Bytes buildChangerClass()
{
    ClassBytesBuilder b("W");
    b.addField(FieldSpec{kAccPublic | kAccStatic, "alpha", "I"});
    b.addField(FieldSpec{kAccPublic | kAccStatic, "ref", "LB;"});   // 对象描述符字段
    std::vector<MethodSpec> ms;
    ms.push_back(m("edited", "()V", 1, 0, [](CodeEmit& e) {
        e.op(Op::NOP);
        e.invoke(Op::INVOKESTATIC, "B", "callee", "()V");
        e.op(Op::NOP);
        e.op(Op::RETURN);
    }));
    ms.push_back(m("renamed", "()I", 1, 0, [](CodeEmit& e) {
        e.op(Op::ICONST_1);
        e.op(Op::IRETURN);
    }));
    ms.push_back(m("victim", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(m("touch", "()V", 1, 0, [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "B");
        e.op(Op::POP);
        e.fieldInsn(Op::GETSTATIC, "W", "ref", "LB;");
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 引用方 R：调用 W 的成员（含将被删除/改名的目标），其事实在 W 变换后应
/// 保持原样（source 面不因目标变化而变）。
Bytes buildReferencingClass()
{
    ClassBytesBuilder b("R");
    std::vector<MethodSpec> ms;
    ms.push_back(m("hop", "()V", 1, 0, [](CodeEmit& e) {
        e.op(Op::NOP);
        e.invoke(Op::INVOKESTATIC, "W", "victim", "()V");
        e.fieldInsn(Op::GETSTATIC, "W", "alpha", "I");
        e.op(Op::POP);
        e.invoke(Op::INVOKESTATIC, "W", "renamed", "()I");
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

/// 管线夹具：两个带 NOP 的类（NopRemover 产生 MethodBody 粒度变更）。
Bytes buildNopMethodClass(std::string_view name)
{
    ClassBytesBuilder b(name);
    std::vector<MethodSpec> ms;
    ms.push_back(m("run", "()V", 1, 0, [name](CodeEmit& e) {
        e.op(Op::NOP);
        e.op(Op::NOP);
        e.invoke(Op::INVOKESTATIC, name, "self", "()V");
        e.op(Op::RETURN);
    }));
    ms.push_back(m("self", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

// ── 六张主表的规范化逐项比较（多重集语义：排序后逐元素相等）──

std::string joinFields(const std::vector<std::string>& parts)
{
    std::string out;
    for (const std::string& p : parts) {
        out.push_back('\x1f');
        out += p;
    }
    return out;
}

std::vector<std::string> canonOf(const ReferenceIndex& idx)
{
    std::vector<std::string> rows;

    for (const auto& d : idx.classDeclarations()) {
        rows.push_back("C" + joinFields({std::string(d.name), std::string(d.superName),
                                         std::to_string(d.access)}));
    }
    for (const auto& d : idx.methodDeclarations()) {
        rows.push_back("M" + joinFields({std::string(d.method.owner), std::string(d.method.name),
                                         std::string(d.method.desc),
                                         std::to_string(d.access)}));
    }
    for (const auto& d : idx.fieldDeclarations()) {
        rows.push_back("F" + joinFields({std::string(d.field.owner), std::string(d.field.name),
                                         std::string(d.field.desc),
                                         std::to_string(d.access)}));
    }
    for (const auto& r : idx.methodReferences()) {
        rows.push_back("m" + joinFields({std::string(r.site.sourceClass),
                                         std::string(r.site.sourceMethod),
                                         std::string(r.site.sourceDesc),
                                         std::to_string(r.site.opcode),
                                         std::to_string(r.site.insnIndex),
                                         std::string(r.target.owner),
                                         std::string(r.target.name), std::string(r.target.desc),
                                         std::string(r.reflectionArgDesc),
                                         r.isInterface ? "1" : "0",
                                         std::to_string(static_cast<int>(r.kind))}));
    }
    for (const auto& r : idx.fieldReferences()) {
        rows.push_back("f" + joinFields({std::string(r.site.sourceClass),
                                         std::string(r.site.sourceMethod),
                                         std::string(r.site.sourceDesc),
                                         std::to_string(r.site.opcode),
                                         std::to_string(r.site.insnIndex),
                                         std::string(r.target.owner),
                                         std::string(r.target.name), std::string(r.target.desc),
                                         std::to_string(static_cast<int>(r.kind))}));
    }
    for (const auto& r : idx.typeReferences()) {
        rows.push_back("t" + joinFields({std::string(r.site.sourceClass),
                                         std::string(r.site.sourceMethod),
                                         std::string(r.site.sourceDesc),
                                         std::to_string(r.site.opcode),
                                         std::to_string(r.site.insnIndex),
                                         std::string(r.targetType),
                                         std::to_string(static_cast<int>(r.kind))}));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

void expectIndexesEquivalent(const ReferenceIndex& oracle, const ReferenceIndex& patched,
                             const char* what)
{
    const auto a = canonOf(oracle);
    const auto b = canonOf(patched);
    EXPECT_EQ(a, b) << what;
    EXPECT_EQ(oracle.classDeclarationCount(), patched.classDeclarationCount());
    EXPECT_EQ(oracle.methodDeclarationCount(), patched.methodDeclarationCount());
    EXPECT_EQ(oracle.fieldDeclarationCount(), patched.fieldDeclarationCount());
    EXPECT_EQ(oracle.methodReferenceCount(), patched.methodReferenceCount());
    EXPECT_EQ(oracle.fieldReferenceCount(), patched.fieldReferenceCount());
    EXPECT_EQ(oracle.typeReferenceCount(), patched.typeReferenceCount());
}

std::shared_ptr<jbc::ir::ClassSession> sessionOf(const Bytes& bytes)
{
    auto r = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{bytes.data(), bytes.size()});
    return r.is_ok() ? std::move(r).unwrap() : nullptr;
}

}   // namespace

// ===========================================================================
// 成员级混合批次：方法粒度精确补丁 + 字段粒度类级事实面重扫
// ===========================================================================

TEST(ReferenceIndexChangeSetTest, memberBatchPatchMatchesFullRebuild)
{
    const Bytes wBytes = buildChangerClass();
    const Bytes rBytes = buildReferencingClass();

    // 变换：体改写（插 NOP，引用位移）/ 改名 / 删方法 / 删字段（对象描述符）/
    // 增方法（带体）/ 增字段——六种成员级粒度一批。
    auto w = sessionOf(wBytes);
    auto r = sessionOf(rBytes);
    ASSERT_NE(w, nullptr);
    ASSERT_NE(r, nullptr);

    {
        // 体改写：edited() 开头插 NOP → callee 调用点 insnIndex 后移。
        const jbc::ir::MethodId mid = w->unit().findMethodId("edited", "()V");
        ASSERT_FALSE(mid == jbc::ir::kNoMethodId);
        auto p = jbc::ir::promoteMethod(*w, mid);
        ASSERT_TRUE(p.is_ok());
        jbc::ir::CodeEditor ed(*p.unwrap().get());
        ASSERT_TRUE(ed.insertAtStart(jbc::ir::CodeEditor::simple(Op::NOP)).is_ok());
        ASSERT_TRUE(jbc::ir::lowerMethod(*w, mid).is_ok());
    }
    {
        // 改名：renamed → renamed2（commit 落盘，MemberMetadata 带 prior）。
        const jbc::ir::MethodId mid = w->unit().findMethodId("renamed", "()I");
        ASSERT_FALSE(mid == jbc::ir::kNoMethodId);
        jbc::ir::MethodUnit* mu = w->unit().method(mid);
        ASSERT_NE(mu, nullptr);
        mu->name = w->unit().symbols().intern("renamed2");
        ASSERT_TRUE(w->commitMethodMetadata(mid).is_ok());
    }
    {
        const jbc::ir::MethodId mid = w->unit().findMethodId("victim", "()V");
        ASSERT_FALSE(mid == jbc::ir::kNoMethodId);
        ASSERT_TRUE(w->removeMethod(mid).is_ok());
    }
    {
        const jbc::ir::FieldId fid = w->unit().findFieldId("ref");
        ASSERT_FALSE(fid == jbc::ir::kNoFieldId);
        ASSERT_TRUE(w->removeField(fid).is_ok());
    }
    {
        jbc::ir::MethodSeed seed;
        seed.name     = "fresh";
        seed.desc     = "()V";
        seed.access   = kAccPublic | kAccStatic;
        seed.withCode = true;
        auto added    = w->addMethod(seed);
        ASSERT_TRUE(added.is_ok());
        auto p = jbc::ir::promoteMethod(*w, added.unwrap());
        ASSERT_TRUE(p.is_ok());
        jbc::ir::CodeEditor ed(*p.unwrap().get());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::simple(Op::RETURN)).is_ok());
        ASSERT_TRUE(jbc::ir::lowerMethod(*w, added.unwrap()).is_ok());
    }
    {
        jbc::ir::FieldSeed seed;
        seed.name   = "freshField";
        seed.desc   = "Ljava/lang/String;";
        seed.access = kAccPublic | kAccStatic;
        ASSERT_TRUE(w->addField(seed).is_ok());
    }
    {
        // 引用方小改：hop() 开头删首 NOP（布局变化 → MethodBody）。
        const jbc::ir::MethodId mid = r->unit().findMethodId("hop", "()V");
        ASSERT_FALSE(mid == jbc::ir::kNoMethodId);
        auto p = jbc::ir::promoteMethod(*r, mid);
        ASSERT_TRUE(p.is_ok());
        jbc::ir::CodeDom& dom = p.unwrap()->dom;
        ASSERT_FALSE(dom.order().empty());
        const jbc::ir::Insn* first = dom.peek(dom.order().front());
        ASSERT_NE(first, nullptr);
        ASSERT_EQ(first->op, Op::NOP);
        jbc::ir::CodeEditor ed(*p.unwrap().get());
        ASSERT_TRUE(ed.erase(dom.order().front()).is_ok());
        ASSERT_TRUE(jbc::ir::lowerMethod(*r, mid).is_ok());
    }

    // 变换后容器（oracle 输入）。
    ResourceContainer transformed;
    auto wb = jbc::core::writeClass(w->backing());
    auto rb = jbc::core::writeClass(r->backing());
    ASSERT_TRUE(wb.is_ok());
    ASSERT_TRUE(rb.is_ok());
    addClassBytes(transformed, "W", wb.unwrap());
    addClassBytes(transformed, "R", rb.unwrap());

    ReferenceIndex oracle;
    oracle.build(transformed);

    // 局部重建：原容器全量 + 逐类 ChangeSet 折叠（会话存活期内）。
    ReferenceIndex patched;
    {
        ResourceContainer original;
        addClassBytes(original, "W", wBytes);
        addClassBytes(original, "R", rBytes);
        patched.build(original);
    }
    patched.applyChangeSet("W", *w, w->changeSet());
    patched.applyChangeSet("R", *r, r->changeSet());

    expectIndexesEquivalent(oracle, patched, "member batch: patched != full rebuild");
}

// ===========================================================================
// 类级变更：整类重扫路径
// ===========================================================================

TEST(ReferenceIndexChangeSetTest, classMetadataPatchMatchesFullRebuild)
{
    const Bytes wBytes = buildChangerClass();

    auto w = sessionOf(wBytes);
    ASSERT_NE(w, nullptr);
    w->unit().access = 0x0031;   // public super → public super + interface 位不动语义，仅验证实改
    ASSERT_TRUE(w->commitClassMetadata().is_ok());
    EXPECT_EQ(w->changeSet().classChange(), jbc::ir::ClassChange::Metadata);

    ResourceContainer transformed;
    auto wb = jbc::core::writeClass(w->backing());
    ASSERT_TRUE(wb.is_ok());
    addClassBytes(transformed, "W", wb.unwrap());
    ReferenceIndex oracle;
    oracle.build(transformed);

    ReferenceIndex patched;
    {
        ResourceContainer original;
        addClassBytes(original, "W", wBytes);
        patched.build(original);
    }
    patched.applyChangeSet("W", *w, w->changeSet());

    expectIndexesEquivalent(oracle, patched, "class metadata: patched != full rebuild");
}

// ===========================================================================
// 真实 pass 流水：GlobalToClassPassAdaptor + NopRemover 经 changeSink 折叠
// ===========================================================================

TEST(ReferenceIndexChangeSetTest, pipelineSinkFoldMatchesFullRebuild)
{
    const Bytes aBytes = buildNopMethodClass("P");
    const Bytes bBytes = buildNopMethodClass("Q");

    // 原容器：索引的初始态。
    ResourceContainer original;
    addClassBytes(original, "P", aBytes);
    addClassBytes(original, "Q", bBytes);

    // 变换容器：同一份字节，跑真实 pass 流水（写回提交进容器）。
    ResourceContainer transformed;
    ClassResource* pRes = nullptr;
    {
        auto pResPtr = std::make_unique<ClassResource>("P.class", aBytes);
        auto qResPtr = std::make_unique<ClassResource>("Q.class", bBytes);
        pRes         = pResPtr.get();
        transformed.addResource("P.class", std::move(pResPtr));
        transformed.addResource("Q.class", std::move(qResPtr));
    }

    ReferenceIndex patched;
    patched.build(original);

    mjbase::KeyPool identityPool;
    ca::str::Utf8StringArena arena;
    mjbase::spi::MethodPassManager mpm;
    mpm.addPass(std::make_unique<mjbase::pass::NopRemover>());
    mjbase::spi::ClassPassManager cpm;
    cpm.addPass(std::make_unique<mjbase::spi::ClassToMethodPassAdaptor>(std::move(mpm),
                                                                        identityPool));

    mjbase::spi::PassPipelineOptions options;
    std::vector<std::string> sinkLog;
    options.changeSink = [&](std::string_view className, jbc::ir::ClassSession& session,
                             const jbc::ir::ChangeSet& changes) {
        sinkLog.emplace_back(className);
        EXPECT_FALSE(changes.empty());
        // sink 契约：会话存活期内折叠；索引以变换前类名建键（未改名即同名）。
        patched.applyChangeSet(className, session, changes);
    };

    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::spi::GlobalToClassPassAdaptor adaptor(std::move(cpm), nullptr, options);
    adaptor.run(transformed, gam);

    ASSERT_EQ(sinkLog.size(), ca::usize{2});   // 两个类都被 NopRemover 实改

    ResourceContainer oracleInput;
    addClassBytes(oracleInput, "P", pRes->get());
    {
        auto* qRaw = transformed.getResources().find("Q.class")->second.get();
        ASSERT_TRUE(ca::core::tag_cast::isa<ClassResource>(qRaw));
        addClassBytes(oracleInput, "Q",
                      ca::core::tag_cast::cast<ClassResource>(qRaw)->get());
    }
    ReferenceIndex oracle;
    oracle.build(oracleInput);

    expectIndexesEquivalent(oracle, patched, "pipeline fold: patched != full rebuild");
}
