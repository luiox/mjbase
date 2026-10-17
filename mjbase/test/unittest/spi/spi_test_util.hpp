/// @file        spi_test_util.hpp
/// @brief       spi 单测共用工具：手工 classfile 字节构造 + 共享测试 pass。
///
/// 字节模式照 asm/classpath 测试与 libs/libjbc/demo/control_flow.cpp：
/// utf8/class 描述符条目 + Code 属性；仅用单槽 CP 条目（无 long/double 双槽坑）。
#pragma once

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"

#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

namespace mjbase::spi::test {

using Bytes = std::vector<ca::u8>;

inline void putU8(Bytes& b, ca::u8 v) { b.push_back(v); }

inline void putU16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

inline void putU32(Bytes& b, ca::u32 v)
{
    b.push_back(static_cast<ca::u8>(v >> 24));
    b.push_back(static_cast<ca::u8>(v >> 16));
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

inline void putRaw(Bytes& b, std::string_view s)
{
    for (const char c : s) b.push_back(static_cast<ca::u8>(c));
}

/// @brief 把 src 追加到 out（临时实参安全版）。
///
/// 禁止直接写 `out.insert(out.end(), f(x).begin(), f(x).end())`：两次 f(x) 是两个
/// 独立临时对象，begin/end 指向两块无关堆缓冲，指针相减距离是垃圾——MSVC 下
/// 或抛 length_error("vector too long")，或静默拷入错位字节。
inline void appendBytes(Bytes& out, Bytes src)
{
    out.insert(out.end(), src.begin(), src.end());
}

/// 方法规格：code 为裸指令字节；access 缺省 public static。
struct MethodSpec
{
    std::string_view name;
    std::string_view desc;
    Bytes            code;
    ca::u16          access = 0x0009;   // ACC_PUBLIC | ACC_STATIC
};

/// @brief 手工拼最小 classfile（V1_8 / public super / super=java/lang/Object /
///        无接口无字段无类属性），方法各带一个 Code 属性。
///
/// 组装分两遍：先把全部 CP 条目物化（cpCount 才算得对），再写字节。
class ClassBytesBuilder
{
public:
    explicit ClassBytesBuilder(std::string_view thisClass)
    {
        thisIdx_  = cls(thisClass);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
    }

    Bytes assemble(const std::vector<MethodSpec>& methods)
    {
        // 第一遍：方法 name/desc 也先物化——cpCount 与 CP 字节必须等全部条目
        // 齐备后再落（否则 name/desc 索引越界，libjbc 直接拒读）。
        struct NameDesc
        {
            ca::u16 name;
            ca::u16 desc;
        };
        std::vector<NameDesc> slots;
        slots.reserve(methods.size());
        for (const MethodSpec& m : methods) {
            slots.push_back({utf8(m.name), utf8(m.desc)});
        }

        Bytes out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);   // V1_8
        putU16(out, next_);  // cpCount（slot 0 无效；此刻全部条目已物化）
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, 0x0021);   // public super
        putU16(out, thisIdx_);
        putU16(out, superIdx_);
        putU16(out, 0);   // interfaces
        putU16(out, 0);   // fields
        putU16(out, static_cast<ca::u16>(methods.size()));

        for (ca::usize i = 0; i < methods.size(); ++i) {
            const MethodSpec& m = methods[i];
            putU16(out, m.access);
            putU16(out, slots[i].name);
            putU16(out, slots[i].desc);
            Bytes attr;
            putU16(attr, 1);   // maxStack
            putU16(attr, 1);   // maxLocals
            putU32(attr, static_cast<ca::u32>(m.code.size()));
            attr.insert(attr.end(), m.code.begin(), m.code.end());
            putU16(attr, 0);   // exc table
            putU16(attr, 0);   // nested attrs
            putU16(out, 1);    // method attr count
            putU16(out, codeName_);
            putU32(out, static_cast<ca::u32>(attr.size()));
            out.insert(out.end(), attr.begin(), attr.end());
        }
        putU16(out, 0);   // class attrs
        return out;
    }

private:
    ca::u16 utf8(std::string_view s)
    {
        std::string key(s);
        const auto  it = utf8s_.find(key);
        if (it != utf8s_.end()) return it->second;
        putU8(cp_, 0x01);
        putU16(cp_, static_cast<ca::u16>(s.size()));
        putRaw(cp_, s);
        const ca::u16 idx = next_++;
        utf8s_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 cls(std::string_view n)
    {
        std::string key(n);
        const auto  it = classes_.find(key);
        if (it != classes_.end()) return it->second;
        const ca::u16 u = utf8(n);
        putU8(cp_, 0x07);
        putU16(cp_, u);
        const ca::u16 idx = next_++;
        classes_.emplace(std::move(key), idx);
        return idx;
    }

    Bytes   cp_;
    ca::u16 next_ = 1;   // slot 0 无效
    std::unordered_map<std::string, ca::u16> utf8s_;
    std::unordered_map<std::string, ca::u16> classes_;
    ca::u16 thisIdx_  = 0;
    ca::u16 superIdx_ = 0;
    ca::u16 codeName_ = 0;
};

/// 常用最小方法体：ICONST_1; IRETURN。
inline Bytes iconstReturn()
{
    return {0x04, 0xac};
}
/// 常用最小方法体：NOP; RETURN（RemoveNop 的产出靶）。
inline Bytes nopReturn()
{
    return {0x00, 0xb1};
}
/// 单指令方法体：RETURN（指令计数类用例的基准形态）。
inline Bytes bareReturn()
{
    return {0xb1};
}

/// 构造一个最小可解析 class（多个方法，code 自带）。
inline Bytes buildClassBytes(const char* className, std::vector<MethodSpec> methods)
{
    ClassBytesBuilder b(className);
    return b.assemble(methods);
}

/// 构造含单个 NOP 方法 run()V 的最小 class（NopRemover 形测试的产出靶）。
inline Bytes buildNopClassBytes(const char* className)
{
    return buildClassBytes(className, {{"run", "()V", nopReturn()}});
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
        jbc::ir::CodeEditor       editor(scope.code);
        const std::vector<jbc::ir::InsnId> ids = scope.code.dom.order();   // 快照防遍历失稳
        bool                    removed = false;
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
