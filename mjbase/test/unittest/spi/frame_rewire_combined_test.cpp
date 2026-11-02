/// @file        frame_rewire_combined_test.cpp
/// @brief       frame_rewire 组合改写下的 Strict 帧再生对账（bozar
///              flow1+flow2 等价最小形态，morpher Combined KNOWN-ISSUE 锁定）。
///
/// 场景（对位 mjt-obf bozar_flow_jbc_test Combined 测试）：同一方法体被两个
/// 控制流改写 pass 先后作用——pass A（GETSTATIC/POP2/ICONST_1 + LOOKUPSWITCH
/// 前奏，命中面 invoke/NEW/field）与 pass B（ICONST_0 + LOOKUPSWITCH，命中面
/// invoke/NEW/field + IF）——两族 lookupswitch 块并存时曾挂 VerifyError
/// "Expecting a stack map frame"；单开任一 pass 逐条全过。
///
/// 根因（HotSpot split verifier 裁决）：线性校验循环对无落穿控制转移（goto/
/// switch/return/athrow）后的下一条指令索帧（verifier.cpp
/// verify_stackmap_table：no_control_flow 且下一帧 offset > bci 即挂），
/// 与可达性无关；smt_build 按 JVMS 口径只对数据流目标产帧，合成死块紧贴
/// switch 表之后就缺这条线性栅栏帧。修复：frame_rewire 在栅栏位置补帧
/// （状态借链中下一帧条目），整链重编码。
///
/// 诊断对账面：终产物字节里 StackMapTable 帧绝对 pc 集合须覆盖全部实际
/// 分支/switch 目标 pc；SMT 独有 pc 只允许是线性栅栏位置（无落穿转移的
/// 指令后继）。
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/insn.hpp"
#include "jbc/core/insn_codec.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

#include "jdk_fixture.hpp"
#include "test_helper.hpp"

