/// @file        pass_golden_compare_test.cpp
/// @brief       pass golden 比对（mjbase 侧，golden 基线比对设施）。
///
/// 验收机器：同一语料跑 mjbase 试点 pass（经 pass_name_registry 按名构造），
/// 输出与冻结的 golden 基线逐字节比较；确有正当差异的类目按显式声明改跑
/// 替代断言（见「比对纪律」第二档）。
/// golden 与语料冻结在 mjbase/test_resource/pass_golden/ 下，属基线文件；再
/// 生成方式见该目录 README 与 doc_ai/pass-golden-pipeline.md。
///
/// 比对纪律：
///   - 默认零差异：语料是「writer 规范形」（no-op pass 的输出 == 语料，
///     提取器带不动点自检），mjbase copy-through 写回在未触碰区天然一致，
///     全部类目逐字节比较。
///   - 显式声明差异：确有正当差异的类目钉死在 manifests/<Pass>.manifest.txt
///     （每 pass 一份，列语料名 + 类目码 + 理由）；本测试对命中声明的类目改跑
///     声明的替代断言（如成员序语义比较），其余仍逐字节。默认清单为空。
///   - golden 缺失 = 基线破坏 = 失败（基线入库，不允许静默跳过）。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/classpath/resolved_call_graph.hpp"
#include "mjbase/jar/manifest.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"

namespace {

using Bytes = std::vector<ca::u8>;

/// pass 名（与 defaultPassRegistry 种子名一致）。
constexpr const char* kSorterName   = "ClassMemberSorter";
constexpr const char* kTightenerName = "AccessModifierTightener";
constexpr const char* kCleanerName  = "IllegalClassFileCleaner";

/// 语料道：tree 道服务 sorter/tightener，
/// jcf 道服务 cleaner。
constexpr const char* kTreeLane = "pass_golden/corpus/tree";
constexpr const char* kJcfLane  = "pass_golden/corpus/jcf";

std::vector<std::string> listLane(const char* lane)
{
    const std::filesystem::path dir = test::getResourcePath(lane);
    if (!std::filesystem::exists(dir)) {
        ADD_FAILURE() << "语料道缺失（基线未入库）: " << dir.string();
        return {};
    }
    std::vector<std::string> names;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        const std::string suffix = ".class";
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            name.erase(name.size() - suffix.size());
            names.push_back(std::move(name));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string lanePath(const char* lane, const std::string& name)
{
    return std::string(lane) + "/" + name + ".class";
}

std::string goldenPath(const char* passName, const std::string& name)
{
    return std::string("pass_golden/golden/") + passName + "/" + name + ".class";
}

std::string manifestPath(const char* passName)
{
    return std::string("pass_golden/manifests/") + passName + ".manifest.txt";
}

/// 方法 Code 嵌套属性中指定 typed kind 的槽数（解码面；声明类目 TA 存活
/// 断言用）。解码失败按 0 计——调用方另行断言解码成功。
ca::usize countNestedTypedAttrs(jbc::core::ClassFile& cf, jbc::core::AttrKind kind)
{
    ca::usize n = 0;
    for (jbc::core::MemberInfo& m : cf.methods) {
        for (jbc::core::AttrSlot& slot : m.attrs) {
            auto d = cf.decodeAttr(slot);
            if (d.is_err() || std::move(d).unwrap()->kind() != jbc::core::AttrKind::Code) {
                continue;
            }
            auto& code = static_cast<jbc::core::CodeAttr&>(*slot.body);
            for (jbc::core::AttrSlot& nested : code.attrs) {
                auto dn = cf.decodeAttr(nested);
                if (dn.is_err()) {
                    continue;
                }
                if (std::move(dn).unwrap()->kind() == kind) {
                    ++n;
                }
            }
        }
    }
    return n;
}

/// manifest 声明面：非空非注释行的首个空白段 = 语料名。
std::vector<std::string> readManifest(const char* passName)
{
    const std::string path = manifestPath(passName);
    if (!test::hasResource(path)) return {};
    std::vector<std::string> declared;
    // manifest 是 ASCII 小文件；test::getResource 交付字节向量，就地解析行。
    const Bytes raw    = test::getResource(path);
    std::string line;
    auto        flush = [&] {
        if (line.empty()) return;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const std::size_t last = line.find_last_not_of(" \t");
            const std::size_t sep  = line.find_first_of(" \t", first);
            declared.push_back(
                line.substr(first, sep == std::string::npos ? last - first + 1 : sep - first));
        }
        line.clear();
    };
    for (const ca::u8 b : raw) {
        if (b == '\n' || b == '\r') {
            flush();
        }
        else {
            line.push_back(static_cast<char>(b));
        }
    }
    flush();
    return declared;
}

/// 逐字节比较，不一致时报首个差异位置与两侧长度。
::testing::AssertionResult bytesEqual(const Bytes& golden, const Bytes& output)
{
    if (golden == output) return ::testing::AssertionSuccess();
    std::size_t i = 0;
    while (i < golden.size() && i < output.size() && golden[i] == output[i]) ++i;
    return ::testing::AssertionFailure()
           << "byte mismatch: golden=" << golden.size() << "B output=" << output.size()
           << "B, first diff at offset " << i;
}

/// 成员序（pass 语义面）：字段 (name,desc) 序列 + 方法 (name,desc) 序列。
std::vector<std::pair<std::string, std::string>> memberSequence(const jbc::core::ClassFile& cf,
                                                                bool methods)
{
    std::vector<std::pair<std::string, std::string>> seq;
    const std::vector<jbc::core::MemberInfo>& members = methods ? cf.methods : cf.fields;
    seq.reserve(members.size());
    for (const jbc::core::MemberInfo& m : members) {
        seq.emplace_back(cf.nameOf(m), cf.descOf(m));
    }
    return seq;
}

::testing::AssertionResult memberSequenceEqual(const jbc::core::ClassFile& golden,
                                               const jbc::core::ClassFile& output)
{
    const auto gFields = memberSequence(golden, /*methods*/ false);
    const auto oFields = memberSequence(output, /*methods*/ false);
    if (gFields != oFields) {
        return ::testing::AssertionFailure() << "field order mismatch: golden=" << gFields.size()
                                             << " entries, output=" << oFields.size() << " entries";
    }
    const auto gMethods = memberSequence(golden, /*methods*/ true);
    const auto oMethods = memberSequence(output, /*methods*/ true);
    if (gMethods != oMethods) {
        return ::testing::AssertionFailure() << "method order mismatch: golden=" << gMethods.size()
                                             << " entries, output=" << oMethods.size() << " entries";
    }
    return ::testing::AssertionSuccess();
}

/// 成员 access 语义面（tightener 声明类目用）：字段/方法 (name,desc,access)
/// 三元组序列逐位一致。成员序不受 tightener 影响，按位比较即覆盖收紧矩阵。
std::vector<std::tuple<std::string, std::string, ca::u16>> accessSequence(
    const jbc::core::ClassFile& cf, bool methods)
{
    std::vector<std::tuple<std::string, std::string, ca::u16>> seq;
    const std::vector<jbc::core::MemberInfo>& members = methods ? cf.methods : cf.fields;
    seq.reserve(members.size());
    for (const jbc::core::MemberInfo& m : members) {
        seq.emplace_back(cf.nameOf(m), cf.descOf(m), m.access);
    }
    return seq;
}

::testing::AssertionResult accessSequenceEqual(const jbc::core::ClassFile& golden,
                                               const jbc::core::ClassFile& output)
{
    if (accessSequence(golden, false) != accessSequence(output, false)) {
        return ::testing::AssertionFailure() << "field (name,desc,access) mismatch";
    }
    if (accessSequence(golden, true) != accessSequence(output, true)) {
        return ::testing::AssertionFailure() << "method (name,desc,access) mismatch";
    }
    return ::testing::AssertionSuccess();
}

/// 读一个 classfile（比对解析面），解析失败报失败。
::testing::AssertionResult parseClass(const Bytes& bytes, const char* what,
                                      jbc::core::ReadResult& out)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) {
        return ::testing::AssertionFailure() << what << ": libjbc readClass failed";
    }
    out = std::move(read).unwrap();
    return ::testing::AssertionSuccess();
}

/// 类是否携带 StackMapTable（Code 嵌套属性面；用于钉死保帧方向）。
bool classHasStackMapTable(jbc::core::ClassFile& cf)
{
    for (jbc::core::MemberInfo& m : cf.methods) {
        auto code = cf.decodeCode(m);
        if (code.is_err()) continue;
        for (const jbc::core::AttrSlot& slot : code.unwrap()->attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, "StackMapTable")) return true;
        }
    }
    return false;
}

}   // namespace

// ── 注册面：三试点按名构造，pass 域与注册表契约互锁 ──

TEST(MjPassGoldenCompare, PilotPassesResolvableViaRegistry)
{
    auto& registry = mjbase::spi::defaultPassRegistry();

    const auto sorter = registry.find(kSorterName);
    ASSERT_TRUE(sorter.is_ok());
    EXPECT_EQ(sorter.unwrap().scope, mjbase::spi::PassScope::Class);

    const auto tightener = registry.find(kTightenerName);
    ASSERT_TRUE(tightener.is_ok());
    EXPECT_EQ(tightener.unwrap().scope, mjbase::spi::PassScope::Global);

    const auto cleaner = registry.find(kCleanerName);
    ASSERT_TRUE(cleaner.is_ok());
    EXPECT_EQ(cleaner.unwrap().scope, mjbase::spi::PassScope::Global);
}

