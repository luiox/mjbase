// SourceNameRestorer 单测：SourceFile 驱动的类名还原映射生成面（真值源 =
// SourceFile；只产映射不改字节）。覆盖：混淆名恢复（java/kt 后缀）、已正名
// 零条目、无属性零产出、形状拒绝面（异形后缀/空白/数字开头标识符/超长）、
// 内部类跳过（具名 InnerClasses 行 + EnclosingMethod 匿名形态）与名字形态
// 不跳过（属性驱动口径）、默认包无前缀、多类独立产出、字节恒等与 PA 纪律、
// 幂等二跑、注册挂名。
//
// 夹具纪律：类骨架一律 jasm 文本装配（JvmJasmService；SourceFile / InnerClasses
// / EnclosingMethod 三面 jasm 方言均有指令（.sourcefile / .inner / .outer-class），
// 不走 CF 级写面），固定片段 R"JASM 定界，变量插入处保留 + 拼接。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "libca/core/tag_cast.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/source_name_restoration.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

namespace {

using Bytes = std::vector<ca::u8>;
using mjbase::mapping::IntermediateMapping;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;

/// UTF-8 文本 → UTF-16（jasm 源入口口径，enum_name_restorer 同式）。
ca::str::Utf16String utf16Of(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 源码 → class 字节（汇编失败逐条报诊断并致命，夹具错误不静默；
/// 失败时附整份源码与行号定位，供 CI 日志直读）。
Bytes assembleJasm(const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    const auto           sourceName = utf16Of("snr_fixture.jasm");
    const auto           sourceText = utf16Of(source);
    const auto           result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            std::string where;
            if (d.location.has_value()) {
                where = " [line " + std::to_string(d.location->line) + " col "
                        + std::to_string(d.location->column) + "]";
            }
            ADD_FAILURE() << "jasm" << where << ": "
                          << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败，源码如下：\n" << source;
        return {};
    }
    return result.value->bytes;
}

/// 最小构造器（jasm 实例类形状：aload_0 转发 super 构造）。
constexpr std::string_view kCtor = R"JASM(  .method public <init> ()V {
    code: {
      aload_0
      invokespecial java/lang/Object.<init> ()V
      return
    }
  }
)JASM";

/// 类骨架：classDirectives 为类级指令文本（.sourcefile / .inner /
/// .outer-class，raw string 定界的固定片段；可为空），members 为成员体。
Bytes buildClass(std::string_view internalName, std::string_view classDirectives,
                 std::string_view members)
{
    const std::string source =
        R"JASM(.super java/lang/Object
.class public super )JASM" + std::string(internalName) + R"JASM( {
)JASM" + std::string(classDirectives) + std::string(members) + R"JASM(}
)JASM";
    return assembleJasm(source);
}

/// 便捷载入：装配并按内部名 + ".class" 入容器。
void loadClass(ResourceContainer& container, const std::string& internalName,
               const std::string& classDirectives)
{
    const std::string uri = internalName + ".class";
    container.addResource(uri,
                          std::make_unique<ClassResource>(uri,
                                                          buildClass(internalName,
                                                                     classDirectives, kCtor)));
}

/// 跑 pass 并返回容器 AM 上的映射（只读面：字节零改动由调用方断言）。
const IntermediateMapping& runPass(ResourceContainer&                   container,
                                   mjbase::spi::GlobalAnalysisManager& gam)
{
    mjbase::pass::SourceNameRestorer                     pass;
    const mjbase::spi::PreservedAnalyses pa = pass.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved());
    return gam.getResult<mjbase::pass::SourceNameRestorationAnalysis>(container);
}

const ClassResource* classResource(ResourceContainer& container, const std::string& uri)
{
    return ca::core::tag_cast::dyn_cast<const ClassResource*>(container.getResource(uri));
}

/// 便捷取用：类名映射新名；未命中返回空串。
std::string mappedClass(const IntermediateMapping& m, std::string_view name)
{
    const auto mapped =
        m.getMappedClassName(ca::str::Utf8StringRef::from_string_view(name));
    return std::string{static_cast<std::string_view>(mapped)};
}

}   // namespace

// ─── 恢复正例：混淆名 + SourceFile 真名 → 类名映射 ─────────────────────
TEST(SourceNameRestorerTest, restoresRenamedClass)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a", R"JASM(  .sourcefile "Foo.java"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_EQ(mappedClass(mapping, "t/a"), "t/Foo");
    EXPECT_EQ(mapping.getClasses().size(), 1u);
    EXPECT_TRUE(mapping.getFields().empty());
    EXPECT_TRUE(mapping.getMethods().empty());
}

// ─── kt 后缀同门 ───────────────────────────────────────────────────────
TEST(SourceNameRestorerTest, ktSuffixAccepted)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a", R"JASM(  .sourcefile "Foo.kt"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_EQ(mappedClass(mapping, "t/a"), "t/Foo");
}

