/// @file        renamer_apply_test.cpp
/// @brief       renamer 应用面接线验收：javac 多类样本 → 建议/计划构造 →
///              apply → 写回 → 重读断言（声明类 CP、成员名与描述符、跨类
///              调用点、InnerClasses/NestHost/NestMembers 结构元数据、未触及
///              类字节不动），加 JVM 真跑门（行为与原名一致 + 反射读新名，
///              照 rename_probe / class_renamer 真跑门写法）。JDK 不可用的
///              机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/renamer/name_proposal.hpp"
#include "mjbase/renamer/name_provider.hpp"
#include "mjbase/renamer/rename_engine.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace mjbase::renamer {

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;
using jbc::core::ClassFile;

/// 多类样本（单文件多顶层类）：ApplyMain 跨类引用 Service（静态方法调用 +
/// 实例字段读写 + 构造 + 嵌套类静态方法）；Service 带成员类 Loader（产出
/// InnerClasses/NestMembers/NestHost 结构元数据）；Sidestay 独立类（未触及
/// 字节不动断言）；ApplyProbe 反射核对新名（不进容器，不受改名影响）。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.rn;

public class ApplyMain {
    public static void main(String[] args) {
        System.out.println(Service.describe("legacy"));
        Service svc = new Service("tag-value");
        System.out.println(svc.tag);
        System.out.println(Service.Loader.unpack(21));
    }
}

class Service {
    public String tag;
    public Service(String tag) {
        this.tag = tag;
    }
    public static String describe(String who) {
        return "desc:" + who;
    }
    public static class Loader {
        public static int unpack(int v) {
            return v * 2;
        }
    }
}

class Sidestay {
    public static String stable() {
        return "side-stay";
    }
}

class ApplyProbe {
    public static void main(String[] args) throws Exception {
        Class<?> core = Class.forName("hb.jvm.rn.Core");
        Class<?> box = Class.forName("hb.jvm.rn.Core$Box");
        System.out.println("host=" + box.getDeclaringClass().getName());
        System.out.println("render=" + (core.getDeclaredMethod("render", String.class) != null));
        System.out.println("split=" + (box.getDeclaredMethod("split", int.class) != null));
        System.out.println("payload=" + core.getField("payload").getName());
        Object svc = core.getConstructor(String.class).newInstance("probe-tag");
        System.out.println("value=" + core.getField("payload").get(svc));
    }
}
)JAVA";

constexpr const char* kOldService = "hb/jvm/rn/Service";
constexpr const char* kOldLoader  = "hb/jvm/rn/Service$Loader";
constexpr const char* kNewService = "hb/jvm/rn/Core";
constexpr const char* kNewLoader  = "hb/jvm/rn/Core$Box";

void writeSource(const fs::path& path, const char* src)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << "cannot write sample source: " << path.string();
    out << src;
}

Bytes readClassBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot read sample class: " << path.string();
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeClassFile(const fs::path& path, const Bytes& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << path.string();
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/// 容器内某 URI 的 class 资源（缺失/异型返回空）。
model::ClassResource* containerClassResource(model::ResourceContainer& container,
                                             const std::string&        uri)
{
    return ca::core::tag_cast::cast<model::ClassResource*>(container.getResource(uri));
}

/// 容器内某 class 资源字节是否含某子串（CP/描述符残留廉价断言用）。
bool bytesContain(model::ResourceContainer& container, const std::string& uri,
                  const std::string& needle)
{
    const auto* res = containerClassResource(container, uri);
    if (res == nullptr) return false;
    const std::string bytes(reinterpret_cast<const char*>(res->get().data()),
                            res->get().size());
    return bytes.find(needle) != std::string::npos;
}

/// 类内全部指令的成员引用 (owner, name) 集合（跨类调用点读回断言用）。
std::vector<std::pair<std::string, std::string>> memberRefsOf(
    model::ResourceContainer& container, const std::string& uri)
{
    std::vector<std::pair<std::string, std::string>> refs;
    const auto*        res    = containerClassResource(container, uri);
    if (res == nullptr) return refs;
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(res->get()));
    if (sessionResult.is_err()) return refs;
    auto session = std::move(sessionResult).unwrap();
    for (auto& visit : jbc::ir::collectCodeUnits(*session)) {
        if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
        const jbc::ir::CodeUnit& cu = *visit.unit.get();
        for (const jbc::ir::InsnId iid : cu.dom.order()) {
            const jbc::ir::Insn* in = cu.dom.peek(iid);
            if (in == nullptr) continue;
            jbc::ir::ClassSession::MemberSem sem;
            if (session->memberOf(*in, sem)) {
                refs.emplace_back(std::string(sem.owner), std::string(sem.name));
            }
        }
    }
    return refs;
}