// ── ClassMemberSorter：no-reorder 类逐字节；声明类
//    按成员序语义比对 ──

TEST(MjPassGoldenCompare, ClassMemberSorter)
{
    const std::vector<std::string> names = listLane(kTreeLane);
    ASSERT_FALSE(names.empty());

    auto created = mjbase::spi::defaultPassRegistry().createClassPass(kSorterName);
    ASSERT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::ClassPass> pass = std::move(created).unwrap();
    ASSERT_NE(pass, nullptr);

    const std::vector<std::string> declared = readManifest(kSorterName);

    ca::str::Utf8StringArena arena;
    for (const std::string& name : names) {
        SCOPED_TRACE(name);
        const Bytes corpus = test::getResource(lanePath(kTreeLane, name));
        ASSERT_FALSE(corpus.empty());
        const Bytes golden = test::getResource(goldenPath(kSorterName, name));
        ASSERT_FALSE(golden.empty());

        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
        ASSERT_TRUE(sessionResult.is_ok());
        auto                          session = std::move(sessionResult).unwrap();
        mjbase::spi::ClassAnalysisManager cam(arena);

        const auto pa = pass->run(*session, cam);
        auto written  = jbc::core::writeClass(session->backing());
        ASSERT_TRUE(written.is_ok());
        const Bytes output = std::move(written).unwrap();

        const bool isDeclared =
            std::find(declared.begin(), declared.end(), name) != declared.end();
        if (!pa.changed()) {
            // 未触发重排：copy-through 应当与 golden 基线逐字节一致。
            EXPECT_TRUE(bytesEqual(golden, output));
            EXPECT_FALSE(isDeclared)
                << "manifest 声明了实际未发生差异的语料（清单过期）";
        }
        else {
            // 触发重排：golden 为全量重序列化形态（CP 按 encounter 序
            // 重编号），mjbase copy-through 保留原池——字节必不同，属 writer 级
            // 已声明差异（manifest 类目 CLASSFILE_REENCODE）；比对落在 pass 的
            // 语义面：成员序与 golden 一致。
            EXPECT_TRUE(isDeclared) << "重排发生但未在 manifest 声明（未声明的输出差异）";
            jbc::core::ReadResult goldenCf;
            ASSERT_TRUE(parseClass(golden, "golden", goldenCf));
            jbc::core::ReadResult outputCf;
            ASSERT_TRUE(parseClass(output, "output", outputCf));
            EXPECT_TRUE(memberSequenceEqual(*goldenCf.file, *outputCf.file));
        }
    }
}

// ── AccessModifierTightener：全语料一容器（跨类普查）；未声明类目逐字节，
//    声明类目按成员 access 语义比对 ──

TEST(MjPassGoldenCompare, AccessModifierTightener)
{
    const std::vector<std::string> names = listLane(kTreeLane);
    ASSERT_FALSE(names.empty());

    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass(kTightenerName);
    ASSERT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::GlobalPass> pass = std::move(created).unwrap();
    ASSERT_NE(pass, nullptr);

    const std::vector<std::string> declared = readManifest(kTightenerName);

    mjbase::model::ResourceContainer container;
    std::vector<Bytes>               corpusByName;
    for (const std::string& name : names) {
        const Bytes corpus = test::getResource(lanePath(kTreeLane, name));
        ASSERT_FALSE(corpus.empty());
        corpusByName.push_back(corpus);
        container.addResource(name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(name + ".class",
                                                                             corpus));
    }

    ca::str::Utf8StringArena          arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto                        pa = pass->run(container, gam);
    (void)pa;

    for (std::size_t i = 0; i < names.size(); ++i) {
        SCOPED_TRACE(names[i]);
        const Bytes golden = test::getResource(goldenPath(kTightenerName, names[i]));
        ASSERT_FALSE(golden.empty());
        auto&      resources = container.getResources();
        const auto it        = resources.find(names[i] + ".class");
        ASSERT_TRUE(it != resources.end());
        const auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(it->second.get());
        ASSERT_NE(cls, nullptr);

        const bool isDeclared = std::find(declared.begin(), declared.end(), names[i]) !=
                                declared.end();
        if (!isDeclared) {
            // access 翻转不改 CP/帧，输出与 golden 应逐字节一致。
            EXPECT_TRUE(bytesEqual(golden, cls->get()));
        }
        else {
            // 声明类目 TREE_SKIP_FRAMES_REENCODE：golden 基线走 SKIP_FRAMES
            // 写出不携带 StackMapTable，mjbase 保帧；比对落在 pass 语义面——收紧
            // 矩阵（成员 name/desc/access 序列）必须一致。
            jbc::core::ReadResult goldenCf;
            ASSERT_TRUE(parseClass(golden, "golden", goldenCf));
            jbc::core::ReadResult outputCf;
            ASSERT_TRUE(parseClass(cls->get(), "output", outputCf));
            EXPECT_TRUE(accessSequenceEqual(*goldenCf.file, *outputCf.file));
            // 帧差异方向也钉死：输出必须含帧（保帧口径，输出更完整），golden 不含。
            EXPECT_TRUE(classHasStackMapTable(*outputCf.file))
                << "mjbase 输出丢失 StackMapTable（保帧契约破坏）";
            EXPECT_FALSE(classHasStackMapTable(*goldenCf.file))
                << "golden 含 StackMapTable（声明类目的差异形态已变，复核 manifest）";
        }
    }
}

// ── IllegalClassFileCleaner：逐类独立容器；jcf 道规范形已在读取期吸收结构性
//    垃圾，golden 与 mjbase copy-through 输出应逐字节一致（规则 1 触发类同样
//    严格比较：非法描述符成员整条剔除） ──

TEST(MjPassGoldenCompare, IllegalClassFileCleaner)
{
    const std::vector<std::string> names = listLane(kJcfLane);
    ASSERT_FALSE(names.empty());

    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass(kCleanerName);
    ASSERT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::GlobalPass> pass = std::move(created).unwrap();
    ASSERT_NE(pass, nullptr);

    const std::vector<std::string> declared = readManifest(kCleanerName);
    (void)declared;   // 默认无声明差异；命中声明时在此扩展替代断言。

    ca::str::Utf8StringArena arena;
    for (const std::string& name : names) {
        SCOPED_TRACE(name);
        const Bytes corpus = test::getResource(lanePath(kJcfLane, name));
        ASSERT_FALSE(corpus.empty());
        const Bytes golden = test::getResource(goldenPath(kCleanerName, name));
        ASSERT_FALSE(golden.empty());

        mjbase::model::ResourceContainer container;
        container.addResource(name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(name + ".class",
                                                                             corpus));
        mjbase::spi::GlobalAnalysisManager gam(arena);
        const auto                         pa = pass->run(container, gam);
        (void)pa;

        auto&       resources = container.getResources();
        const auto  it        = resources.find(name + ".class");
        ASSERT_TRUE(it != resources.end());
        const auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(it->second.get());
        ASSERT_NE(cls, nullptr);
        EXPECT_TRUE(bytesEqual(golden, cls->get()));
    }
}

// ── A 批方法级 pass（NopRemover / RedundantLoadValEliminator /
//    CharArrToStrSimplifier / UnusedCheckCleaner）：tree 道语料 → mjbase
//    方法级管线（promote → pass → 管线统一 lower）→ 与 golden 基线比对。──
//
// 比对口径按 pass 的 CP 触碰面分两档（均由 manifest 驱动）：
//   - 未命中 manifest：逐字节（CP 不变的删除族；NopRemover 实测逐字节成立）；
//   - 命中 CLASSFILE_REENCODE：指令语义指纹比对。golden 为「改树全量重序列
//     化」冻结形态——常量池按新 encounter 序重
//     编号（未引用条目连带剪除），字节与 mjbase copy-through（保留原池）必然
//     不同，属 writer 级差异非 pass 语义差异；指纹 = 逐方法指令序列，每条带
//     LDC 解析常量面 / INVOKE 解析 member 三段（对编号不敏感，对语义敏感）。

namespace {

/// 方法级比对驱动：registry 按名构造 → 单 pass 管线驱动全部方法 → 写回字节。
Bytes runMjMethodPass(const char* passName, const Bytes& corpus)
{
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass(passName);
    EXPECT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::MethodPass> pass = std::move(created).unwrap();
    EXPECT_NE(pass, nullptr);
    if (pass == nullptr) return {};

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
    EXPECT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    ca::str::Utf8StringArena       arena;
    mjbase::spi::MethodAnalysisManager mam(arena);
    mjbase::KeyPool                    pool;
    mjbase::spi::MethodPassManager     mpm;
    mpm.addPass(std::move(pass));
    (void)mjbase::spi::runMethodPipelineOverSession(*session, mpm, mam, pool, nullptr, {});

    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok());
    return std::move(written).unwrap();
}

/// 单条指令的语义指纹：op + CP 引用面的解析结果（常量字面 / member 三段）。
struct InsnFingerprint
{
    int         op = 0;
    std::string semantic;   ///< CP 解析面（LDC 常量字面 / invoke member 三段）；无引用为空
};

