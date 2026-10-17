/// @file        renamer_fixture.hpp
/// @brief       renamer 测试族共享夹具：jasm 源码拼装 + 组装为 class 字节 +
///              容器登记助手（仅测试域；组装失败 fail-loud）。
///
/// 旧栈测试用 jbytecode ClassWriter 造类；mjbase 线一律 jasm 组装
/// （JvmJasmService），语义对齐点：`.super` 缺省 = 无 super 入口
/// （java/lang/Object 夹具口径）、`{ }` 空体 abstract 方法、`.implements`。
#pragma once

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjbase::renamer::test_support {

/// @brief UTF-8 文本 → UTF-16（jasm 源入口口径）。
inline ca::str::Utf16String utf16Of(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// @brief jasm 源码 → class 字节（组装失败直接致命，夹具错误不静默）。
inline std::vector<ca::u8> assembleJasm(const std::string& source)
{
    jasm::JvmJasmService service;
    auto                 sourceName = utf16Of("renamer_fixture.jasm");
    auto                 text       = utf16Of(source);
    auto                 result     = service.assemble(sourceName.ref(), text.ref());
    if (!result.isOk()) throw std::runtime_error("renamer jasm fixture failed to assemble");
    return result.value->bytes;
}

/// @brief 声明一个 RETURN 收尾的 void 方法（类体缩进两格）。
inline std::string voidMethod(const std::string& modifiers, const std::string& name,
                              const std::string& desc)
{
    return "  .method " + modifiers + " " + name + " " + desc +
           " {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n";
}

/// @brief 声明一个空体 abstract 方法（无 code 属性）。
inline std::string abstractMethod(const std::string& modifiers, const std::string& name,
                                  const std::string& desc)
{
    return "  .method " + modifiers + " " + name + " " + desc + " {}\n";
}

/// @brief 声明一个字段。
inline std::string fieldDecl(const std::string& modifiers, const std::string& name,
                             const std::string& desc)
{
    return "  .field " + modifiers + " " + name + " " + desc + "\n";
}

/// @brief 组装一个类并返回字节。superName 为空 = 无 `.super` 行（无 super 入口）。
inline std::vector<ca::u8> assembleClass(const std::string&               name,
                                         const std::string&               classModifiers,
                                         const std::string&               superName,
                                         const std::vector<std::string>&  interfaces,
                                         const std::string&               memberDecls)
{
    std::string source;
    if (!superName.empty()) source += ".super " + superName + "\n";
    for (const auto& implemented : interfaces) source += ".implements " + implemented + "\n";
    source += ".class " + classModifiers + " " + name + " {\n" + memberDecls + "}\n";
    return assembleJasm(source);
}

/// @brief 便捷重载：public super 类、显式 super、无接口。
inline std::vector<ca::u8> assembleClass(const std::string& name, const std::string& superName,
                                         const std::string& memberDecls)
{
    return assembleClass(name, "public super", superName, {}, memberDecls);
}

/// @brief 无 super 入口的空类（java/lang/Object 夹具口径）。
inline std::vector<ca::u8> buildBareObject()
{
    return assembleClass("java/lang/Object", "public super", "", {}, "");
}

/// @brief 夹具类 A：oldField(I)/other(J) + <init>/oldMethod/X/alpha（全 ()V）。
inline std::vector<ca::u8> buildA()
{
    std::string members;
    members += fieldDecl("public", "oldField", "I");
    members += fieldDecl("public", "other", "J");
    members += voidMethod("public", "<init>", "()V");
    members += voidMethod("public", "oldMethod", "()V");
    members += voidMethod("public", "X", "()V");
    members += voidMethod("public", "alpha", "()V");
    return assembleClass("A", "java/lang/Object", members);
}

/// @brief 夹具类 A 的缩水变体：只剩 oldField(I) + <init>（prune 成员级用例）。
inline std::vector<ca::u8> buildAShrunk()
{
    std::string members;
    members += fieldDecl("public", "oldField", "I");
    members += voidMethod("public", "<init>", "()V");
    return assembleClass("A", "java/lang/Object", members);
}

/// @brief 夹具类 B：继承 A，声明 <init>/oldMethod/X（override A.oldMethod）。
inline std::vector<ca::u8> buildB()
{
    std::string members;
    members += voidMethod("public", "<init>", "()V");
    members += voidMethod("public", "oldMethod", "()V");
    members += voidMethod("public", "X", "()V");
    return assembleClass("B", "A", members);
}

/// @brief 夹具类 C：<init> 的独立类（无成员交互）。
inline std::vector<ca::u8> buildC()
{
    return assembleClass("C", "java/lang/Object", voidMethod("public", "<init>", "()V"));
}

/// @brief 接口 I：声明 abstract oldMethod()V（同组件双建议测试用）。
inline std::vector<ca::u8> buildInterfaceI()
{
    return assembleClass("I",
                         "public abstract interface",
                         "java/lang/Object",
                         {},
                         abstractMethod("public abstract", "oldMethod", "()V"));
}

/// @brief Impl 实现 I.oldMethod（同组件双建议测试用）。
inline std::vector<ca::u8> buildImpl()
{
    std::string members;
    members += voidMethod("public", "<init>", "()V");
    members += voidMethod("public", "oldMethod", "()V");
    return assembleClass("Impl", "public super", "java/lang/Object", {"I"}, members);
}

/// @brief 继承者：继承 A 但不声明成员；broken=true 时 implements 容器外接口。
inline std::vector<ca::u8> buildInheritor(const std::string& name, bool broken)
{
    std::vector<std::string> interfaces;
    if (broken) interfaces.push_back("ext/Missing");
    return assembleClass(name, "public super", "A", interfaces,
                         voidMethod("public", "<init>", "()V"));
}

/// @brief 双组件继承者 C：继承 A 且实现 I，自身不声明 oldMethod。
inline std::vector<ca::u8> buildDualInheritorC()
{
    return assembleClass("C", "public super", "A", {"I"},
                         voidMethod("public", "<init>", "()V"));
}

/// @brief 后缀墙类：oldMethod(+alpha) 与 X/X_1..X_64 共 65+ 个占位方法。
inline std::vector<ca::u8> buildSuffixWallClass(const std::string& name,
                                                const std::string& superName, bool withAlpha)
{
    std::string members;
    members += voidMethod("public", "<init>", "()V");
    members += voidMethod("public", "oldMethod", "()V");
    if (withAlpha) members += voidMethod("public", "alpha", "()V");
    for (int i = 0; i <= 64; ++i) {
        const auto slot = i == 0 ? std::string("X") : "X_" + std::to_string(i);
        members += voidMethod("public", slot, "()V");
    }
    return assembleClass(name, superName, members);
}

}   // namespace mjbase::renamer::test_support
