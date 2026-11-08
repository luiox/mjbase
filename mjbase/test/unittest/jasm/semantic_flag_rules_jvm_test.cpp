// JVMS 语义级 flag 规则门的 JDK defineClass 对拍（issue #179 产出判据：
// 合法形态 0x1019 过、0x100A/0x1009 拒）。
//
// 同一批字节（shared/jasm_fixture 的 flipMemberAccess 从装配合法接口出发
// 单点位改写）先过 mjbase validate 门、再喂 JDK defineClass：两端结论必须
// 一致（过/拒）。JDK 不可用 GTEST_SKIP（合法 skip——对拍无对象）。
#include <gtest/gtest.h>

#include <string>

#include "jdk_fixture_gtest.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"

#include "shared/jasm_fixture.hpp"

namespace mjbase::jasm {

using namespace mjbase::test;   // Bytes / flip* 夹具面

namespace {

constexpr ca::u16 kAccPublic    = 0x0001;
constexpr ca::u16 kAccPrivate   = 0x0002;
constexpr ca::u16 kAccFinal     = 0x0010;
constexpr ca::u16 kAccSynthetic = 0x1000;

/// 接口夹具（合法形态，internalName 可变）：字段 public static final K。
Bytes assembleInterface(const std::string& internalName)
{
    const std::string source = ".super java/lang/Object\n"
                               ".class public interface abstract " + internalName + " {\n"
                               "  .field public static final K I\n"
                               "  .method public abstract compute ()I { }\n"
                               "}\n";
    return test::assembleJasm(source);
}

}   // namespace

TEST(MjSemanticFlagRulesJvmTest, DefineClassVerdictMatchesValidateGate)
{
    // 全限定 ::test::jdk——本文件处于 mjbase::jasm，非限定 test:: 会先命中
    // mjbase::test（jasm_fixture）并断链。
    const auto tc = ::test::jdk::probeJdk();
    if (!tc.available) GTEST_SKIP() << "JDK 不可用: " << tc.reason;

    // 0x1019（public static final synthetic，issue 实测过检位面）。
    const Bytes legalBytes = assembleInterface("t/Ifx19");
    ASSERT_FALSE(legalBytes.empty());
    const Bytes legalSynthetic = test::flipMemberAccess(legalBytes, true, 0, kAccSynthetic, 0);
    ASSERT_FALSE(legalSynthetic.empty());

    // 0x100A / 0x1009（issue 实测 JDK 17.0.8 defineClass 拒收位面）。
    const Bytes illegalABytes = assembleInterface("t/Ifx100A");
    ASSERT_FALSE(illegalABytes.empty());
    const Bytes illegalA = test::flipMemberAccess(illegalABytes, true, 0,
                                                  kAccPrivate | kAccSynthetic,
                                                  kAccPublic | kAccFinal);
    ASSERT_FALSE(illegalA.empty());

    const Bytes illegalBBytes = assembleInterface("t/Ifx1009");
    ASSERT_FALSE(illegalBBytes.empty());
    const Bytes illegalB = test::flipMemberAccess(illegalBBytes, true, 0, kAccSynthetic,
                                                  kAccFinal);
    ASSERT_FALSE(illegalB.empty());

    // 先过 mjbase 门：拒/过结论与 JDK 对拍同款。
    {
        JvmJasmService service;
        EXPECT_FALSE(service.validate(illegalA).isOk()) << "0x100A 须被 flag 门拒收";
        EXPECT_FALSE(service.validate(illegalB).isOk()) << "0x1009 须被 flag 门拒收";
        EXPECT_TRUE(service.validate(legalSynthetic).isOk()) << "0x1019 须被放行";
    }

    ::test::jdk::RunnerSpec spec;
    spec.classes = {{"t/Ifx19", legalSynthetic}, {"t/Ifx100A", illegalA},
                    {"t/Ifx1009", illegalB}};
    // defineClass 逐个喂字节（classpath 指向夹具落盘目录，getResourceAsStream
    // 取回原始字节），过/拒与异常类名逐行打印；IOException 与 defineClass 的
    // Throwable 同路归并（行内 catch (Throwable) 兼收）。
    spec.runnerBody
        = "var l = new ClassLoader() {"
          " Class<?> def(byte[] b) { return defineClass(null, b, 0, b.length); } };"
          "for (String n : new String[] {\"t/Ifx19\", \"t/Ifx100A\", \"t/Ifx1009\"}) {"
          " try { byte[] b = Runner.class.getResourceAsStream(\"/\" + n + \".class\").readAllBytes();"
          " l.def(b); System.out.println(n + \" OK\"); }"
          " catch (Throwable t) { System.out.println(n + \" REJECTED \""
          " + t.getClass().getSimpleName()); } }";
    std::string out;
    ASSERT_TRUE(::test::jdk::runRunnerChecked(tc, spec, &out));
    EXPECT_EQ("t/Ifx19 OK\n"
              "t/Ifx100A REJECTED ClassFormatError\n"
              "t/Ifx1009 REJECTED ClassFormatError\n",
              out);
}

}   // namespace mjbase::jasm
