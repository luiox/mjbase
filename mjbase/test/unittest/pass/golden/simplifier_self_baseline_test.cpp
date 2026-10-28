// 三件 simplifier（#17：FixPatternSimplifier / ExpressionSimplifier /
// TypeCastSimplifier）golden 自基线对拍。
//
// golden 纪律「基线在切换前从旧栈提取」对本族不适用：旧栈与 mjbase 的
// FixPatternSimplifier 规则集已分叉（#44 增补规则 12-20 仅在新栈），且本族
// 语料为仓内新建——无旧栈对拍基线可取。替代基线走 JunkCodeInserter 自基线
// 同款纪律：确定性 pass 的输出按字节冻结为自基线（MJBASE_PASS_GOLDEN_REGEN=1
// 再生成），常规运行逐字节对拍——行为漂移即失败。规则语义面（逐规则命中/
// 拒绝/边界守卫）由各自单测钉死，本面只管整体输出的字节级防漂移。
//
// 语料道独立为 corpus/smp（tree 道被 Sorter/Tightener/ClassRenamer 全量迭
// 代，入 tree 道需为全部族外 pass 补 golden）。语料为仓内生成器的冻结输出：
// 生成源码即本测试的 fixpatCorpus/exprCorpus/castCorpus（确定性构建 + 规范
// 形 write∘read 不动点自检），常规运行按字节复核语料未漂移——提取器在仓内，
// 语料可复现。语料方法全部线性无分支（分支型规则的命中/拒绝面在单测钉死）；
// 每件 pass 只对其同名义语料必有改写面，跨语料 copy-through 是合法 no-op
// （一并钉死「不命中形状字节原样」）。
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/fix_pattern_simplifier.hpp"
#include "mjbase/pass/window_rule.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

#include "shared/structural_pass_util.hpp"
#include "test_helper.hpp"

using namespace mjbase::test;