/// 全类指令指纹（按方法序拼接；无 Code 的方法跳过——A 批语料全带 Code）。
std::vector<InsnFingerprint> classFingerprint(const Bytes& bytes)
{
    std::vector<InsnFingerprint> fps;
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return fps;
    auto session = std::move(sessionResult).unwrap();

    const ca::usize methodCount = session->backing().methods.size();
    for (ca::usize mi = 0; mi < methodCount; ++mi) {
        if (!jbc::ir::hasCode(*session, mi)) continue;   // 抽象/无码方法无指令面
        auto promoted = jbc::ir::promoteMethod(*session, mi);
        EXPECT_TRUE(promoted.is_ok());
        if (promoted.is_err()) continue;
        auto cu = std::move(promoted).unwrap();
        for (const jbc::ir::InsnId id : cu->dom.order()) {
            const jbc::ir::Insn* in = cu->dom.peek(id);
            EXPECT_NE(in, nullptr);
            if (in == nullptr) continue;
            InsnFingerprint fp;
            fp.op = static_cast<int>(in->op);
            jbc::ir::ClassSession::ConstSem c;
            if (session->constOf(*in, c)) {
                switch (c.kind) {
                case jbc::ir::ClassSession::ConstSem::Kind::Int:
                    fp.semantic = "int:" + std::to_string(c.num);
                    break;
                case jbc::ir::ClassSession::ConstSem::Kind::String:
                    fp.semantic = std::string("str:") + std::string(c.str);
                    break;
                default: break;
                }
            }
            jbc::ir::ClassSession::MemberSem m;
            if (session->memberOf(*in, m)) {
                fp.semantic = std::string(m.owner) + "." + std::string(m.name) + ":"
                              + std::string(m.desc);
            }
            fps.push_back(std::move(fp));
        }
    }
    return fps;
}

/// 双方指令指纹逐一比较（op 与 CP 语义面都必须一致）。
::testing::AssertionResult fingerprintsEqual(const std::vector<InsnFingerprint>& golden,
                                             const std::vector<InsnFingerprint>& output)
{
    if (golden.size() != output.size()) {
        return ::testing::AssertionFailure()
               << "insn count mismatch: golden=" << golden.size()
               << " output=" << output.size();
    }
    for (std::size_t i = 0; i < golden.size(); ++i) {
        if (golden[i].op != output[i].op || golden[i].semantic != output[i].semantic) {
            return ::testing::AssertionFailure()
                   << "insn #" << i << " mismatch: golden op=" << golden[i].op << " ["
                   << golden[i].semantic << "] vs output op=" << output[i].op << " ["
                   << output[i].semantic << "]";
        }
    }
    return ::testing::AssertionSuccess();
}

/// A 批单 pass 比对主体：遍历 A 批语料，manifest 命中走指纹比对，否则逐字节。
void compareAbatchMethodPass(const char* passName)
{
    const std::vector<std::string> names = listLane(kTreeLane);
    ASSERT_FALSE(names.empty());
    const std::vector<std::string> declared = readManifest(passName);

    for (const std::string& name : names) {
        const bool isAbatch = name.rfind("hb_nop_seq", 0) == 0
                              || name.rfind("hb_pop_pairs", 0) == 0
                              || name.rfind("hb_char_arr", 0) == 0
                              || name.rfind("hb_unused_check", 0) == 0
                              || name.rfind("hb_const_char_fold", 0) == 0
                              || name.rfind("hb_enum_cast", 0) == 0;
        if (!isAbatch) continue;
        SCOPED_TRACE(name);
        const Bytes corpus = test::getResource(lanePath(kTreeLane, name));
        ASSERT_FALSE(corpus.empty());
        const Bytes golden = test::getResource(goldenPath(passName, name));
        ASSERT_FALSE(golden.empty());
        const Bytes output = runMjMethodPass(passName, corpus);
        ASSERT_FALSE(output.empty());

        const bool isDeclared =
            std::find(declared.begin(), declared.end(), name) != declared.end();
        if (golden == output) {
            // 逐字节一致：不得命中声明（清单过期 = 反向错误）。
            EXPECT_FALSE(isDeclared)
                << "manifest 声明了实际未发生差异的语料（清单过期）";
            continue;
        }
        EXPECT_TRUE(isDeclared) << "输出有差异但未在 manifest 声明（未声明的输出差异）";
        EXPECT_TRUE(fingerprintsEqual(classFingerprint(golden), classFingerprint(output)));
    }
}

}   // namespace

TEST(MjPassGoldenCompare, NopRemover)
{
    compareAbatchMethodPass("NopRemover");
}

TEST(MjPassGoldenCompare, RedundantLoadValEliminator)
{
    compareAbatchMethodPass("RedundantLoadValEliminator");
}

TEST(MjPassGoldenCompare, CharArrToStrSimplifier)
{
    compareAbatchMethodPass("CharArrToStrSimplifier");
}

TEST(MjPassGoldenCompare, UnusedCheckCleaner)
{
    compareAbatchMethodPass("UnusedCheckCleaner");
}

TEST(MjPassGoldenCompare, EnumCastCheckRemover)
{
    compareAbatchMethodPass("EnumCastCheckRemover");
}

// ── 批 C 方法级指令变换组第一件：ConstCharArrayStringFolder（整窗折叠）──
TEST(MjPassGoldenCompare, ConstCharArrayStringFolder)
{
    compareAbatchMethodPass("ConstCharArrayStringFolder");
}

// ── 批 A ClassRenamer：全部语料必被重命名，逐字节比较无意义——比对面 = 重命名映射一致性 +
//    逐类骨架语义矩阵（this/super/接口/access/成员序）+ 方法体指令指纹。
//    golden 文件名即新类名（命名确定性：装载序按改名前类名字节序 +
//    全局递增编号），本 pass 不走 manifest（无「未声明字节一致」路径）。

/// 递归列出 golden 目录下的类文件（相对名，去 .class 后缀）。
std::vector<std::string> listGoldenDir(const std::string& passName)
{
    const std::filesystem::path dir =
        test::getResourcePath(std::string("pass_golden/golden/") + passName);
    if (!std::filesystem::exists(dir)) {
        ADD_FAILURE() << "golden 目录缺失（基线未入库）: " << dir.string();
        return {};
    }
    std::vector<std::string> names;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string rel = std::filesystem::relative(entry.path(), dir).generic_string();
        const std::string suffix = ".class";
        if (rel.size() > suffix.size() &&
            rel.compare(rel.size() - suffix.size(), suffix.size(), suffix) == 0) {
            rel.erase(rel.size() - suffix.size());
        }
        names.push_back(std::move(rel));
    }
    std::sort(names.begin(), names.end());
    return names;
}

/// 类级骨架语义矩阵：this/super/接口/access + 字段/方法 (name,desc,access) 序列。
::testing::AssertionResult classSkeletonEqual(const jbc::core::ClassFile& golden,
                                              const jbc::core::ClassFile& output)
{
    if (golden.thisName() != output.thisName()) {
        return ::testing::AssertionFailure() << "this name mismatch: golden="
                                             << golden.thisName() << " output="
                                             << output.thisName();
    }
    if (golden.access != output.access) {
        return ::testing::AssertionFailure() << "class access mismatch";
    }
    if (golden.superName() != output.superName()) {
        return ::testing::AssertionFailure() << "super name mismatch";
    }
    if (golden.interfaces.size() != output.interfaces.size()) {
        return ::testing::AssertionFailure() << "interface count mismatch";
    }
    for (std::size_t i = 0; i < golden.interfaces.size(); ++i) {
        if (golden.cp.utf8Of(golden.interfaces[i]) !=
            output.cp.utf8Of(output.interfaces[i])) {
            return ::testing::AssertionFailure() << "interface #" << i << " mismatch";
        }
    }
    if (accessSequence(golden, false) != accessSequence(output, false)) {
        return ::testing::AssertionFailure() << "field (name,desc,access) mismatch";
    }
    if (accessSequence(golden, true) != accessSequence(output, true)) {
        return ::testing::AssertionFailure() << "method (name,desc,access) mismatch";
    }
    return ::testing::AssertionSuccess();
}

