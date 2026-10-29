// StringConcatIndirector：StringBuilder 拼接链 indy 化简——javac 9+ 形态
// 折叠、无参链、多窗 BSM 同形复用、计算实参/窗内标签/异类 NEW/版本门拒收、
// 幂等与写回、注册面。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/string_concat_indirector.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// v53 起始的会话夹具（StringConcatFactory 版本门）；DomFixture 固定 v52，
/// 本地按 seed.version 抬版本，方法构建复用 addBodyMethod。
struct IndyFixture
{
    std::shared_ptr<jbc::ir::ClassSession> session;

    IndyFixture(ca::u16 major, ca::u16 maxLocals,
                const std::function<void(Body&)>& fill)
    {
        jbc::ir::NewClassSeed seed;
        seed.thisName  = "t/C";
        seed.superName = "java/lang/Object";
        seed.version   = static_cast<ca::u32>(major) << 16;
        auto made      = jbc::ir::makeSession(seed);
        if (made.is_err()) {
            ADD_FAILURE() << "makeSession failed";
            return;
        }
        session = std::move(made).unwrap();
        addBodyMethod(*session, "m", "()Ljava/lang/String;", 6, maxLocals, fill);
    }

    jbc::ir::CodeUnit& unit()
    {
        auto* code = session->unit().methods[0].code.get();
        EXPECT_NE(code, nullptr);
        return *code;
    }

    mjbase::spi::PreservedAnalyses run()
    {
        ca::str::Utf8StringArena           arena;
        mjbase::spi::MethodAnalysisManager mam(arena);
        mjbase::KeyPool                    identityPool;
        mjbase::spi::MethodPassManager     mpm;
        mpm.addPass(std::make_unique<mjbase::pass::StringConcatIndirector>());
        return mjbase::spi::runMethodPipelineOverSession(*session, mpm, mam,
                                                         identityPool, nullptr, {});
    }

    Bytes writeBack() const
    {
        auto written = jbc::core::writeClass(session->backing());
        EXPECT_TRUE(written.is_ok());
        return std::move(written).unwrap();
    }
};

/// 方法体操作码序列（IndyFixture 版 opsOf）。
std::vector<jbc::core::Op> opsOfFx(const IndyFixture& fx)
{
    std::vector<jbc::core::Op> ops;
    for (const jbc::ir::InsnId id : fx.session->unit().methods[0].code->dom.order()) {
        const jbc::ir::Insn* in = fx.session->unit().methods[0].code->dom.peek(id);
        EXPECT_NE(in, nullptr);
        ops.push_back(in->op);
    }
    return ops;
}

/// Body 构建便利位：NEW + dup + [ldc str] + init。
void buildChainHead(Body& b, bool withStr, const char* str)
{
    const ca::u32 sbCp = b.session().internClass("java/lang/StringBuilder");
    b.invoke(Op::NEW, sbCp);
    b.dup();
    if (withStr) {
        b.ldcString(str);
        b.invoke(Op::INVOKESPECIAL,
                 b.session().internMethodRef("java/lang/StringBuilder", "<init>",
                                             "(Ljava/lang/String;)V"));
    } else {
        b.invoke(Op::INVOKESPECIAL,
                 b.session().internMethodRef("java/lang/StringBuilder", "<init>",
                                             "()V"));
    }
}

void buildAppend(Body& b, std::string_view desc)
{
    b.invoke(Op::INVOKEVIRTUAL,
             b.session().internMethodRef("java/lang/StringBuilder", "append",
                                         std::string(desc)));
}

void buildToString(Body& b)
{
    b.invoke(Op::INVOKEVIRTUAL,
             b.session().internMethodRef("java/lang/StringBuilder", "toString",
                                         "()Ljava/lang/String;"));
    b.areturn();
}

