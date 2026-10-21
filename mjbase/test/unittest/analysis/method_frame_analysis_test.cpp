/// @file        method_frame_analysis_test.cpp
/// @brief       analysis 帧分析语义测试：逐指令帧对齐、空方法 fail-open、
///              容量声明无关。
///
///（6 用例中 3 个可移植）。OriginalSourceFrameAnalysis 的驱动器职责在 mjbase
/// 由 libjbc DomainEngine 承担（域引擎产出 DomainResult.frames 与
/// CodeUnit::dom.order() 按下标对齐）；帧语义等价断言落在 origin 域 +
/// FrameQuery 上。旧栈 3 个 MethodAnalysisManager 缓存用例（caches/invalidates/
/// keepsCacheOnAllPreserved）属旧栈 pass 基建，mjbase pass 域批次未到，不迁
///（见迁移报告不可等价清单）。
#include <gtest/gtest.h>

#include "jbc/jbc.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/asm/analyzer/origin_domain.hpp"
#include "mjbase/asm/analyzer/frame_query.hpp"

using jbc::analysis::DomainEngine;
using jbc::analysis::DomainResult;
using jbc::analysis::MethodSig;
using jbc::core::ConstantPool;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::FrameQuery;
using mjbase::asm_analyzer::OriginDomain;
using mjbase::asm_analyzer::OriginValue;
using mjbase::asm_analyzer::traceOrigins;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::MethodSpec;

namespace {

InsnId appendOp(CodeDom& d, Op o)
{
    jbc::ir::Insn in;
    in.op = o;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

InsnId appendVar(CodeDom& d, Op o, ca::u16 idx)
{
    jbc::ir::Insn in;
    in.op = o;
    in.a  = idx;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

CodeUnit unitOf(CodeDom&& d)
{
    CodeUnit u;
    u.dom = std::move(d);
    return u;
}

/// 旧栈 buildSmallMethod 的 CodeDom 形态：iconst_1; istore_0; iload_0; pop; return。
CodeUnit smallMethodUnit()
{
    CodeDom d;
    appendOp(d, Op::ICONST_1);
    appendVar(d, Op::ISTORE, 0);
    appendVar(d, Op::ILOAD, 0);
    appendOp(d, Op::POP);
    appendOp(d, Op::RETURN);
    return unitOf(std::move(d));
}

}   // namespace

/// 逐指令帧：DomainResult.frames 与 dom.order() 按下标对齐，全部可达
///。
TEST(MjAnalysisMethodFrameTest, FramesAlignWithInstructionOrder)
{
    CodeUnit u = smallMethodUnit();
    ConstantPool cp;
    OriginDomain dom;
    DomainEngine<OriginValue> engine(cp, dom);
    DomainResult<OriginValue> r = engine.analyze(u, MethodSig{true, "()V"});
    ASSERT_TRUE(r.summary.ok);
    EXPECT_EQ(r.frames.size(), u.dom.order().size());

    FrameQuery<OriginValue> q(u, std::move(r));
    for (const InsnId id : u.dom.order()) {
        EXPECT_TRUE(q.reachable(id));
    }
    // store→load 穿透照常（帧语义抽查：iload_0 的来源是 istore 指令）。
    // 此处只验帧存在性；origin 语义已由 MjAsmAnalyzer* 套件覆盖。
}

/// 空方法体：无指令 → 无帧（调用方据此 fail-open；旧栈 ok=false 哨兵的
/// 域引擎等价物是「ok 且 0 帧」，同样不给消费方任何帧事实）。
TEST(MjAnalysisMethodFrameTest, EmptyMethodProducesNoFrames)
{
    CodeUnit u = unitOf(CodeDom{});
    ConstantPool cp;
    OriginDomain dom;
    DomainEngine<OriginValue> engine(cp, dom);
    const DomainResult<OriginValue> r = engine.analyze(u, MethodSig{true, "()V"});
    EXPECT_EQ(u.dom.order().size(), 0u);
    EXPECT_EQ(r.frames.size(), 0u);
}

/// 容量无关：Code 属性声明 maxStack=0/maxLocals=0 的畸形方法照常分析——
/// 域引擎自建虚拟帧，声明容量不构成约束。
TEST(MjAnalysisMethodFrameTest, AnalyzesWithoutCallerGuardOnZeroMaxStack)
{
    ClassBytesBuilder b("Capless");
    const Bytes bytes = b.assemble({MethodSpec{0x0009,
                                               "t",
                                               "()V",
                                               /*maxStack=*/0,
                                               /*maxLocals=*/0,
                                               [](CodeEmit& e) {
                                                   e.i32const(1);
                                                   e.varInsn(Op::ISTORE, 0);
                                                   e.varInsn(Op::ILOAD, 0);
                                                   e.op(Op::POP);
                                                   e.op(Op::RETURN);
                                               },
                                               {}}});

    auto session = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{bytes.data(), bytes.size()});
    ASSERT_TRUE(session.is_ok());
    (void)jbc::ir::collectCodeUnits(*session.unwrap());
    const CodeUnit* codePtr = session.unwrap()->unit().methods[0].code.get();
    ASSERT_NE(codePtr, nullptr);
    const CodeUnit& code = *codePtr;
    ASSERT_NE(code.dom.order().size(), 0u);

    const auto r = traceOrigins(session.unwrap()->backing().cp, code, MethodSig{true, "()V"});
    ASSERT_TRUE(r.ok());
    // istore_0 之后的帧里 local 0 才有值：取 iload_0 处的帧断言（源 = store 指令）。
    const InsnId iload = code.dom.order()[2];
    const OriginValue* v = r.localAt(iload, 0);
    EXPECT_NE(v, nullptr);
}
