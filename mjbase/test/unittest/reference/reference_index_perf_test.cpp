/// @file        reference_index_perf_test.cpp
/// @brief       ReferenceIndex 全容器 build 性能软门：运行时生成 9600 类
///              扇入语料（jasm 逐类装配：8700 个调用类 × 各 10 个调用点，
///              共享 900 深的容器内链式 callee 尾 + static Class 字段 +
///              getDeclaredMethod 反射查找面），量全容器 build（含共享
///              别名分析全程序固定点）的耗时上界。上界取宽松值只防数量级
///              劣化（CI 抖动防护），真实数字落 PR 正文。
///
/// 机理：容器内「多调用点共享深 callee 子树」会让别名展开走查在类间重复
/// 遍历同一下游链——修复前逐调用点现算展开流，本语料随调用点数 × 链深
/// 二次方增长（实测 >60s）；修复后展开流按 (槽位, 只读快照) 缓存复用，
/// 恢复线性（实测低秒档）。链深 900 有界：展开走查为 DFS 递归，深度随
/// 链深增长，超深单链（~3000+）触发 1MB 测试栈溢出是该走查的既有边界
///（与本批无关），语料深度取安全档。
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/reference_index.hpp"

namespace {

using Bytes = std::vector<ca::u8>;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;

/// 单次测量的墙钟毫秒。
template <typename Fn>
double measureOnce(Fn&& fn)
{
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

/// 软门断言：上限按数量级取宽松值（防劣化，不防抖动），实测值打进输出。
void expectWithinSoftBound(const char* tag, double actualMs, double boundMs)
{
    std::printf("perf %s: %.1f ms (soft bound %.0f ms)\n", tag, actualMs, boundMs);
    std::cout.flush();
    EXPECT_LT(actualMs, boundMs) << tag << " regressed beyond the soft bound";
}

/// 共享 callee 链深（展开 DFS 递归深度的安全档）与调用类数（每类 10 个
/// 调用点绑同一链头）：修复前展开步进 = 8700×10×900/轮，二次方档；
/// 修复后链只走查一次，调用点全部缓存切片命中。
constexpr ca::usize kChainDepth  = 900;
constexpr ca::usize kCallerCount = 8700;
constexpr ca::usize kSitesPerCaller = 10;

/// 单类源码。链类：static Class 字段 + 链式 b（invokestatic 下一类）+
/// getDeclaredMethod 反射查找 r；调用类：static Class 字段 + b（10 个
/// invokestatic 链头调用点）+ 同款 r。
std::string classSource(bool isChain, const std::string& name, const std::string& next)
{
    std::string body;
    body += "  .field public static cache Ljava/lang/Class;\n";
    body += "  .method public static b ()V {\n";
    body += "    code: {\n";
    if (isChain) {
        if (!next.empty()) body += "      invokestatic " + next + ".b ()V\n";
    }
    else {
        for (ca::usize i = 0; i < kSitesPerCaller; ++i) {
            body += "      invokestatic perf/Chain00000.b ()V\n";
        }
    }
    body += "      return\n";
    body += "    }\n";
    body += "  }\n";
    body += "  .method public static r ()Ljava/lang/reflect/Method; {\n";
    body += "    code: {\n";
    body += "      ldc Ljava/lang/Class;\n";
    body += "      ldc \"run\"\n";
    body += "      iconst_1\n";
    body += "      anewarray java/lang/Class\n";
    body += "      invokevirtual java/lang/Class.getDeclaredMethod "
            "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;\n";
    body += "      areturn\n";
    body += "    }\n";
    body += "  }\n";
    return ".super java/lang/Object\n.class public super " + name + " {\n" + body + "}\n";
}

Bytes assembleFanInClass(bool isChain, const std::string& name, const std::string& next)
{
    mjbase::jasm::JvmJasmService service;
    mjbase::jasm::JvmJasmContext context;
    context.classMajorVersion = 49;
    const std::string source = classSource(isChain, name, next);
    const auto sourceText    = ca::str::Utf16String::from_utf8_string(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(source.data()),
                                          source.size()));
    const auto sourceName = ca::str::Utf16String::from_utf8_string(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>("fixture.jasm"), 12));
    const auto result = service.assemble(sourceName.ref(), sourceText.ref(), context);
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败: " << name;
        return {};
    }
    return result.value->bytes;
}

void buildFanInContainer(ResourceContainer& container)
{
    for (ca::usize i = 0; i < kChainDepth; ++i) {
        char        buf[32];
        std::snprintf(buf, sizeof(buf), "Chain%05zu", static_cast<std::size_t>(i));
        const std::string name = std::string("perf/") + buf;
        std::string       nextName;
        if (i + 1 < kChainDepth) {
            char nbuf[32];
            std::snprintf(nbuf, sizeof(nbuf), "Chain%05zu", static_cast<std::size_t>(i + 1));
            nextName = std::string("perf/") + nbuf;
        }
        const Bytes bytes = assembleFanInClass(true, name, nextName);
        ASSERT_FALSE(bytes.empty());
        container.addResource(name + ".class",
                              std::make_unique<ClassResource>(name + ".class", bytes));
    }
    for (ca::usize j = 0; j < kCallerCount; ++j) {
        char        buf[32];
        std::snprintf(buf, sizeof(buf), "User%05zu", static_cast<std::size_t>(j));
        const std::string name = std::string("perf/") + buf;
        const Bytes bytes   = assembleFanInClass(false, name, {});
        ASSERT_FALSE(bytes.empty());
        container.addResource(name + ".class",
                              std::make_unique<ClassResource>(name + ".class", bytes));
    }
}

}   // namespace

TEST(MjReferenceIndexPerfTest, fullContainerBuildOnFanInCorpusStaysWithinSoftBound)
{
    ResourceContainer container;
    buildFanInContainer(container);
    ASSERT_EQ(kChainDepth + kCallerCount, container.size());

    const double buildMs = measureOnce([&] {
        mjbase::reference::ReferenceIndex index;
        index.build(container);
        // 语料自检：调用面 + 链面 + 反射面事实齐备（分布不空转）。
        EXPECT_EQ(2 * (kChainDepth + kCallerCount), index.methodDeclarationCount());
        // 方法边 = 调用类 10×8700 链头 invoke + 链内 899 invoke + 反射查找行。
        EXPECT_GE(index.methodReferenceCount(),
                  kCallerCount * kSitesPerCaller + kChainDepth);
        EXPECT_GE(index.typeReferenceCount(), kCallerCount * kSitesPerCaller);
    });
    // 修复后本语料 build 走低秒档；上界只防数量级劣化（展开缓存回退到
    // 逐调用点现算即回到二次方档，本门必炸）。
    expectWithinSoftBound("reference index full build (9600-class fan-in corpus)", buildMs,
                          60000.0);
}
