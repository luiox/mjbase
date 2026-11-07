/// @file        illegal_varargs_remover_test.cpp
/// @brief       varargs 位与描述符失配修复 pass 单测：位与 desc 四象限矩阵
///              （位有+阵列尾参保、位有+无尾参摘、位无+阵列尾参置、位无+
///              无尾参保）、无参描述符边界、畸形描述符 fail-closed（fixture
///              纪律第一类例外：jasm 拒收坏描述符，从装配类出发单点最小偏差
///              + relaxed 自检）、两面落盘（unit + backing）、幂等二跑、与
///              IllegalAccessRemover 共存、注册面与 opt-in 接线钉形。
///
/// 载体口径：基类一律 jasm 文本装配；位态构造走 CF 级 access 位单点改写
/// （模拟混淆器位污染工序），改写前后各做一次 relaxed validate 自检。
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/jasm/relaxed_verifier.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/illegal_varargs_remover.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/pipeline_config.hpp"

using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::pass::IllegalVarargsRemover;

namespace {

using Bytes = std::vector<ca::u8>;

constexpr ca::u16 kAccVarargs  = 0x0080;
constexpr ca::u16 kAccInterface = 0x0200;

// ── jasm 文本装配 ──

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 文本汇编最小类（方法 decl/body 对；汇编失败即测试失败）。
Bytes assembleClass(const std::string&                                     className,
                    const std::vector<std::pair<std::string, std::string>>& methods)
{
    mjbase::jasm::JvmJasmService service;
    std::string                  source =
        ".super java/lang/Object\n.class public super " + className + " {\n";
    for (const auto& [decl, body] : methods) {
        source += "  .method " + decl + " {\n    code: {\n" + body + "    }\n  }\n";
    }
    source += "}\n";
    const auto sourceName = utf16(className + ".jasm");
    const auto sourceText = utf16(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败";
        return {};
    }
    return result.value->bytes;
}

Bytes writeBytes(jbc::ir::ClassSession& session)
{
    auto written = jbc::core::writeClass(session.backing());
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// CF 级 access 位单点改写（fixture 纪律第一类例外：jasm 方言不产出非法位
/// 态，从装配类出发模拟混淆器位污染工序；改写后 relaxed 自检见各用例）。
Bytes withMethodAccessBits(const Bytes& bytes, const ca::usize methodIdx, const ca::u16 bits)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto session = std::move(created).unwrap();
    session->backing().methods[methodIdx].access =
        static_cast<ca::u16>(session->backing().methods[methodIdx].access | bits);
    return writeBytes(*session);
}

/// CF 级描述符单点改写（畸形描述符样本——jasm 装配器拒收的非法形态）。
Bytes withMethodDescriptor(const Bytes& bytes, const ca::usize methodIdx,
                           const std::string& desc)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto session = std::move(created).unwrap();
    session->backing().methods[methodIdx].descCp =
        session->backing().cp.addUtf8(desc);
    return writeBytes(*session);
}

// ── 断言面 ──

/// backing 与 unit 两面的 varargs 位事实。
bool backingBit(jbc::ir::ClassSession& session, const ca::usize idx)
{
    return (session.backing().methods[idx].access & kAccVarargs) != 0;
}

bool unitBit(jbc::ir::ClassSession& session, const ca::usize idx)
{
    return (session.unit().methods[idx].access & kAccVarargs) != 0;
}

std::string methodDesc(jbc::ir::ClassSession& session, const ca::usize idx)
{
    return std::string(session.backing().descOf(session.backing().methods[idx]));
}

/// relaxed validate 自检（层次图可空：外部类型按不可解析处置；本夹具方法体
/// 只有无参 return，不触引用赋值面）。
bool relaxedClean(const Bytes& bytes)
{
    return mjbase::jasm::analyzeRelaxed(bytes, nullptr).empty();
}

// ── pass 驱动 ──

struct RunResult
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    mjbase::spi::PreservedAnalyses         pa;
};

RunResult runVarargs(const Bytes& bytes)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    auto                              session = std::move(created).unwrap();
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    IllegalVarargsRemover             pass;
    RunResult                         run;
    run.session = session;
    run.pa      = pass.run(*session, cam);
    return run;
}

void addClassResource(ResourceContainer& container, const std::string& uri, const Bytes& bytes)
{
    container.addResource(uri, std::make_unique<ClassResource>(uri, bytes));
}

