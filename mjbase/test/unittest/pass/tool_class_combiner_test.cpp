/// ToolClassCombiner（批 A 收口件）单元测试：准入/分桶/搬移/重映射语义面。
/// golden 对拍与 JVM 真跑门另在 pass_golden_compare_test 与
/// tool_class_combiner_jvm_test；本文件钉实现面契约：候选不足 no-op、
/// NotModeled 候选保守保留、宿主成员序确定性、ConstantValue 重建、
/// 供体引用零残留与消费面重指向。
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/tool_class_combiner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublic     = 0x0001;
constexpr ca::u16 kAccStatic     = 0x0008;

Bytes u2Operand(ca::u16 cp) { return {static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp)}; }

/// 候选甲：static 字段 + 自引用 GETSTATIC + 线性方法 + 默认构造器。
/// 方法体全部为验证器干净形（Strict 帧再生与 JVM 门的字节前提）。
Bytes buildUtAlpha()
{
    FixtureBuilder b("hb/ut/Alpha");
    b.addField({"FLAG", "I", static_cast<ca::u16>(kAccPublic | kAccStatic)});
    {
        FixtureMethod init;
        init.name      = "<init>";
        init.desc      = "()V";
        init.access    = kAccPublic;
        init.maxStack  = 1;
        init.maxLocals = 1;
        const Bytes superCtor = u2Operand(b.methodRef("java/lang/Object", "<init>", "()V"));
        init.code     = {0x2A, 0xB7, superCtor[0], superCtor[1], 0xB1};
        b.addMethod(init);
    }
    {
        FixtureMethod flag;
        flag.name     = "flag";
        flag.desc     = "()I";
        flag.access   = static_cast<ca::u16>(kAccPublic | kAccStatic);
        flag.maxStack = 1;
        flag.maxLocals = 0;
        const Bytes operand = u2Operand(b.fieldRef("hb/ut/Alpha", "FLAG", "I"));
        flag.code     = {0xB2, operand[0], operand[1], 0xAC};   // GETSTATIC; IRETURN
        b.addMethod(flag);
    }
    {
        FixtureMethod trim;
        trim.name      = "trim";
        trim.desc      = "(I)I";
        trim.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
        trim.maxStack  = 2;
        trim.maxLocals = 1;
        trim.code      = {0x1A, 0x04, 0x60, 0xAC};   // ILOAD_0; ICONST_1; IADD; IRETURN
        b.addMethod(trim);
    }
    return b.assemble();
}

/// 候选乙：分支体（IFEQ/GOTO）+ LDC Integer 方法（帧再生与宿主合并面）。
Bytes buildUtBeta()
{
    FixtureBuilder b("hb/ut/Beta");
    b.addField({"STATE", "I", static_cast<ca::u16>(kAccPublic | kAccStatic)});
    {
        FixtureMethod sign;
        sign.name      = "sign";
        sign.desc      = "(I)I";
        sign.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
        sign.maxStack  = 1;
        sign.maxLocals = 1;
        // 0:ILOAD_0; 1..3:IFEQ→8（pc 1 + 偏移 7）; 4:ICONST_M1;
        // 5..7:GOTO→9（pc 5 + 偏移 4）; 8:ICONST_1; 9:IRETURN——两条到 9
        // 的路径栈高一致（各 1 个 int），验证器干净形（Strict 帧再生的
        // 字节前提；分支偏移相对分支指令自身 pc）。
        sign.code = {0x1A, 0x99, 0x00, 0x07, 0x02, 0xA7, 0x00, 0x04, 0x04, 0xAC};
        b.addMethod(sign);
    }
    {
        FixtureMethod only;
        only.name     = "betaOnly";
        only.desc     = "()I";
        only.access   = static_cast<ca::u16>(kAccPublic | kAccStatic);
        only.maxStack = 1;
        only.maxLocals = 0;
        const Bytes operand = u2Operand(b.intConst(7));
        only.code     = {0x13, operand[0], operand[1], 0xAC};   // LDC_W 7; IRETURN
        b.addMethod(only);
    }
    return b.assemble();
}

