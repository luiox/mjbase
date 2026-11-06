#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::project {

/// @file library_origin.hpp
/// @brief 已知库识别第一层：包前缀分类（census origin 列取值面）。
///
/// 输入类内部名（'/' 分段的二进制名，如 com/google/gson/Foo），输出三档
/// Origin：Jdk（官方前缀命中）/ Known（第三方前缀命中）/ Unknown。
/// 识别结果只用于分层、标注、过滤——不改写任何字节，映射文件不收库符号。
///
/// 边界要诚实：前缀可以被混淆器抹掉，但抹 JDK 前缀需要
/// 重链接，风险大收益小，多数混淆器不动它。所以本层只敢说「确定是库」，
/// 不敢说「Unknown 就不是库」——Unknown 是「无前缀证据」，不是「业务代码」。
///
/// 匹配语义 = 前缀段边界匹配：两张表条目全部以 '/' 收尾（编译期校验
/// 锁死），命中判据即字节前缀对齐——'/' 锚在段边界上，首段超长自动失配
/// （javaxx/Y 对 java/ 在第 5 字节 'x' vs '/' 处失配），天然防前缀串误吸；
/// 大小写敏感按字节比较，不做任何大小写折叠、不做编码假设。
///
/// 规则版本契约：前缀表内容（任一条目增删改）变化时递增
/// kLibraryOriginRuleVersion；census 接线时该值进导出版本契约。

/// @brief 类内部名来源三档。
enum class LibraryOrigin : ca::u8
{
    Jdk     = 0, ///< 官方前缀命中（JDK 事实发行的包根）
    Known   = 1, ///< 第三方前缀命中（高频库坐标根）
    Unknown = 2, ///< 无前缀证据（不等于业务代码）
};

/// @brief 前缀规则版本：任一前缀表条目增删改即递增（census 导出版本
///        契约的组成部分；表条目数契约见 library_origin_test.cpp）。
inline constexpr ca::u32 kLibraryOriginRuleVersion = 1;

/// @brief JDK 官方包根：随 JDK 事实发行的核心 API 与历史捆绑扩展/内部
///        实现包根（JVMS 不为 java 平台保留具体前缀，下列集合以「JDK
///        实际携带」为准核对；条目全部以 '/' 收尾锚定段边界）。
inline constexpr std::string_view kOfficialPackageRoots[] = {
    "java/",                      ///< 核心 Java SE API（java.lang 等全部官方包）
    "javax/",                     ///< 官方扩展根（swing/crypto/naming/management 等，含历史捆绑的 JSR 包）
    "jdk/",                       ///< JDK 9+ 模块内部与非导出包（jdk.internal.* 等）
    "sun/",                       ///< JDK 内部实现包（sun.misc 等）
    "com/sun/",                   ///< JDK 内部实现包第二根（com.sun.tools.javac 等；历史上有第三方假借该根，识别口径按官方）
    "org/omg/",                   ///< CORBA / RMI-IIOP（JDK 10 及之前捆绑）
    "org/ietf/jgss/",             ///< GSS-API（java.security.jgss）
    "org/w3c/dom/",               ///< DOM（java.xml）
    "org/xml/sax/",               ///< SAX（java.xml）
    "org/jcp/xml/dsig/internal/", ///< XML DSig 内部实现（java.xml.crypto）
};

/// @brief 高频第三方库坐标根（宁缺勿滥：只收高频且无歧义的根，扩展是
///        数据工作——增删任一条目须同步递增 kLibraryOriginRuleVersion
///        并更新 library_origin_test.cpp 的条目数契约）。
inline constexpr std::string_view kThirdPartyLibraryRoots[] = {
    "com/google/common/",         ///< Guava
    "com/google/gson/",           ///< Gson
    "com/google/protobuf/",       ///< protobuf-java
    "com/fasterxml/jackson/",     ///< Jackson
    "org/apache/",                ///< Apache 系（commons / log4j / httpclient 等，单一根覆盖）
    "org/json/",                  ///< org.json
    "org/junit/",                 ///< JUnit 4 / 5
    "org/hamcrest/",              ///< Hamcrest
    "org/slf4j/",                 ///< SLF4J
    "org/springframework/",       ///< Spring Framework
    "org/objectweb/asm/",         ///< ASM
    "org/jetbrains/annotations/", ///< JetBrains 注解
    "io/netty/",                  ///< Netty
    "kotlin/",                    ///< kotlin-stdlib
    "kotlinx/",                   ///< kotlinx-coroutines / kotlinx-serialization
    "scala/",                     ///< scala-library（scala-compiler / reflect 同根）
};

/// @brief 前缀表条目编译期校验（表侧不变量）：非空、不以前导 '/' 开头、
///        以 '/' 收尾（段边界锚点）、无空段（"//"）、字符集限于小写字母
///        /数字/'/'/'_'（内部名形态；禁大写与 '.'——'.' 是普通名形态，
///        混入即数据错误）。任一不满足返回 false。
template <ca::usize kN>
constexpr bool prefixRootsWellFormed(const std::string_view (&roots)[kN]) noexcept
{
    for (ca::usize i = 0; i < kN; ++i) {
        const std::string_view root = roots[i];
        if (root.empty()) return false;
        for (ca::usize j = 0; j < root.size(); ++j) {
            const char c = root[j];
            if (c == '/') {
                if (j == 0) return false;             ///< 前导 '/'
                if (root[j - 1] == '/') return false; ///< 空段 "//"
            } else {
                const bool lowerOrDigit = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                if (!lowerOrDigit && c != '_') return false;
            }
        }
        if (root.back() != '/') return false;         ///< 未以 '/' 收尾
    }
    return true;
}

static_assert(prefixRootsWellFormed(kOfficialPackageRoots),
              "official package roots must be lowercase slash-terminated segment roots");
static_assert(prefixRootsWellFormed(kThirdPartyLibraryRoots),
              "third-party library roots must be lowercase slash-terminated segment roots");

/// @brief 类内部名包前缀分类。
///
/// 官方表先查、第三方表后查、均未命中落 Unknown（序恒定保证分类确定）。
/// 匹配按字节段（见文件头），输入不做清洗、不折叠大小写。
///
/// @param internalName 类内部名（'/' 分段二进制名；空名合法，落 Unknown）
LibraryOrigin classifyLibraryOrigin(const ca::str::Utf8StringRef& internalName) noexcept;

/// @brief Origin 稳定名（census origin 列值："jdk"/"known"/"unknown"
///        小写单 token，与枚举一一对应，永不返回空）。
std::string_view libraryOriginName(LibraryOrigin origin) noexcept;

}   // namespace mjbase::project
