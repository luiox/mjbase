/// IndySiteInliner 用例（jasm 夹具面）：测试内实现假 resolver 走 SPI 全流程
/// （resolve → inline 前后 jasm 对照），拒绝面逐项钉死（无目标逐字节保留、
/// lambda/concat 生成面不动、调用形态不匹配拒、尾参越界拒、返回不等形拒、
/// 槽号越界拒），洗牌面与幂等二跑另列。JVM 真跑门在
/// indy_site_inliner_jvm_test。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/str/utf16_string.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/indy_site_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/indy_test_resolver.hpp"

namespace {


using Bytes = std::vector<ca::u8>;

using mjbase::pass::IndySiteInliner;
using mjbase::spi::GlobalAnalysisManager;
using mjbase::spi::PreservedAnalyses;
using mjbase::spi::ResolvedIndyTarget;

using TestResolver = TableIndyResolver;

/// jasm 源面 UTF-16 载体（jasm_service_test 同款局部助手）。
ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 源装配成类字节（major 61；装配失败显红返回 nullopt）。
std::optional<Bytes> assemble(const std::string& jasm)
{
    mjbase::jasm::JvmJasmService service;
    auto                         name   = utf16("fixture.jasm");
    auto                         source = utf16(jasm);
    auto                         built  = service.assemble(name.ref(), source.ref());
    if (!built.isOk()) {
        ADD_FAILURE() << "jasm assemble failed with " << built.diagnostics.size()
                      << " diagnostics";
        for (const auto& diagnostic : built.diagnostics) {
            std::string where;
            if (diagnostic.location.has_value() && diagnostic.location->line > 0)
                where = " (line " + std::to_string(diagnostic.location->line) + ")";
            ADD_FAILURE() << "  diagnostic: "
                          << diagnostic.message.to_utf8_string().to_std_string() << where;
        }
        return std::nullopt;
    }
    return built.value->bytes;
}

/// 类字节反汇编为规范化 jasm 文本（UTF-16，contains 断言用）。
std::optional<ca::str::Utf16String> jasmOf(const Bytes& bytes)
{
    mjbase::jasm::JvmJasmService service;
    auto                         printed = service.disassemble(bytes);
    if (!printed.isOk()) {
        ADD_FAILURE() << "jasm disassemble failed";
        return std::nullopt;
    }
    return std::move(printed.value->source);
}

/// 容器 + 单类资源装配。
mjbase::model::ResourceContainer containerOf(const std::string& uri, const Bytes& bytes)
{
    mjbase::model::ResourceContainer container;
    container.addResource(uri, std::make_unique<mjbase::model::ClassResource>(uri, bytes));
    return container;
}

/// 单类单 resolver 跑 pass，返回（PreservedAnalyses，产物字节视图）。
std::pair<PreservedAnalyses, const Bytes*>
    runInliner(mjbase::model::ResourceContainer& container, const std::string& uri,
               std::vector<TestResolver::Spec> specs)
{
    std::vector<std::unique_ptr<mjbase::spi::IndyResolver>> resolvers;
    resolvers.push_back(std::make_unique<TestResolver>(std::move(specs)));
    IndySiteInliner          pass(std::move(resolvers));
    ca::str::Utf8StringArena arena;
    GlobalAnalysisManager    gam(arena);
    const auto               pa = pass.run(container, gam);
    const auto*              res = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource(uri));
    if (res == nullptr) ADD_FAILURE() << "class resource missing: " << uri;
    return {pa, res == nullptr ? nullptr : &res->get()};
}

/// 调用站点三段（值语义：memberOf 视图指向会话，出函数即悬垂，落地拷贝）。
struct CallSite3
{
    std::string owner;
    std::string name;
    std::string desc;
};

/// 结构检查：方法体里指定 opcode 的首个调用站点三段（未命中返回 nullopt）。
std::optional<CallSite3> callInsnOf(const Bytes& bytes, std::string_view methodName,
                                    jbc::core::Op op)
{
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (loaded.is_err()) return std::nullopt;
    auto&               session = *loaded.unwrap();
    jbc::ir::ClassUnit& unit    = session.unit();
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (unit.symbols().str(unit.methods[i].name) != methodName) continue;
        if (!jbc::ir::hasCode(session, i)) continue;
        auto promoted = jbc::ir::promoteMethod(session, i);
        if (promoted.is_err()) continue;
        const jbc::ir::CodeUnit&            cu    = *promoted.unwrap().get();
        const std::vector<jbc::ir::InsnId>& order = cu.dom.order();
        for (const jbc::ir::InsnId id : order) {
            const jbc::ir::Insn* in = cu.dom.peek(id);
            if (in == nullptr || in->op != op) continue;
            jbc::ir::ClassSession::MemberSem sem;
            if (!session.memberOf(*in, sem)) continue;
            CallSite3 site;
            site.owner = std::string(sem.owner);
            site.name  = std::string(sem.name);
            site.desc  = std::string(sem.desc);
            return site;
        }
    }
    return std::nullopt;
}