/// 候选丙（NotModeled 载体）：LDC2_W 宽常量方法体——准入即落选，原资源保持。
Bytes buildUtWide()
{
    FixtureBuilder b("hb/ut/Wide");
    b.addField({"TOKEN", "J", static_cast<ca::u16>(kAccPublic | kAccStatic)});
    {
        FixtureMethod wideConst;
        wideConst.name     = "wideConst";
        wideConst.desc     = "()J";
        wideConst.access   = static_cast<ca::u16>(kAccPublic | kAccStatic);
        wideConst.maxStack = 2;
        wideConst.maxLocals = 0;
        const Bytes operand = u2Operand(b.longConst(5));
        wideConst.code     = {0x14, operand[0], operand[1], 0xAD};   // LDC2_W 5; LRETURN
        b.addMethod(wideConst);
    }
    return b.assemble();
}

/// 消费面（非候选：含实例方法）：引用甲乙的 static 成员。方法体验证器
/// 干净形：GETSTATIC(h1);POP(h0);ICONST_1(h1);INVOKESTATIC(h0→h1);
/// POP(h0);GETSTATIC(h1);POP(h0);RETURN。
Bytes buildUtApp()
{
    FixtureBuilder b("hb/ut/App");
    {
        FixtureMethod init;
        init.name      = "<init>";
        init.desc      = "()V";
        init.access    = kAccPublic;
        init.maxStack  = 1;
        init.maxLocals = 1;
        const Bytes superCtor = u2Operand(b.methodRef("java/lang/Object", "<init>", "()V"));
        init.code     = {0x2A, 0xB7, superCtor[0], superCtor[1], 0xB1};
        b.addMethod(init);
    }
    {
        FixtureMethod call;
        call.name      = "call";
        call.desc      = "()V";
        call.access    = static_cast<ca::u16>(kAccPublic | kAccStatic);
        call.maxStack  = 2;
        call.maxLocals = 0;
        call.code      = getStaticPop(b.fieldRef("hb/ut/Alpha", "FLAG", "I"));
        call.code.push_back(0x04);   // ICONST_1（trim 的实参）
        const Bytes trim = invokeStatic(b.methodRef("hb/ut/Alpha", "trim", "(I)I"));
        appendBytes(call.code, trim);
        call.code.push_back(0x57);   // POP（消费 trim 返回值）
        const Bytes state = getStaticPop(b.fieldRef("hb/ut/Beta", "STATE", "I"));
        appendBytes(call.code, state);
        call.code.push_back(0xB1);   // RETURN
        b.addMethod(call);
    }
    {
        FixtureMethod touch;
        touch.name      = "instanceTouch";
        touch.desc      = "()V";
        touch.access    = kAccPublic;
        touch.maxStack  = 0;
        touch.maxLocals = 1;
        touch.code      = {0xB1};
        b.addMethod(touch);
    }
    return b.assemble();
}

/// 给字段挂 ConstantValue(Integer) 属性（adopt 只搬元数据、本 pass 负责重建
/// 的输入面）：走 libjbc 公共 DOM 面，写回字节。
Bytes withConstantValue(Bytes classBytes, std::string_view fieldName, ca::i32 value)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(classBytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    const jbc::ir::FieldId fid = session->unit().findFieldId(fieldName);
    EXPECT_NE(fid, jbc::ir::kNoFieldId);
    const jbc::ir::FieldUnit* fu = session->unit().field(fid);
    EXPECT_NE(fu, nullptr);
    if (fu == nullptr) return {};
    jbc::core::ClassFile&  cf      = session->backing();
    jbc::core::MemberInfo& member  = cf.fields[fu->backingIndex];
    const ca::u32 nameCp = cf.cp.addUtf8("ConstantValue");
    const ca::u32 valIdx = cf.cp.addInteger(value);
    member.attrs.push_back(jbc::core::AttrSlot::makeNew(
        nameCp, std::make_unique<jbc::core::RawAttr>(
                    std::vector<ca::u8>{static_cast<ca::u8>(valIdx >> 8),
                                        static_cast<ca::u8>(valIdx)})));
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    return written.is_ok() ? std::move(written).unwrap() : Bytes{};
}

