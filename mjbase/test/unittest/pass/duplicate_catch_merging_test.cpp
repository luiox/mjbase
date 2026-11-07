// DuplicateCatchMerging 单测：重复 catch 表项合并面（表序相邻 + 同 handler
// 锚点 + 同 catch 类型面 + 区间无间隙 → 单条目保护区并集）。覆盖：相切合并
// （同型 / catch-any / 三游程收口）、相交 / 包含 / 左相交合并、方法末悬空
// end 合并、零指令改写与 handler 入口 pc 不变、拒绝面（异型 / 有间隙 / 异
// handler 同体 / 表序中隔第三方表项）、无表零改、幂等二跑、注册面与 PA 纪律。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService，汇编器自带
// frame 分析与验证；handler 体只经异常派发可达，pop 收口即帧合法），固定
// 片段与逐用例指令体 / 表项文本全部 R"JASM 定界，变量插入处保留 + 拼接。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/pass/duplicate_catch_merging.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// UTF-8 文本 → UTF-16（jasm 源入口口径，enum_name_restorer 同式）。
ca::str::Utf16String utf16Of(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 源码 → class 字节（汇编失败逐条报诊断并致命，夹具错误不静默；
/// 失败时附整份源码与行号定位，供 CI 日志直读）。
Bytes assembleJasm(const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    const auto           sourceName = utf16Of("dcm_fixture.jasm");
    const auto           sourceText = utf16Of(source);
    const auto           result     = service.assemble(sourceName.ref(), sourceText.ref());
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

/// 单方法夹具骨架：t/C.m ()V，exceptions 块与 code 体由用例给文本
/// （raw string 定界的固定片段）。
Bytes oneMethodClass(std::string_view excEntries, std::string_view codeBody)
{
    const std::string source = R"JASM(.super java/lang/Object
.class public super t/C {
  .method public static m ()V {
    exceptions: {
)JASM" + std::string(excEntries) + R"JASM(    },
    code: {
)JASM" + std::string(codeBody) + R"JASM(    }
  }
}
)JASM";
    return assembleJasm(source);
}

/// 写回字节异常表的位置视图（end 悬空 = 方法末语义，取指令数）。
struct ExcPos
{
    ca::usize   start   = 0;
    ca::usize   end     = 0;
    ca::usize   handler = 0;
    std::string type;
};

std::vector<ExcPos> excOf(const Inspection& ins)
{
    const auto ids = ins.cu->dom.order();
    std::vector<std::pair<jbc::ir::LabelId, ca::usize>> labelPos;
    for (ca::usize i = 0; i < ids.size(); ++i) {
        for (const jbc::ir::LabelId l : ins.cu->dom.labelsAt(ids[i])) {
            labelPos.emplace_back(l, i);
        }
    }
    const auto find = [&](const jbc::ir::LabelId l) {
        for (const auto& [label, pos] : labelPos) {
            if (label == l) return pos;
        }
        return ids.size();   // end 边界标签（方法末语义）
    };
    std::vector<ExcPos> out;
    for (const auto& e : ins.cu->exc) {
        out.push_back({find(e.start), find(e.end), find(e.handler),
                       e.catchCp == 0 ? std::string{"*"}
                                      : std::string{ins.session->backing().className(e.catchCp)}});
    }
    return out;
}

// ── 夹具文本库（raw string 定界；布局注记见各用例）────────────────────

/// 两段保护区 + 共享 handler：E1=[0,2) E2=[2,4) H@4，同型 java/lang/Exception。
constexpr std::string_view kExcAdjacent =
    R"JASM(      { L0, L2, L4, java/lang/Exception },
      { L2, L4, L4, java/lang/Exception }
)JASM";

/// 保护区各以 return 收口 + handler pop;return（handler 仅异常派发可达，
/// pop 收口帧合法）的六指令体：L0..L4 布局。
constexpr std::string_view kCodeAdjacent =
    R"JASM(      L0:
      nop
      return
      L2:
      nop
      return
      L4:
      pop
      return
)JASM";

}   // namespace

// ── 相切合并：同 handler 同型相邻区间 → 单条目 [0,4)，指令零改写 ────────
TEST(DuplicateCatchMergingTest, adjacentSameHandlerSameTypeMerged)
{
    const Bytes in = oneMethodClass(kExcAdjacent, kCodeAdjacent);
    const MethodPassRun r = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    // 指令序列零改写（handler 入口 pc 不变的载体面）。
    EXPECT_EQ(opsOf(ins), (std::vector<Op>{Op::NOP, Op::RETURN, Op::NOP, Op::RETURN,
                                           Op::POP, Op::RETURN}));
    const std::vector<ExcPos> exc = excOf(ins);
    ASSERT_EQ(exc.size(), 1u);
    EXPECT_EQ(exc[0].start, 0u);
    EXPECT_EQ(exc[0].end, 4u);
    EXPECT_EQ(exc[0].handler, 4u);   // handler 入口 pc 不变
    EXPECT_EQ(exc[0].type, "java/lang/Exception");
}