TEST(MjPassGoldenCompare, ClassRenamer)
{
    const std::vector<std::string> goldenNames = listGoldenDir("ClassRenamer");
    ASSERT_FALSE(goldenNames.empty());
    const std::vector<std::string> names = listLane(kTreeLane);
    ASSERT_FALSE(names.empty());
    const std::vector<std::string> declared = readManifest("ClassRenamer");

    // golden 的 this-class 名 → 文件名对照（重命名后 golden 文件名即新名，
    // 与语料旧名的对应关系要经解析得出）。
    std::unordered_map<std::string, std::string> thisToGoldenFile;
    for (const std::string& newName : goldenNames) {
        const Bytes golden = test::getResource(
            std::string("pass_golden/golden/ClassRenamer/") + newName + ".class");
        ASSERT_FALSE(golden.empty());
        jbc::core::ReadResult goldenCf;
        ASSERT_TRUE(parseClass(golden, "golden", goldenCf)) << newName;
        thisToGoldenFile.emplace(std::string(goldenCf.file->thisName()), newName);
    }

    // ── 声明类目（manifest）：remapper 面对携带 Code 嵌套 Runtime*Type-
    //    Annotations 的类做操作数改写。libjbc relocation contract 封闭集落地
    //    （TA 补建模 + 重定位）之前，lower 预检 fail-closed（RelocationNot-
    //    Modeled），本 pass 原子放弃——测试曾在此钉死「全语料运行抛出 + 容器
    //    逐字节原样」。契约落地后 pass 正常完成，本节改钉新契约：全语料运行
    //    不抛、声明类按全语料编号契约改名、Code 嵌套 TA 存活（typed 判型 +
    //    条数不丢）、输出全解码重读恒等。该类仍不入 golden 逐字节比对：
    //    golden 基线对该形态的重序列化未重定位 Code 嵌套 TA（基线已知偏差），
    //    不可当仲裁。缩减语料比对
    //    仍在声明类目之外做（下方）。──


    // ── 缩减语料比对：剔除声明类目语料 → 剩余类 golden 语义比对（骨架矩阵 +
    //    指令指纹）。配对建立在 pass 的命名确定性契约上：装载序按旧名字节序
    //    + 全局递增编号（单计数器 + 集合查重）——测试按同一契约重算期望新名，
    //    再经 golden 的 thisName→文件名对照取得 golden 字节。剔除类之后的编号
    //    位移由同一重算自然覆盖。──
    struct CorpusEntry
    {
        std::string internalName;
        std::string corpusName;
    };
    std::vector<CorpusEntry> corpusEntries;
    for (const std::string& name : names) {
        const Bytes corpus = test::getResource(lanePath(kTreeLane, name));
        ASSERT_FALSE(corpus.empty());
        jbc::core::ReadResult corpusCf;
        ASSERT_TRUE(parseClass(corpus, "corpus", corpusCf)) << name;
        corpusEntries.push_back({std::string(corpusCf.file->thisName()), name});
    }
    std::sort(corpusEntries.begin(), corpusEntries.end(),
              [](const CorpusEntry& l, const CorpusEntry& r) {
                  return l.internalName < r.internalName;
              });
    // 命名契约重算（与 pass 一致：包前缀 + "Class"，全局递增编号，集合查重）。
    // 两个口径：全语料（golden 提取时的编号）与缩减语料（本次运行的编号）。
    const auto allocateNames = [](const std::vector<CorpusEntry>& entries) {
        std::unordered_map<std::string, std::string> out;
        std::set<std::string>                        allocated;
        int                                          id = 0;
        for (const CorpusEntry& entry : entries) {
            const std::string& oldName = entry.internalName;
            const auto         slash   = oldName.rfind('/');
            const std::string  base =
                slash == std::string::npos ? "Class" : oldName.substr(0, slash + 1) + "Class";
            std::string mapped;
            do {
                mapped = base + std::to_string(id++);
            } while (!allocated.insert(mapped).second);
            out.emplace(oldName, mapped);
        }
        return out;
    };
    std::vector<CorpusEntry> reducedEntries;
    for (const CorpusEntry& entry : corpusEntries) {
        if (std::find(declared.begin(), declared.end(), entry.corpusName)
            != declared.end()) {
            continue;
        }
        reducedEntries.push_back(entry);
    }
    // 比对驱动约束：缩减集必须剔除「排序在首个声明类目之后」的全部语料
    // （声明类 + 其后所有类），否则全局递增编号位移使 golden 新名与输出新名
    // 错位——golden 基线冻结自全语料，缩减集保持为全排序的**前缀**，保留类
    // 编号才与 golden 一致。被剔除的殿后类（tech/E 与 B 批 meta 语料 zz/* 对）
    // 的行为覆盖由单测与 JVM 门补足（非声明类目差异）。
    {
        std::size_t cut = corpusEntries.size();
        for (std::size_t i = 0; i < corpusEntries.size(); ++i) {
            if (std::find(declared.begin(), declared.end(), corpusEntries[i].corpusName)
                != declared.end()) {
                cut = i;
                break;
            }
        }
        while (reducedEntries.size() > cut) {
            reducedEntries.pop_back();
        }
    }
    const std::unordered_map<std::string, std::string> corpusToReducedNew =
        allocateNames(reducedEntries);
    // 全语料口径的期望新名（声明类目断言用：pass 现在在全语料上正常运行）。
    const std::unordered_map<std::string, std::string> corpusToFullNew =
        allocateNames(corpusEntries);

    // ── 声明类目新契约（full 容器）：运行不抛、改名走全语料编号契约、
    //    Code 嵌套 TA 存活且条数不丢、输出全解码重读恒等。──
    for (const std::string& corpusName : declared) {
        SCOPED_TRACE(corpusName);
        const Bytes corpus = test::getResource(lanePath(kTreeLane, corpusName));
        ASSERT_FALSE(corpus.empty());
        jbc::core::ReadResult corpusCf;
        ASSERT_TRUE(parseClass(corpus, "corpus", corpusCf)) << corpusName;
        const std::string internalName(corpusCf.file->thisName());

        mjbase::model::ResourceContainer full;
        for (const std::string& name : names) {
            const Bytes bytes = test::getResource(lanePath(kTreeLane, name));
            ASSERT_FALSE(bytes.empty());
            full.addResource(name + ".class",
                             std::make_unique<mjbase::model::ClassResource>(name + ".class",
                                                                            bytes));
        }
        auto createdFull = mjbase::spi::defaultPassRegistry().createGlobalPass("ClassRenamer");
        ASSERT_TRUE(createdFull.is_ok());
        std::unique_ptr<mjbase::spi::GlobalPass> fullPass = std::move(createdFull).unwrap();
        ca::str::Utf8StringArena                   fullArena;
        mjbase::spi::GlobalAnalysisManager         fullGam(fullArena);
        const auto                                 paFull = fullPass->run(full, fullGam);
        EXPECT_FALSE(paFull.areAllPreserved())
            << "TA 类已入 relocation 封闭集：全语料运行应正常完成改名";

        // 声明类输出：URI 随新名改写、thisName = 全语料编号契约期望新名。
        const auto expectedNew = corpusToFullNew.find(internalName);
        ASSERT_TRUE(expectedNew != corpusToFullNew.end()) << internalName;
        const auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            full.getResource(expectedNew->second + ".class"));
        ASSERT_NE(res, nullptr);
        const Bytes out = res->get();
        EXPECT_NE(out, corpus) << "声明类目应被正常改写（TA 已可重定位）";
        jbc::core::ReadResult outCf;
        ASSERT_TRUE(parseClass(out, "output", outCf)) << corpusName;
        EXPECT_EQ(std::string(outCf.file->thisName()), expectedNew->second);

        // Code 嵌套 TA 存活：改写前后 typed TA 嵌套属性条数一致，输出全解码
        // 后再写逐字节一致（重定位后的结构保真）。
        const ca::usize taSrc =
            countNestedTypedAttrs(*corpusCf.file, jbc::core::AttrKind::TypeAnnotations);
        const ca::usize taOut =
            countNestedTypedAttrs(*outCf.file, jbc::core::AttrKind::TypeAnnotations);
        EXPECT_GT(taSrc, ca::usize(0)) << "声明类目语料必须携带 Code 嵌套 TA";
        EXPECT_EQ(taOut, taSrc) << "改写不得增删 Code 嵌套 TA";
        ASSERT_TRUE(jbc::core::decodeAllAttrs(*outCf.file).is_ok()) << corpusName;
        auto w = writeClass(*outCf.file);
        ASSERT_TRUE(w.is_ok());
        EXPECT_EQ(std::move(w).unwrap(), out) << "声明类目输出重读恒等";
    }

    mjbase::model::ResourceContainer container;
    std::set<std::string>            reducedCorpusNames;
    for (const CorpusEntry& entry : reducedEntries) {
        const Bytes corpus = test::getResource(lanePath(kTreeLane, entry.corpusName));
        ASSERT_FALSE(corpus.empty());
        container.addResource(entry.corpusName + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  entry.corpusName + ".class", corpus));
        reducedCorpusNames.insert(entry.corpusName);
    }

    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass("ClassRenamer");
    ASSERT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::GlobalPass> pass = std::move(created).unwrap();

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto                         pa = pass->run(container, gam);
    EXPECT_FALSE(pa.areAllPreserved()) << "缩减语料上重命名必然发生";

    // 配对：输出类 thisName（新名）直接查 golden 文件；期望新名由契约重算给
    // 出（thisName 必须等于期望新名——重命名映射一致性顺带钉死）。
    auto&        resources = container.getResources();
    std::size_t  matched   = 0;
    for (const auto& [uri, resource] : resources) {
        const auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            resource.get());
        ASSERT_NE(cls, nullptr);
        jbc::core::ReadResult outputCf;
        ASSERT_TRUE(parseClass(cls->get(), "output", outputCf)) << uri;
        const std::string outputName(outputCf.file->thisName());

        // 该输出类来自哪条语料：缩减口径期望新名反查。
        const CorpusEntry* from = nullptr;
        for (const CorpusEntry& entry : reducedEntries) {
            if (corpusToReducedNew.at(entry.internalName) == outputName) {
                from = &entry;
                break;
            }
        }
        ASSERT_NE(from, nullptr) << "输出类 " << outputName << " 无语料来源（重命名漂移）";
        EXPECT_EQ(outputName, corpusToReducedNew.at(from->internalName));

        // 缩减集与全语料在保留类上的编号一致（殿后类剔除约束），golden 直接
        // 按保留类期望新名取。
        const auto g = thisToGoldenFile.find(corpusToReducedNew.at(from->internalName));
        ASSERT_TRUE(g != thisToGoldenFile.end())
            << "语料 " << from->corpusName << " 的 golden 缺失";
        SCOPED_TRACE(g->second);
        const Bytes golden = test::getResource(
            std::string("pass_golden/golden/ClassRenamer/") + g->second + ".class");
        ASSERT_FALSE(golden.empty());
        jbc::core::ReadResult goldenCf;
        ASSERT_TRUE(parseClass(golden, "golden", goldenCf));
        EXPECT_EQ(goldenCf.file->thisName(), outputName)
            << "golden 与输出的新名不同（剔除位移后编号一致性破坏）";
        EXPECT_TRUE(classSkeletonEqual(*goldenCf.file, *outputCf.file));
        EXPECT_TRUE(fingerprintsEqual(classFingerprint(golden), classFingerprint(cls->get())));
        ++matched;
    }
    EXPECT_EQ(matched, reducedEntries.size())
        << "配对数与缩减 golden 集合不一致（声明类目剔除面漂移）";
}