/// Code 属性 maxLocals 读取（首个带 code 的指定方法；读取失败返回 0）。
ca::u16 maxLocalsOf(const Bytes& bytes, std::string_view methodName)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return 0;
    auto  parsed = std::move(read).unwrap();
    auto& file   = *parsed.file;
    for (auto& method : file.methods) {
        if (file.utf8(method.nameCp) != methodName) continue;
        auto code = file.decodeCode(method);
        if (code.is_err()) return 0;
        return code.unwrap()->maxLocals;
    }
    return 0;
}

// ── 夹具（jasm 源；BSM 桩体 aconst_null/areturn——家族判定只读 CP 句柄，
//    单测不执行 BSM；JVM 真跑门另用真 BSM 夹具）──

/// BSM 桩体模板（Lookup 名字空间内联声明在 INVOKEDYNAMIC 句柄段即可）。
constexpr std::string_view kStubBsm =
    "    .method public static bsm (Ljava/lang/invoke/MethodHandles$Lookup;"
    "Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; {\n"
    "      code: {\n"
    "        aconst_null\n"
    "        areturn\n"
    "      }\n"
    "    }\n";

constexpr std::string_view kBsmHandleRef =
    "invokestatic, t/T1.bsm, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;";

/// 静态 exact 正例 + 同类 unresolvable 对照站点（miss 无 resolver 应答）。
std::string staticFixture()
{
    return std::string(".super java/lang/Object\n"
                       ".class public super t/T1 {\n")
           + std::string(kStubBsm)
           + "    .method public static real (I)I {\n"
             "      code: {\n"
             "        bipush 7\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "    .method public static run (I)I {\n"
             "      code: {\n"
             "        iload_0\n"
             "        invokedynamic enc (I)I { "
           + std::string(kBsmHandleRef)
           + " } { }\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "    .method public static miss (I)I {\n"
             "      code: {\n"
             "        iload_0\n"
             "        invokedynamic miss (I)I { "
           + std::string(kBsmHandleRef)
           + " } { }\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "}\n";
}

/// 虚调用 exact 正例：站点 (Lt/Vt;)I，目标 Vt.mul(I)I。
std::string virtualFixture()
{
    return std::string(".super java/lang/Object\n"
                       ".class public super t/Vt {\n")
           + std::string(kStubBsm)
           + "    .method public mul (I)I {\n"
             "      code: {\n"
             "        iload_1\n"
             "        iload_1\n"
             "        imul\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "    .method public static run (Lt/Vt;I)I {\n"
             "      code: {\n"
             "        aload_0\n"
             "        iload_1\n"
             "        invokedynamic vmul (Lt/Vt;I)I { "
           + std::string(kBsmHandleRef)
           + " } { }\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "}\n";
}

/// 接口形态正例：站点 (Lt/I3;)I，目标按 Interface 形交付（单测只验结构——
/// emit InterfaceMethodref；JVM 链接门不在本测）。
std::string interfaceFixture()
{
    return std::string(".super java/lang/Object\n"
                       ".class public super t/I3 {\n")
           + std::string(kStubBsm)
           + "    .method public add (I)I {\n"
             "      code: {\n"
             "        bipush 1\n"
             "        iadd\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "    .method public static run (Lt/I3;I)I {\n"
             "      code: {\n"
             "        aload_0\n"
             "        iload_1\n"
             "        invokedynamic iadd (Lt/I3;I)I { "
           + std::string(kBsmHandleRef)
           + " } { }\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "}\n";
}

/// 尾部元数据洗牌正例：站点 (I Ljava/lang/String;)I（尾参 = BSM 元数据），
/// 目标 real2(I)I，resolver 应答 trailing = 1。
std::string shuffleFixture()
{
    return std::string(".super java/lang/Object\n"
                       ".class public super t/T4 {\n")
           + std::string(kStubBsm)
           + "    .method public static real2 (I)I {\n"
             "      code: {\n"
             "        iload_0\n"
             "        bipush 3\n"
             "        imul\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "    .method public static run (ILjava/lang/String;)I {\n"
             "      code: {\n"
             "        iload_0\n"
             "        ldc \"meta\"\n"
             "        invokedynamic dec (ILjava/lang/String;)I { "
           + std::string(kBsmHandleRef)
           + " } { }\n"
             "        ireturn\n"
             "      }\n"
             "    }\n"
             "}\n";
}

/// lambda 生成面：BSM 句柄指 LambdaMetafactory（家族判定面）。
std::string lambdaFixture()
{
    return std::string(".super java/lang/Object\n"
                       ".class public super t/TL {\n"
                       "    .method public static run (I)Ljava/util/function/IntUnaryOperator; {\n"
                       "      code: {\n"
                       "        iload_0\n"
                       "        invokedynamic apply (I)Ljava/util/function/IntUnaryOperator;"
                       " { invokestatic, java/lang/invoke/LambdaMetafactory.metafactory,"
                       " (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                       "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;"
                       "Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)"
                       "Ljava/lang/invoke/CallSite; } { }\n"
                       "        areturn\n"
                       "      }\n"
                       "    }\n"
                       "}\n");
}

/// 字符串拼接生成面：BSM 句柄指 StringConcatFactory（家族判定面）。
std::string concatFixture()
{
    return std::string(".super java/lang/Object\n"
                       ".class public super t/TC {\n"
                       "    .method public static run (I)Ljava/lang/String; {\n"
                       "      code: {\n"
                       "        iload_0\n"
                       "        invokedynamic concat (I)Ljava/lang/String;"
                       " { invokestatic, java/lang/invoke/StringConcatFactory.makeConcatWithConstants,"
                       " (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                       "Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)"
                       "Ljava/lang/invoke/CallSite; } { \"v=\\u0001\" }\n"
                       "        areturn\n"
                       "      }\n"
                       "    }\n"
                       "}\n");
}

}   // namespace