namespace {

using Bytes = std::vector<ca::u8>;

using jbc::core::Op;
using jbc::core::Shape;
using jbc::ir::CodeEditor;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

// JVMS 4.5 access flags（构造面内联，与生产侧同款纪律）。
constexpr ca::u32 kAccStatic = 0x0008;

/// flow1 字段名（U+13E8，与 morpher 同一常量字节序列）。
constexpr std::string_view kFlow1Name = "\xE1\x8F\xA8";
/// flow2 字段名（U+13E9）。
constexpr std::string_view kFlow2Name = "\xE1\x8F\xA9";

/// lookupswitch 表长 2..4（morpher 同式）。
constexpr int kMinSwitchSize = 2;
constexpr int kMaxSwitchSize = 4;

// ── 夹具：morpher makeFlowFixtureClass 同构（compute 带 IF 分支 + main 打印）──

/// CodeDom 直构一段方法体（延后绑定态收尾全量补验 + 一次 lower 落 backing）。
void addBodyMethod(jbc::ir::ClassSession& s, std::string_view name,
                   std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals,
                   const std::function<void(CodeEditor&, jbc::ir::CodeUnit&)>& fill)
{
    auto added = s.addMethod({name, desc, 0x0009, true, maxStack, maxLocals});
    ASSERT_TRUE(added.is_ok());
    const ca::usize idx = s.unit().methods.size() - 1;
    auto promoted = jbc::ir::promoteMethod(s, idx);
    ASSERT_TRUE(promoted.is_ok());
    auto code = std::move(promoted).unwrap();

    code->dom.beginDeferredTargets();
    CodeEditor editor(*code);
    fill(editor, *code);
    auto closed = code->dom.endDeferredTargets();
    ASSERT_TRUE(closed.is_ok()) << "分支目标未全部锚定: " << std::string(name);

    auto lowered = jbc::ir::lowerMethod(s, idx);
    ASSERT_TRUE(lowered.is_ok());
}

/// 夹具会话：compute(int) 带 IFGE 分支；main 打印（GETSTATIC + INVOKEVIRTUAL
/// 是 flow 命中面）。V52 缺省版本；SMT 由 attachFrames 补（对位 COMPUTE_FRAMES）。
std::shared_ptr<jbc::ir::ClassSession> makeFixtureSession()
{
    jbc::ir::NewClassSeed seed;
    seed.thisName  = "T";
    seed.superName = "java/lang/Object";
    auto made = jbc::ir::makeSession(seed);
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return nullptr;
    auto session = std::move(made).unwrap();

    // compute：ILOAD 0; IFGE L; ICONST_M1; IRETURN; L: ILOAD 0; ICONST_1; IADD; IRETURN
    addBodyMethod(*session, "compute", "(I)I", 2, 1,
                  [](CodeEditor& ed, jbc::ir::CodeUnit&) {
                      const LabelId elseL = ed.createLabel().unwrap();
                      (void)ed.append(CodeEditor::local(Op::ILOAD, 0)).unwrap();
                      (void)ed.append(CodeEditor::branch(Op::IFGE, elseL)).unwrap();
                      (void)ed.append(CodeEditor::simple(Op::ICONST_M1)).unwrap();
                      (void)ed.append(CodeEditor::simple(Op::IRETURN)).unwrap();
                      const InsnId cont = ed.append(CodeEditor::local(Op::ILOAD, 0)).unwrap();
                      ASSERT_TRUE(ed.bindLabel(elseL, cont).is_ok());
                      (void)ed.append(CodeEditor::simple(Op::ICONST_1)).unwrap();
                      (void)ed.append(CodeEditor::simple(Op::IADD)).unwrap();
                      (void)ed.append(CodeEditor::simple(Op::IRETURN)).unwrap();
                  });

    // main：GETSTATIC System.out; LDC; INVOKEVIRTUAL println; RETURN
    addBodyMethod(*session, "main", "([Ljava/lang/String;)V", 2, 1,
                  [&session](CodeEditor& ed, jbc::ir::CodeUnit&) {
                      const ca::u32 outCp  = session->internFieldRef(
                          "java/lang/System", "out", "Ljava/io/PrintStream;");
                      const ca::u32 textCp = session->internString(
                          "val=42|compute(-5)=-1|compute(5)=6");
                      const ca::u32 printlnCp = session->internMethodRef(
                          "java/io/PrintStream", "println", "(Ljava/lang/String;)V");
                      (void)ed.append(CodeEditor::cpInsn(Op::GETSTATIC, outCp)).unwrap();
                      (void)ed.append(CodeEditor::loadConst(textCp)).unwrap();
                      (void)ed.append(CodeEditor::cpInsn(Op::INVOKEVIRTUAL, printlnCp))
                          .unwrap();
                      (void)ed.append(CodeEditor::simple(Op::RETURN)).unwrap();
                  });
    if (testing::Test::HasFatalFailure()) return nullptr;
    return session;
}

/// 注入 static long 字段（morpher injectFlow1/2FieldJbc 同构；ConstantValue
/// 附带，字段值不参与行为面）。
void injectLongField(jbc::ir::ClassSession& session, std::string_view name,
                     ca::u32 access, long long value)
{
    jbc::ir::FieldSeed fseed;
    fseed.name   = name;
    fseed.desc   = "J";
    fseed.access = access;
    auto field   = session.addField(fseed);
    ASSERT_TRUE(field.is_ok());
    jbc::ir::ConstValueSpec cvalue;
    cvalue.kind = jbc::ir::ConstValueSpec::Kind::Long;
    cvalue.num  = value;
    ASSERT_TRUE(session.attachConstantValue(field.unwrap(), cvalue).is_ok());
    ASSERT_TRUE(session.commitFieldMetadata(field.unwrap()).is_ok());
}

// ── bozar 形态插入（morpher controlflow/bozar_flow_jbc.cpp 机械对位）──

/// 32 位无放回采样（morpher uniqueRandomInts 同式）。
std::vector<ca::i32> uniqueRandomInts(std::mt19937_64& rng, int size)
{
    std::vector<ca::i32> result;
    result.reserve(static_cast<std::size_t>(size));
    std::uniform_int_distribution<ca::i32> dist;
    for (int i = 0; i < size; ++i) {
        for (int tries = 0; tries < 64; ++tries) {
            const ca::i32 v = dist(rng);
            if (std::find(result.begin(), result.end(), v) == result.end()) {
                result.push_back(v);
                break;
            }
        }
        if (static_cast<int>(result.size()) <= i) {
            result.push_back(result.empty() ? 0 : result.back() + 1);
        }
    }
    return result;
}

/// 命中面判定：invoke/NEW/field（flow1 与 flow2 共享面）。
bool isInvokeFieldOp(Op op)
{
    switch (op) {
    case Op::INVOKEVIRTUAL:
    case Op::INVOKESPECIAL:
    case Op::INVOKESTATIC:
    case Op::INVOKEINTERFACE:
    case Op::INVOKEDYNAMIC:
    case Op::GETSTATIC:
    case Op::PUTSTATIC:
    case Op::GETFIELD:
    case Op::PUTFIELD:
    case Op::NEW:
        return true;
    default:
        return false;
    }
}

/// IF 家族（flow2 独有面）。
bool isIfOp(Op op)
{
    switch (op) {
    case Op::IFEQ:
    case Op::IFNE:
    case Op::IFLT:
    case Op::IFGE:
    case Op::IFGT:
    case Op::IFLE:
    case Op::IF_ICMPEQ:
    case Op::IF_ICMPNE:
    case Op::IF_ICMPLT:
    case Op::IF_ICMPGE:
    case Op::IF_ICMPGT:
    case Op::IF_ICMPLE:
    case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE:
    case Op::IFNULL:
    case Op::IFNONNULL:
        return true;
    default:
        return false;
    }
}

/// 命中点快照（先收集后插入，morpher collectTargets 同构）。
std::vector<InsnId> collectTargets(const jbc::ir::CodeUnit& code, bool includeIf)
{
    std::vector<InsnId> targets;
    for (const InsnId id : code.dom.order()) {
        const Insn* in = code.dom.peek(id);
        if (in == nullptr) continue;
        if (isInvokeFieldOp(in->op) || (includeIf && isIfOp(in->op))) {
            targets.push_back(id);
        }
    }
    return targets;
}

/// 在命中指令前插 prelude + LOOKUPSWITCH（全分支含 default 锚定命中指令，
/// morpher insertLookupSwitchBefore 同构）。
bool insertSwitchBefore(CodeEditor& ed, InsnId target, const std::vector<Insn>& prelude,
                        std::mt19937_64& rng)
{
    auto fallResult = ed.createLabel(target);
    if (fallResult.is_err()) return false;
    const LabelId fall = fallResult.unwrap();

    const int switchSize = kMinSwitchSize
                           + static_cast<int>(rng() % static_cast<ca::u64>(
                               kMaxSwitchSize - kMinSwitchSize + 1));
    std::vector<ca::i32> keys = uniqueRandomInts(rng, switchSize - 1);
    std::sort(keys.begin(), keys.end());

    for (const Insn& pre : prelude) {
        if (ed.insertBefore(target, pre).is_err()) return false;
    }
    Insn sw;
    sw.op     = Op::LOOKUPSWITCH;
    sw.target = fall;
    sw.keys   = keys;
    sw.targets.assign(static_cast<std::size_t>(switchSize - 1), fall);
    return ed.insertBefore(target, std::move(sw)).is_ok();
}

/// flow1 等价改写：GETSTATIC/POP2/ICONST_1 前奏，命中面 invoke/NEW/field。
bool flow1Rewrite(jbc::ir::ClassSession& session, jbc::ir::CodeUnit& code,
                  std::mt19937_64& rng)
{
    if (code.dom.size() == 0) return false;
    jbc::ir::ClassUnit& unit     = session.unit();
    const std::string_view owner = unit.symbols().str(unit.thisClass);
    const ca::u32 fieldCp        = session.internFieldRef(owner, kFlow1Name, "J");

    Insn getstatic;
    getstatic.op = Op::GETSTATIC;
    getstatic.cp = fieldCp;
    const std::vector<Insn> prelude{getstatic, CodeEditor::simple(Op::POP2),
                                    CodeEditor::simple(Op::ICONST_1)};

    const std::vector<InsnId> targets = collectTargets(code, /*includeIf=*/false);
    if (targets.empty()) return false;

    CodeEditor editor(code);
    bool changed = false;
    for (const InsnId target : targets) {
        changed |= insertSwitchBefore(editor, target, prelude, rng);
    }
    if (changed) code.maxStack = std::max<ca::u16>(code.maxStack, 64);
    return changed;
}

/// flow2 等价改写：方法头 ACONST_NULL/ASTORE maxLocals+4 预置 + ICONST_0 前奏，
/// 命中面 invoke/NEW/field + IF。
bool flow2Rewrite(jbc::ir::ClassSession&, jbc::ir::CodeUnit& code, std::mt19937_64& rng)
{
    if (code.dom.size() == 0) return false;
    const int slot = static_cast<int>(code.maxLocals) + 4;
    if (slot + 1 > 65535) return false;

    CodeEditor editor(code);
    (void)editor.insertAtStart(CodeEditor::local(Op::ASTORE, static_cast<ca::u32>(slot)))
        .unwrap();
    (void)editor.insertAtStart(CodeEditor::simple(Op::ACONST_NULL)).unwrap();
    code.maxLocals = std::max<ca::u16>(code.maxLocals, static_cast<ca::u16>(slot + 1));

    const std::vector<InsnId> targets = collectTargets(code, /*includeIf=*/true);
    const std::vector<Insn> prelude{CodeEditor::simple(Op::ICONST_0)};

    bool changed = true;
    for (const InsnId target : targets) {
        (void)insertSwitchBefore(editor, target, prelude, rng);
    }
    code.maxStack = std::max<ca::u16>(code.maxStack, 64);
    return changed;
}

// ── 方法 pass 包装 + 管线驱动 ──

/// 借用外部 pass 实例的适配器（PassManager unique_ptr 独占，借引用需壳）。
class SharedPassAdapter final : public mjbase::spi::MethodPass
{
public:
    explicit SharedPassAdapter(mjbase::spi::MethodPass* inner) : inner_(inner) {}

    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&            scope,
                                       mjbase::spi::MethodAnalysisManager& am) override
    {
        return inner_->run(scope, am);
    }

private:
    mjbase::spi::MethodPass* inner_;
};