/// 供体引用零残留：全方法体 memberOf/classOf 面扫描（member_move_test 同款）。
void expectNoDonorRefs(const Bytes& classBytes, std::string_view donorInternalName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(classBytes));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();
    for (auto& visit : jbc::ir::collectCodeUnits(*session)) {
        ASSERT_NE(visit.kind, jbc::ir::CodeVisit::Kind::Failed);
        if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
        const jbc::ir::CodeUnit& cu = *visit.unit.get();
        for (const jbc::ir::InsnId iid : cu.dom.order()) {
            const jbc::ir::Insn* in = cu.dom.peek(iid);
            ASSERT_NE(in, nullptr);
            jbc::ir::ClassSession::MemberSem sem;
            if (session->memberOf(*in, sem)) {
                EXPECT_NE(sem.owner, donorInternalName);
            }
            const std::string_view cn = session->classOf(*in);
            if (!cn.empty()) {
                EXPECT_NE(cn, donorInternalName);
            }
        }
    }
}

Bytes resourceBytes(mjbase::model::ResourceContainer& container, const std::string& uri)
{
    const auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr);
    const auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource);
    EXPECT_NE(cls, nullptr);
    return cls != nullptr ? cls->get() : Bytes{};
}

}   // namespace

// 注册面：按名构造 + scope 标签（defaultPassRegistry 纯追加种子）。
TEST(MjPassToolClassCombinerTest, RegistryResolvable)
{
    auto& registry = mjbase::spi::defaultPassRegistry();
    auto  entry    = registry.find("ToolClassCombiner");
    ASSERT_TRUE(entry.is_ok());
    EXPECT_EQ(entry.unwrap().scope, mjbase::spi::PassScope::Global);
    auto created = registry.createGlobalPass("ToolClassCombiner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap().get(), nullptr);
}

// 候选不足 2：整体 no-op（all()，容器零触碰）。
TEST(MjPassToolClassCombinerTest, FewerThanTwoCandidatesIsNoOp)
{
    mjbase::model::ResourceContainer container;
    const Bytes                      alpha = buildUtAlpha();
    const Bytes                      app   = buildUtApp();
    addClassResource(container, "hb/ut/Alpha.class", alpha);
    addClassResource(container, "hb/ut/App.class", app);

    mjbase::pass::ToolClassCombiner     pass;
    ca::str::Utf8StringArena            arena;
    mjbase::spi::GlobalAnalysisManager  gam(arena);
    const auto                          pa = pass.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_FALSE(pa.changed());
    EXPECT_EQ(resourceBytes(container, "hb/ut/Alpha.class"), alpha);
    EXPECT_EQ(resourceBytes(container, "hb/ut/App.class"), app);
}

// 合并主体：供体删除、宿主合成、消费面重指向、宿主体内供体引用零残留。
TEST(MjPassToolClassCombinerTest, MergesCandidatesAndRewritesOwners)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "hb/ut/Alpha.class", buildUtAlpha());
    addClassResource(container, "hb/ut/Beta.class", buildUtBeta());
    const Bytes app = buildUtApp();
    addClassResource(container, "hb/ut/App.class", app);

    mjbase::pass::ToolClassCombiner     pass;
    ca::str::Utf8StringArena            arena;
    mjbase::spi::GlobalAnalysisManager  gam(arena);
    const auto                          pa = pass.run(container, gam);
    EXPECT_TRUE(pa.changed());
    EXPECT_FALSE(pa.areAllPreserved());

    EXPECT_FALSE(container.hasResource("hb/ut/Alpha.class"));
    EXPECT_FALSE(container.hasResource("hb/ut/Beta.class"));
    EXPECT_TRUE(container.hasResource("hb/ut/App.class"));
    const std::string hostUri = "mjbase/ToolClassMergedClass_0.class";
    ASSERT_TRUE(container.hasResource(hostUri));

    // 消费面：call 体的 INVOKESTATIC/GETSTATIC owner 全部重指向宿主。
    const Bytes appOut = resourceBytes(container, "hb/ut/App.class");
    EXPECT_NE(appOut, app) << "消费类应有改写面";
    expectNoDonorRefs(appOut, "hb/ut/Alpha");
    expectNoDonorRefs(appOut, "hb/ut/Beta");

    // 宿主面：成员集合 = 两供体全部可搬成员 + 宿主 <init>；体内零供体引用。
    const Bytes hostBytes = resourceBytes(container, hostUri);
    ASSERT_FALSE(hostBytes.empty());
    auto read = jbc::core::readClass(jbc::core::ByteView(hostBytes));
    ASSERT_TRUE(read.is_ok());
    jbc::core::ReadResult hostResult = std::move(read).unwrap();
    jbc::core::ClassFile& host       = *hostResult.file;
    EXPECT_EQ(std::string(host.thisName()), "mjbase/ToolClassMergedClass_0");
    std::set<std::string> hostMembers;
    for (const jbc::core::MemberInfo& m : host.fields) {
        hostMembers.emplace("F:" + std::string(host.nameOf(m)));
    }
    for (const jbc::core::MemberInfo& m : host.methods) {
        hostMembers.emplace("M:" + std::string(host.nameOf(m)));
    }
    EXPECT_EQ(hostMembers, std::set<std::string>({"M:<init>", "F:FLAG", "M:flag", "M:trim",
                                                  "F:STATE", "M:sign", "M:betaOnly"}));
    expectNoDonorRefs(hostBytes, "hb/ut/Alpha");
    expectNoDonorRefs(hostBytes, "hb/ut/Beta");
}