TEST(IndySiteInlinerTest, InlinesStaticSiteAndRewritesJasm)
{
    auto before = assemble(staticFixture());
    ASSERT_TRUE(before.has_value());
    auto beforeJasm = jasmOf(*before);
    ASSERT_TRUE(beforeJasm.has_value());
    EXPECT_TRUE(beforeJasm->contains(utf16("invokedynamic enc").ref()));

    auto container = containerOf("t/T1.class", *before);
    auto [pa, out] = runInliner(container, "t/T1.class",
                                {{std::string_view("enc"), ResolvedIndyTarget::Form::Static,
                                  std::string_view("t/T1"), std::string_view("real"),
                                  std::string_view("(I)I"), 0, false}});
    ASSERT_NE(out, nullptr);
    EXPECT_FALSE(pa.areAllPreserved()) << "应有改写面";
    EXPECT_NE(*out, *before);

    auto afterJasm = jasmOf(*out);
    ASSERT_TRUE(afterJasm.has_value());
    EXPECT_FALSE(afterJasm->contains(utf16("invokedynamic enc").ref()));
    EXPECT_TRUE(afterJasm->contains(utf16("invokestatic").ref()));
    const auto site = callInsnOf(*out, "run", jbc::core::Op::INVOKESTATIC);
    ASSERT_TRUE(site.has_value());
    EXPECT_EQ(site->owner, "t/T1");
    EXPECT_EQ(site->name, "real");
    EXPECT_EQ(site->desc, "(I)I");

    // 同类 miss 站点（无 resolver 应答）保留原站。
    EXPECT_TRUE(afterJasm->contains(utf16("invokedynamic miss").ref()));
}

TEST(IndySiteInlinerTest, NoResolverLeavesBytesByteIdentical)
{
    auto before = assemble(staticFixture());
    ASSERT_TRUE(before.has_value());
    auto container = containerOf("t/T1.class", *before);
    auto [pa, out] = runInliner(container, "t/T1.class", {});
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(*out, *before);
}