/// 单 flow pass（flow1 或 flow2，轮驱动形态）。
template<bool kIsFlow1>
class FlowPass final : public mjbase::spi::MethodPass
{
public:
    explicit FlowPass(std::mt19937_64& rng) : rng_(rng) {}

    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&            scope,
                                       mjbase::spi::MethodAnalysisManager&) override
    {
        const bool changed = kIsFlow1 ? flow1Rewrite(scope.session, scope.code, rng_)
                                      : flow2Rewrite(scope.session, scope.code, rng_);
        return changed ? mjbase::spi::PreservedAnalyses::none()
                       : mjbase::spi::PreservedAnalyses::all();
    }

private:
    std::mt19937_64& rng_;
};

/// 组合单轮 pass（morpher BozarFlowBodyJbcPass 同序：heavy 先于 light）。
class CombinedPass final : public mjbase::spi::MethodPass
{
public:
    explicit CombinedPass(std::mt19937_64& rng) : rng_(rng) {}

    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&            scope,
                                       mjbase::spi::MethodAnalysisManager&) override
    {
        bool changed = flow2Rewrite(scope.session, scope.code, rng_);
        changed |= flow1Rewrite(scope.session, scope.code, rng_);
        return changed ? mjbase::spi::PreservedAnalyses::none()
                       : mjbase::spi::PreservedAnalyses::all();
    }

