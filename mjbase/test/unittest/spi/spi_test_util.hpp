// spi 单测共用工具：jasm 装配类夹具 + 共享测试 pass。
//
// 类与方法夹具一律 jasm 文本装配（JvmJasmService），原手拼
// ClassBytesBuilder 字节构造器退役——方法规格直接携带 jasm code: 段指令
// 文本，期望值可读可改。净化器字料（格式即坏的非法样本：坏 tag / 截断
// 头 / 悬垂引用）不经此头——合法基类 + DOM 偏差无法表达「CP 条目 tag 本身
// 非法」等字节格式态，各用例文件按字节语料申报书写。
#pragma once

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"

#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/jasm_fixture.hpp"

namespace mjbase::spi::test {

using Bytes = std::vector<ca::u8>;

/// 方法规格：codeText 为 `code:` 段内指令文本（行首 6 空格缩进 + 换行）。
struct MethodSpec
{
    std::string_view name;
    std::string_view desc;
    std::string_view codeText;
};

/// 常用最小 code 体：ICONST_1; IRETURN。
inline std::string_view iconstReturnText()
{
    return "      iconst_1\n      ireturn\n";
}

/// 常用最小 code 体：NOP; RETURN（RemoveNop 的产出靶）。
inline std::string_view nopReturnText()
{
    return "      nop\n      return\n";
}

/// 单指令 code 体：RETURN（指令计数类用例的基准形态）。
inline std::string_view bareReturnText()
{
    return "      return\n";
}

/// @brief jasm 装配最小类（public super / super=java/lang/Object /
///        无接口无字段无类属性，方法各带一个 Code 属性）。
inline Bytes buildClassBytes(const char* className,
                             const std::vector<MethodSpec>& methods)
{
    std::string memberText;
    memberText.reserve(methods.size() * 64);
    for (const MethodSpec& m : methods) {
        memberText += "  .method public static ";
        memberText += m.name;
        memberText += ' ';
        memberText += m.desc;
        memberText += " {\n    code: {\n";
        memberText += m.codeText;
        memberText += "    }\n  }\n";
    }
    const std::string src = ".super java/lang/Object\n.class public super "
        + std::string(className) + " {\n" + memberText + "}\n";
    return mjbase::test::assembleJasm(src);
}

/// 构造含单个 NOP 方法 run()V 的最小 class（NopRemover 形测试的产出靶）。
inline Bytes buildNopClassBytes(const char* className)
{
    return buildClassBytes(className, {{"run", "()V", nopReturnText()}});
}

/// bytes → 会话（gtest 断言失败即停）。
inline std::shared_ptr<jbc::ir::ClassSession> makeSession(const Bytes& bytes)
{
    auto r = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(r.is_ok());
    if (r.is_err()) return nullptr;
    return std::move(r).unwrap();
}

/// @brief 方法级单测夹具：会话 + 第 methodIdx 个方法的 promote 产物 + 手工
///        MethodScope（地址稳定，跨断言存活——AM 按 scope 地址缓存）。
struct ScopeFixture
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    jbc::ir::CodeUnitRef                   code;
    std::unique_ptr<MethodScope>           scope;

    ScopeFixture(const Bytes& bytes, ca::usize methodIdx = 0)
    {
        session = makeSession(bytes);
        if (session == nullptr) return;
        auto r = jbc::ir::promoteMethod(*session, methodIdx);
        EXPECT_TRUE(r.is_ok());
        if (r.is_err()) return;
        code             = std::move(r).unwrap();
        auto&       unit = session->unit();
        const auto& mu   = unit.methods[methodIdx];
        scope            = std::make_unique<MethodScope>(
            MethodScope{MethodTarget{mjbase::MemberKey{}, mu.id, unit.symbols().str(mu.name),
                                     unit.symbols().str(mu.desc), mu.access, *session},
                        *code.get()});
    }
};

// ── 共享测试 pass ──

/// 记录型方法 pass：把方法名追加到共享 log，返回 none()（默认全失效）。
class RecordingMethodPass : public MethodPass
{
public:
    explicit RecordingMethodPass(std::vector<std::string>& log)
        : log_(log)
    {}
    PreservedAnalyses run(MethodScope& scope, MethodAnalysisManager&) override
    {
        log_.emplace_back(scope.name);
        return PreservedAnalyses::none();
    }

private:
    std::vector<std::string>& log_;
};

