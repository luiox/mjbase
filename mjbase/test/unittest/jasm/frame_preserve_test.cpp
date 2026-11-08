// mjbase jasm 保留原帧档单测（issue #190）。
//
// 覆盖面：ResourceJasmService::assemble 的 FrameMode 两档——Regenerate 对坏帧
// 类如实失败（失败信号 = Error/Assemble 诊断，点名方法）；PreserveOriginal
// 保留原 StackMapTable 字节（code[] 逐字节对齐才挂回，版本回填原类），指令
// 面改动按 fail-closed 逐方法申报边界、事务不落改动。
//
// 夹具纪律（mjbase/doc/test-fixtures.rst）例外①申报，两条构造路线：
//   1. jsr/ret 子程序类（保留原帧档的目标面）：jsr 文本按夹具版本口径 ≤49
//      直装（jasm_fixture.hpp 头注「JSR/RET 形态给 ≤49」），DOM 单点抬
//      major 61 落盘——得到「分析干净 + v61 帧序列化必败」类（jsr 槽持
//      ReturnAddress，SMT 无法编码）。这是 preserve 档真正可救的坏帧形态，
//      头条用例钉：Regenerate 报 StackMapTable generation failed、preserve
//      落盘且版本回填。
//   2. branchy 类 1 字节 opcode 偏差（aconst_null→iconst_2，制造 int vs ref
//      合并 Bottom 槽）：jhasm 对此类数据流 Strict 拒绝一律硬失败，与目标
//      版本和 FrameMode 无关（探针实证）——偏差前后各一次 relaxed validate
//      自检，用于钉「分析失败类两档同败」的申报边界。
//
// 边界申报：preserve 档豁免的是帧序列化，不是数据流分析；字节码本身分析
// 不干净的类（类型冲突/栈深不符）两档同败，属如实申报的失败面。
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/jasm/resource_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "shared/jasm_fixture.hpp"

namespace mjbase::jasm {

using namespace mjbase::test;   // Bytes / assembleJasm / 例外① 自检夹具面

namespace {

model::ResourceContainer containerWith(const std::string& uri, const Bytes& bytes)
{
    model::ResourceContainer container;
    container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
    return container;
}

// 合法分支基类（major 61，join 合并两侧同为 ref——装配即生成 StackMapTable）。
// 指令流（fixture 钉面）：iload_0(1B) ifeq(3B) aconst_null(1B) goto(3B)
// aconst_null(1B) areturn(1B)；ifeq/goto 的操作数字节为 0x0007/0x0004，
// 全流仅两个 0x01 字节，即两条 aconst_null。
std::string branchClassSource()
{
    return R"JASM(.super java/lang/Object
.class public super example/BranchSubject {
  .method public static choose (Z)Ljava/lang/Object; {
    parameters: { flag },
    code: {
      iload flag
      ifeq right
      aconst_null
      goto join
    right:
      aconst_null
    join:
      areturn
    }
  }
}
)JASM";
}

// 构造路线②，单点最小偏差：首支 aconst_null(0x01) → iconst_2(0x05)，join
// 合并变 int vs ref → Bottom 槽。此类在 jhasm 数据流 Strict 阶段即被拒绝
// （硬失败，与目标版本和 FrameMode 无关，探针实证）——只用于钉「分析失败
// 类两档同败」的申报边界，不是 preserve 档的可救面。
Bytes deviateFirstAconstNull(const Bytes& legal)
{
    return mutateClassFile(legal, "deviate-branch-frame", [](jbc::core::ClassFile& cf) {
        for (auto& method : cf.methods) {
            if (cf.nameOf(method) != "choose") continue;
            for (auto& slot : method.attrs) {
                if (cf.utf8(slot.nameCp) != "Code") continue;
                auto decoded = cf.decodeAttr(slot);
                ASSERT_TRUE(decoded.is_ok()) << "夹具 Code 解码失败";
                auto* code = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
                bool patched = false;
                for (auto& byte : code->code) {
                    if (byte == 0x01) {
                        byte = 0x05;
                        patched = true;
                        break;
                    }
                }
                ASSERT_TRUE(patched) << "夹具指令流无 aconst_null";
                // AttrSlot 状态机：Decoded 态写回走源 span copy，改动会静默
                // 丢失——置脏 Modified 是字节偏差真正落盘的唯一通路。
                (void)slot.mutate();
            }
        }
    });
}

// jsr/ret 子程序类（保留原帧档的目标面）：构造路线①，≤49 直装 + DOM 抬 61。
// 指令流（探针实证）：jsr(3B) aconst_null(1B) areturn(1B) astore(2B) ret(2B)。
std::string subroutineClassSource()
{
    return R"JASM(.super java/lang/Object
.class public super example/SubroutineSubject {
  .method public static choose (Z)Ljava/lang/Object; {
    code: {
      jsr sub
      aconst_null
      areturn
    sub:
      astore 1
      ret 1
    }
  }
}
)JASM";
}