// NotModeled 保守化：宽常量候选整体落选、原资源逐位保持；其余照常合并。
TEST(MjPassToolClassCombinerTest, NotModeledWideConstCandidateKept)
{
    mjbase::model::ResourceContainer container;
    const Bytes                      wide = buildUtWide();
    addClassResource(container, "hb/ut/Wide.class", wide);
    addClassResource(container, "hb/ut/Alpha.class", buildUtAlpha());
    addClassResource(container, "hb/ut/Beta.class", buildUtBeta());

    mjbase::pass::ToolClassCombiner     pass;
    ca::str::Utf8StringArena            arena;
    mjbase::spi::GlobalAnalysisManager  gam(arena);
    const auto                          pa = pass.run(container, gam);
    EXPECT_TRUE(pa.changed());

    EXPECT_EQ(resourceBytes(container, "hb/ut/Wide.class"), wide)
        << "NotModeled 候选必须原样保留";
    EXPECT_FALSE(container.hasResource("hb/ut/Alpha.class"));
    EXPECT_FALSE(container.hasResource("hb/ut/Beta.class"));
    const Bytes hostBytes = resourceBytes(container, "mjbase/ToolClassMergedClass_0.class");
    ASSERT_FALSE(hostBytes.empty());
    expectNoDonorRefs(hostBytes, "hb/ut/Wide");
    auto read = jbc::core::readClass(jbc::core::ByteView(hostBytes));
    ASSERT_TRUE(read.is_ok());
    jbc::core::ReadResult hostResult = std::move(read).unwrap();
    bool hasWideMembers = false;
    for (const jbc::core::MemberInfo& m : hostResult.file->methods) {
        if (hostResult.file->nameOf(m) == std::string_view("wideConst")) hasWideMembers = true;
    }
    EXPECT_FALSE(hasWideMembers) << "保守保留候选的成员不得出现在宿主";
}