// ── 批 A 未使用成员清理家族（EmptyClassRemover / UnusedClassRemover /
//    UnusedFieldRemover / GlobalUnusedMethodRemover）：um 道专用 mini 语料 +
//    services / Manifest 两件非类 keep 根（本面同步重建，golden 只落类
//    字节）。删除类 pass（EmptyClassRemover / UnusedClassRemover）无类改写
//    面：幸存类逐字节一致，被删类以 golden 缺席钉死——「golden 缺失 = 该类
//    必须被删除」是本族比对的删除集语言。成员删除两件（UnusedFieldRemover /
//    GlobalUnusedMethodRemover）有改写面：golden 为全量重序列
//    化形态（删成员后失引 CP 条目连带剪除），mjbase 在 libjbc DOM 上 erase + CP 只追加
//    保留原池——字节必不同，manifest 声明 CLASSFILE_REENCODE，比对落语义面：
//    成员 (name,desc,access) 矩阵 + 方法体指令指纹；未声明类目仍逐字节。
//    非类资源（services / Manifest）四件 pass 均须原样保留，一并钉死。──

namespace {

constexpr const char* kUmLane = "pass_golden/corpus/um";

/// um 道语料名（golden 缺席 = 删除语义）。
const std::vector<std::string>& umCorpusNames()
{
    static const std::vector<std::string> names = {
        "hb_um_agent", "hb_um_app", "hb_um_dead", "hb_um_service", "hb_um_used",
    };
    return names;
}

/// 重建 um 容器：5 类语料 + services 文件（hb.UMService）+ Manifest
/// （Agent-Class: hb.UMAgent）。
void addUmContainerResources(mjbase::model::ResourceContainer& container)
{
    for (const std::string& name : umCorpusNames()) {
        const Bytes corpus = test::getResource(std::string(kUmLane) + "/" + name + ".class");
        ASSERT_FALSE(corpus.empty()) << name;
        container.addResource(name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(name + ".class",
                                                                             corpus));
    }
    const std::string serviceText = "hb.UMService\n";
    container.addResource("META-INF/services/hb.UMService",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "META-INF/services/hb.UMService",
                              Bytes(serviceText.begin(), serviceText.end())));
    mjbase::jar::Manifest manifest;
    manifest.setMainAttribute(ca::str::Utf8StringRef::from_cstr("Agent-Class"),
                              ca::str::Utf8StringRef::from_cstr("hb.UMAgent"));
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<mjbase::model::ManifestResource>(manifest));
}

/// um 可达性面：六分析全注册（层次
/// 事实由 HierarchyBundleAnalysis 承载）。
void registerUmReachability(mjbase::spi::GlobalAnalysisManager& gam)
{
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::asm_classpath::HierarchyBundleAnalysis>();
    gam.registerAnalysis<mjbase::asm_classpath::ResolvedCallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::GlobalReachabilityAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedGlobalReachabilityAnalysis>();
}

/// 家族单 pass 比对主体：golden 在场 → 幸存（声明类目语义比对，否则逐字节）；
/// golden 缺席 → 资源必须已删除。
void compareUnusedMemberFamilyPass(const char* passName)
{
    const std::vector<std::string> declared = readManifest(passName);

    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass(passName);
    ASSERT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::GlobalPass> pass = std::move(created).unwrap();
    ASSERT_NE(pass, nullptr);

    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    ASSERT_NO_FATAL_FAILURE(addUmContainerResources(container));
    registerUmReachability(gam);
    const auto pa = pass->run(container, gam);
    (void)pa;

    for (const std::string& name : umCorpusNames()) {
        SCOPED_TRACE(name);
        const Bytes corpus = test::getResource(std::string(kUmLane) + "/" + name + ".class");
        ASSERT_FALSE(corpus.empty());
        const std::string goldenRel = goldenPath(passName, name);

        auto&      resources = container.getResources();
        const auto it        = resources.find(name + ".class");
        if (!test::hasResource(goldenRel)) {
            // 删除集语言：golden 缺席 = 该类必须被本 pass 删除。
            EXPECT_TRUE(it == resources.end())
                << name << " 应被删除（golden 缺席）但资源仍在";
            continue;
        }
        ASSERT_TRUE(it != resources.end()) << name << " 应幸存（golden 在场）但资源被删除";
        const auto* cls =
            ca::core::tag_cast::cast<mjbase::model::ClassResource*>(it->second.get());
        ASSERT_NE(cls, nullptr);
        const Bytes golden = test::getResource(goldenRel);
        ASSERT_FALSE(golden.empty());

        const bool isDeclared =
            std::find(declared.begin(), declared.end(), name) != declared.end();
        if (golden == cls->get()) {
            // 逐字节一致：不得命中声明（清单过期 = 反向错误）。
            EXPECT_FALSE(isDeclared)
                << "manifest 声明了实际未发生差异的语料（清单过期）";
            continue;
        }
        EXPECT_TRUE(isDeclared) << "输出有差异但未在 manifest 声明（未声明的输出差异）";
        jbc::core::ReadResult goldenCf;
        ASSERT_TRUE(parseClass(golden, "golden", goldenCf));
        jbc::core::ReadResult outputCf;
        ASSERT_TRUE(parseClass(cls->get(), "output", outputCf));
        // 语义面：成员保留/删除集 + 矩阵（name,desc,access）+ 指令指纹。
        EXPECT_TRUE(accessSequenceEqual(*goldenCf.file, *outputCf.file));
        EXPECT_TRUE(fingerprintsEqual(classFingerprint(golden), classFingerprint(cls->get())));
    }

    // 非 keep 根资源面：services 与 Manifest 两件非类资源四件 pass 均原样保留。
    EXPECT_TRUE(container.hasResource("META-INF/services/hb.UMService"));
    EXPECT_TRUE(container.hasResource("META-INF/MANIFEST.MF"));
}

}   // namespace

TEST(MjPassGoldenCompare, EmptyClassRemover)
{
    compareUnusedMemberFamilyPass("EmptyClassRemover");
}

TEST(MjPassGoldenCompare, UnusedClassRemover)
{
    compareUnusedMemberFamilyPass("UnusedClassRemover");
}

TEST(MjPassGoldenCompare, UnusedFieldRemover)
{
    compareUnusedMemberFamilyPass("UnusedFieldRemover");
}

TEST(MjPassGoldenCompare, GlobalUnusedMethodRemover)
{
    compareUnusedMemberFamilyPass("GlobalUnusedMethodRemover");
}

// ── 批 A ToolClassCombiner：cc 道专用 mini 语料（合并语义面 pass，宿主为
//    新合成类，逐字节不可仲裁——比对语言按资源角色分档）：
//    - 供体（候选）：golden 缺席 = 必须被合并删除；声明类目（宽常量）
//      反转 = 必须原样保留且字节与语料一致；
//    - 消费面：golden 为全量重编码冻结形态 → 声明
//      CLASSFILE_REENCODE，指令语义指纹比对；
//    - 拒绝面（非候选无引用）：pass 不动点 == 语料不动，逐字节；
//    - 宿主（me/canrad/morpher/ToolClassMergedClass_N）：两侧都是新合成
//      类但 writer/属性面不同——按成员集合配对后
//      骨架矩阵 + 指令指纹比对，帧方向钉死（golden 无帧、输出有帧）。──