std::optional<Bytes> methodCodeOf(const Bytes& bytes, const std::string& name,
                                  const std::string& descriptor)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return std::nullopt;
    auto                  parsed = std::move(read).unwrap();
    jbc::core::ClassFile& file   = *parsed.file;
    for (auto& method : file.methods) {
        if (file.nameOf(method) != name || file.descOf(method) != descriptor) continue;
        for (auto& slot : method.attrs) {
            if (file.utf8(slot.nameCp) != "Code") continue;
            auto decoded = file.decodeAttr(slot);
            if (decoded.is_err()) return std::nullopt;
            auto* code = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
            return Bytes(code->code.begin(), code->code.end());
        }
    }
    return std::nullopt;
}

/// StackMapTable 属性内容字节（span 原样副本）；方法无该属性返回空向量。
std::optional<Bytes> stackMapTableOf(const Bytes& bytes, const std::string& name,
                                     const std::string& descriptor)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return std::nullopt;
    auto                  parsed = std::move(read).unwrap();
    jbc::core::ClassFile& file   = *parsed.file;
    for (auto& method : file.methods) {
        if (file.nameOf(method) != name || file.descOf(method) != descriptor) continue;
        for (auto& slot : method.attrs) {
            if (file.utf8(slot.nameCp) != "Code") continue;
            auto decoded = file.decodeAttr(slot);
            if (decoded.is_err()) return std::nullopt;
            auto* code = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
            for (const auto& nested : code->attrs) {
                if (file.utf8(nested.nameCp) != "StackMapTable") continue;
                const auto raw = file.source().spanOrEmpty(nested.data);
                return Bytes(raw.data(), raw.data() + raw.size());
            }
            return Bytes{};
        }
    }
    return std::nullopt;
}

/// jsr 文本 v49 直装（分析干净，无帧参与）→ DOM 抬 major 61——v61 + jsr/ret
/// 子程序类：数据流重算可过、StackMapTable 序列化必败。夹具自检 = v49 直装
/// 成功且产物无 SMT（v49 本无帧，preserve 档「原帧」对它即空集）。
Bytes subroutineClassAt61()
{
    const Bytes at49 = assembleJasm(subroutineClassSource(), 49);
    if (at49.empty()) return {};
    const auto smt = stackMapTableOf(at49, "choose", "(Z)Ljava/lang/Object;");
    if (!smt.has_value()) return {};
    EXPECT_TRUE(smt->empty())
        << "v49 直装产物不应带 StackMapTable（夹具形态自检）";
    return mutateClassFile(at49, "subroutine-major-bump", [](jbc::core::ClassFile& cf) {
        cf.major = 61;
    });
}

std::pair<ca::u16, ca::u16> classVersionOf(const Bytes& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return {0, 0};
    auto parsed = std::move(read).unwrap();
    return {parsed.file->major, parsed.file->minor};
}

/// 诊断集合里是否存在任意 Error 级条目（失败信号鲁棒断言——上游帧失败
/// 消息文本不属本仓契约，只锚严重级）。
testing::AssertionResult hasAnyErrorDiagnostic(const std::vector<Diagnostic>& diagnostics)
{
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error)
            return testing::AssertionSuccess();
    }
    testing::AssertionResult failure = testing::AssertionFailure()
                                       << "no error diagnostic; diagnostics:\n";
    for (const auto& diagnostic : diagnostics) {
        failure << "  - "
                << (diagnostic.severity == DiagnosticSeverity::Error ? "error" : "warning")
                << ": " << diagnostic.message.to_utf8_string().to_std_string() << "\n";
    }
    return failure;
}

/// 诊断集合里是否存在 Error 级、消息含 expect 片段的条目；未命中时把全部
/// 诊断打进失败消息供直读。
testing::AssertionResult containsErrorDiagnostic(const std::vector<Diagnostic>& diagnostics,
                                                 const std::string&             expect)
{
    for (const auto& diagnostic : diagnostics) {
        const std::string message = diagnostic.message.to_utf8_string().to_std_string();
        if (diagnostic.severity == DiagnosticSeverity::Error &&
            message.find(expect) != std::string::npos)
            return testing::AssertionSuccess();
    }
    testing::AssertionResult failure = testing::AssertionFailure()
                                       << "no error diagnostic containing \"" << expect
                                       << "\"; diagnostics:\n";
    for (const auto& diagnostic : diagnostics) {
        failure << "  - "
                << (diagnostic.severity == DiagnosticSeverity::Error ? "error" : "warning")
                << ": " << diagnostic.message.to_utf8_string().to_std_string() << "\n";
    }
    return failure;
}

}   // namespace