// ── 异型不并（联合面扩 any 拒绝）：同 handler 相邻区间不同 catch 型 ─────
TEST(DuplicateCatchMergingTest, differentCatchTypeKept)
{
    const std::string_view exc = R"JASM(      { L0, L2, L4, java/lang/Exception },
      { L2, L4, L4, java/lang/Throwable }
)JASM";
    const Bytes         in = oneMethodClass(exc, kCodeAdjacent);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);   // 零字节改动
    EXPECT_EQ(excOf(inspect(r.out)).size(), 2u);
}

// ── catch-any 对合并：双方 catchCp 同为 0 ──────────────────────────────
TEST(DuplicateCatchMergingTest, catchAnyPairMerged)
{
    const std::string_view exc = R"JASM(      { L0, L2, L4, * },
      { L2, L4, L4, * }
)JASM";
    const Bytes         in = oneMethodClass(exc, kCodeAdjacent);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<ExcPos> out = excOf(inspect(r.out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].type, "*");   // any 面保持 any，不扩大
    EXPECT_EQ(out[0].start, 0u);
    EXPECT_EQ(out[0].end, 4u);
}

// ── 三游程收口：同 handler 同型三连区间一轮归一 ────────────────────────
TEST(DuplicateCatchMergingTest, runOfThreeCollapsed)
{
    const std::string_view exc = R"JASM(      { L0, L2, L6, java/lang/Exception },
      { L2, L4, L6, java/lang/Exception },
      { L4, L6, L6, java/lang/Exception }
)JASM";
    const std::string_view code = R"JASM(      L0:
      nop
      return
      L2:
      nop
      return
      L4:
      nop
      return
      L6:
      pop
      return
)JASM";
    const Bytes         in = oneMethodClass(exc, code);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<ExcPos> out = excOf(inspect(r.out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].start, 0u);
    EXPECT_EQ(out[0].end, 6u);
    EXPECT_EQ(out[0].handler, 6u);
}

// ── 相交合并：[0,4) + [2,6) → [0,6) ───────────────────────────────────
TEST(DuplicateCatchMergingTest, overlapMerged)
{
    const std::string_view exc = R"JASM(      { L0, L4, L6, java/lang/Exception },
      { L2, L6, L6, java/lang/Exception }
)JASM";
    const std::string_view code = R"JASM(      L0:
      nop
      nop
      L2:
      nop
      return
      L4:
      nop
      return
      L6:
      pop
      return
)JASM";
    const Bytes         in = oneMethodClass(exc, code);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<ExcPos> out = excOf(inspect(r.out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].start, 0u);
    EXPECT_EQ(out[0].end, 6u);
}

// ── 包含合并：[0,6) 吸收 [2,4)（被并项区间为保留项子集）─────────────────
TEST(DuplicateCatchMergingTest, containmentMerged)
{
    const std::string_view exc = R"JASM(      { L0, L6, L6, java/lang/Exception },
      { L2, L4, L6, java/lang/Exception }
)JASM";
    const std::string_view code = R"JASM(      L0:
      nop
      nop
      L2:
      nop
      return
      L4:
      nop
      return
      L6:
      pop
      return
)JASM";
    const Bytes         in = oneMethodClass(exc, code);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<ExcPos> out = excOf(inspect(r.out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].start, 0u);
    EXPECT_EQ(out[0].end, 6u);
}

// ── 左相交合并：表序 [2,5) 后随 [0,3) → 并集 [0,5)，start 标签取更小方 ──
TEST(DuplicateCatchMergingTest, leftOverlapMerged)
{
    const std::string_view exc = R"JASM(      { L2, L5, L6, java/lang/Exception },
      { L0, L3, L6, java/lang/Exception }
)JASM";
    const std::string_view code = R"JASM(      L0:
      nop
      nop
      L2:
      nop
      L3:
      return
      nop
      L5:
      return
      L6:
      pop
      return
)JASM";
    const Bytes         in = oneMethodClass(exc, code);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<ExcPos> out = excOf(inspect(r.out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].start, 0u);
    EXPECT_EQ(out[0].end, 5u);
    EXPECT_EQ(out[0].handler, 6u);
}

// ── 方法末悬空 end 合并：[0,2) + [2,End) → [0,End)，end 保持方法末语义 ──
TEST(DuplicateCatchMergingTest, endLabelAtMethodEndMerged)
{
    const std::string_view exc = R"JASM(      { L0, L2, L4, java/lang/Exception },
      { L2, Lend, L4, java/lang/Exception }
)JASM";
    const std::string_view code = R"JASM(      L0:
      nop
      return
      L2:
      nop
      return
      L4:
      pop
      return
      Lend:
)JASM";
    const Bytes         in = oneMethodClass(exc, code);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection         ins = inspect(r.out);
    const std::vector<ExcPos> out = excOf(ins);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].start, 0u);
    EXPECT_EQ(out[0].end, opsOf(ins).size());   // end 悬空 = 方法末语义保持
    EXPECT_EQ(out[0].handler, 4u);
}