private:
    std::mt19937_64& rng_;
};

/// 给会话内全部方法补 SMT（fix_pattern_fixture attachFrames 同构）：真 JVM
/// 验证器对 V50+ 分支方法要求帧表。
void attachFrames(jbc::ir::ClassSession& session,
                  mjbase::spi::StrictTypeResolverAdapter& resolver)
{
    for (ca::usize idx = 0; idx < session.unit().methods.size(); ++idx) {
        auto promoted = jbc::ir::promoteMethod(session, idx);
        ASSERT_TRUE(promoted.is_ok());
        auto code = std::move(promoted).unwrap();
        auto lowered = jbc::ir::lowerMethod(session, idx);
        ASSERT_TRUE(lowered.is_ok());

        const auto& unit = session.unit();
        const auto& mu   = unit.methods[idx];
        auto scope = std::make_unique<mjbase::spi::MethodScope>(mjbase::spi::MethodScope{
            mjbase::spi::MethodTarget{mjbase::MemberKey{}, mu.id,
                                      unit.symbols().str(mu.name),
                                      unit.symbols().str(mu.desc), mu.access, session},
            *code.get()});
        mjbase::spi::frame_rewire::rewireFramesAfterLower(session, *scope, resolver);
    }
}

/// 构造初始类字节（夹具 + 双字段注入 + SMT 补齐），对位 morpher 夹具产物。
Bytes makeBaseClass(std::mt19937_64& rng)
{
    auto session = makeFixtureSession();
    if (session == nullptr) return {};
    injectLongField(*session, kFlow1Name, kAccStatic, 0x1122334455667788LL);
    injectLongField(*session, kFlow2Name, kAccStatic | 0x0002, 0);
    if (testing::Test::HasFatalFailure()) return {};

    mjbase::KeyPool                        pool;
    mjbase::hierarchy::ClassHierarchy      hierarchy(pool);
    mjbase::spi::CommonSuperClassResolver  superResolver(hierarchy);
    mjbase::spi::StrictTypeResolverAdapter resolver(superResolver);
    attachFrames(*session, resolver);
    if (testing::Test::HasFatalFailure()) return {};

    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// 一轮方法管线（typeResolver 注入，morpher 轮驱动同构）。
void runRound(jbc::ir::ClassSession& session, mjbase::spi::MethodPass& pass)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::MethodAnalysisManager mam(arena);
    mjbase::KeyPool                    pool;
    mjbase::spi::MethodPassManager     mpm;
    mpm.addPass(std::make_unique<SharedPassAdapter>(&pass));

    mjbase::hierarchy::ClassHierarchy hierarchy(pool);
    auto added = hierarchy.addClass(mjbase::hierarchy::fillSummary(session, pool));
    ASSERT_TRUE(added.is_ok());

    mjbase::spi::CommonSuperClassResolver  superResolver(hierarchy);
    mjbase::spi::StrictTypeResolverAdapter typeResolver(superResolver);
    mjbase::spi::PassPipelineOptions       options;
    options.typeResolver = &typeResolver;

    (void)mjbase::spi::runMethodPipelineOverSession(session, mpm, mam, pool, nullptr,
                                                    options);
}