TEST(MjFramePreserveTest, RegenerateFailsLoudlyOnBadFrameClass)
{
    // 构造路线①：v61 + jsr/ret 子程序类——数据流干净、SMT 序列化必败，
    // Regenerate 档的失败信号如实在此。
    const Bytes badFrame = subroutineClassAt61();
    ASSERT_FALSE(badFrame.empty());

    auto container = containerWith("example/SubroutineSubject.class", badFrame);
    const auto disassembled =
        ResourceJasmService(container).disassemble("example/SubroutineSubject.class");
    ASSERT_TRUE(disassembled.isOk());
    const auto sourceName = jasmUtf16("example/SubroutineSubject.jasm");
    const auto sourceText = jasmUtf16(disassembled.value->source.to_utf8_string().to_std_string());

    JvmJasmContext context;
    context.validationMode = ValidationMode::Off;
    const auto applied     = ResourceJasmService(container)
                         .assemble("example/SubroutineSubject.class", sourceName.ref(),
                                   sourceText.ref(), context, FrameMode::Regenerate);
    ASSERT_FALSE(applied.isOk());
    // 失败信号锚帧序列化段（探针实证 libjhasm 0.3.3 钉版下的稳定消息）：
    // 区别于构造路线②的数据流硬失败，这正是 v49 杠杆可救的面。
    EXPECT_TRUE(containsErrorDiagnostic(applied.diagnostics,
                                        "StackMapTable generation failed"));
    // 失败不落改动：复读字节仍是容器字节。
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource("example/SubroutineSubject.class"));
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->get(), badFrame);
}

TEST(MjFramePreserveTest, PreserveOriginalLandsSubroutineClassOffGate)
{
    // 同一 v61 + jsr/ret 类走 PreserveOriginal：v49 杠杆跳过帧序列化后装配
    // 通过，事务落盘、版本回填原类；原类无帧（v49 直装产物），preserve 即
    // 保持无帧——不重生成、不凭空发帧。
    const Bytes badFrame = subroutineClassAt61();
    ASSERT_FALSE(badFrame.empty());
    const auto originalSmt =
        stackMapTableOf(badFrame, "choose", "(Z)Ljava/lang/Object;");
    ASSERT_TRUE(originalSmt.has_value());
    EXPECT_TRUE(originalSmt->empty());   // 夹具形态：v49 直装产物无帧

    auto container = containerWith("example/SubroutineSubject.class", badFrame);
    const auto disassembled =
        ResourceJasmService(container).disassemble("example/SubroutineSubject.class");
    ASSERT_TRUE(disassembled.isOk());
    const auto sourceName = jasmUtf16("example/SubroutineSubject.jasm");
    const auto sourceText = jasmUtf16(disassembled.value->source.to_utf8_string().to_std_string());

    JvmJasmContext context;
    context.validationMode = ValidationMode::Off;
    const auto applied     = ResourceJasmService(container)
                         .assemble("example/SubroutineSubject.class", sourceName.ref(),
                                   sourceText.ref(), context, FrameMode::PreserveOriginal);
    ASSERT_TRUE(applied.isOk())
        << "坏帧类在保留原帧档应可落盘（豁免帧序列化，不是豁免校验）";
    EXPECT_FALSE(applied.value->bytes.empty());

    // 版本回填：产物 major/minor 与原类一致（v49 只是汇编面跳帧杠杆）。
    const auto [major, minor] = classVersionOf(applied.value->bytes);
    EXPECT_EQ(major, 61);
    EXPECT_EQ(minor, 0);
    // 保持无帧：原类无 SMT，preserve 不得凭空发帧。
    const auto preservedSmt =
        stackMapTableOf(applied.value->bytes, "choose", "(Z)Ljava/lang/Object;");
    ASSERT_TRUE(preservedSmt.has_value());
    EXPECT_TRUE(preservedSmt->empty());
    // 指令面 = 提交文本的装配结果（文本来自容器反汇编，往返应回到原代码）。
    const auto appliedCode =
        methodCodeOf(applied.value->bytes, "choose", "(Z)Ljava/lang/Object;");
    const auto originalCode =
        methodCodeOf(badFrame, "choose", "(Z)Ljava/lang/Object;");
    ASSERT_TRUE(appliedCode.has_value());
    ASSERT_TRUE(originalCode.has_value());
    EXPECT_EQ(*appliedCode, *originalCode);
    // 事务落盘：容器字节 = 结果字节。
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource("example/SubroutineSubject.class"));
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->get(), applied.value->bytes);
}