TEST(IndySiteInlinerTest, LambdaAndConcatSitesUntouchedEvenWhenResolverAnswers)
{
    // resolver 对两站都有应答：排除发生在采集面（生成面语义），不依赖 resolver 缺席。
    struct GenerationCase
    {
        const char*      name;
        std::string      (*fixture)();
        std::string_view site;
    };
    const GenerationCase cases[] = {
        {"t/TL.class", &lambdaFixture, std::string_view("apply")},
        {"t/TC.class", &concatFixture, std::string_view("concat")},
    };
    for (const GenerationCase& c : cases) {
        SCOPED_TRACE(c.name);
        auto before = assemble(c.fixture());
        ASSERT_TRUE(before.has_value());
        auto container = containerOf(c.name, *before);
        auto [pa, out] = runInliner(container, c.name,
                                    {{c.site, ResolvedIndyTarget::Form::Static,
                                      std::string_view("t/X"), std::string_view("y"),
                                      std::string_view("(I)I"), 0, false}});
        ASSERT_NE(out, nullptr);
        EXPECT_TRUE(pa.areAllPreserved()) << c.name << " 生成面站点被改写";
        EXPECT_EQ(*out, *before) << c.name << " 字节被触碰";
    }
}

TEST(IndySiteInlinerTest, FormMismatchAndGateRejectionsKeepBytesByteIdentical)
{
    auto before = assemble(staticFixture());
    ASSERT_TRUE(before.has_value());
    const TestResolver::Spec base = {std::string_view("enc"), ResolvedIndyTarget::Form::Static,
                                     std::string_view("t/T1"), std::string_view("real"),
                                     std::string_view("(I)I"), 0, false};
    // 形态不匹配：站点 (I)I 无接收者，Virtual 形要求首参 = owner 类形。
    {
        TestResolver::Spec spec  = base;
        spec.form                = ResolvedIndyTarget::Form::Virtual;
        auto container           = containerOf("t/T1.class", *before);
        auto [pa, out]           = runInliner(container, "t/T1.class", {spec});
        ASSERT_NE(out, nullptr);
        EXPECT_TRUE(pa.areAllPreserved());
        EXPECT_EQ(*out, *before);
    }
    // 尾部元数据数越界：trailing > |Ps|。
    {
        TestResolver::Spec spec  = base;
        spec.trailing            = 5;
        auto container           = containerOf("t/T1.class", *before);
        auto [pa, out]           = runInliner(container, "t/T1.class", {spec});
        ASSERT_NE(out, nullptr);
        EXPECT_TRUE(pa.areAllPreserved());
        EXPECT_EQ(*out, *before);
    }
    // 返回段不等形：目标 (I)V vs 站点 (I)I。
    {
        TestResolver::Spec spec  = base;
        spec.desc                = std::string_view("(I)V");
        auto container           = containerOf("t/T1.class", *before);
        auto [pa, out]           = runInliner(container, "t/T1.class", {spec});
        ASSERT_NE(out, nullptr);
        EXPECT_TRUE(pa.areAllPreserved());
        EXPECT_EQ(*out, *before);
    }
    // 参数不等形：目标 (J)I vs 站点 (I)I。
    {
        TestResolver::Spec spec  = base;
        spec.desc                = std::string_view("(J)I");
        auto container           = containerOf("t/T1.class", *before);
        auto [pa, out]           = runInliner(container, "t/T1.class", {spec});
        ASSERT_NE(out, nullptr);
        EXPECT_TRUE(pa.areAllPreserved());
        EXPECT_EQ(*out, *before);
    }
    // ResolveFailed：站点保守保留。
    {
        TestResolver::Spec spec  = base;
        spec.fail                = true;
        auto container           = containerOf("t/T1.class", *before);
        auto [pa, out]           = runInliner(container, "t/T1.class", {spec});
        ASSERT_NE(out, nullptr);
        EXPECT_TRUE(pa.areAllPreserved());
        EXPECT_EQ(*out, *before);
    }
}