/// runRound 的致命失败传播壳（非 void 驱动函数里的 gtest 规约形态）。
bool runRoundChecked(jbc::ir::ClassSession& session, mjbase::spi::MethodPass& pass)
{
    runRound(session, pass);
    return !testing::Test::HasFatalFailure();
}

/// 驱动形态：单轮组合 / 双轮（flow2 轮后 flow1 轮，morpher 生产次序）。
Bytes driveCombined(const Bytes& base, bool singleRound, std::mt19937_64& rng)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{base});
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();

    if (singleRound) {
        CombinedPass pass(rng);
        if (!runRoundChecked(*session, pass)) return {};
    }
    else {
        {
            FlowPass<false> pass(rng);   // heavy（flow2）先行
            if (!runRoundChecked(*session, pass)) return {};
        }
        {
            FlowPass<true> pass(rng);   // light（flow1）后行
            if (!runRoundChecked(*session, pass)) return {};
        }
    }

    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// 单 flow 基线（回归面：单开必须过 JVM verify）。
Bytes driveSingle(const Bytes& base, bool flow1, std::mt19937_64& rng)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{base});
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();

    if (flow1) {
        FlowPass<true> pass(rng);
        if (!runRoundChecked(*session, pass)) return {};
    }
    else {
        FlowPass<false> pass(rng);
        if (!runRoundChecked(*session, pass)) return {};
    }

    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