TEST(MjFramePreserveTest, PreserveOriginalGraftsOriginalStackMapBytesOnCleanClass)
{
    // 构造路线①的对照面：干净 branchy 类（有合法 SMT）+ 未改动文本，
    // preserve 档把原 SMT 字节逐字节挂回产物（graft 而非重生成）。
    const Bytes legal = assembleJasm(branchClassSource(), 61);
    ASSERT_FALSE(legal.empty());
    expectValidateOk(legal);
    const auto containerSmt =
        stackMapTableOf(legal, "choose", "(Z)Ljava/lang/Object;");
    ASSERT_TRUE(containerSmt.has_value());
    EXPECT_FALSE(containerSmt->empty());   // major 61 分支方法必有帧

    auto container = containerWith("example/BranchSubject.class", legal);
    const auto disassembled =
        ResourceJasmService(container).disassemble("example/BranchSubject.class");
    ASSERT_TRUE(disassembled.isOk());
    const auto sourceName = jasmUtf16("example/BranchSubject.jasm");
    const auto sourceText = jasmUtf16(disassembled.value->source.to_utf8_string().to_std_string());

    JvmJasmContext context;
    context.validationMode = ValidationMode::Off;
    const auto applied     = ResourceJasmService(container)
                         .assemble("example/BranchSubject.class", sourceName.ref(),
                                   sourceText.ref(), context, FrameMode::PreserveOriginal);
    ASSERT_TRUE(applied.isOk());
    // SMT 逐字节 graft：与容器原帧内容恒等。
    const auto graftedSmt =
        stackMapTableOf(applied.value->bytes, "choose", "(Z)Ljava/lang/Object;");
    ASSERT_TRUE(graftedSmt.has_value());
    EXPECT_EQ(*graftedSmt, *containerSmt);
    // 版本不漂移、code 往返恒等。
    const auto [major, minor] = classVersionOf(applied.value->bytes);
    EXPECT_EQ(major, 61);
    EXPECT_EQ(minor, 0);
    const auto appliedCode =
        methodCodeOf(applied.value->bytes, "choose", "(Z)Ljava/lang/Object;");
    const auto originalCode =
        methodCodeOf(legal, "choose", "(Z)Ljava/lang/Object;");
    ASSERT_TRUE(appliedCode.has_value());
    ASSERT_TRUE(originalCode.has_value());
    EXPECT_EQ(*appliedCode, *originalCode);
    // 事务落盘。
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource("example/BranchSubject.class"));
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->get(), applied.value->bytes);
}

TEST(MjFramePreserveTest, PreserveOriginalRejectsInstructionChangeWithBoundaryReport)
{
    const Bytes deviated = deviateFirstAconstNull(assembleJasm(branchClassSource(), 61));
    ASSERT_FALSE(deviated.empty());
    expectValidateFail(deviated);

    auto container = containerWith("example/BranchSubject.class", deviated);
    // 提交合法基类文本：与容器内偏差类 code[] 不一致——原帧对指令流的描述
    // 失效，保留即产坏类，fail-closed 逐方法申报。
    const auto sourceName = jasmUtf16("example/BranchSubject.jasm");
    const auto sourceText = jasmUtf16(branchClassSource());
    JvmJasmContext context;
    context.validationMode = ValidationMode::Off;
    const auto applied     = ResourceJasmService(container)
                         .assemble("example/BranchSubject.class", sourceName.ref(),
                                   sourceText.ref(), context, FrameMode::PreserveOriginal);
    ASSERT_FALSE(applied.isOk());
    EXPECT_TRUE(containsErrorDiagnostic(
        applied.diagnostics, "instructions changed, the original StackMapTable no longer"));
    // 事务不落改动。
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource("example/BranchSubject.class"));
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->get(), deviated);
}

