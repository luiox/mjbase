/// @file        jasm_fixture.hpp
/// @brief       测试夹具的 jasm 文本装配共用面（fixture 纪律唯一路线）。
///
/// test-fixtures.rst 纪律落地件：类/方法/指令序列夹具一律从 jasm 文本装配
/// （JvmJasmService），本头只做三件事——诊断直报（汇编失败不静默）、UTF-8
/// 到 UTF-16 入参换算、常用类骨架便捷形。固定片段用 R"JASM(...)JASM 定界，
/// 变量插值处保留 + 拼接（duplicate_catch_merging_test 同式）。
///
/// 版本口径：夹具类默认 major 49（无 StackMapTable 的帧less 世界，与退役的
/// 字节级 FixtureBuilder 夹具同实——指令改写族 pass 的变换不遇帧悬空拒发）；
/// 需要 52 帧面的用例（异常表合并等零改写件）显式给 52，JSR/RET 形态给
/// ≤49。
///
/// 例外①（装配器拒收的非法形态）自检面：expectValidateOk / expectValidateFail
/// ——从装配合法类出发做单点最小偏差的前后自检用。
#pragma once

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"

#include "shared/structural_pass_util.hpp"

namespace mjbase::test {

/// UTF-8 文本 → UTF-16（jasm 服务源入口口径）。
inline ca::str::Utf16String jasmUtf16(const std::string& s)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(s.data()), s.size()));
}

/// jasm 源码 → class 字节（汇编失败逐条报诊断并返回空 Bytes；夹具错误不静默，
/// 附整份源码与行号定位供 CI 日志直读）。
inline Bytes assembleJasm(const std::string& source, ca::i32 majorVersion)
{
    mjbase::jasm::JvmJasmService service;
    mjbase::jasm::JvmJasmContext context;
    context.classMajorVersion = majorVersion;
    const auto sourceName = jasmUtf16("fixture.jasm");
    const auto sourceText = jasmUtf16(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref(), context);
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

/// @brief 上式的默认版本便捷形（夹具帧less 口径，见头注版本段）。
inline Bytes assembleJasm(const std::string& source)
{
    return assembleJasm(source, 49);
}

/// @brief 类骨架便捷形：.super superName + .class public super internalName { body }。
///
/// body 为类体文本（.field / .method / .inner 声明序列），定界由调用方的
/// raw string 片段负责。
inline Bytes assembleClass(const std::string& internalName, const std::string& superName,
                           const std::string& body)
{
    const std::string source = R"JASM(.super )JASM" + superName + R"JASM(
.class public super )JASM" + internalName + R"JASM( {
)JASM" + body + R"JASM(}
)JASM";
    return assembleJasm(source);
}

/// @brief 单方法便捷形：public static name/desc 方法 + code 体文本。
///
/// 指令体用例侧给 raw string 定界片段（不含 code: {} 包裹）。
inline Bytes assembleMethodClass(std::string_view name, std::string_view desc,
                                 std::string_view codeBody,
                                 const std::string& internalName = "t/Fx",
                                 ca::i32            majorVersion = 49)
{
    const std::string source = R"JASM(.super java/lang/Object
.class public super )JASM" + internalName + R"JASM( {
  .method public static )JASM" + std::string(name) + " " + std::string(desc)
        + R"JASM( {
    code: {
)JASM" + std::string(codeBody) + R"JASM(    }
  }
}
)JASM";
    return assembleJasm(source, majorVersion);
}

/// 例外①自检（前）：装配产物 relaxed validate 通过。
inline void expectValidateOk(const Bytes& bytes)
{
    mjbase::jasm::JvmJasmService service;
    const auto validated = service.validate(bytes);
    EXPECT_TRUE(validated.isOk());
}

/// 例外①自检（后）：单点偏差产物 relaxed validate 拒收（诊断非空）。
inline void expectValidateFail(const Bytes& bytes)
{
    mjbase::jasm::JvmJasmService service;
    const auto validated = service.validate(bytes);
    EXPECT_FALSE(validated.isOk());
}

}   // namespace mjbase::test