// ── 诊断对账：SMT 帧绝对 pc 集合 vs 分支目标 + 线性栅栏位置 ──

/// 无落穿控制转移（与 frame_rewire::cutsControlFlow 同判据，测试面独立复刻）。
bool testCutsControlFlow(jbc::core::Op op)
{
    switch (op) {
    case Op::GOTO:
    case Op::GOTO_W:
    case Op::TABLESWITCH:
    case Op::LOOKUPSWITCH:
    case Op::IRETURN:
    case Op::LRETURN:
    case Op::FRETURN:
    case Op::DRETURN:
    case Op::ARETURN:
    case Op::RETURN:
    case Op::ATHROW:
        return true;
    default:
        return false;
    }
}

struct AlignmentReport
{
    bool                          ok = false;
    std::set<ca::u32>             smtPcs;
    std::set<ca::u32>             branchPcs;
    std::set<ca::u32>             cutSuccs;   ///< 无落穿转移的指令后继（栅栏候选）
    std::vector<ca::u32>          smtOnly;
    std::vector<ca::u32>          branchOnly;
};

/// 第 methodIdx 个方法的对账（字节面：readClass → decodeCode → decodeInsns /
/// SMT 嵌套属性链算术）。判据：分支目标全被 SMT 覆盖（branchOnly 空）；
/// SMT 独有 pc 只允许是栅栏位置（无落穿转移的线性后继）。
AlignmentReport alignOfMethod(const Bytes& bytes, ca::usize methodIdx)
{
    AlignmentReport rep;
    auto read = jbc::core::readClass(jbc::core::ByteView{bytes});
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return rep;
    auto cf = std::move(read).unwrap();
    if (methodIdx >= cf.file->methods.size()) {
        ADD_FAILURE() << "methodIdx 越界";
        return rep;
    }
    auto codeR = cf.file->decodeCode(cf.file->methods[methodIdx]);
    EXPECT_TRUE(codeR.is_ok());
    if (codeR.is_err()) return rep;
    jbc::core::CodeAttr* code = codeR.unwrap();

    // 实际分支目标 pc：Branch2/4 目标 + switch default/cases；栅栏候选 =
    // 无落穿转移的下一条指令 pc（RawInsn 升序，后继取下一条的 pc）。
    auto insnsR = jbc::core::decodeInsns(jbc::core::ByteView{code->code});
    EXPECT_TRUE(insnsR.is_ok());
    if (insnsR.is_err()) return rep;
    const std::vector<jbc::core::RawInsn> insns = std::move(insnsR).unwrap();
    for (ca::usize i = 0; i < insns.size(); ++i) {
        const jbc::core::RawInsn& r = insns[i];
        const Shape sh = jbc::core::shapeOf(r.op);
        if (sh == Shape::Branch2 || sh == Shape::Branch4 || sh == Shape::TableSwitch
            || sh == Shape::LookupSwitch)
        {
            rep.branchPcs.insert(static_cast<ca::u32>(
                static_cast<ca::i64>(r.pc) + r.t));
            if (sh == Shape::TableSwitch) {
                for (const ca::i32 off : r.offs) {
                    rep.branchPcs.insert(static_cast<ca::u32>(
                        static_cast<ca::i64>(r.pc) + off));
                }
            }
            else if (sh == Shape::LookupSwitch) {
                for (const jbc::core::SwitchCase& c : r.cases) {
                    rep.branchPcs.insert(static_cast<ca::u32>(
                        static_cast<ca::i64>(r.pc) + c.off));
                }
            }
        }
        if (testCutsControlFlow(r.op) && i + 1 < insns.size()) {
            rep.cutSuccs.insert(insns[i + 1].pc);
        }
    }

    // SMT 帧绝对 pc（链算术与 ir::relocateSmt 同源）。
    for (jbc::core::AttrSlot& slot : code->attrs) {
        if (!cf.file->cp.utf8Equals(slot.nameCp, "StackMapTable")) continue;
        auto body = cf.file->decodeAttr(slot);
        EXPECT_TRUE(body.is_ok());
        if (body.is_err()) return rep;
        const auto& smt = *static_cast<const jbc::core::StackMapTableAttr*>(body.unwrap());
        ca::u32 prevAbs = 0;
        for (ca::usize i = 0; i < smt.frames.size(); ++i) {
            const ca::u32 abs = i == 0 ? smt.frames[i].offset
                                       : jbc::core::smtAbsPc(prevAbs, smt.frames[i].offset);
            rep.smtPcs.insert(abs);
            prevAbs = abs;
        }
    }

    std::set_difference(rep.smtPcs.begin(), rep.smtPcs.end(), rep.branchPcs.begin(),
                        rep.branchPcs.end(), std::back_inserter(rep.smtOnly));
    std::set_difference(rep.branchPcs.begin(), rep.branchPcs.end(), rep.smtPcs.begin(),
                        rep.branchPcs.end(), std::back_inserter(rep.branchOnly));
    rep.ok = rep.branchOnly.empty();
    for (const ca::u32 pc : rep.smtOnly) {
        if (rep.cutSuccs.count(pc) == 0) rep.ok = false;
    }
    return rep;
}