bool hasMemberRef(const std::vector<std::pair<std::string, std::string>>& refs,
                  const std::string& owner, const std::string& name)
{
    for (const auto& ref : refs) {
        if (ref.first == owner && ref.second == name) return true;
    }
    return false;
}

/// 字节 → 独立只读 ClassFile（结构元数据断言用）。
std::unique_ptr<ClassFile> parseClass(const Bytes& bytes)
{
    auto r = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (r.is_err()) {
        ADD_FAILURE() << "readClass failed";
        return nullptr;
    }
    return std::move(r).unwrap().file;
}

std::string utf8TextOf(const ClassFile& cf, ca::u32 index)
{
    return std::string(cf.cp.utf8Of(index));
}

std::string classTextOf(const ClassFile& cf, ca::u32 index)
{
    if (!cf.cp.valid(index)) return {};
    const auto& entry = cf.cp.at(index);
    if (entry.tag != jbc::core::CpTag::Class) return {};
    return utf8TextOf(cf, entry.a);
}

/// 类内全部指令的类型引用内部名集合（classOf 活跃位点；CP 孤儿条目与无引用
/// 残留 Utf8 不在此列——remapper 面只重指向不改写池条目内容，残留语义无害）。
std::vector<std::string> instructionClassRefsOf(model::ResourceContainer& container,
                                                const std::string&        uri)
{
    std::vector<std::string> names;
    const auto*              res = containerClassResource(container, uri);
    if (res == nullptr) return names;
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(res->get()));
    if (sessionResult.is_err()) return names;
    auto session = std::move(sessionResult).unwrap();
    for (auto& visit : jbc::ir::collectCodeUnits(*session)) {
        if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
        const jbc::ir::CodeUnit& cu = *visit.unit.get();
        for (const jbc::ir::InsnId iid : cu.dom.order()) {
            const jbc::ir::Insn* in = cu.dom.peek(iid);
            if (in == nullptr) continue;
            const std::string_view cn = session->classOf(*in);
            if (!cn.empty()) names.emplace_back(cn);
        }
    }
    return names;
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    for (const auto& item : values) {
        if (item == value) return true;
    }
    return false;
}

/// 声明方法名集合（成员槽级读回）。
std::vector<std::string> declaredMethodNamesOf(const ClassFile& cf)
{
    std::vector<std::string> names;
    for (const auto& method : cf.methods) {
        names.push_back(utf8TextOf(cf, method.nameCp));
    }
    return names;
}

/// 声明字段名集合（成员槽级读回）。
std::vector<std::string> declaredFieldNamesOf(const ClassFile& cf)
{
    std::vector<std::string> names;
    for (const auto& field : cf.fields) {
        names.push_back(utf8TextOf(cf, field.nameCp));
    }
    return names;
}

