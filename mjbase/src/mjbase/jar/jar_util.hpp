/// @file        jar_util.hpp
/// @brief       JAR 相关工具方法：条目名后缀判定 + class 字节嗅探。
///
/// 直迁自 jar/jar_util（语义照旧，std::string 形参换成 Utf8StringRef
/// 借用视图；后缀判定仍是大小写敏感的字节级比较）。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::jar {

using ca::str::Utf8String;
using ca::str::Utf8StringRef;

/// JAR 相关工具方法。
class JarUtil
{
public:
    static constexpr ca::u32     kClassMagic       = 0xCAFEBABE;
    static constexpr const char* kManifestFileName = "META-INF/MANIFEST.MF";

    /// 判断文件名是否为 .class（兼容混淆器产出的 .class/ 尾斜杠形式）。
    static bool isClassFile(const Utf8StringRef& fileName)
    {
        // 混淆器（如 OS2.9）可能把类 entry 命名为 *.class/，先按 7 字符尾缀放行。
        if (fileName.byte_length() >= 7 &&
            fileName.ends_with(Utf8StringRef::from_cstr(".class/")))
            return true;
        return fileName.byte_length() >= 6 &&
               fileName.ends_with(Utf8StringRef::from_cstr(".class"));
    }

    /// 判断文件名是否以 .jar/.JAR 结尾。
    /// JMOD 作为 ZIP 容器由上层按场景决定是否接受，不在这里混入通用 jar 语义。
    static bool isJarFile(const Utf8StringRef& fileName)
    {
        return (fileName.byte_length() >= 4 &&
                fileName.ends_with(Utf8StringRef::from_cstr(".jar"))) ||
               (fileName.byte_length() >= 4 &&
                fileName.ends_with(Utf8StringRef::from_cstr(".JAR")));
    }

    /// 判断文件名是否以 .jmod/.JMOD 结尾。
    static bool isJmodFile(const Utf8StringRef& fileName)
    {
        return (fileName.byte_length() >= 5 &&
                fileName.ends_with(Utf8StringRef::from_cstr(".jmod"))) ||
               (fileName.byte_length() >= 5 &&
                fileName.ends_with(Utf8StringRef::from_cstr(".JMOD")));
    }

    /// 通过魔数 + 常量池头部 tag 合法性判断字节数组是不是 class 文件。
    ///
    /// 仅查魔数会被"magic 对但常量池被打乱"的伪造内容骗过（Recaf
    /// 98dd1f528 的同类修复）：额外校验前至多 16 个 CP 条目的 tag 是否落在
    /// JVM 合法集合（1..20，剔除废弃的 2/13/14）。截断或过短的内容走宽松
    /// 语义放行——精确诊断由后续解析层给出，诊断型 cleaner 也依赖
    /// "magic 对即 class"来接管其跳过上报逻辑。
    static bool isClassFile(const std::vector<ca::u8>& bytes)
    {
        if (bytes.size() < 4) return false;
        auto magic = (static_cast<ca::u32>(bytes[0]) << 24) |
                     (static_cast<ca::u32>(bytes[1]) << 16) |
                     (static_cast<ca::u32>(bytes[2]) << 8) | (static_cast<ca::u32>(bytes[3]));
        if (magic != kClassMagic) return false;
        if (bytes.size() < 10) return true;   // 连常量池计数都不完整，宽松放行

        // 头部布局：magic(4) minor(2) major(2) cpCount(2)，条目从偏移 10 起。
        const ca::u32 cpCount =
            (static_cast<ca::u32>(bytes[8]) << 8) | static_cast<ca::u32>(bytes[9]);
        ca::usize off  = 10;
        auto  need = [&bytes, &off](ca::usize n) { return off + n <= bytes.size(); };
        auto  skip = [&off](ca::usize n) { off += n; };
        auto  readU2 = [&bytes, &off]() {
            const ca::u32 v =
                (static_cast<ca::u32>(bytes[off]) << 8) | static_cast<ca::u32>(bytes[off + 1]);
            off += 2;
            return v;
        };

        int checked = 0;
        for (ca::u32 index = 1; index < cpCount && checked < 16; ++index) {
            if (!need(1)) return true;   // 中途截断：宽松放行
            const ca::u32 tag = bytes[off++];
            ++checked;
            switch (tag) {
            case 1:   // Utf8: u2 length + bytes
                if (!need(2)) return true;
                {
                    const ca::u32 len = readU2();
                    if (!need(len)) return true;
                    skip(len);
                }
                break;
            case 3:    // Integer
            case 4:    // Float
            case 9:    // Fieldref
            case 10:   // Methodref
            case 11:   // InterfaceMethodref
            case 12:   // NameAndType
            case 17:   // Dynamic
            case 18:   // InvokeDynamic
                if (!need(4)) return true;
                skip(4);
                break;
            case 5:   // Long（占两个槽位）
            case 6:   // Double（占两个槽位）
                if (!need(8)) return true;
                skip(8);
                ++index;
                break;
            case 7:    // Class
            case 8:    // String
            case 16:   // MethodType
            case 19:   // Module
            case 20:   // Package
                if (!need(2)) return true;
                skip(2);
                break;
            case 15:   // MethodHandle: kind(1) + reference(2)
                if (!need(3)) return true;
                skip(3);
                break;
            default:   // 2/13/14 废弃槽位与越界 tag：常量池被打乱，不是 class
                return false;
            }
        }
        return true;
    }
};

}   // namespace mjbase::jar