// 宿主成员序确定性：同一供体的成员连续且保持声明序（跨供体的先后随容器
// 迭代序，非契约面——golden 对拍按集合配对）。
TEST(MjPassToolClassCombinerTest, HostMemberOrderDeterministicPerDonor)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "hb/ut/Alpha.class", buildUtAlpha());
    addClassResource(container, "hb/ut/Beta.class", buildUtBeta());

    mjbase::pass::ToolClassCombiner     pass;
    ca::str::Utf8StringArena            arena;
    mjbase::spi::GlobalAnalysisManager  gam(arena);
    (void)pass.run(container, gam);

    const Bytes hostBytes =
        resourceBytes(container, "mjbase/ToolClassMergedClass_0.class");
    ASSERT_FALSE(hostBytes.empty());
    auto read = jbc::core::readClass(jbc::core::ByteView(hostBytes));
    ASSERT_TRUE(read.is_ok());
    jbc::core::ReadResult hostResult = std::move(read).unwrap();
    jbc::core::ClassFile& host       = *hostResult.file;

    // <init> 居首（宿主自建），其后按供体分块：块内字段/方法保持声明序。
    ASSERT_FALSE(host.methods.empty());
    EXPECT_EQ(std::string(host.nameOf(host.methods.front())), "<init>");
    const std::vector<std::pair<std::string, std::string>> alphaSequence = {
        {"flag", "()I"}, {"trim", "(I)I"}};
    const std::vector<std::pair<std::string, std::string>> betaSequence = {
        {"sign", "(I)I"}, {"betaOnly", "()I"}};
    std::vector<std::pair<std::string, std::string>> alphaSeq;
    std::vector<std::pair<std::string, std::string>> betaSeq;
    for (const jbc::core::MemberInfo& m : host.methods) {
        const std::string n(host.nameOf(m));
        const std::string d(host.descOf(m));
        if (n == "flag" || n == "trim") alphaSeq.emplace_back(n, d);
        else if (n == "sign" || n == "betaOnly") betaSeq.emplace_back(n, d);
    }
    EXPECT_EQ(alphaSeq, alphaSequence);
    EXPECT_EQ(betaSeq, betaSequence);
}

// ConstantValue 重建：搬移字段在宿主侧保有常量值。
TEST(MjPassToolClassCombinerTest, ConstantValueRebuiltOnAdoptedField)
{
    mjbase::model::ResourceContainer container;
    const Bytes alpha = withConstantValue(buildUtAlpha(), "FLAG", 4242);
    addClassResource(container, "hb/ut/Alpha.class", alpha);
    addClassResource(container, "hb/ut/Beta.class", buildUtBeta());

    mjbase::pass::ToolClassCombiner     pass;
    ca::str::Utf8StringArena            arena;
    mjbase::spi::GlobalAnalysisManager  gam(arena);
    (void)pass.run(container, gam);

    const Bytes hostBytes =
        resourceBytes(container, "mjbase/ToolClassMergedClass_0.class");
    ASSERT_FALSE(hostBytes.empty());
    auto read = jbc::core::readClass(jbc::core::ByteView(hostBytes));
    ASSERT_TRUE(read.is_ok());
    jbc::core::ReadResult hostResult = std::move(read).unwrap();
    jbc::core::ClassFile& host       = *hostResult.file;
    jbc::core::MemberInfo* flag = nullptr;
    for (jbc::core::MemberInfo& m : host.fields) {
        if (host.nameOf(m) == std::string_view("FLAG")) flag = &m;
    }
    ASSERT_NE(flag, nullptr);
    bool rebuilt = false;
    for (jbc::core::AttrSlot& slot : flag->attrs) {
        if (!host.cp.utf8Equals(slot.nameCp, "ConstantValue")) continue;
        auto decoded = host.decodeAttr(slot);
        ASSERT_TRUE(decoded.is_ok());
        ca::u32 idx = 0;
        if (decoded.unwrap()->kind() == jbc::core::AttrKind::ConstantValue) {
            idx = static_cast<const jbc::core::ConstantValueAttr*>(slot.body.get())
                      ->constantValueCp;
        } else {
            ASSERT_EQ(decoded.unwrap()->kind(), jbc::core::AttrKind::Raw);
            const auto* raw = static_cast<const jbc::core::RawAttr*>(slot.body.get());
            ASSERT_EQ(raw->bytes().size(), 2u);
            idx = (static_cast<ca::u32>(raw->bytes()[0]) << 8)
                  | static_cast<ca::u32>(raw->bytes()[1]);
        }
        EXPECT_EQ(host.cp.tagOf(idx), jbc::core::CpTag::Integer);
        EXPECT_EQ(host.cp.at(idx).ival, 4242);
        rebuilt = true;
    }
    EXPECT_TRUE(rebuilt) << "宿主字段必须重建 ConstantValue";
}