/// 只读方法 pass：返回 all()（不失效任何分析）。
class ReadOnlyMethodPass : public MethodPass
{
public:
    PreservedAnalyses run(MethodScope&, MethodAnalysisManager&) override
    {
        return PreservedAnalyses::all();
    }
};

/// 方法名等于 target 时抛 std::runtime_error。
class ThrowingMethodPassByName : public MethodPass
{
public:
    explicit ThrowingMethodPassByName(std::string target)
        : target_(std::move(target))
    {}
    PreservedAnalyses run(MethodScope& scope, MethodAnalysisManager&) override
    {
        if (scope.name == target_) throw std::runtime_error("pass failed");
        return PreservedAnalyses::all();
    }

private:
    std::string target_;
};

/// 类内部名等于 target 时抛 std::runtime_error（敌意类注入点）。
class ThrowingClassPassByName : public ClassPass
{
public:
    explicit ThrowingClassPassByName(std::string target)
        : target_(std::move(target))
    {}
    PreservedAnalyses run(jbc::ir::ClassSession& session, ClassAnalysisManager&) override
    {
        if (session.unit().symbols().str(session.unit().thisClass) == target_) {
            throw std::runtime_error("hostile class");
        }
        return PreservedAnalyses::all();
    }

private:
    std::string target_;
};

/// 非 std 异常类型（验证容错档位不吞非 std 异常）。
struct FakeThrowable
{
};

/// 方法级 pass：一律抛非 std 异常。
class NonStdThrowingMethodPass : public MethodPass
{
public:
    PreservedAnalyses run(MethodScope&, MethodAnalysisManager&) override
    {
        throw FakeThrowable{};
    }
};

/// 类级 pass：一律抛非 std 异常。
class NonStdThrowingClassPass : public ClassPass
{
public:
    PreservedAnalyses run(jbc::ir::ClassSession&, ClassAnalysisManager&) override
    {
        throw FakeThrowable{};
    }
};

/// 移除全部 NOP 的方法 pass（old NopRemover 的最小替身）：改了 IR → none()。
class RemoveNopPass : public MethodPass
{
public:
    PreservedAnalyses run(MethodScope& scope, MethodAnalysisManager&) override
    {
        jbc::ir::CodeEditor                       editor(scope.code);
        const std::vector<jbc::ir::InsnId> ids    = scope.code.dom.order();   // 快照防遍历失稳
        bool                               removed = false;
        for (const jbc::ir::InsnId id : ids) {
            const jbc::ir::Insn* in = scope.code.dom.peek(id);
            if (in != nullptr && in->op == jbc::core::Op::NOP) {
                auto r = editor.erase(id);
                EXPECT_TRUE(r.is_ok());
                removed = true;
            }
        }
        return removed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};

/// 追加一个 NOP 的方法 pass：改了 IR → none()。
class AddNopPass : public MethodPass
{
public:
    PreservedAnalyses run(MethodScope& scope, MethodAnalysisManager&) override
    {
        jbc::ir::CodeEditor editor(scope.code);
        auto                r = editor.append(jbc::ir::Insn{});
        EXPECT_TRUE(r.is_ok());
        return PreservedAnalyses::none();
    }
};

/// 类级 pass：不改 IR 但声明改动（驱动跨层缓存清理路径）。
class TouchingClassPass : public ClassPass
{
public:
    PreservedAnalyses run(jbc::ir::ClassSession&, ClassAnalysisManager&) override
    {
        // 不能证明方法级分析仍有效，故按变更返回 none。
        return PreservedAnalyses::none();
    }
};

/// 类内部名捕获 pass（CurrentClassContext 折叠进 MethodScope 的验收形态）。
class OwnerNameCapturingPass : public MethodPass
{
public:
    explicit OwnerNameCapturingPass(std::vector<std::string>& captured)
        : captured_(captured)
    {}
    PreservedAnalyses run(MethodScope& scope, MethodAnalysisManager&) override
    {
        captured_.emplace_back(scope.session.unit().symbols().str(scope.session.unit().thisClass));
        return PreservedAnalyses::all();
    }

private:
    std::vector<std::string>& captured_;
};

}   // namespace mjbase::spi::test