// ─── 已正名零条目（映射面最小化）───────────────────────────────────────
TEST(SourceNameRestorerTest, canonicalNameZeroEntries)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/Foo", R"JASM(  .sourcefile "Foo.java"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 无 SourceFile 属性零产出 ─────────────────────────────────────────
TEST(SourceNameRestorerTest, noSourceFileZeroEntries)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a", "");

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 形状拒绝面：非 .java/.kt 结尾的值不产映射 ─────────────────────────
TEST(SourceNameRestorerTest, malformedSuffixRejected)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a", R"JASM(  .sourcefile "Foo.java.bak"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 形状拒绝面：含空白（word 字符面外）不产映射 ───────────────────────
TEST(SourceNameRestorerTest, whitespaceRejected)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a", R"JASM(  .sourcefile "Foo Bar.java"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 标识符合法门（本件收紧申报）：数字开头源名不产映射 ────────────────
TEST(SourceNameRestorerTest, digitLeadingRejected)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a", R"JASM(  .sourcefile "1Foo.java"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 形状拒绝面：源名超长（>50 word 字符）不产映射 ─────────────────────
TEST(SourceNameRestorerTest, overlengthRejected)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const std::string longName = std::string(51, 'a') + ".java";
    loadClass(container, "t/a", "  .sourcefile \"" + longName + "\"\n");

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 具名内部类跳过：InnerClasses 本类行带 outer（沿用外围类 SourceFile，
//     不跳过会与外围类映射相撞）─────────────────────────────────────────
TEST(SourceNameRestorerTest, namedInnerClassSkipped)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/Foo$Bar",
              R"JASM(  .inner { name: Bar, inner: t/Foo$Bar, outer: t/Foo }
  .sourcefile "Foo.java"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 匿名内部类跳过：EnclosingMethod 形态 ─────────────────────────────
TEST(SourceNameRestorerTest, anonymousEnclosingSkipped)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/Foo$1",
              R"JASM(  .sourcefile "Foo.java"
  .outer-class t/Foo
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 名字形态不跳过：无 InnerClasses/EnclosingMethod 属性的 $ 名类照常
//     恢复（属性驱动口径，不做名字形状猜测）────────────────────────────
TEST(SourceNameRestorerTest, nameShapeAloneNotSkipped)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a$1", R"JASM(  .sourcefile "Foo.java"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_EQ(mappedClass(mapping, "t/a$1"), "t/Foo");
}

// ─── 默认包：无包前缀，还原名裸源名 ───────────────────────────────────
TEST(SourceNameRestorerTest, defaultPackageNoPrefix)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "a", R"JASM(  .sourcefile "Foo.java"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_EQ(mappedClass(mapping, "a"), "Foo");
}

// ─── 多类独立：逐类独立产出互不影响 ───────────────────────────────────
TEST(SourceNameRestorerTest, multiClassIndependent)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a", R"JASM(  .sourcefile "Foo.java"
)JASM");
    loadClass(container, "t/b", R"JASM(  .sourcefile "Bar.kt"
)JASM");
    loadClass(container, "t/Keep", R"JASM(  .sourcefile "Keep.java"
)JASM");

    const auto& mapping = runPass(container, gam);
    EXPECT_EQ(mappedClass(mapping, "t/a"), "t/Foo");
    EXPECT_EQ(mappedClass(mapping, "t/b"), "t/Bar");
    EXPECT_EQ(mappedClass(mapping, "t/Keep"), "");   // 已正名零条目
    EXPECT_EQ(mapping.getClasses().size(), 2u);
}

// ─── 只读件：容器字节恒等 ─────────────────────────────────────────────
TEST(SourceNameRestorerTest, bytesUntouched)
{
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    loadClass(container, "t/a", R"JASM(  .sourcefile "Foo.java"
)JASM");
    const Bytes inBytes = classResource(container, "t/a.class")->get();

    (void)runPass(container, gam);

    EXPECT_EQ(classResource(container, "t/a.class")->get(), inBytes);
}

// ─── 幂等二跑：同 GAM 二跑缓存命中恒等；独立 GAM 三跑逐条相等 ──────────
TEST(SourceNameRestorerTest, idempotentSecondRun)
{
    ResourceContainer container;
    loadClass(container, "t/a", R"JASM(  .sourcefile "Foo.java"
)JASM");

    const auto nameRef = ca::str::Utf8StringRef::from_string_view("t/a");
    ca::str::Utf8StringArena           arena1;
    mjbase::spi::GlobalAnalysisManager gam1(arena1);
    const auto&                        first = runPass(container, gam1);
    const auto* firstCls = first.getClassMapping(nameRef);
    ASSERT_NE(firstCls, nullptr);

    const auto& second = runPass(container, gam1);
    EXPECT_EQ(second.getClassMapping(nameRef), firstCls);   // 缓存命中同址

    ca::str::Utf8StringArena           arena2;
    mjbase::spi::GlobalAnalysisManager gam2(arena2);
    const auto&                        third = runPass(container, gam2);
    const auto* thirdCls = third.getClassMapping(nameRef);
    ASSERT_NE(thirdCls, nullptr);
    EXPECT_EQ(static_cast<std::string_view>(thirdCls->newName),
              static_cast<std::string_view>(firstCls->newName));
    EXPECT_EQ(static_cast<std::string_view>(thirdCls->oldName),
              static_cast<std::string_view>(firstCls->oldName));
}

// ─── 注册挂名面：名字可解析且产出本 pass ──────────────────────────────
TEST(SourceNameRestorerTest, registeredInRegistry)
{
    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass("SourceNameRestorer");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
    const auto e = mjbase::spi::defaultPassRegistry().find("SourceNameRestorer");
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.unwrap().scope, mjbase::spi::PassScope::Global);
}