namespace {

constexpr const char* kSmpLane = "pass_golden/corpus/smp";

// ── 语料一：FixPatternSimplifier 形状（位恒等式 1/8 + 死代码 9 + 保持形）──
// mNeg      iload_0; ineg; iconst_m1; iadd; ireturn → iload_0; iconst_m1; ixor; ireturn
// mSelfXor  bipush 7; dup; ixor; ireturn            → iconst_0; ireturn
// mDeadPair iconst_1; pop; bipush 3; ireturn        → bipush 3; ireturn
// mKeep     iload_0; iconst_1; iadd; ireturn        （不命中，保持）
Bytes fixpatCorpus()
{
    FixtureBuilder b("t/HbFixPat");
    b.addMethod({"mNeg", "(I)I", {0x1a, 0x74, 0x02, 0x60, 0xac}, 0x0009, {}, 2, 1});
    b.addMethod({"mSelfXor", "(I)I", {0x10, 0x07, 0x59, 0x82, 0xac}, 0x0009, {}, 2, 0});
    b.addMethod({"mDeadPair", "()I", {0x04, 0x57, 0x10, 0x03, 0xac}, 0x0009, {}, 1, 0});
    b.addMethod({"mKeep", "(I)I", {0x1a, 0x04, 0x60, 0xac}, 0x0009, {}, 2, 0});
    return b.assemble();
}

// ── 语料二：ExpressionSimplifier 形状（恒等式族 + 保持形）──
// mAddZero   iload_0; iconst_0; iadd; ireturn   → iload_0; ireturn
// mZeroAnd   iload_0; iconst_0; iand; ireturn   → iconst_0; ireturn
// mSelfXor   iload_0; dup; ixor; ireturn        → iconst_0; ireturn
// mShlOne    iload_0; iconst_1; ishl; ireturn   → iload_0; iconst_2; imul; ireturn
// mDoubleNeg iload_0; ineg; ineg; ireturn       → iload_0; ireturn
// mKeep      iload_0; iconst_2; iadd; ireturn   （不命中，保持）
Bytes exprCorpus()
{
    FixtureBuilder b("t/HbExpr");
    b.addMethod({"mAddZero", "(I)I", {0x1a, 0x03, 0x60, 0xac}, 0x0009, {}, 2, 1});
    b.addMethod({"mZeroAnd", "(I)I", {0x1a, 0x03, 0x7e, 0xac}, 0x0009, {}, 2, 1});
    b.addMethod({"mSelfXor", "(I)I", {0x1a, 0x59, 0x82, 0xac}, 0x0009, {}, 2, 1});
    b.addMethod({"mShlOne", "(I)I", {0x1a, 0x04, 0x78, 0xac}, 0x0009, {}, 2, 1});
    b.addMethod({"mDoubleNeg", "(I)I", {0x1a, 0x74, 0x74, 0xac}, 0x0009, {}, 2, 1});
    b.addMethod({"mKeep", "(I)I", {0x1a, 0x05, 0x60, 0xac}, 0x0009, {}, 2, 0});
    return b.assemble();
}

// ── 语料三：TypeCastSimplifier 形状（常量生产者 cast 折叠 + 局部保持形）──
// mI2b       sipush 200; i2b; ireturn   → bipush -56; ireturn
// mI2c       iconst_m1; i2c; ireturn    → ldc 65535; ireturn
// mI2l       iconst_1; i2l; lreturn     → lconst_1; lreturn
// mI2s       sipush 100; i2s; ireturn   → bipush 100; ireturn
// mKeepLocal iload_0; i2l; lreturn      （cast on local，保持）
Bytes castCorpus()
{
    FixtureBuilder b("t/HbCast");
    b.addMethod({"mI2b", "()I", {0x11, 0x00, 0xc8, 0x91, 0xac}, 0x0009, {}, 1, 0});
    b.addMethod({"mI2c", "()I", {0x02, 0x92, 0xac}, 0x0009, {}, 1, 0});
    b.addMethod({"mI2l", "()J", {0x04, 0x85, 0xad}, 0x0009, {}, 2, 0});
    b.addMethod({"mI2s", "()I", {0x11, 0x00, 0x64, 0x93, 0xac}, 0x0009, {}, 1, 0});
    b.addMethod({"mKeepLocal", "(I)J", {0x1a, 0x85, 0xad}, 0x0009, {}, 2, 1});
    return b.assemble();
}

/// 规范形定冻：write(read(x)) 一次落位 + 不动点自检（语料文件是 writer 产物，
/// 二次写回必须逐字节相同）。
Bytes canonicalize(const Bytes& raw, const char* name)
{
    auto first = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(raw));
    EXPECT_TRUE(first.is_ok()) << name;
    if (first.is_err()) return {};
    auto once = jbc::core::writeClass(std::move(first).unwrap()->backing());
    EXPECT_TRUE(once.is_ok()) << name;
    if (once.is_err()) return {};
    const Bytes b1 = std::move(once).unwrap();

    auto second = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(b1));
    EXPECT_TRUE(second.is_ok()) << name;
    if (second.is_err()) return {};
    auto twice = jbc::core::writeClass(std::move(second).unwrap()->backing());
    EXPECT_TRUE(twice.is_ok()) << name;
    if (twice.is_err()) return {};
    const Bytes b2 = std::move(twice).unwrap();
    EXPECT_TRUE(b1 == b2) << name << ": 语料非 writer 规范形（write∘read 不动点破坏）";
    return b1;
}

struct CorpusSpec
{
    const char* name;
    Bytes (*build)();
};

const std::vector<CorpusSpec>& corpusSpecs()
{
    static const std::vector<CorpusSpec> specs = {
        {"hb_fixpat", fixpatCorpus}, {"hb_expr", exprCorpus}, {"hb_cast", castCorpus}};
    return specs;
}