/// 类属性槽按名定位 BootstrapMethods typed 体（空 = 无属性）。
jbc::core::BootstrapMethodsAttr* bsmOf(IndyFixture& fx)
{
    for (jbc::core::AttrSlot& slot : fx.session->backing().attrs) {
        if (!fx.session->backing().cp.utf8Equals(slot.nameCp, "BootstrapMethods")) {
            continue;
        }
        if (!slot.decoded()) {
            if (fx.session->backing().decodeAttr(slot).is_err()) return nullptr;
        }
        if (slot.body == nullptr
            || slot.body->kind() != jbc::core::AttrKind::BootstrapMethods) {
            return nullptr;
        }
        return static_cast<jbc::core::BootstrapMethodsAttr*>(slot.body.get());
    }
    return nullptr;
}

}   // namespace

// ── javac 9+ 形态：ldc 起始串 + (iload; append(I); ldc; append(String))+ ──
TEST(StringConcatIndirectorTest, javacShapeFolded)
{
    IndyFixture fx(53, 2, [&](Body& b) {
        buildChainHead(b, true, "a");
        b.iload(1);
        buildAppend(b, "(I)Ljava/lang/StringBuilder;");
        b.ldcString("b");
        buildAppend(b, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
        buildToString(b);
    });
    EXPECT_TRUE(fx.run().changed());
    const std::vector<Op> ops = opsOfFx(fx);
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::LDC);             // 实参保留：起始串
    EXPECT_EQ(ops[1], Op::ILOAD);           // 实参保留：iload 1
    EXPECT_EQ(ops[2], Op::LDC);             // 实参保留：追加串
    EXPECT_EQ(ops[3], Op::INVOKEDYNAMIC);   // toString 位
    EXPECT_EQ(ops[4], Op::ARETURN);
    const jbc::core::BootstrapMethodsAttr* bsm = bsmOf(fx);
    ASSERT_NE(bsm, nullptr);
    ASSERT_EQ(bsm->entries.size(), 1u);
    ASSERT_EQ(bsm->entries[0].args.size(), 1u);
    // 配方 = 全实参槽位（3 参：起始串/iload/追加串）。
    const ca::u32 strCp = fx.session->backing().cp.at(bsm->entries[0].args[0]).a;
    EXPECT_EQ(fx.session->backing().cp.utf8Of(strCp), std::string("\x01\x01\x01"));
}

// ── 无参链：init()V + 单 append(String) ──
TEST(StringConcatIndirectorTest, noArgChainFolded)
{
    IndyFixture fx(53, 1, [&](Body& b) {
        buildChainHead(b, false, "");
        b.ldcString("x");
        buildAppend(b, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
        buildToString(b);
    });
    EXPECT_TRUE(fx.run().changed());
    const std::vector<Op> ops = opsOfFx(fx);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::LDC);   // append 实参保留
    EXPECT_EQ(ops[1], Op::INVOKEDYNAMIC);
    const jbc::core::BootstrapMethodsAttr* bsm = bsmOf(fx);
    ASSERT_NE(bsm, nullptr);
    ASSERT_EQ(bsm->entries.size(), 1u);
    const ca::u32 strCp = fx.session->backing().cp.at(bsm->entries[0].args[0]).a;
    EXPECT_EQ(fx.session->backing().cp.utf8Of(strCp), std::string(1, '\x01'));
}