Bytes containerBytes(ResourceContainer& container, const std::string& uri)
{
    const auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr) << uri;
    const auto* cls = ca::core::tag_cast::cast<const ClassResource*>(resource);
    EXPECT_NE(cls, nullptr) << uri;
    return cls != nullptr ? cls->get() : Bytes{};
}

}   // namespace

// ── a) 位与 desc 四象限 ──

TEST(MjPassIllegalVarargsRemoverTest, ClearsBitWithoutArrayTail)
{
    // Q2：位有 + desc 尾参非阵列 → 摘位（Recaf 对齐方向）。
    const Bytes base  = assembleClass("R", {{"public static f (I)V", "      return\n"}});
    EXPECT_TRUE(relaxedClean(base)) << "基类 relaxed 自检（偏差前）";
    const Bytes bytes = withMethodAccessBits(base, 0, kAccVarargs);
    const auto  run   = runVarargs(bytes);

    EXPECT_TRUE(run.pa.changed());
    EXPECT_FALSE(backingBit(*run.session, 0)) << "backing 位摘除";
    EXPECT_FALSE(unitBit(*run.session, 0)) << "unit 位同步摘除";
}

TEST(MjPassIllegalVarargsRemoverTest, ClearsBitOnZeroParamMethod)
{
    // Q2 边界：无参描述符 ()V——尾参不存在即非阵列，摘位。
    const Bytes base  = assembleClass("R", {{"public static f ()V", "      return\n"}});
    const Bytes bytes = withMethodAccessBits(base, 0, kAccVarargs);
    const auto  run   = runVarargs(bytes);

    EXPECT_TRUE(run.pa.changed());
    EXPECT_FALSE(backingBit(*run.session, 0));
    EXPECT_FALSE(unitBit(*run.session, 0));
}

TEST(MjPassIllegalVarargsRemoverTest, KeepsBitWithArrayTail)
{
    // Q1：位有 + 阵列尾参 → 匹配，原样（字节不动）。
    const Bytes base  = assembleClass("R", {{"public static g ([I)V", "      return\n"}});
    EXPECT_TRUE(relaxedClean(base));
    const Bytes bytes = withMethodAccessBits(base, 0, kAccVarargs);
    const auto  run   = runVarargs(bytes);

    EXPECT_FALSE(run.pa.changed());
    EXPECT_TRUE(backingBit(*run.session, 0));
    EXPECT_TRUE(unitBit(*run.session, 0));
    auto written = jbc::core::writeClass(run.session->backing());
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(std::move(written).unwrap(), bytes) << "字节不动";
}

TEST(MjPassIllegalVarargsRemoverTest, SetsBitForArrayTail)
{
    // Q3：位无 + 阵列尾参 → 置位（修复方向，Recaf 之外的扩展，见头注）。
    const Bytes bytes =
        assembleClass("R", {{"public static h ([Ljava/lang/String;)V", "      return\n"}});
    const auto run = runVarargs(bytes);

    EXPECT_TRUE(run.pa.changed());
    EXPECT_TRUE(backingBit(*run.session, 0)) << "backing 位恢复";
    EXPECT_TRUE(unitBit(*run.session, 0)) << "unit 位同步恢复";
}

TEST(MjPassIllegalVarargsRemoverTest, KeepsNoBitWithoutArrayTail)
{
    // Q4：位无 + 尾参非阵列 → 匹配，原样。
    const Bytes bytes = assembleClass("R", {{"public static f (I)V", "      return\n"}});
    const auto  run   = runVarargs(bytes);

    EXPECT_FALSE(run.pa.changed());
    auto written = jbc::core::writeClass(run.session->backing());
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(std::move(written).unwrap(), bytes) << "字节不动";
}