/// registry 按名构造单 pass 管线跑全方法 → 写回字节（生产入口，与
/// pass_golden_compare 的方法级对拍同驱动）。
Bytes runMethodPassBytes(const char* passName, const Bytes& corpus)
{
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass(passName);
    EXPECT_TRUE(created.is_ok()) << mjbase::spi::to_string(created.unwrap_err());
    std::unique_ptr<mjbase::spi::MethodPass> pass = std::move(created).unwrap();
    EXPECT_NE(pass, nullptr);
    if (pass == nullptr) return {};

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();

    ca::str::Utf8StringArena           arena;
    mjbase::spi::MethodAnalysisManager mam(arena);
    mjbase::KeyPool                    pool;
    mjbase::spi::MethodPassManager     mpm;
    mpm.addPass(std::move(pass));
    (void)mjbase::spi::runMethodPipelineOverSession(*session, mpm, mam, pool, nullptr, {});

    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok());
    return std::move(written).unwrap();
}

void writeBaselineFile(const std::string& rel, const Bytes& bytes)
{
    const std::filesystem::path path = test::getResourcePath(rel);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << "cannot write baseline: " << rel;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/// 自基线对拍主体：同名义语料必有改写面（防空转 pin）；全部语料输出按
/// 字节与 golden_self 基线对拍；regen 模式改写基线文件。
void selfBaselineCompare(const char* passName, const char* ownCorpus)
{
    const char* regen = std::getenv("MJBASE_PASS_GOLDEN_REGEN");
    for (const CorpusSpec& spec : corpusSpecs()) {
        SCOPED_TRACE(spec.name);
        const Bytes corpus = canonicalize(spec.build(), spec.name);
        ASSERT_FALSE(corpus.empty());
        const Bytes output = runMethodPassBytes(passName, corpus);
        ASSERT_FALSE(output.empty());
        if (std::string(spec.name) == ownCorpus) {
            EXPECT_TRUE(output != corpus) << "同名义语料零改写（对拍空转）";
        }
        const std::string rel = std::string("pass_golden/golden_self/") + passName + "/"
                                + spec.name + ".class";
        if (regen != nullptr && regen[0] != '\0') {
            writeBaselineFile(rel, output);
            continue;
        }
        ASSERT_TRUE(test::hasResource(rel))
            << "自基线缺失（先以 MJBASE_PASS_GOLDEN_REGEN=1 生成）: " << rel;
        EXPECT_TRUE(test::getResource(rel) == output);
    }
}

}   // namespace

// ── 语料冻结面：生成器重建 == 冻结文件（防语料漂移）；regen 模式重写。──
TEST(SimplifierGoldenSelfBaseline, CorpusFrozenCanonical)
{
    const char* regen = std::getenv("MJBASE_PASS_GOLDEN_REGEN");
    for (const CorpusSpec& spec : corpusSpecs()) {
        SCOPED_TRACE(spec.name);
        const Bytes bytes = canonicalize(spec.build(), spec.name);
        ASSERT_FALSE(bytes.empty());
        const std::string rel = std::string(kSmpLane) + "/" + spec.name + ".class";
        if (regen != nullptr && regen[0] != '\0') {
            writeBaselineFile(rel, bytes);
            continue;
        }
        ASSERT_TRUE(test::hasResource(rel))
            << "语料缺失（先以 MJBASE_PASS_GOLDEN_REGEN=1 生成）: " << rel;
        EXPECT_TRUE(test::getResource(rel) == bytes) << "语料与生成器输出漂移";
    }
}

TEST(SimplifierGoldenSelfBaseline, FixPatternSimplifier)
{
    selfBaselineCompare("FixPatternSimplifier", "hb_fixpat");
}

TEST(SimplifierGoldenSelfBaseline, ExpressionSimplifier)
{
    selfBaselineCompare("ExpressionSimplifier", "hb_expr");
}

TEST(SimplifierGoldenSelfBaseline, TypeCastSimplifier)
{
    selfBaselineCompare("TypeCastSimplifier", "hb_cast");
}