// ── 同形两窗：BSM 条目同形复用（单条目）──
TEST(StringConcatIndirectorTest, twinChainsShareBsm)
{
    IndyFixture fx(53, 1, [&](Body& b) {
        buildChainHead(b, false, "");
        b.ldcString("x");
        buildAppend(b, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
        b.invoke(Op::INVOKEVIRTUAL,
                 b.session().internMethodRef("java/lang/StringBuilder", "toString",
                                             "()Ljava/lang/String;"));
        b.pop();
        buildChainHead(b, false, "");
        b.ldcString("x");
        buildAppend(b, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
        buildToString(b);
    });
    EXPECT_TRUE(fx.run().changed());
    const std::vector<Op> ops = opsOfFx(fx);
    ASSERT_EQ(ops.size(), 6u);   // LDC, INDY, POP, LDC, INDY, ARETURN
    EXPECT_EQ(ops[1], Op::INVOKEDYNAMIC);
    EXPECT_EQ(ops[2], Op::POP);
    EXPECT_EQ(ops[4], Op::INVOKEDYNAMIC);
    const jbc::core::BootstrapMethodsAttr* bsm = bsmOf(fx);
    ASSERT_NE(bsm, nullptr);
    EXPECT_EQ(bsm->entries.size(), 1u);
}

// ── 计算形实参（两指令产出）不认领 → 整窗拒收 ──
TEST(StringConcatIndirectorTest, computedArgKept)
{
    IndyFixture fx(53, 2, [&](Body& b) {
        buildChainHead(b, true, "a");
        b.iconst_1();
        b.iconst_2();
        b.iadd();
        buildAppend(b, "(I)Ljava/lang/StringBuilder;");
        buildToString(b);
    });
    EXPECT_TRUE(fx.run().areAllPreserved());
    ASSERT_EQ(opsOfFx(fx).size(), 10u);
}

// ── 窗内标签锚定（分支目标进入链体）→ 拒收 ──
TEST(StringConcatIndirectorTest, labelInsideChainKept)
{
    IndyFixture fx(53, 1, [&](Body& b) {
        jbc::ir::LabelId tgt = b.label();
        b.branch(Op::IFEQ, tgt);
        buildChainHead(b, false, "");
        b.ldcString("x");
        b.bindHere(tgt);
        buildAppend(b, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
        buildToString(b);
    });
    EXPECT_TRUE(fx.run().areAllPreserved());
}

// ── 异类 NEW（StringBuffer）不动 ──
TEST(StringConcatIndirectorTest, stringBufferChainKept)
{
    IndyFixture fx(53, 1, [&](Body& b) {
        const ca::u32 sbCp = b.session().internClass("java/lang/StringBuffer");
        b.invoke(Op::NEW, sbCp);
        b.dup();
        b.invoke(Op::INVOKESPECIAL,
                 b.session().internMethodRef("java/lang/StringBuffer", "<init>",
                                             "()V"));
        b.ldcString("x");
        b.invoke(Op::INVOKEVIRTUAL,
                 b.session().internMethodRef("java/lang/StringBuffer", "append",
                                             "(Ljava/lang/String;)Ljava/lang/StringBuffer;"));
        b.invoke(Op::INVOKEVIRTUAL,
                 b.session().internMethodRef("java/lang/StringBuffer", "toString",
                                             "()Ljava/lang/String;"));
        b.areturn();
    });
    EXPECT_TRUE(fx.run().areAllPreserved());
}

// ── 版本门：v52 整件不动 ──
TEST(StringConcatIndirectorTest, versionGateKept)
{
    IndyFixture fx(52, 1, [&](Body& b) {
        buildChainHead(b, false, "");
        b.ldcString("x");
        buildAppend(b, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
        buildToString(b);
    });
    EXPECT_TRUE(fx.run().areAllPreserved());
}

// ── 幂等：已收敛方法再跑不动；写回一致 ──
TEST(StringConcatIndirectorTest, idempotentAndWriteBack)
{
    IndyFixture fx(53, 1, [&](Body& b) {
        buildChainHead(b, false, "");
        b.ldcString("x");
        buildAppend(b, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
        buildToString(b);
    });
    ASSERT_TRUE(fx.run().changed());
    EXPECT_TRUE(fx.run().areAllPreserved());
    const Bytes out = fx.writeBack();
    EXPECT_FALSE(out.empty());
}

// ── 注册面 ──
TEST(StringConcatIndirectorTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("StringConcatIndirector"));
    auto created =
        mjbase::spi::defaultPassRegistry().createMethodPass("StringConcatIndirector");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
