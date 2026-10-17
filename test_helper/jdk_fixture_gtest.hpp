// jdk_fixture_gtest.hpp — jdk_fixture 的 gtest 语义封装
//
// 修复"吞失败"掩蔽（#917 评审）：此前调用方对 runWithRunner 的 nullopt 一律
// GTEST_SKIP，导致"被测产物运行失败"（如 keychain Phase 3 key2 不自洽的首链
// AIOOBE）在 CI 全绿下存活两周。本文件把失败语义收敛到一处：
//   - JDK 真不可用  → GTEST_SKIP（环境限制，合法 skip，保留）；
//   - javac 编译失败 → 测试失败（harness 误用或产物无法链接，必须显红）；
//   - JVM 运行失败  → 测试失败并带关键输出（异常栈 / exit code）。
// 仅供 gtest 测试目标包含；非 gtest 消费方请直接用 jdk_fixture.hpp。
//
// .. 实现注记：GTEST_SKIP 宏展开带 return，只能落在 void 语境——因此 SKIP
// 分支独立成 void helper（TestPartResult 记到当前测试），runRunnerChecked
// 本体保持 bool 返回，调用方以 `if (!runJvmChecked(...)) return;` 短路收尾。

#pragma once

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>

#include "jdk_fixture.hpp"

namespace test::jdk {

/// SKIP 分支（必须 void：GTEST_SKIP 展开带 return）。TestPartResult(kSkip)
/// 记到当前正在运行的测试，与本函数所在栈深无关。
inline void skipOnJdkUnavailable(const JvmRunOutcome& r)
{
    GTEST_SKIP() << "JDK 不可用: " << r.output;
}

/// gtest 语义执行（在 TEST/夹具函数体内用）：
/// - JDK 不可用 → 记 GTEST_SKIP；
/// - 编译/运行失败 → ADD_FAILURE + 诊断输出（异常栈等），测试判负。
///
/// @return true = 可继续，*out 为归一化 stdout；false = 已记 SKIP/FAIL，
///         调用方必须立即 return（bool 短路模式交给调用方收尾）。
inline bool runRunnerChecked(const JdkToolchain& tc, const RunnerSpec& spec, std::string* out)
{
    const auto r = runWithRunnerOutcome(tc, spec);
    if (r.status == JvmRunStatus::Ok) {
        *out = std::move(r.output);
        return true;
    }
    if (r.status == JvmRunStatus::JdkUnavailable) {
        skipOnJdkUnavailable(r);
        return false;
    }
    if (r.status == JvmRunStatus::CompileFailed) {
        ADD_FAILURE() << "javac 编译失败:\n" << r.output;
    }
    else {
        ADD_FAILURE() << "JVM 运行失败 (exit=" << r.exitCode << "):\n" << r.output;
    }
    return false;
}

/// 负向用例两步收口（R12-NIT-3 收敛 5 处同款三行 SKIP 守卫）：
/// - JdkUnavailable → 走 skipOnJdkUnavailable 记官方 SKIP，返回 true（调用方
///   必须立即 return——与 runRunnerChecked 同款 bool 短路体例，保证 SKIP 后
///   的追加断言不被执行）；
/// - 其余终态 → EXPECT_FALSE(ok)（谓词与消息原样保留，!ok ⇔ nullopt 时代
///   的 nullopt 语义），返回 false，调用方可继续追加失败原因断言。
inline bool skipIfNoJdkThenExpectFailure(const JvmRunOutcome& r, const std::string& message)
{
    if (r.status == JvmRunStatus::JdkUnavailable) {
        skipOnJdkUnavailable(r);
        return true;
    }
    EXPECT_FALSE(r.ok()) << message;
    return false;
}

/// 便捷重载：内部用 probeJdk() 的缓存工具链。
inline bool runRunnerChecked(const RunnerSpec& spec, std::string* out)
{
    static const auto tc = probeJdk();
    return runRunnerChecked(tc, spec, out);
}

}   // namespace test::jdk