TEST(MjPassIllegalVarargsRemoverTest, FourQuadrantMatrixSingleClass)
{
    // 四象限单类矩阵：q1 位+阵列（保）、q2 位+标量（摘）、q3 无位+阵列（置）、
    // q4 无位+标量（保）——仅 q2 摘位、q3 置位。
    const Bytes base  = assembleClass("R",
                                     {{"public static q1 ([I)V", "      return\n"},
                                      {"public static q2 (I)V", "      return\n"},
                                      {"public static q3 ([I)V", "      return\n"},
                                      {"public static q4 (I)V", "      return\n"}});
    const Bytes bytes = withMethodAccessBits(base, 0, kAccVarargs);
    const Bytes bytes2 = withMethodAccessBits(bytes, 1, kAccVarargs);
    const auto  run    = runVarargs(bytes2);

    EXPECT_TRUE(run.pa.changed());
    EXPECT_TRUE(backingBit(*run.session, 0)) << "q1 匹配保留";
    EXPECT_FALSE(backingBit(*run.session, 1)) << "q2 非法位摘除";
    EXPECT_TRUE(backingBit(*run.session, 2)) << "q3 缺位恢复";
    EXPECT_FALSE(backingBit(*run.session, 3)) << "q4 匹配保留";
    for (ca::usize i = 0; i < 4; ++i) {
        EXPECT_EQ(backingBit(*run.session, i), unitBit(*run.session, i)) << "quadrant " << i;
    }
}

// ── b) fail-closed：畸形描述符跳过 ──

TEST(MjPassIllegalVarargsRemoverTest, MalformedDescriptorSkipped)
{
    // fixture 纪律第一类例外：jasm 拒收坏描述符——从装配出的合法类出发做
    // 单点最小偏差（descCp → "(Q)V" 基类型字符越 JVMS 集 + 位污染），偏差前
    // relaxed 自检通过；偏差后 relaxed 判据面是指令数据流、不裁描述符文法
    // （此处不以其失败为断言），形态合法性由直接读回 desc 断言钉死。
    const Bytes base  = assembleClass("R", {{"public static f (I)V", "      return\n"}});
    ASSERT_TRUE(relaxedClean(base)) << "偏差前 relaxed 自检";
    const Bytes bytes  = withMethodAccessBits(base, 0, kAccVarargs);
    const Bytes broken = withMethodDescriptor(bytes, 0, "(Q)V");
    const auto  run    = runVarargs(broken);

    EXPECT_EQ(methodDesc(*run.session, 0), "(Q)V") << "偏差形态就位";
    EXPECT_TRUE(backingBit(*run.session, 0)) << "描述符形状非法：fail-closed 两向不动";
}

// ── c) 幂等二跑 ──

TEST(MjPassIllegalVarargsRemoverTest, IdempotentSecondRun)
{
    const Bytes base = assembleClass(
        "R",
        {{"public static f (I)V", "      return\n"},
         {"public static g ([I)V", "      return\n"}});
    const Bytes bytes = withMethodAccessBits(base, 0, kAccVarargs);
    const auto  first = runVarargs(bytes);
    EXPECT_TRUE(first.pa.changed());

    {
        ca::str::Utf8StringArena          arena;
        mjbase::spi::ClassAnalysisManager cam(arena);
        IllegalVarargsRemover             pass;
        EXPECT_FALSE(pass.run(*first.session, cam).changed()) << "同会话二跑零命中";
    }
    {
        const Bytes settled = writeBytes(*first.session);
        const auto  second  = runVarargs(settled);
        EXPECT_FALSE(second.pa.changed()) << "重载二跑零命中";
    }
}

// ── d) 与其他修复件共存 ──

TEST(MjPassIllegalVarargsRemoverTest, CoexistsWithIllegalAccessRemover)
{
    // 方法非法位（INTERFACE）由 IllegalAccessRemover 清、varargs 非法位由
    // 本件清——两位面正交同链。
    const Bytes base  = assembleClass("R", {{"public static f (I)V", "      return\n"}});
    const Bytes bytes = withMethodAccessBits(base, 0,
                                             static_cast<ca::u16>(kAccVarargs | kAccInterface));
    ResourceContainer container;
    addClassResource(container, "R.class", bytes);

    const auto loaded = mjbase::spi::loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"IllegalAccessRemover"},)json"
        R"json({"pass":"IllegalVarargsRemover"}]})json",
        mjbase::spi::defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    mjbase::spi::PipelineBuilder builder;
    ASSERT_TRUE(mjbase::spi::applyConfigTo(mjbase::spi::defaultPassRegistry(),
                                           loaded.unwrap(), builder)
                    .is_ok());
    mjbase::spi::PassJobContext job;
    auto                        mgr = builder.buildNestedPipeline(job);
    const auto                  ran = mjbase::spi::runContainerPipeline(container, mgr, job);
    ASSERT_TRUE(ran.is_ok()) << ran.unwrap_err().detail;

    const Bytes after = containerBytes(container, "R.class");
    const auto  read  = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(after));
    ASSERT_TRUE(read.is_ok());
    auto session = std::move(read).unwrap();
    EXPECT_EQ(session->backing().methods[0].access & kAccInterface, 0)
        << "非法 INTERFACE 位已清";
    EXPECT_EQ(session->backing().methods[0].access & kAccVarargs, 0)
        << "varargs 非法位已清";
}

