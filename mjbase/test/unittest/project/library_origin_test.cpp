/// @file        library_origin_test.cpp
/// @brief       已知库识别第一层：包前缀分类——段边界正反例、两类代表例、
///              Unknown 兜底、大小写敏感口径、规则版本与表条目契约。
#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

#include "libca/str/utf8_string.hpp"

#include "mjbase/project/library_origin.hpp"

using mjbase::project::classifyLibraryOrigin;
using mjbase::project::kLibraryOriginRuleVersion;
using mjbase::project::kOfficialPackageRoots;
using mjbase::project::kThirdPartyLibraryRoots;
using mjbase::project::libraryOriginName;
using mjbase::project::LibraryOrigin;

namespace {

/// @brief C 字符串便捷分类入口（Utf8StringRef 接口的测试侧包装）。
LibraryOrigin classify(const char* internalName)
{
    return classifyLibraryOrigin(ca::str::Utf8StringRef::from_cstr(internalName));
}

}   // namespace

TEST(MjLibraryOriginTest, OfficialPrefixRepresentatives)
{
    // 官方表每条目至少一个代表例（十根逐一命中 Jdk）。
    EXPECT_EQ(LibraryOrigin::Jdk, classify("java/lang/String"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("javax/crypto/Cipher"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("javax/net/ssl/SSLSocket"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("jdk/internal/misc/Unsafe"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("sun/misc/Unsafe"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("com/sun/jndi/ldap/LdapCtx"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("org/omg/CORBA/Any"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("org/ietf/jgss/GSSManager"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("org/w3c/dom/Document"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("org/xml/sax/ContentHandler"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("org/jcp/xml/dsig/internal/dom/DOMSignContext"));
}

TEST(MjLibraryOriginTest, ThirdPartyPrefixRepresentatives)
{
    // 第三方表每条目至少一个代表例（十六根逐一命中 Known）。
    EXPECT_EQ(LibraryOrigin::Known, classify("com/google/common/collect/ImmutableList"));
    EXPECT_EQ(LibraryOrigin::Known, classify("com/google/gson/Gson"));
    EXPECT_EQ(LibraryOrigin::Known, classify("com/google/protobuf/CodedInputStream"));
    EXPECT_EQ(LibraryOrigin::Known, classify("com/fasterxml/jackson/databind/ObjectMapper"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/apache/commons/lang3/StringUtils"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/json/JSONObject"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/junit/jupiter/api/Test"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/hamcrest/CoreMatchers"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/slf4j/LoggerFactory"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/springframework/context/ApplicationContext"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/objectweb/asm/ClassVisitor"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/jetbrains/annotations/NotNull"));
    EXPECT_EQ(LibraryOrigin::Known, classify("io/netty/bootstrap/Bootstrap"));
    EXPECT_EQ(LibraryOrigin::Known, classify("kotlin/jvm/internal/Intrinsics"));
    EXPECT_EQ(LibraryOrigin::Known, classify("kotlinx/coroutines/Job"));
    EXPECT_EQ(LibraryOrigin::Known, classify("scala/collection/immutable/List"));
}

TEST(MjLibraryOriginTest, SegmentBoundaryPositive)
{
    // 命中是前缀段对齐：深层嵌套照样命中，段数不影响。
    EXPECT_EQ(LibraryOrigin::Jdk, classify("java/lang/invoke/MethodHandles/Lookup"));
    EXPECT_EQ(LibraryOrigin::Known, classify("kotlin/collections/CollectionsKt"));
    // kotlinx/ 与 kotlin/ 是两个独立根，各自命中各自的。
    EXPECT_EQ(LibraryOrigin::Known, classify("kotlinx/serialization/json/Json"));
}

TEST(MjLibraryOriginTest, SegmentBoundaryNegative)
{
    // 首段超长：javaxx/Y 不许命中 java/（'/' 锚在段边界上失配）。
    EXPECT_EQ(LibraryOrigin::Unknown, classify("javaxx/Y"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("javar/Foo"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("jav/X"));
    // 前缀串误吸：命中锚在首字节，xjava/ 不是 java/。
    EXPECT_EQ(LibraryOrigin::Unknown, classify("xjava/Foo"));
    // 表内更深的根同理：gsonX、asmX、kotlinxx 不是 gson/、asm/、kotlinx/。
    EXPECT_EQ(LibraryOrigin::Unknown, classify("com/google/gsonX/Foo"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("com/google/gsons/Foo"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("org/objectweb/asmX/Foo"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("kotlinxx/Foo"));
    // 只列了 google 的三个子根，google 根本身与未列子根不吸。
    EXPECT_EQ(LibraryOrigin::Unknown, classify("com/google/Foo"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("com/goog/Foo"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("org/apacheX/Foo"));
    // 对照：根命中后根以下的段不受边界规则约束（gson 子包任意深层都命中）。
    EXPECT_EQ(LibraryOrigin::Known, classify("com/google/gson/extraX/Foo"));
    EXPECT_EQ(LibraryOrigin::Known, classify("com/google/gson/internal/bind/TypeAdapters"));
}

TEST(MjLibraryOriginTest, CaseSensitiveMatchingIsHardPinned)
{
    // 大小写敏感写死：表侧编译期锁全小写，匹配按字节比较不折叠。
    // 前缀覆盖段内大小写不符即不命中。
    EXPECT_EQ(LibraryOrigin::Unknown, classify("Java/lang/String"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("JAVA/lang/String"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("Com/Google/Gson/Foo"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("KOTLIN/Foo"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("ORG/JSON/Foo"));
    // 对照：根命中只看被覆盖的前缀字节——首段字节面严格相等时，
    // 根以下段的任何大小写都不影响判定（java/Lang 仍命中 java/）。
    EXPECT_EQ(LibraryOrigin::Jdk, classify("java/Lang/String"));
    EXPECT_EQ(LibraryOrigin::Known, classify("org/junit/Jupiter/api/Test"));
}

TEST(MjLibraryOriginTest, UnknownFallback)
{
    // 兜底语义：无前缀证据一律 Unknown，包括业务包、默认包类、空名。
    EXPECT_EQ(LibraryOrigin::Unknown, classify("com/example/App"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("Main"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("Foo/Bar"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify(""));
    // 无 '/' 的裸名是默认包类，不是官方包根本身。
    EXPECT_EQ(LibraryOrigin::Unknown, classify("java"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("javax"));
    EXPECT_EQ(LibraryOrigin::Unknown, classify("kotlin"));
    // 字节透明：多字节 UTF-8 段（0xC3 0xA9 = 'é'）按字节处理，
    // 不干扰前缀对齐也不崩。
    EXPECT_EQ(LibraryOrigin::Unknown, classify("com/example/Caf\xC3\xA9"));
    EXPECT_EQ(LibraryOrigin::Jdk, classify("java/lang/Caf\xC3\xA9"));
}

TEST(MjLibraryOriginTest, RuleVersionConstantPresent)
{
    // 规则版本常量存在且 >= 1（表内容变化递增，census 导出版本契约用）。
    EXPECT_GE(kLibraryOriginRuleVersion, static_cast<ca::u32>(1));
}

TEST(MjLibraryOriginTest, TableContractCounts)
{
    // 表条目数契约：任何增删必须同步本断言 + kLibraryOriginRuleVersion 递增。
    constexpr std::size_t kOfficialCount =
        sizeof(kOfficialPackageRoots) / sizeof(kOfficialPackageRoots[0]);
    constexpr std::size_t kThirdPartyCount =
        sizeof(kThirdPartyLibraryRoots) / sizeof(kThirdPartyLibraryRoots[0]);
    static_assert(kOfficialCount == 10, "official roots changed: bump kLibraryOriginRuleVersion");
    static_assert(kThirdPartyCount == 16, "third-party roots changed: bump kLibraryOriginRuleVersion");
    EXPECT_EQ(std::size_t{10}, kOfficialCount);
    EXPECT_EQ(std::size_t{16}, kThirdPartyCount);
    // 运行时对表侧不变量的二次确认（编译期 static_assert 的镜像检查）。
    for (const std::string_view root : kOfficialPackageRoots) {
        EXPECT_EQ('/', root.back()) << root;
    }
    for (const std::string_view root : kThirdPartyLibraryRoots) {
        EXPECT_EQ('/', root.back()) << root;
    }
}

TEST(MjLibraryOriginTest, OriginNamesAreStableTokens)
{
    // census origin 列值：蛇形小写单 token，与枚举一一对应。
    EXPECT_EQ("jdk", libraryOriginName(LibraryOrigin::Jdk));
    EXPECT_EQ("known", libraryOriginName(LibraryOrigin::Known));
    EXPECT_EQ("unknown", libraryOriginName(LibraryOrigin::Unknown));
}