const jbc::core::AttrSlot* slotOf(const ClassFile&                        cf,
                                  const std::vector<jbc::core::AttrSlot>& attrs,
                                  std::string_view                        name)
{
    for (const jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// 固定建议流的测试 provider（与 renamer 测试族同款形态，本文件自持）。
class FixedProvider final : public NameProvider
{
public:
    FixedProvider(const char* name, std::vector<NameProposal> proposals)
        : name_(name)
        , proposals_(std::move(proposals))
    {}

    std::string_view name() const override { return name_; }

    ca::core::Result<std::vector<NameProposal>, std::string> propose(
        const RenameContext&) override
    {
        return ca::core::Ok(std::move(proposals_));
    }

private:
    std::string               name_;
    std::vector<NameProposal> proposals_;
};

/// 建议/计划构造 → apply（policy 全开：URI 随动供落盘真跑）。
RenamerRunResult applySample(model::ResourceContainer& container)
{
    std::vector<NameProposal> batch;
    batch.push_back(NameProposal(ProposalKind::Class, "", kOldService, "", kNewService, 1.0,
                                 "apply"));
    batch.push_back(NameProposal(ProposalKind::Class, "", kOldLoader, "", kNewLoader, 1.0,
                                 "apply"));
    batch.push_back(NameProposal(ProposalKind::Field, kOldService, "tag",
                                 "Ljava/lang/String;", "payload", 1.0, "apply"));
    batch.push_back(NameProposal(ProposalKind::Method, kOldService, "describe",
                                 "(Ljava/lang/String;)Ljava/lang/String;", "render", 1.0,
                                 "apply"));
    batch.push_back(NameProposal(ProposalKind::Method, kOldLoader, "unpack", "(I)I",
                                 "split", 1.0, "apply"));
    FixedProvider  provider("apply", std::move(batch));
    RenamerOptions options;
    options.planOnly  = false;
    options.policy    = mjbase::asm_remapper::RemapPolicy::all();
    RenamerEngine engine;
    return engine.run(container, {&provider}, options);
}

}   // namespace

TEST(MjRenamerApplyTest, javacMultiClassSampleRewritesDeclarationsCallsitesAndMetadata)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate_rn";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";
    writeSource(srcDir / "hb/jvm/rn/ApplyMain.java", kMainSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/rn/ApplyMain.java", binDir,
                                       &compileErr))
        << compileErr;

    // ── 容器装载 + 未触及类快照 ──
    model::ResourceContainer container;
    const Bytes sidestayBefore =
        readClassBytes(binDir / "hb/jvm/rn/Sidestay.class");
    container.addResource("hb/jvm/rn/ApplyMain.class",
                          std::make_unique<model::ClassResource>(
                              "hb/jvm/rn/ApplyMain.class",
                              readClassBytes(binDir / "hb/jvm/rn/ApplyMain.class")));
    container.addResource("hb/jvm/rn/Service.class",
                          std::make_unique<model::ClassResource>(
                              "hb/jvm/rn/Service.class",
                              readClassBytes(binDir / "hb/jvm/rn/Service.class")));
    container.addResource("hb/jvm/rn/Service$Loader.class",
                          std::make_unique<model::ClassResource>(
                              "hb/jvm/rn/Service$Loader.class",
                              readClassBytes(binDir / "hb/jvm/rn/Service$Loader.class")));
    container.addResource("hb/jvm/rn/Sidestay.class",
                          std::make_unique<model::ClassResource>(
                              "hb/jvm/rn/Sidestay.class", sidestayBefore));

    // ── 建议/计划构造 → apply ──
    const auto result = applySample(container);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.applied);
    EXPECT_EQ(result.stats.proposed, 5U);
    EXPECT_EQ(result.stats.accepted, 5U);
    EXPECT_EQ(result.stats.renamedMembers, 5U);

    // ── URI 面：改名类资源随类换 URI（all policy），未触及类原位不动 ──
    EXPECT_EQ(container.getResource("hb/jvm/rn/Service.class"), nullptr);
    EXPECT_EQ(container.getResource("hb/jvm/rn/Service$Loader.class"), nullptr);
    ASSERT_NE(containerClassResource(container, "hb/jvm/rn/Core.class"), nullptr);
    ASSERT_NE(containerClassResource(container, "hb/jvm/rn/Core$Box.class"), nullptr);
    ASSERT_NE(containerClassResource(container, "hb/jvm/rn/ApplyMain.class"), nullptr);
    ASSERT_NE(containerClassResource(container, "hb/jvm/rn/Sidestay.class"), nullptr);
    {
        const auto* res = containerClassResource(container, "hb/jvm/rn/Sidestay.class");
        ASSERT_NE(res, nullptr);
        EXPECT_EQ(res->get(), sidestayBefore) << "未触及类字节被改动";
    }

    // ── 跨类调用点：Main 对 Service/Loader 的全部引用换新名，零旧名残留 ──
    //（活跃位点读回：指令 memberOf/classOf 不得再指向旧名；池内无引用的孤儿
    //  条目/Utf8 不判——remapper 面只重指向不改写池条目内容。）
    {
        const auto classRefs = instructionClassRefsOf(container, "hb/jvm/rn/ApplyMain.class");
        EXPECT_FALSE(contains(classRefs, kOldService)) << "旧类名指令引用残留";
        EXPECT_FALSE(contains(classRefs, kOldLoader)) << "旧嵌套类名指令引用残留";
        EXPECT_TRUE(contains(classRefs, kNewService));   // new Service → new Core
        //（Core$Box 只有静态方法引用面，归 memberRefs 断言管。）
    }
    const auto mainRefs = memberRefsOf(container, "hb/jvm/rn/ApplyMain.class");
    EXPECT_TRUE(hasMemberRef(mainRefs, kNewService, "render"));
    EXPECT_TRUE(hasMemberRef(mainRefs, kNewService, "payload"));
    EXPECT_TRUE(hasMemberRef(mainRefs, kNewService, "<init>"));
    EXPECT_TRUE(hasMemberRef(mainRefs, kNewLoader, "split"));
    EXPECT_FALSE(hasMemberRef(mainRefs, kOldService, "describe"));
    EXPECT_FALSE(hasMemberRef(mainRefs, kOldService, "tag"));
    EXPECT_FALSE(hasMemberRef(mainRefs, kOldLoader, "unpack"));

    // ── 声明类：this_class/成员名换新；结构元数据走 typed 面 ──
    {
        const auto* res = containerClassResource(container, "hb/jvm/rn/Core.class");
        ASSERT_NE(res, nullptr);
        auto cf = parseClass(res->get());
        ASSERT_NE(cf, nullptr);
        EXPECT_EQ(classTextOf(*cf, cf->thisClass), kNewService);
        // 声明成员槽：字段 payload、方法 render/<init>；旧名槽不复存在。
        const auto fields  = declaredFieldNamesOf(*cf);
        const auto methods = declaredMethodNamesOf(*cf);
        EXPECT_TRUE(contains(fields, "payload"));
        EXPECT_FALSE(contains(fields, "tag"));
        EXPECT_TRUE(contains(methods, "render"));
        EXPECT_TRUE(contains(methods, "<init>"));
        EXPECT_FALSE(contains(methods, "describe"));
        // InnerClasses（typed 体）：成员类行 inner/outer/innerName 全换新。
        const jbc::core::AttrSlot* innerSlot = slotOf(*cf, cf->attrs, "InnerClasses");
        ASSERT_NE(innerSlot, nullptr);
        auto decoded = jbc::core::decodeInnerClassesBody(*cf, *innerSlot);
        ASSERT_TRUE(decoded.is_ok());
        const auto body = std::move(decoded).unwrap();
        const auto& ic  = static_cast<const jbc::core::InnerClassesAttr&>(*body);
        ASSERT_GE(ic.rows.size(), 1U);
        EXPECT_EQ(classTextOf(*cf, ic.rows[0].innerClassInfoCp), kNewLoader);
        EXPECT_EQ(classTextOf(*cf, ic.rows[0].outerClassInfoCp), kNewService);
        EXPECT_EQ(utf8TextOf(*cf, ic.rows[0].innerNameCp), "Box");
        // NestMembers：成员表指向新嵌套名。
        const jbc::core::AttrSlot* nestMembersSlot = slotOf(*cf, cf->attrs, "NestMembers");
        ASSERT_NE(nestMembersSlot, nullptr);
        auto nestMembersDecoded = jbc::core::decodeNestMembersBody(*cf, *nestMembersSlot);
        ASSERT_TRUE(nestMembersDecoded.is_ok());
        const auto nestMembersBody = std::move(nestMembersDecoded).unwrap();
        const auto& nestMembers =
            static_cast<const jbc::core::NestMembersAttr&>(*nestMembersBody);
        ASSERT_EQ(nestMembers.memberCps.size(), 1U);
        EXPECT_EQ(classTextOf(*cf, nestMembers.memberCps[0]), kNewLoader);
    }
    {
        const auto* res = containerClassResource(container, "hb/jvm/rn/Core$Box.class");
        ASSERT_NE(res, nullptr);
        auto cf = parseClass(res->get());
        ASSERT_NE(cf, nullptr);
        EXPECT_EQ(classTextOf(*cf, cf->thisClass), kNewLoader);
        EXPECT_TRUE(contains(declaredMethodNamesOf(*cf), "split"));
        EXPECT_FALSE(contains(declaredMethodNamesOf(*cf), "unpack"));
        // NestHost：宿主指向新外围名。
        const jbc::core::AttrSlot* nestHostSlot = slotOf(*cf, cf->attrs, "NestHost");
        ASSERT_NE(nestHostSlot, nullptr);
        auto nestHostDecoded = jbc::core::decodeNestHostBody(*cf, *nestHostSlot);
        ASSERT_TRUE(nestHostDecoded.is_ok());
        const auto nestHostBody = std::move(nestHostDecoded).unwrap();
        const auto& nestHost = static_cast<const jbc::core::NestHostAttr&>(*nestHostBody);
        EXPECT_EQ(classTextOf(*cf, nestHost.hostClassCp), kNewService);
        // InnerClasses 行同步换新。
        const jbc::core::AttrSlot* innerSlot = slotOf(*cf, cf->attrs, "InnerClasses");
        ASSERT_NE(innerSlot, nullptr);
        auto decoded = jbc::core::decodeInnerClassesBody(*cf, *innerSlot);
        ASSERT_TRUE(decoded.is_ok());
        const auto body = std::move(decoded).unwrap();
        const auto& ic  = static_cast<const jbc::core::InnerClassesAttr&>(*body);
        ASSERT_GE(ic.rows.size(), 1U);
        EXPECT_EQ(classTextOf(*cf, ic.rows[0].innerClassInfoCp), kNewLoader);
        EXPECT_EQ(classTextOf(*cf, ic.rows[0].outerClassInfoCp), kNewService);
    }
}