// ── 有间隙不并：[0,2) + [3,5)（间隙 pc2 会并入保护区，改派发行为）───────
TEST(DuplicateCatchMergingTest, gapKept)
{
    const std::string_view exc = R"JASM(      { L0, L2, L5, java/lang/Exception },
      { L3, L5, L5, java/lang/Exception }
)JASM";
    const std::string_view code = R"JASM(      L0:
      nop
      return
      L2:
      nop
      L3:
      nop
      return
      L5:
      pop
      return
)JASM";
    const Bytes         in = oneMethodClass(exc, code);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
    EXPECT_EQ(excOf(inspect(r.out)).size(), 2u);
}

// ── 异 handler 同体不并（Recaf 体去重面不在本件）：pop;return 双 handler
//    体完全一致但锚点不同 → 保留 ────────────────────────────────────────
TEST(DuplicateCatchMergingTest, sameBodyDifferentHandlerKept)
{
    const std::string_view exc = R"JASM(      { L0, L2, L4, java/lang/Exception },
      { L2, L4, L6, java/lang/Exception }
)JASM";
    const std::string_view code = R"JASM(      L0:
      nop
      return
      L2:
      nop
      return
      L4:
      pop
      return
      L6:
      pop
      return
)JASM";
    const Bytes         in = oneMethodClass(exc, code);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
    const std::vector<ExcPos> out = excOf(inspect(r.out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].handler, 4u);
    EXPECT_EQ(out[1].handler, 6u);
}

// ── 表序中隔第三方表项不并：E1/E2 同 handler 同型相切，但 M 居表序中位
//    （E2 保护区内异常点改写后会跳过 M 的先中权）→ 保留 ─────────────────
TEST(DuplicateCatchMergingTest, interleavingEntryKept)
{
    const std::string_view exc = R"JASM(      { L0, L2, L4, java/lang/Exception },
      { L1, L3, L6, java/lang/Throwable },
      { L2, L4, L4, java/lang/Exception }
)JASM";
    const std::string_view code = R"JASM(      L0:
      nop
      L1:
      nop
      L2:
      nop
      L3:
      return
      L4:
      pop
      return
      L6:
      pop
      return
)JASM";
    const Bytes         in = oneMethodClass(exc, code);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
    const std::vector<ExcPos> out = excOf(inspect(r.out));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].start, 0u);
    EXPECT_EQ(out[0].end, 2u);
    EXPECT_EQ(out[1].start, 1u);
    EXPECT_EQ(out[1].end, 3u);
    EXPECT_EQ(out[2].start, 2u);
    EXPECT_EQ(out[2].end, 4u);
}

// ── 无异常表方法：零改直通 ────────────────────────────────────────────
TEST(DuplicateCatchMergingTest, noExcTableNoop)
{
    const std::string_view code = R"JASM(      nop
      return
)JASM";
    const std::string source = R"JASM(.super java/lang/Object
.class public super t/C {
  .method public static m ()V {
    code: {
)JASM" + std::string(code) + R"JASM(    }
  }
}
)JASM";
    const Bytes         in = assembleJasm(source);
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 幂等二跑：合并后二跑 no-op 且表内容恒等 ───────────────────────────
TEST(DuplicateCatchMergingTest, idempotentSecondRun)
{
    const Bytes in = oneMethodClass(kExcAdjacent, kCodeAdjacent);
    const MethodPassRun  r = runMethodPass(in, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r.pa.changed());
    const MethodPassRun r2 = runMethodPass(r.out, mjbase::pass::DuplicateCatchMerging{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
    EXPECT_EQ(r2.out, r.out);
    const std::vector<ExcPos> first  = excOf(inspect(r.out));
    const std::vector<ExcPos> second = excOf(inspect(r2.out));
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].start, first[0].start);
    EXPECT_EQ(second[0].end, first[0].end);
    EXPECT_EQ(second[0].handler, first[0].handler);
    EXPECT_EQ(second[0].type, first[0].type);
}

// ── 注册面：名字可解析且 scope 为方法级 ───────────────────────────────
TEST(DuplicateCatchMergingTest, registeredInRegistry)
{
    const mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    const auto                           e   = reg.find("DuplicateCatchMerging");
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.unwrap().scope, mjbase::spi::PassScope::Method);
}

// ── 注册面：经注册表工厂产出可用实例 ─────────────────────────────────
TEST(DuplicateCatchMergingTest, creatableByName)
{
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("DuplicateCatchMerging");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