TEST(MjFramePreserveTest, PreserveOriginalRejectsNewMethodBodyOnFrameClass)
{
    const Bytes legal = assembleJasm(branchClassSource(), 61);
    ASSERT_FALSE(legal.empty());

    auto container = containerWith("example/BranchSubject.class", legal);
    // 合法文本 + 新增分支方法：新方法体无原帧可保留，v50+ 缺帧不可加载，
    // 保守拒收（不做逐方法分支分析）。
    const std::string editedSource = R"JASM(.super java/lang/Object
.class public super example/BranchSubject {
  .method public static choose (Z)Ljava/lang/Object; {
    parameters: { flag },
    code: {
      iload flag
      ifeq right
      aconst_null
      goto join
    right:
      aconst_null
    join:
      areturn
    }
  }
  .method public static extra (I)I {
    parameters: { n },
    code: {
      iload n
      ifeq other
      bipush 1
      goto done
    other:
      bipush 2
    done:
      ireturn
    }
  }
}
)JASM";
    const auto sourceName = jasmUtf16("example/BranchSubject.jasm");
    const auto sourceText = jasmUtf16(editedSource);
    JvmJasmContext context;
    context.validationMode = ValidationMode::Off;
    const auto applied     = ResourceJasmService(container)
                         .assemble("example/BranchSubject.class", sourceName.ref(),
                                   sourceText.ref(), context, FrameMode::PreserveOriginal);
    ASSERT_FALSE(applied.isOk());
    EXPECT_TRUE(containsErrorDiagnostic(
        applied.diagnostics, "new method body has no original StackMapTable to preserve"));
    // 事务不落改动。
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource("example/BranchSubject.class"));
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->get(), legal);
}

TEST(MjFramePreserveTest, PreserveOriginalPassesFrameFreeLinearMethod)
{
    // v61 线性方法：原类本就无 StackMapTable，保留原帧档无可失效面——
    // 指令改动放行，行为等同常规替换（不 sabotage 无帧类）。
    const Bytes legal = assembleJasm(R"JASM(.super java/lang/Object
.class public super example/LinearSubject {
  .method public static value ()I {
    code: {
      bipush 1
      ireturn
    }
  }
}
)JASM", 61);
    ASSERT_FALSE(legal.empty());
    expectValidateOk(legal);

    auto container = containerWith("example/LinearSubject.class", legal);
    const auto sourceName = jasmUtf16("example/LinearSubject.jasm");
    const auto sourceText = jasmUtf16(R"JASM(.super java/lang/Object
.class public super example/LinearSubject {
  .method public static value ()I {
    code: {
      bipush 2
      ireturn
    }
  }
}
)JASM");
    JvmJasmContext context;
    context.validationMode = ValidationMode::Off;
    const auto applied     = ResourceJasmService(container)
                         .assemble("example/LinearSubject.class", sourceName.ref(),
                                   sourceText.ref(), context, FrameMode::PreserveOriginal);
    ASSERT_TRUE(applied.isOk());
    const auto [major, minor] = classVersionOf(applied.value->bytes);
    EXPECT_EQ(major, 61);
    EXPECT_EQ(minor, 0);
    const auto smt = stackMapTableOf(applied.value->bytes, "value", "()I");
    ASSERT_TRUE(smt.has_value());
    EXPECT_TRUE(smt->empty());   // 线性方法无帧可保留
}

TEST(MjFramePreserveTest, PreserveOriginalComposesWithValidationGates)
{
    // 构造路线② + relaxed 门（申报边界用例）：int vs ref 偏差类在 jhasm
    // 数据流 Strict 阶段即被硬失败——jhasm 对 Strict 拒绝一律失败，与目标
    // 版本和 FrameMode 无关（探针实证）。preserve 档豁免的是帧序列化，不是
    // 数据流分析；分析失败类两档同败，本用例钉该边界下的失败信号与事务。
    const Bytes deviated = deviateFirstAconstNull(assembleJasm(branchClassSource(), 61));
    ASSERT_FALSE(deviated.empty());
    expectValidateFail(deviated);
    auto container = containerWith("example/BranchSubject.class", deviated);
    const auto disassembled =
        ResourceJasmService(container).disassemble("example/BranchSubject.class");
    ASSERT_TRUE(disassembled.isOk());
    const auto sourceName = jasmUtf16("example/BranchSubject.jasm");
    const auto sourceText = jasmUtf16(disassembled.value->source.to_utf8_string().to_std_string());
    JvmJasmContext context;
    context.validationMode = ValidationMode::Relaxed;
    const auto applied     = ResourceJasmService(container)
                         .assemble("example/BranchSubject.class", sourceName.ref(),
                                   sourceText.ref(), context, FrameMode::PreserveOriginal);
    ASSERT_FALSE(applied.isOk());
    EXPECT_TRUE(hasAnyErrorDiagnostic(applied.diagnostics));
    // 失败不落改动。
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource("example/BranchSubject.class"));
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->get(), deviated);
}

}   // namespace mjbase::jasm