// ── JVM 运行面（JDK 缺失 → SKIP）──

/// 跑 T.main 并归一化输出；verify=true 严格校验。失败时诊断写 *diagnostic。
bool runJvmChecked(const Bytes& cls, std::string* outOrDiagnostic)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        if (outOrDiagnostic != nullptr) *outOrDiagnostic = tc.reason;
        return false;
    }
    test::jdk::RunnerSpec spec;
    spec.classes.emplace_back("T", cls);
    spec.runnerBody =
        "T.main(new String[0]); "
        "if (T.compute(-5) != -1) throw new AssertionError(\"compute(-5)\"); "
        "if (T.compute(5) != 6) throw new AssertionError(\"compute(5)\");";
    const test::jdk::JvmRunOutcome r = test::jdk::runWithRunnerOutcome(tc, spec);
    if (outOrDiagnostic != nullptr) *outOrDiagnostic = r.output;
    return r.status == test::jdk::JvmRunStatus::Ok;
}

// ── 测试 ──

/// 结构对账面：组合改写产物里 SMT 帧目标集合必须覆盖全部实际分支目标，
/// SMT 独有 pc 只允许是线性栅栏位置（不依赖 JDK，门禁恒跑）。单 flow
/// 形态作对照（无栅栏时两集合恒等）。
TEST(FrameRewireCombined, SmtTargetSetMatchesBranchTargets)
{
    std::mt19937_64 rng(12345);
    const Bytes base = makeBaseClass(rng);
    ASSERT_FALSE(base.empty());

    // 单 flow 对照：目标集恒等（switch 后继即目标，无栅栏缺口）。
    {
        std::mt19937_64 rng1(12345);
        const Bytes     out = driveSingle(base, /*flow1=*/true, rng1);
        ASSERT_FALSE(out.empty());
        const AlignmentReport rep = alignOfMethod(out, 0);
        EXPECT_TRUE(rep.ok) << "flow1 单开目标集错位 smtOnly=" << rep.smtOnly.size()
                            << " branchOnly=" << rep.branchOnly.size();
    }
    // 组合双轮（morpher 生产次序）：曾复现点——栅栏帧修复后 SMT 覆盖
    // 全部目标 + 死块入口栅栏位。
    {
        std::mt19937_64 rng2(12345);
        const Bytes     out = driveCombined(base, /*singleRound=*/false, rng2);
        ASSERT_FALSE(out.empty());
        // 逐方法对账（方法 0 = compute 单族死块无栅栏；方法 1 = main 双族
        // 死块入口是栅栏帧的判别面）。
        for (ca::usize m = 0; m < 2; ++m) {
            const AlignmentReport rep = alignOfMethod(out, m);
            EXPECT_TRUE(rep.ok) << "组合双轮 方法 " << m << " 目标集错位："
                                << "smtOnly=" << rep.smtOnly.size() << " branchOnly="
                                << rep.branchOnly.size();
            for (const ca::u32 pc : rep.smtOnly) {
                EXPECT_GT(rep.cutSuccs.count(pc), 0u)
                    << "  方法 " << m << " SMT 独有 pc 非栅栏位置 pc=" << pc;
            }
            for (const ca::u32 pc : rep.branchOnly) {
                ADD_FAILURE() << "  方法 " << m << " 分支独有目标 pc=" << pc;
            }
        }
        const AlignmentReport mainRep = alignOfMethod(out, 1);
        EXPECT_GE(mainRep.smtOnly.size(), 2u)
            << "组合 main 栅栏帧缺失（两族死块入口未补帧）";
    }
    // 组合单轮（同 pass 内 heavy→light）。
    {
        std::mt19937_64 rng3(12345);
        const Bytes     out = driveCombined(base, /*singleRound=*/true, rng3);
        ASSERT_FALSE(out.empty());
        for (ca::usize m = 0; m < 2; ++m) {
            const AlignmentReport rep = alignOfMethod(out, m);
            EXPECT_TRUE(rep.ok) << "组合单轮 方法 " << m << " 目标集错位";
            for (const ca::u32 pc : rep.smtOnly) {
                EXPECT_GT(rep.cutSuccs.count(pc), 0u)
                    << "  方法 " << m << " SMT 独有 pc 非栅栏位置 pc=" << pc;
            }
            for (const ca::u32 pc : rep.branchOnly) {
                ADD_FAILURE() << "  方法 " << m << " 分支独有目标 pc=" << pc;
            }
        }
        const AlignmentReport mainRep = alignOfMethod(out, 1);
        EXPECT_GE(mainRep.smtOnly.size(), 2u)
            << "组合单轮 main 栅栏帧缺失（两族死块入口未补帧）";
    }
}