TEST(MjRenamerApplyTest, renamedSampleRunsOnRealJvmAndReflectionSeesNewNames)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate    = test::getTopProjectPath() / "test/jvm_gate_rn";
    const fs::path srcDir  = gate / "src";
    const fs::path binDir  = gate / "bin";
    const fs::path obfDir  = gate / "obf_apply";
    writeSource(srcDir / "hb/jvm/rn/ApplyMain.java", kMainSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/rn/ApplyMain.java", binDir,
                                       &compileErr))
        << compileErr;

    // ── 原样本基线运行 ──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.rn.ApplyMain");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(origOut.find("desc:legacy"), std::string::npos);
    EXPECT_NE(origOut.find("tag-value"), std::string::npos);
    EXPECT_NE(origOut.find("42"), std::string::npos);

    // ── apply（与重读断言用例同一条路径）→ 产物按 URI 落盘 ──
    model::ResourceContainer container;
    for (const char* uri :
         {"hb/jvm/rn/ApplyMain.class", "hb/jvm/rn/Service.class",
          "hb/jvm/rn/Service$Loader.class", "hb/jvm/rn/Sidestay.class"}) {
        container.addResource(
            uri, std::make_unique<model::ClassResource>(uri, readClassBytes(binDir / uri)));
    }
    const auto result = applySample(container);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.applied);
    for (const auto& [uri, resource] : container.getResources()) {
        const auto* cls = ca::core::tag_cast::cast<model::ClassResource*>(resource.get());
        ASSERT_NE(cls, nullptr) << uri;
        writeClassFile(obfDir / std::string(uri), cls->get());
    }
    // 反射探针不进容器（不受改名影响），随产物目录运行。
    writeClassFile(obfDir / "hb/jvm/rn/ApplyProbe.class",
                   readClassBytes(binDir / "hb/jvm/rn/ApplyProbe.class"));

    // ── 硬验收：改名后真 java 执行行为与原名一致 ──
    const auto renamed = test::jdk::runJavaMain(tc, obfDir, "hb.jvm.rn.ApplyMain");
    EXPECT_EQ(renamed.exitCode, 0) << "renamed ApplyMain rejected/failed:\n"
                                   << renamed.out;
    EXPECT_EQ(test::jdk::normalizeOutput(renamed.out), origOut);

    // ── 反射断言新名：getDeclaringClass 读新外围（InnerClasses typed 面）、
    //      新成员名/构造可反射可达、字段值随行 ──
    const auto probe = test::jdk::runJavaMain(tc, obfDir, "hb.jvm.rn.ApplyProbe");
    ASSERT_EQ(probe.exitCode, 0) << "probe failed:\n" << probe.out;
    const std::string probeOut = test::jdk::normalizeOutput(probe.out);
    EXPECT_NE(probeOut.find("host=hb.jvm.rn.Core"), std::string::npos) << probeOut;
    EXPECT_NE(probeOut.find("render=true"), std::string::npos) << probeOut;
    EXPECT_NE(probeOut.find("split=true"), std::string::npos) << probeOut;
    EXPECT_NE(probeOut.find("payload=payload"), std::string::npos) << probeOut;
    EXPECT_NE(probeOut.find("value=probe-tag"), std::string::npos) << probeOut;
    EXPECT_EQ(probeOut.find("Service"), std::string::npos) << probeOut;
}

}   // namespace mjbase::renamer