TEST(IndySiteInlinerTest, InlinesVirtualAndInterfaceForms)
{
    // Virtual：站点首参接收者 = owner，emit invokevirtual。
    {
        auto before = assemble(virtualFixture());
        ASSERT_TRUE(before.has_value());
        auto container = containerOf("t/Vt.class", *before);
        auto [pa, out] = runInliner(container, "t/Vt.class",
                                    {{std::string_view("vmul"), ResolvedIndyTarget::Form::Virtual,
                                      std::string_view("t/Vt"), std::string_view("mul"),
                                      std::string_view("(I)I"), 0, false}});
        ASSERT_NE(out, nullptr);
        EXPECT_FALSE(pa.areAllPreserved());
        const auto site = callInsnOf(*out, "run", jbc::core::Op::INVOKEVIRTUAL);
        ASSERT_TRUE(site.has_value());
        EXPECT_EQ(site->owner, "t/Vt");
        EXPECT_EQ(site->name, "mul");
    }
    // Interface：emit invokeinterface + InterfaceMethodref。
    {
        auto before = assemble(interfaceFixture());
        ASSERT_TRUE(before.has_value());
        auto container = containerOf("t/I3.class", *before);
        auto [pa, out] = runInliner(container, "t/I3.class",
                                    {{std::string_view("iadd"), ResolvedIndyTarget::Form::Interface,
                                      std::string_view("t/I3"), std::string_view("add"),
                                      std::string_view("(I)I"), 0, false}});
        ASSERT_NE(out, nullptr);
        EXPECT_FALSE(pa.areAllPreserved());
        const auto site = callInsnOf(*out, "run", jbc::core::Op::INVOKEINTERFACE);
        ASSERT_TRUE(site.has_value());
        EXPECT_EQ(site->owner, "t/I3");
        EXPECT_EQ(site->name, "add");
    }
    // 接收者形不符：Virtual 目标 owner 与站点首参不同类 → 拒。
    {
        auto before = assemble(virtualFixture());
        ASSERT_TRUE(before.has_value());
        auto container = containerOf("t/Vt.class", *before);
        auto [pa, out] = runInliner(container, "t/Vt.class",
                                    {{std::string_view("vmul"), ResolvedIndyTarget::Form::Virtual,
                                      std::string_view("t/Other"), std::string_view("mul"),
                                      std::string_view("(I)I"), 0, false}});
        ASSERT_NE(out, nullptr);
        EXPECT_TRUE(pa.areAllPreserved());
        EXPECT_EQ(*out, *before);
    }
}

TEST(IndySiteInlinerTest, InlinesTrailingMetadataWithArgShuffle)
{
    auto before = assemble(shuffleFixture());
    ASSERT_TRUE(before.has_value());
    auto container = containerOf("t/T4.class", *before);
    auto [pa, out] = runInliner(container, "t/T4.class",
                                {{std::string_view("dec"), ResolvedIndyTarget::Form::Static,
                                  std::string_view("t/T4"), std::string_view("real2"),
                                  std::string_view("(I)I"), 1, false}});
    ASSERT_NE(out, nullptr);
    EXPECT_FALSE(pa.areAllPreserved());
    auto afterJasm = jasmOf(*out);
    ASSERT_TRUE(afterJasm.has_value());
    EXPECT_FALSE(afterJasm->contains(utf16("invokedynamic dec").ref()));
    EXPECT_TRUE(afterJasm->contains(utf16("invokestatic").ref()));
    // 洗牌面：元数据实参存 fresh 局部（istore/astore）后按 represented 装载（iload）。
    EXPECT_TRUE(afterJasm->contains(utf16("istore").ref()));
    EXPECT_TRUE(afterJasm->contains(utf16("astore").ref()));
    EXPECT_TRUE(afterJasm->contains(utf16("iload").ref()));
    const auto site = callInsnOf(*out, "run", jbc::core::Op::INVOKESTATIC);
    ASSERT_TRUE(site.has_value());
    EXPECT_EQ(site->owner, "t/T4");
    EXPECT_EQ(site->name, "real2");
    EXPECT_EQ(site->desc, "(I)I");
    // maxLocals 抬升：洗牌前 run 只占参数槽（2），fresh 局部至少再开 2 槽。
    EXPECT_GE(maxLocalsOf(*out, "run"), 4);
    EXPECT_EQ(maxLocalsOf(*before, "run"), 2);
}

TEST(IndySiteInlinerTest, SecondRunIsStrictNoOp)
{
    auto before = assemble(staticFixture());
    ASSERT_TRUE(before.has_value());
    auto          container = containerOf("t/T1.class", *before);
    const auto    specs     = std::vector<TestResolver::Spec>{
        {std::string_view("enc"), ResolvedIndyTarget::Form::Static, std::string_view("t/T1"),
         std::string_view("real"), std::string_view("(I)I"), 0, false}};
    auto [pa1, out1] = runInliner(container, "t/T1.class", specs);
    ASSERT_NE(out1, nullptr);
    EXPECT_FALSE(pa1.areAllPreserved());
    const Bytes settled = *out1;

    auto [pa2, out2] = runInliner(container, "t/T1.class", specs);
    ASSERT_NE(out2, nullptr);
    EXPECT_TRUE(pa2.areAllPreserved()) << "二次运行应严格 no-op";
    EXPECT_EQ(*out2, settled);
}