/// JVM 硬验收：单开逐条全过 + 组合形态 verify=true 可加载（栅栏帧修复后）。
TEST(FrameRewireCombined, JvmVerifySingleAndCombined)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    std::mt19937_64 rng(12345);
    const Bytes     base = makeBaseClass(rng);
    ASSERT_FALSE(base.empty());
    std::string diag;

    // 基线（未改写）可加载。
    EXPECT_TRUE(runJvmChecked(base, &diag)) << "baseline: " << diag;

    // 单开 flow1 / flow2。
    {
        std::mt19937_64 r1(12345);
        const Bytes     out = driveSingle(base, true, r1);
        ASSERT_FALSE(out.empty());
        EXPECT_TRUE(runJvmChecked(out, &diag)) << "flow1: " << diag;
    }
    {
        std::mt19937_64 r2(12345);
        const Bytes     out = driveSingle(base, false, r2);
        ASSERT_FALSE(out.empty());
        EXPECT_TRUE(runJvmChecked(out, &diag)) << "flow2: " << diag;
    }

    // 组合双轮（morpher 生产次序）：曾挂 VerifyError @ 死块入口。
    {
        std::mt19937_64 r3(12345);
        const Bytes     out = driveCombined(base, false, r3);
        ASSERT_FALSE(out.empty());
        EXPECT_TRUE(runJvmChecked(out, &diag)) << "combined-two-rounds: " << diag;
    }

    // 组合单轮。
    {
        std::mt19937_64 r4(12345);
        const Bytes     out = driveCombined(base, true, r4);
        ASSERT_FALSE(out.empty());
        EXPECT_TRUE(runJvmChecked(out, &diag)) << "combined-single-round: " << diag;
    }
}

}   // namespace