// ── e) 注册面与 opt-in 接线 ──

TEST(MjPassIllegalVarargsRemoverTest, RegisteredAsIllegalVarargsRemover)
{
    using mjbase::spi::PassScope;
    mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    EXPECT_TRUE(reg.has("IllegalVarargsRemover"));
    const auto found = reg.find("IllegalVarargsRemover");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, PassScope::Class);
    EXPECT_FALSE(found.unwrap().description.empty());

    auto created = reg.createClassPass("IllegalVarargsRemover");
    ASSERT_TRUE(created.is_ok());
    const Bytes bytes =
        assembleClass("RG", {{"public static h ([I)V", "      return\n"}});
    auto createdSession = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(createdSession.is_ok());
    auto                              session = std::move(createdSession).unwrap();
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    const auto pa = std::move(created).unwrap()->run(*session, cam);
    EXPECT_TRUE(pa.changed()) << "注册表工厂默认口径开箱可用";
    EXPECT_TRUE(backingBit(*session, 0));
}

TEST(MjPassIllegalVarargsRemoverTest, PipelineConfigOptInEnablesFix)
{
    ResourceContainer container;
    const Bytes bytes =
        assembleClass("PI", {{"public static h ([I)V", "      return\n"},
                             {"public static f (I)V", "      return\n"}});
    const Bytes bytes2 = withMethodAccessBits(bytes, 1, kAccVarargs);
    addClassResource(container, "PI.class", bytes2);

    const auto loaded = mjbase::spi::loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"IllegalVarargsRemover"}]})json",
        mjbase::spi::defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    EXPECT_EQ(loaded.unwrap().sequence.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(loaded.unwrap().sequence[0].scope, mjbase::spi::PassScope::Class);

    mjbase::spi::PipelineBuilder builder;
    ASSERT_TRUE(mjbase::spi::applyConfigTo(mjbase::spi::defaultPassRegistry(),
                                           loaded.unwrap(), builder)
                    .is_ok());
    mjbase::spi::PassJobContext job;
    auto                        mgr = builder.buildNestedPipeline(job);
    const auto                  ran = mjbase::spi::runContainerPipeline(container, mgr, job);
    ASSERT_TRUE(ran.is_ok()) << ran.unwrap_err().detail;

    const Bytes after = containerBytes(container, "PI.class");
    const auto  read  = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(after));
    ASSERT_TRUE(read.is_ok());
    auto session = std::move(read).unwrap();
    EXPECT_TRUE((session->backing().methods[0].access & kAccVarargs) != 0)
        << "h 阵列尾参缺位恢复";
    EXPECT_EQ(session->backing().methods[1].access & kAccVarargs, 0)
        << "f 非法位摘除";
}

TEST(MjPassIllegalVarargsRemoverTest, DefaultChainLeavesBitsUntouched)
{
    (void)mjbase::spi::registerBuiltinPipelines();
    const auto& pipelines = mjbase::spi::PassRegistry::instance();
    ASSERT_TRUE(pipelines.hasPipeline("default-optimize"));
    const auto pipeline = pipelines.getPipeline("default-optimize");
    for (const std::string& name : pipeline.classPassNames()) {
        EXPECT_NE(name, "IllegalVarargsRemover") << "opt-in 不入默认链";
    }
    for (const std::string& name : pipeline.methodPassNames()) {
        EXPECT_NE(name, "IllegalVarargsRemover") << "opt-in 不入默认链";
    }

    ResourceContainer container;
    const Bytes bytes =
        assembleClass("PD", {{"public static h ([I)V", "      return\n"}});
    addClassResource(container, "PD.class", bytes);

    mjbase::spi::PassJobContext job;
    auto                        mgr = pipeline.buildNestedPipeline(job);
    const auto                  ran = mjbase::spi::runContainerPipeline(container, mgr, job);
    ASSERT_TRUE(ran.is_ok()) << ran.unwrap_err().detail;

    const Bytes after = containerBytes(container, "PD.class");
    const auto  read  = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(after));
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(std::move(read).unwrap()->backing().methods[0].access & kAccVarargs, 0)
        << "默认链不收：缺位原样";
}