namespace {

constexpr const char* kCcLane = "pass_golden/corpus/cc";

/// cc 道语料名。
const std::vector<std::string>& ccCorpusNames()
{
    static const std::vector<std::string> names = {
        "hb_cc_alpha",     "hb_cc_beta",  "hb_cc_gamma",
        "hb_cc_wide",      "hb_cc_rej_clinit", "hb_cc_rej_iface",
        "hb_cc_rej_instance", "hb_cc_rej_nested", "hb_cc_consumer",
    };
    return names;
}

/// 宿主类名判定（pass 契约命名）。
bool isMergedHostName(std::string_view internalName)
{
    return internalName.find("ToolClassMergedClass_") != std::string_view::npos;
}

/// 宿主成员集合指纹：字段/方法 (name,desc,access) 排序序列（配对面，
/// 与顺序无关——宿主内成员序是容器迭代序的函数，配对不押注它）。
std::vector<std::string> hostMemberSet(const jbc::core::ClassFile& cf)
{
    std::vector<std::string> out;
    for (const jbc::core::MemberInfo& m : cf.fields) {
        out.push_back("F:" + std::string(cf.nameOf(m)) + ":" + std::string(cf.descOf(m)) + ":"
                      + std::to_string(m.access));
    }
    for (const jbc::core::MemberInfo& m : cf.methods) {
        out.push_back("M:" + std::string(cf.nameOf(m)) + ":" + std::string(cf.descOf(m)) + ":"
                      + std::to_string(m.access));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// 宿主成员集合（扣声明保留供体成员后）：golden 宿主对声明保留供体照常
/// 合并成员（输出不并入），golden 宿主面因此多出该供体的成员——配对前先扣除。
/// 按成员名过滤（本语料组内宿主成员名全局唯一，名滤即成员滤）。
std::vector<std::string> hostMemberSetExcept(const jbc::core::ClassFile& cf,
                                             const std::set<std::string>& keptNames)
{
    std::vector<std::string> out;
    for (const jbc::core::MemberInfo& m : cf.fields) {
        const std::string n(cf.nameOf(m));
        if (keptNames.count(n) != 0) continue;
        out.push_back("F:" + n + ":" + std::string(cf.descOf(m)) + ":"
                      + std::to_string(m.access));
    }
    for (const jbc::core::MemberInfo& m : cf.methods) {
        const std::string n(cf.nameOf(m));
        if (keptNames.count(n) != 0) continue;
        out.push_back("M:" + n + ":" + std::string(cf.descOf(m)) + ":"
                      + std::to_string(m.access));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// 宿主骨架矩阵（扣声明保留供体成员后）：access/super/接口 + 过滤后的
/// 字段/方法 (name,desc,access) 序列。this 名不在此核对（配对按成员集
/// 合建立，两侧宿主命名确定性由单测钉死）。
::testing::AssertionResult hostSkeletonEqualExcept(const jbc::core::ClassFile& golden,
                                                   const jbc::core::ClassFile& output,
                                                   const std::set<std::string>& keptNames)
{
    if (golden.access != output.access) {
        return ::testing::AssertionFailure() << "class access mismatch";
    }
    if (golden.superName() != output.superName()) {
        return ::testing::AssertionFailure() << "super name mismatch";
    }
    if (golden.interfaces.size() != output.interfaces.size()) {
        return ::testing::AssertionFailure() << "interface count mismatch";
    }
    for (std::size_t i = 0; i < golden.interfaces.size(); ++i) {
        if (golden.cp.utf8Of(golden.interfaces[i]) != output.cp.utf8Of(output.interfaces[i])) {
            return ::testing::AssertionFailure() << "interface #" << i << " mismatch";
        }
    }
    const auto filtered = [&](const jbc::core::ClassFile& cf, bool methods) {
        std::vector<std::tuple<std::string, std::string, ca::u16>> seq;
        const std::vector<jbc::core::MemberInfo>& members =
            methods ? cf.methods : cf.fields;
        for (const jbc::core::MemberInfo& m : members) {
            if (keptNames.count(std::string(cf.nameOf(m))) != 0) continue;
            seq.emplace_back(cf.nameOf(m), cf.descOf(m), m.access);
        }
        return seq;
    };
    if (filtered(golden, false) != filtered(output, false)) {
        return ::testing::AssertionFailure() << "field (name,desc,access) mismatch";
    }
    if (filtered(golden, true) != filtered(output, true)) {
        return ::testing::AssertionFailure() << "method (name,desc,access) mismatch";
    }
    return ::testing::AssertionSuccess();
}

/// 宿主逐方法指令指纹：方法物理序的 (name:desc, 指纹序列) 对——声明保留
/// 供体的方法混在 golden 宿主方法序中，整体拼接式指纹（classFingerprint）
/// 无法扣除，逐方法比对才能过滤。
std::vector<std::pair<std::string, std::vector<InsnFingerprint>>>
hostMethodFingerprints(const Bytes& bytes)
{
    std::vector<std::pair<std::string, std::vector<InsnFingerprint>>> out;
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return out;
    auto session = std::move(sessionResult).unwrap();

    const ca::usize methodCount = session->backing().methods.size();
    for (ca::usize mi = 0; mi < methodCount; ++mi) {
        if (!jbc::ir::hasCode(*session, mi)) continue;
        auto promoted = jbc::ir::promoteMethod(*session, mi);
        EXPECT_TRUE(promoted.is_ok());
        if (promoted.is_err()) continue;
        auto cu = std::move(promoted).unwrap();
        const jbc::core::MemberInfo& m =
            session->backing().methods[session->unit().methods[mi].backingIndex];
        std::vector<InsnFingerprint> fps;
        for (const jbc::ir::InsnId id : cu->dom.order()) {
            const jbc::ir::Insn* in = cu->dom.peek(id);
            EXPECT_NE(in, nullptr);
            if (in == nullptr) continue;
            InsnFingerprint fp;
            fp.op = static_cast<int>(in->op);
            jbc::ir::ClassSession::ConstSem c;
            if (session->constOf(*in, c)) {
                switch (c.kind) {
                case jbc::ir::ClassSession::ConstSem::Kind::Int:
                    fp.semantic = "int:" + std::to_string(c.num);
                    break;
                case jbc::ir::ClassSession::ConstSem::Kind::String:
                    fp.semantic = std::string("str:") + std::string(c.str);
                    break;
                default: break;
                }
            }
            jbc::ir::ClassSession::MemberSem mm;
            if (session->memberOf(*in, mm)) {
                fp.semantic = std::string(mm.owner) + "." + std::string(mm.name) + ":"
                              + std::string(mm.desc);
            }
            fps.push_back(std::move(fp));
        }
        std::string key = std::string(session->backing().nameOf(m)) + ":"
                          + std::string(session->backing().descOf(m));
        out.emplace_back(std::move(key), std::move(fps));
    }
    return out;
}

}   // namespace

TEST(MjPassGoldenCompare, ToolClassCombiner)
{
    const std::vector<std::string>& names = ccCorpusNames();
    const std::vector<std::string> goldenNames = listGoldenDir("ToolClassCombiner");
    ASSERT_FALSE(goldenNames.empty());

    // manifest 声明面：非空非注释行 = `语料名 类目码`。类目：
    //   CC_NOTMODELED_KEPT——ldc 宽常量候选按 libjbc Err(NotModeled) 口径
    //   保守保持原资源；
    //   CLASSFILE_REENCODE——全量重编码形态的 writer 级差异。
    std::map<std::string, std::string> declared;
    {
        const Bytes raw = test::getResource(manifestPath("ToolClassCombiner"));
        std::string line;
        auto flush = [&] {
            if (line.empty()) return;
            const std::size_t hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            std::istringstream stream(line);
            std::string       name;
            std::string       category;
            stream >> name >> category;
            if (!name.empty()) declared.emplace(name, category);
            line.clear();
        };
        for (const ca::u8 b : raw) {
            if (b == '\n' || b == '\r') flush();
            else line.push_back(static_cast<char>(b));
        }
        flush();
    }

    // golden 分档：宿主（内部名判定）与逐语料终态。
    std::vector<std::pair<std::string, Bytes>> goldenHosts;   // (内部名, bytes)
    std::map<std::string, Bytes>               goldenByCorpus;
    for (const std::string& rel : goldenNames) {
        const Bytes bytes =
            test::getResource(std::string("pass_golden/golden/ToolClassCombiner/") + rel
                              + ".class");
        ASSERT_FALSE(bytes.empty()) << rel;
        jbc::core::ReadResult cf;
        ASSERT_TRUE(parseClass(bytes, "golden", cf)) << rel;
        if (isMergedHostName(cf.file->thisName())) {
            goldenHosts.emplace_back(std::string(cf.file->thisName()), bytes);
        }
        else {
            goldenByCorpus.emplace(rel, bytes);
        }
    }
    ASSERT_FALSE(goldenHosts.empty()) << "golden 必须含宿主（合并发生过）";

    // 同序容器 + registry 按名构造 + 运行。
    mjbase::model::ResourceContainer container;
    std::map<std::string, Bytes>     corpusByName;
    for (const std::string& name : names) {
        const Bytes corpus = test::getResource(lanePath(kCcLane, name));
        ASSERT_FALSE(corpus.empty()) << name;
        corpusByName.emplace(name, corpus);
        container.addResource(name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(name + ".class",
                                                                             corpus));
    }
    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass("ToolClassCombiner");
    ASSERT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::GlobalPass> pass = std::move(created).unwrap();
    ca::str::Utf8StringArena                 arena;
    mjbase::spi::GlobalAnalysisManager       gam(arena);
    const auto                               pa = pass->run(container, gam);
    EXPECT_FALSE(pa.areAllPreserved()) << "cc 语料上合并必然发生";

    auto& resources = container.getResources();

    // 供体面：未声明候选必须消失；声明类目（NOTMODELED_KEPT）必须原样保留。
    static const char* kDonors[] = {"hb_cc_alpha", "hb_cc_beta", "hb_cc_gamma", "hb_cc_wide"};
    bool               keptPresent = false;
    for (const char* donor : kDonors) {
        SCOPED_TRACE(donor);
        const auto decl = declared.find(donor);
        const auto it  = resources.find(std::string(donor) + ".class");
        if (decl != declared.end() && decl->second == "CC_NOTMODELED_KEPT") {
            ASSERT_TRUE(it != resources.end()) << "声明保留的供体不得被合并";
            const auto* cls =
                ca::core::tag_cast::cast<mjbase::model::ClassResource*>(it->second.get());
            ASSERT_NE(cls, nullptr);
            EXPECT_TRUE(bytesEqual(corpusByName.at(donor), cls->get()))
                << "保守保留的供体必须与语料逐位一致";
            keptPresent = true;
        }
        else {
            EXPECT_TRUE(it == resources.end()) << "候选供体应被合并删除";
        }
    }
    EXPECT_TRUE(keptPresent) << "声明类目语料必须在场（清单防腐）";

    // 幸存面：拒绝矩阵逐字节；消费面按声明类目指纹比对。
    for (const std::string& name : names) {
        if (std::find(std::begin(kDonors), std::end(kDonors), name) != std::end(kDonors)) {
            continue;
        }
        SCOPED_TRACE(name);
        const auto g = goldenByCorpus.find(name);
        ASSERT_TRUE(g != goldenByCorpus.end()) << "golden 缺失: " << name;
        const auto it = resources.find(name + ".class");
        ASSERT_TRUE(it != resources.end()) << "幸存类不得被删除: " << name;
        const auto* cls =
            ca::core::tag_cast::cast<mjbase::model::ClassResource*>(it->second.get());
        ASSERT_NE(cls, nullptr);
        const auto decl = declared.find(name);
        if (cls->get() == g->second) {
            EXPECT_FALSE(decl != declared.end())
                << "manifest 声明了实际未发生差异的语料（清单过期）";
            continue;
        }
        ASSERT_TRUE(decl != declared.end() && decl->second == "CLASSFILE_REENCODE")
            << "输出有差异但未声明（未声明的输出差异）";
        jbc::core::ReadResult goldenCf;
        ASSERT_TRUE(parseClass(g->second, "golden", goldenCf));
        EXPECT_TRUE(fingerprintsEqual(classFingerprint(g->second), classFingerprint(cls->get())));
    }

    // 宿主面：声明保留供体的成员先从 golden 宿主面扣除，再按成员集合配对 → 骨架矩阵 +
    // 逐方法指令指纹；帧方向钉死。
    std::set<std::string> keptMemberNames;
    for (const auto& [donor, category] : declared) {
        if (category != "CC_NOTMODELED_KEPT") continue;
        jbc::core::ReadResult dCf;
        ASSERT_TRUE(parseClass(corpusByName.at(donor), "kept donor", dCf)) << donor;
        for (const jbc::core::MemberInfo& m : dCf.file->fields) {
            keptMemberNames.insert(std::string(dCf.file->nameOf(m)));
        }
        for (const jbc::core::MemberInfo& m : dCf.file->methods) {
            const std::string n(dCf.file->nameOf(m));
            // 构造器/clinit 一律不搬移：宿主的同名成员不在扣除面。
            if (n == "<init>" || n == "<clinit>") continue;
            keptMemberNames.insert(n);
        }
    }
    ASSERT_FALSE(keptMemberNames.empty()) << "声明保留供体必须提供成员（清单防腐）";

    std::vector<std::pair<std::string, Bytes>> outputHosts;
    for (const auto& [uri, resource] : resources) {
        const auto* cls =
            ca::core::tag_cast::cast<mjbase::model::ClassResource*>(resource.get());
        ASSERT_NE(cls, nullptr) << uri;
        jbc::core::ReadResult cf;
        ASSERT_TRUE(parseClass(cls->get(), "output", cf)) << uri;
        if (isMergedHostName(cf.file->thisName())) {
            outputHosts.emplace_back(std::string(cf.file->thisName()), cls->get());
        }
    }
    ASSERT_EQ(outputHosts.size(), goldenHosts.size())
        << "宿主数两侧一致（声明保留供体并入共享宿主，无独占宿主）";
    ASSERT_FALSE(outputHosts.empty());

    std::vector<bool> goldenPaired(goldenHosts.size(), false);
    for (const auto& [outName, outBytes] : outputHosts) {
        SCOPED_TRACE(outName);
        jbc::core::ReadResult outCf;
        ASSERT_TRUE(parseClass(outBytes, "output host", outCf));
        const auto outSet = hostMemberSetExcept(*outCf.file, keptMemberNames);

        std::size_t matched = goldenHosts.size();
        for (std::size_t i = 0; i < goldenHosts.size(); ++i) {
            if (goldenPaired[i]) continue;
            jbc::core::ReadResult gCf;
            ASSERT_TRUE(parseClass(goldenHosts[i].second, "golden host", gCf));
            if (hostMemberSetExcept(*gCf.file, keptMemberNames) == outSet) {
                matched = i;
                break;
            }
        }
        ASSERT_TRUE(matched < goldenHosts.size())
            << "输出宿主 " << outName << " 无 golden 配对（扣除声明保留成员后）";
        goldenPaired[matched] = true;
        jbc::core::ReadResult gCf;
        ASSERT_TRUE(parseClass(goldenHosts[matched].second, "golden host", gCf));

        EXPECT_TRUE(hostSkeletonEqualExcept(*gCf.file, *outCf.file, keptMemberNames))
            << "宿主骨架矩阵不一致（成员序随容器迭代序，配对后须一致）";
        auto goldenFps = hostMethodFingerprints(goldenHosts[matched].second);
        auto outputFps = hostMethodFingerprints(outBytes);
        goldenFps.erase(std::remove_if(goldenFps.begin(),
                                       goldenFps.end(),
                                       [&keptMemberNames](
                                           const std::pair<
                                               std::string,
                                               std::vector<InsnFingerprint>>& e) {
                                           const std::size_t colon = e.first.find(':');
                                           const std::string name =
                                               colon == std::string::npos
                                                   ? e.first
                                                   : e.first.substr(0, colon);
                                           return keptMemberNames.count(name) != 0;
                                       }),
                        goldenFps.end());
        ASSERT_EQ(goldenFps.size(), outputFps.size())
            << "宿主方法数不一致（扣声明保留成员后的指纹面）";
        for (std::size_t i = 0; i < goldenFps.size(); ++i) {
            EXPECT_EQ(goldenFps[i].first, outputFps[i].first)
                << "宿主方法 #" << i << " 错位";
            EXPECT_TRUE(fingerprintsEqual(goldenFps[i].second, outputFps[i].second))
                << "宿主方法 " << goldenFps[i].first << " 指令指纹不一致";
        }
        // 帧方向：输出为分支体再生 SMT，golden 必无帧。
        EXPECT_FALSE(classHasStackMapTable(*gCf.file))
            << "golden 宿主不应含帧";
    }
    // 全部 golden 宿主必须被配对（声明保留供体成员已在配对面扣除）。
    for (std::size_t i = 0; i < goldenHosts.size(); ++i) {
        EXPECT_TRUE(goldenPaired[i])
            << "golden 宿主 " << goldenHosts[i].first << " 未配对（合并集漂移）";
    }
    // 至少一个输出宿主携带 SMT（beta 分支体帧再生的方向钉死）。
    bool anyOutputSmt = false;
    for (const auto& [name, bytes] : outputHosts) {
        jbc::core::ReadResult cf;
        ASSERT_TRUE(parseClass(bytes, "output host", cf));
        if (classHasStackMapTable(*cf.file)) anyOutputSmt = true;
    }
    EXPECT_TRUE(anyOutputSmt) << "分支体并入宿主后必须再生 StackMapTable";
}

// ── B 批 JunkCodeInserter 自基线（防漂移）──
//
// 自基线模式：
// 确定性 pass 的输出按字节冻结为基线（MJBASE_PASS_GOLDEN_REGEN=1 时再生成），
// 常规运行逐字节比对——行为漂移即失败。硬验收面（真 JVM 执行）另在 JVM 用例。
// 基线语料取 hb_char_arr（15 指令方法，kStride 命中一次插入）与 hb_nop_seq
// （无插入点，copy-through 不动面）。

TEST(MjPassGoldenCompare, JunkCodeInserterSelfBaseline)
{
    const char*                     regen = std::getenv("MJBASE_PASS_GOLDEN_REGEN");
    const std::vector<std::string>  names = listLane(kTreeLane);
    ASSERT_FALSE(names.empty());

    for (const std::string& name : names) {
        const bool inScope = name.rfind("hb_char_arr", 0) == 0
                             || name.rfind("hb_nop_seq", 0) == 0;
        if (!inScope) continue;
        SCOPED_TRACE(name);
        const Bytes corpus = test::getResource(lanePath(kTreeLane, name));
        ASSERT_FALSE(corpus.empty());
        const Bytes output = runMjMethodPass("JunkCodeInserter", corpus);
        ASSERT_FALSE(output.empty());

        const std::string rel = std::string("pass_golden/golden_self/JunkCodeInserter/")
                                + name + ".class";
        if (regen != nullptr && regen[0] != '\0') {
            const std::filesystem::path dir =
                test::getResourcePath("pass_golden/golden_self/JunkCodeInserter");
            std::filesystem::create_directories(dir);
            std::ofstream out(dir / (name + ".class"), std::ios::binary);
            ASSERT_TRUE(out.good()) << "cannot write self baseline: " << rel;
            out.write(reinterpret_cast<const char*>(output.data()),
                      static_cast<std::streamsize>(output.size()));
            continue;
        }
        ASSERT_TRUE(test::hasResource(rel)) << "自基线缺失（先以 MJBASE_PASS_GOLDEN_REGEN=1 生成）: " << rel;
        const Bytes baseline = test::getResource(rel);
        ASSERT_FALSE(baseline.empty());
        EXPECT_TRUE(bytesEqual(baseline, output));
    }
}
// ── 批 B meta 元数据清理家族（13 件 ClassPass）：tree 道全语料 → mjbase
//    ClassPass → 写回字节，与 golden 基线比对。比对口径分两档：
//    - golden == output（逐字节一致）即通过。注意这合法地涵盖两类：未触发
//      类目（copy-through 恒等），以及**纯标志位触发**类目（FinalRemover /
//      SyntheticBridgeRemover / IllegalAccessRemover：清位不触碰 CP 与布局）
//      ——manifest 的
//      「触发」语义（golden != 语料）不蕴含「字节必异」，故本族不做
//      「声明则必须字节差异」的反向检查（与有强制重编码面的家族不同）。
//    - golden != output：必须命中 manifest 声明（未声明的字节差异 = 失败），
//      并落 meta 语义矩阵比对：类 access + 成员 (name,desc,access) 矩阵 +
//      三层属性槽面（槽序、槽内注解条目描述符序、Signature/SourceFile/
//      SourceDebugExtension 载荷串）+ Code 嵌套属性名序 + 逐方法指令指纹。──

namespace {

/// 单属性槽的语义面：槽名 + 槽内注解条目描述符序（注解两族）+ 载荷串
/// （Signature/SourceFile/SourceDebug）。解码失败保守记空面（两侧同败即一致）。
struct SlotFace
{
    std::string              name;
    std::vector<std::string> annoSeq;
    std::string              payload;
    bool operator==(const SlotFace& rhs) const
    {
        return name == rhs.name && annoSeq == rhs.annoSeq && payload == rhs.payload;
    }
};

std::vector<SlotFace> locationFace(jbc::core::ClassFile&              cf,
                                   std::vector<jbc::core::AttrSlot>& attrs)
{
    std::vector<SlotFace> out;
    out.reserve(attrs.size());
    for (jbc::core::AttrSlot& slot : attrs) {
        SlotFace face;
        face.name = std::string(cf.utf8(slot.nameCp));
        auto d    = cf.decodeAttr(slot);
        if (d.is_ok()) {
            const jbc::core::AttrBody* body = std::move(d).unwrap();
            if (body->kind() == jbc::core::AttrKind::Annotations) {
                for (const jbc::core::AnnotationsAttr::Annotation& a :
                     static_cast<const jbc::core::AnnotationsAttr*>(body)->annos) {
                    face.annoSeq.emplace_back(cf.utf8(a.typeCp));
                }
            }
            else if (body->kind() == jbc::core::AttrKind::TypeAnnotations) {
                for (const jbc::core::TypeAnnotationsAttr::Annotation& a :
                     static_cast<const jbc::core::TypeAnnotationsAttr*>(body)->annotations) {
                    face.annoSeq.emplace_back(cf.utf8(a.typeIndexCp));
                }
            }
            else if (body->kind() == jbc::core::AttrKind::Signature) {
                face.payload = std::string(cf.utf8(
                    static_cast<const jbc::core::SignatureAttr*>(body)->signatureCp));
            }
            else if (face.name == "SourceFile") {
                const auto view = cf.source().spanOrEmpty(slot.data);
                if (view.size() >= 2) {
                    const ca::u32 idx = (static_cast<ca::u32>(view[0]) << 8)
                                        | static_cast<ca::u32>(view[1]);
                    face.payload = std::string(cf.utf8(idx));
                }
            }
            else if (face.name == "SourceDebug") {
                const auto view = cf.source().spanOrEmpty(slot.data);
                face.payload.assign(reinterpret_cast<const char*>(view.data()), view.size());
            }
        }
        out.push_back(std::move(face));
    }
    return out;
}

/// meta 语义矩阵：类 access + 成员 (name,desc,access) 矩阵 + 类/字段/方法三
/// 层槽面 + 每方法 Code 嵌套属性名序（LNT/LVT 摘除面）。
::testing::AssertionResult metaFaceEqual(jbc::core::ClassFile& golden,
                                         jbc::core::ClassFile& output)
{
    if (golden.access != output.access) {
        return ::testing::AssertionFailure() << "class access mismatch";
    }
    if (accessSequence(golden, false) != accessSequence(output, false)) {
        return ::testing::AssertionFailure() << "field (name,desc,access) mismatch";
    }
    if (accessSequence(golden, true) != accessSequence(output, true)) {
        return ::testing::AssertionFailure() << "method (name,desc,access) mismatch";
    }
    if (locationFace(golden, golden.attrs) != locationFace(output, output.attrs)) {
        return ::testing::AssertionFailure() << "class attr slots mismatch";
    }
    if (golden.fields.size() != output.fields.size()) {
        return ::testing::AssertionFailure() << "field count mismatch";
    }
    for (std::size_t i = 0; i < golden.fields.size(); ++i) {
        if (locationFace(golden, golden.fields[i].attrs)
            != locationFace(output, output.fields[i].attrs)) {
            return ::testing::AssertionFailure() << "field #" << i << " attr slots mismatch";
        }
    }
    if (golden.methods.size() != output.methods.size()) {
        return ::testing::AssertionFailure() << "method count mismatch";
    }
    for (std::size_t i = 0; i < golden.methods.size(); ++i) {
        if (locationFace(golden, golden.methods[i].attrs)
            != locationFace(output, output.methods[i].attrs)) {
            return ::testing::AssertionFailure() << "method #" << i << " attr slots mismatch";
        }
        jbc::core::CodeAttr* gCode = nullptr;
        jbc::core::CodeAttr* oCode = nullptr;
        if (auto g = golden.decodeCode(golden.methods[i]); g.is_ok()) {
            gCode = std::move(g).unwrap();
        }
        if (auto o = output.decodeCode(output.methods[i]); o.is_ok()) {
            oCode = std::move(o).unwrap();
        }
        if ((gCode != nullptr) != (oCode != nullptr)) {
            return ::testing::AssertionFailure()
                   << "method #" << i << " Code presence mismatch";
        }
        if (gCode != nullptr) {
            std::vector<std::string> gNames;
            std::vector<std::string> oNames;
            for (const jbc::core::AttrSlot& nested : gCode->attrs) {
                gNames.emplace_back(golden.utf8(nested.nameCp));
            }
            for (const jbc::core::AttrSlot& nested : oCode->attrs) {
                oNames.emplace_back(output.utf8(nested.nameCp));
            }
            if (gNames != oNames) {
                return ::testing::AssertionFailure()
                       << "method #" << i << " Code nested attr names mismatch";
            }
        }
    }
    return ::testing::AssertionSuccess();
}

/// 批 B meta 家族语料（6 条既有 tree 道
/// 语料 + hb_meta_* 厨房水槽双件；A 批方法体语料不在本族）。
const std::vector<std::string>& metaCorpusNames()
{
    static const std::vector<std::string> names = {
        "anno_ica",            "anno_vva",         "default_package",
        "hb_meta_minimal",     "hb_meta_quirks",   "jdk5_enum_class",
        "jdk8_all_structures", "mbase_constant_folder1",
    };
    return names;
}

/// 家族单 pass 比对主体：13 件同构（scope:class、单类会话载体）。
void compareMetaFamilyPass(const char* passName)
{
    const std::vector<std::string>& names = metaCorpusNames();
    const std::vector<std::string>  declared = readManifest(passName);

    auto created = mjbase::spi::defaultPassRegistry().createClassPass(passName);
    ASSERT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::ClassPass> pass = std::move(created).unwrap();
    ASSERT_NE(pass, nullptr);

    ca::str::Utf8StringArena arena;
    for (const std::string& name : names) {
        SCOPED_TRACE(name);
        const Bytes corpus = test::getResource(lanePath(kTreeLane, name));
        ASSERT_FALSE(corpus.empty());
        const Bytes golden = test::getResource(goldenPath(passName, name));
        ASSERT_FALSE(golden.empty()) << "golden 缺失 = 基线破坏";

        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
        ASSERT_TRUE(sessionResult.is_ok());
        auto                              session = std::move(sessionResult).unwrap();
        mjbase::spi::ClassAnalysisManager cam(arena);
        const auto                        pa = pass->run(*session, cam);
        (void)pa;
        auto written = jbc::core::writeClass(session->backing());
        ASSERT_TRUE(written.is_ok());
        const Bytes output = std::move(written).unwrap();

        const bool isDeclared =
            std::find(declared.begin(), declared.end(), name) != declared.end();
        if (golden == output) {
            continue;   // 逐字节一致即通过（含纯标志位触发类目，见节首注）。
        }
        EXPECT_TRUE(isDeclared) << "输出有差异但未在 manifest 声明（未声明的输出差异）";
        jbc::core::ReadResult goldenCf;
        ASSERT_TRUE(parseClass(golden, "golden", goldenCf));
        jbc::core::ReadResult outputCf;
        ASSERT_TRUE(parseClass(output, "output", outputCf));
        EXPECT_TRUE(metaFaceEqual(*goldenCf.file, *outputCf.file));
        EXPECT_TRUE(fingerprintsEqual(classFingerprint(golden), classFingerprint(output)));
    }
}

}   // namespace

TEST(MjPassGoldenCompare, MetaInfoRemover)
{
    compareMetaFamilyPass("MetaInfoRemover");
}

TEST(MjPassGoldenCompare, FinalRemover)
{
    compareMetaFamilyPass("FinalRemover");
}

TEST(MjPassGoldenCompare, SyntheticBridgeRemover)
{
    compareMetaFamilyPass("SyntheticBridgeRemover");
}

TEST(MjPassGoldenCompare, KotlinMetadataRemover)
{
    compareMetaFamilyPass("KotlinMetadataRemover");
}

TEST(MjPassGoldenCompare, SourceInfoRemover)
{
    compareMetaFamilyPass("SourceInfoRemover");
}

TEST(MjPassGoldenCompare, SignatureRemover)
{
    compareMetaFamilyPass("SignatureRemover");
}

TEST(MjPassGoldenCompare, IllegalAnnotationRemover)
{
    compareMetaFamilyPass("IllegalAnnotationRemover");
}

TEST(MjPassGoldenCompare, IllegalSignatureRemover)
{
    compareMetaFamilyPass("IllegalSignatureRemover");
}

TEST(MjPassGoldenCompare, IllegalTypeAnnotationRemover)
{
    compareMetaFamilyPass("IllegalTypeAnnotationRemover");
}

TEST(MjPassGoldenCompare, IllegalSourceInfoRemover)
{
    compareMetaFamilyPass("IllegalSourceInfoRemover");
}

TEST(MjPassGoldenCompare, UnknownAttributeCleaner)
{
    compareMetaFamilyPass("UnknownAttributeCleaner");
}

TEST(MjPassGoldenCompare, IllegalAccessRemover)
{
    compareMetaFamilyPass("IllegalAccessRemover");
}

TEST(MjPassGoldenCompare, SpecificAnnotationRemover)
{
    compareMetaFamilyPass("SpecificAnnotationRemover");
}
