/// @file        merge_plan_test.cpp
/// @brief       MergePlan 测试：往返等价验收（outline → inline 对拍原文）+
///              识别判据 fail-closed 逐条钉死 + JVM 真跑门（JDK 缺失 SKIP）。
///
/// 往返口径：语义等价门 = JVM 实际运行对拍（-Xverify:all）+ 帧形/指令
/// 序列规范化比较——不做字节级逐位相等（合并产物保留显式编码 aload 0
/// 而原文可为紧凑 aload_0；字节级目标需要编码规范化器，不在本门口径
/// 内）。规范化口径：紧凑局部形态归一显式形 + 局部槽按首次出现重编号
/// （无盖章自足槽 fresh 分配的结构等价口径；盖章后自足槽原槽号 exact
/// 对回，见 merge_plan.hpp 头注）+ 分支目标归一到指令序。
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/jbc.hpp"

#include "mjbase/asm/insn/insn_op.hpp"
#include "mjbase/asm/sem/merge_plan.hpp"
#include "mjbase/asm/sem/outline_plan.hpp"
#include "mjbase/asm/sem/region_view.hpp"

#include "sem_fixture.hpp"

#include "jdk_fixture_gtest.hpp"

namespace {

using jbc::core::Op;
using jbc::ir::CodeUnit;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::asm_sem::InlineRequest;
using mjbase::asm_sem::MergePlan;
using mjbase::asm_sem::OutlineError;
using mjbase::asm_sem::OutlinePlan;
using mjbase::asm_sem::OutlineRequest;
using mjbase::asm_sem::RegionCandidate;
using mjbase::asm_sem::RegionView;

/// 三元形态样本（同 outline_plan_test 的 pickSpec）：then 臂 B（iload a;
/// iconst 2; imul; goto E）为可 outline 直线区域。
MethodSpec pickSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIIZ)I";
    m.maxStack  = 4;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);
        e.varInsn(Op::ILOAD, 0);        /// B
        e.op(Op::ICONST_2);
        e.op(Op::IMUL);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C（fallthrough → E）
        e.bindLabel(join);
        e.op(Op::ICONST_0);             /// E
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// goto 入边 + 局部参数样本：区域头标签 = B 臂
/// goto 目标，区域 = [iload 2, goto]。
MethodSpec gotoEntrySpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(III)I";
    m.maxStack  = 3;
    m.maxLocals = 3;
    m.emit      = [](CodeEmit& e) {
        const std::size_t join    = e.newLabel();   /// 0
        const std::size_t regHead = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 0);
        e.varInsn(Op::ILOAD, 1);
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, join);
        e.varInsn(Op::ISTORE, 2);       /// B 臂：t2 = b
        e.branch(Op::GOTO, regHead);
        e.bindLabel(regHead);
        e.varInsn(Op::ILOAD, 2);        /// 区域成员（槽 2 局部入参）
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 边界 DUP 形态样本（调用点侧栈形重排那一档）：B 臂 = [dup, goto E]，
/// 出口栈顶为入口值的复制 → retIsEntryCopy（必测形态：调用点复位 DUP 消解）。
MethodSpec boundaryDupSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(I)I";
    m.maxStack  = 3;
    m.maxLocals = 1;
    m.emit      = [](CodeEmit& e) {
        const std::size_t ret0 = e.newLabel();   /// 0
        const std::size_t join = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 0);        /// A：[a]
        e.op(Op::DUP);
        e.branch(Op::IFEQ, ret0);
        e.op(Op::DUP);                  /// B：[a] → [a, a]，goto join
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::POP);
        e.op(Op::IRETURN);
        e.bindLabel(ret0);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 自足槽样本：区域先写后读 t（读的到达定义全在区域内）→ t 不进参数
/// 集；盖章（kOutlineStampAttr）档回迁时原槽号 exact 对回，无章档父
/// 槽号不可恢复 → fresh 分配（规范化对齐，兼容外来形态）。
MethodSpec selfSufficientSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IZ)I";
    m.maxStack  = 3;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 1);
        e.branch(Op::IFEQ, elseArm);
        e.op(Op::BIPUSH);
        e.u8(7);
        e.varInsn(Op::ISTORE, 3);       /// t = 7（区域内定义）
        e.varInsn(Op::ILOAD, 3);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::ICONST_0);
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 栈切片样本：区域消费入口栈值（pop 后自产新值）——出口 [7] 与入口 [a]
/// 前缀失配 → a 为 Stack 参数（调用点免装载）；装载链回溯可含原文
/// iload（栈参边界滑动的对偶档，不改变合并结果）。
MethodSpec stackSliceSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIZ)I";
    m.maxStack  = 3;
    m.maxLocals = 3;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 0);        /// [a]
        e.varInsn(Op::ILOAD, 2);        /// [a, f]
        e.branch(Op::IFEQ, elseArm);    /// 弹 f：两臂均携 [a]
        e.op(Op::POP);                  /// B：消费切片 a
        e.op(Op::BIPUSH);
        e.u8(7);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::POP);                  /// C：纯净对照臂
        e.op(Op::BIPUSH);
        e.u8(9);
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 无关方法（调用方错配 / 自动发现落空用例的第二个消费方）。
MethodSpec otherSpec()
{
    MethodSpec m;
    m.name      = "other";
    m.desc      = "()I";
    m.maxStack  = 1;
    m.maxLocals = 0;
    m.emit      = [](CodeEmit& e) {
        e.op(Op::ICONST_5);
        e.op(Op::IRETURN);
    };
    return m;
}

struct InlineContext
{
    SemFixture                   fixture;
    CodeUnit*                    unit = nullptr;
    RegionView                   view{};
    std::vector<RegionCandidate> candidates;
    jbc::analysis::ShapeResolver resolver;

    explicit InlineContext(const Bytes& bytes)
        : fixture(bytes)
    {
        unit = fixture.code("pick");
        auto built = RegionView::build(*unit);
        if (built.is_ok()) {
            view = std::move(built).unwrap();
            candidates = view.candidates();
        }
    }

    jbc::ir::MethodId pickId() const
    {
        return fixture.session->unit().findMethodId("pick");
    }

    jbc::analysis::MethodSig sig() const
    {
        const jbc::ir::ClassUnit&  cu = fixture.session->unit();
        const jbc::ir::MethodUnit* mu = cu.method(pickId());
        return sigOf(true, cu.symbols().str(mu->desc), "pick", "T");
    }
};

/// outline（首个通过过滤且可识别的候选）；全拒时 ADD_FAILURE。
/// 自动发现可能选中同一样本里另一可接受区域（如纯值臂），形态定制的
/// 往返用谓词钉住目标候选。
template <typename Pred>
OutlinePlan outlineWhere(InlineContext& ctx, Pred pred, ca::u32 seq = 1)
{
    for (const RegionCandidate& c : ctx.candidates) {
        if (!pred(c)) {
            continue;
        }
        auto planned = mjbase::asm_sem::planOutline(
            OutlineRequest{*ctx.fixture.session, ctx.pickId(), ctx.sig(),
                           ctx.resolver, ctx.view, c, seq});
        if (planned.is_ok()) {
            return std::move(planned).unwrap();
        }
    }
    ADD_FAILURE() << "no outlineable candidate matching filter";
    return OutlinePlan{};
}

/// outline（首个可识别候选）→ apply；全拒时 ADD_FAILURE。
OutlinePlan outlineFirst(InlineContext& ctx, ca::u32 seq = 1)
{
    return outlineWhere(ctx, [](const RegionCandidate&) { return true; }, seq);
}

/// 局部操作数助手（与 asm/sem 实现同口径，测试规范化用）。
ca::u32 normLocalSlot(const jbc::ir::Insn& in) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(in.op);
    const bool compact = (v >= static_cast<ca::u8>(Op::ILOAD_0)
                          && v <= static_cast<ca::u8>(Op::ALOAD_3))
                         || (v >= static_cast<ca::u8>(Op::ISTORE_0)
                             && v <= static_cast<ca::u8>(Op::ASTORE_3));
    if (compact) {
        const ca::u8 base = v <= static_cast<ca::u8>(Op::ALOAD_3)
                                ? static_cast<ca::u8>(Op::ILOAD_0)
                                : static_cast<ca::u8>(Op::ISTORE_0);
        return static_cast<ca::u32>(v - base) % 4u;
    }
    return static_cast<ca::u32>(in.a);
}

Op normExplicitLocal(Op op) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(op);
    if (v >= static_cast<ca::u8>(Op::ILOAD_0) && v <= static_cast<ca::u8>(Op::ILOAD_3)) return Op::ILOAD;
    if (v >= static_cast<ca::u8>(Op::LLOAD_0) && v <= static_cast<ca::u8>(Op::LLOAD_3)) return Op::LLOAD;
    if (v >= static_cast<ca::u8>(Op::FLOAD_0) && v <= static_cast<ca::u8>(Op::FLOAD_3)) return Op::FLOAD;
    if (v >= static_cast<ca::u8>(Op::DLOAD_0) && v <= static_cast<ca::u8>(Op::DLOAD_3)) return Op::DLOAD;
    if (v >= static_cast<ca::u8>(Op::ALOAD_0) && v <= static_cast<ca::u8>(Op::ALOAD_3)) return Op::ALOAD;
    if (v >= static_cast<ca::u8>(Op::ISTORE_0) && v <= static_cast<ca::u8>(Op::ISTORE_3)) return Op::ISTORE;
    if (v >= static_cast<ca::u8>(Op::LSTORE_0) && v <= static_cast<ca::u8>(Op::LSTORE_3)) return Op::LSTORE;
    if (v >= static_cast<ca::u8>(Op::FSTORE_0) && v <= static_cast<ca::u8>(Op::FSTORE_3)) return Op::FSTORE;
    if (v >= static_cast<ca::u8>(Op::DSTORE_0) && v <= static_cast<ca::u8>(Op::DSTORE_3)) return Op::DSTORE;
    if (v >= static_cast<ca::u8>(Op::ASTORE_0) && v <= static_cast<ca::u8>(Op::ASTORE_3)) return Op::ASTORE;
    return op;
}

/// 规范化指令序列：op + 操作数（局部槽首次出现重编号 / 分支目标指令序）。
struct NormInsn
{
    int    op     = 0;
    ca::i32 a     = 0;
    ca::i32 target = -1;
};

std::vector<NormInsn> normalizedInsns(jbc::ir::ClassSession& session, jbc::ir::MethodId id)
{
    std::vector<NormInsn> out;
    jbc::ir::MethodUnit* mu = session.unit().method(id);
    if (mu == nullptr || mu->code == nullptr) {
        return out;
    }
    CodeUnit& unit = *mu->code;
    const std::vector<jbc::ir::InsnId>& order = unit.dom.order();

    /// 第一遍：原始序列（op / 局部槽首次出现重编号 / 分支目标指令序）+
    /// goto→下一条 的冗余转移标记（fallthrough ≡ goto-next：outline 剥离
    /// 块间 glue goto，inline 回迁为直落——规范化两侧同删）。
    struct RawEntry
    {
        int     op     = 0;
        ca::i32 a      = 0;
        ca::i32 target = -1;
        bool    drop   = false;
    };
    std::vector<RawEntry> raws;
    std::unordered_map<ca::u32, ca::i32> canon;
    ca::i32 next = 0;
    for (ca::usize i = 0; i < order.size(); ++i) {
        const jbc::ir::Insn* in = unit.dom.peek(order[i]);
        if (in == nullptr) {
            continue;
        }
        RawEntry n;
        n.op = static_cast<int>(in->op);
        if (mjbase::asm_insn::isLoad(in->op) || mjbase::asm_insn::isStore(in->op)
            || in->op == Op::IINC) {
            const ca::u32 s = normLocalSlot(*in);
            auto it = canon.find(s);
            if (it == canon.end()) {
                it = canon.emplace(s, next++).first;
            }
            n.op = static_cast<int>(normExplicitLocal(in->op));
            n.a  = it->second;
        } else if (mjbase::asm_insn::isBranch(in->op) || mjbase::asm_insn::isSwitch(in->op)) {
            auto res = unit.dom.resolveLabel(in->target);
            if (res.is_ok()) {
                for (ca::usize j = 0; j < order.size(); ++j) {
                    if (order[j].slot == res.unwrap().insn.slot) {
                        n.target = static_cast<ca::i32>(j);
                        if (in->op == Op::GOTO && j == i + 1u) {
                            n.drop = true;
                        }
                        break;
                    }
                }
            }
        } else {
            n.a = in->a;
        }
        raws.push_back(n);
    }
    std::vector<ca::i32> remap(raws.size(), -1);
    ca::i32 kept = 0;
    for (ca::usize i = 0; i < raws.size(); ++i) {
        if (!raws[i].drop) {
            remap[i] = kept++;
        }
    }
    for (ca::usize i = 0; i < raws.size(); ++i) {
        if (raws[i].drop) {
            continue;
        }
        NormInsn n;
        n.op     = raws[i].op;
        n.a      = raws[i].a;
        n.target = raws[i].target >= 0 ? remap[static_cast<ca::usize>(raws[i].target)] : -1;
        out.push_back(n);
    }
    return out;
}

void expectSameInsns(const std::vector<NormInsn>& before, const std::vector<NormInsn>& after)
{
    ASSERT_EQ(after.size(), before.size());
    for (ca::usize i = 0; i < before.size(); ++i) {
        EXPECT_EQ(after[i].op, before[i].op) << "insn[" << i << "]";
        EXPECT_EQ(after[i].a, before[i].a) << "insn[" << i << "]";
        EXPECT_EQ(after[i].target, before[i].target) << "insn[" << i << "]";
    }
}

/// 往返公共体（谓词选候选版）：outline → inline（自动发现）→ apply；返回
/// inline 后会话（session 出参）与 pick 身份（同一会话口径）。
template <typename Pred>
void roundtripWhere(const Bytes&                          raw,
                    Pred                                  pred,
                    std::shared_ptr<jbc::ir::ClassSession>& sessionOut,
                    jbc::ir::MethodId&                     pickIdOut)
{
    InlineContext ctx{raw};
    pickIdOut = ctx.pickId();
    OutlinePlan plan = outlineWhere(ctx, pred);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok()) << mjbase::asm_sem::to_string(applied.unwrap_err());

    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, pickIdOut, ctx.resolver, {}});
    ASSERT_TRUE(inlined.is_ok()) << mjbase::asm_sem::to_string(inlined.unwrap_err());
    MergePlan merge = std::move(inlined).unwrap();
    auto done = merge.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(done.is_ok()) << mjbase::asm_sem::to_string(done.unwrap_err());
    sessionOut = ctx.fixture.session;
}

/// 往返公共体：outline → inline（自动发现）→ apply；返回 inline 后会话
/// （session 出参）与 pick 身份（同一会话口径）。
void roundtripOutlineInline(const Bytes& raw,
                            std::shared_ptr<jbc::ir::ClassSession>& sessionOut,
                            jbc::ir::MethodId&                      pickIdOut)
{
    roundtripWhere(raw, [](const RegionCandidate&) { return true; },
                   sessionOut, pickIdOut);
}

/// 原文（raw）的 pick 规范化序列（独立会话基线）。
std::vector<NormInsn> normalizedBaseline(const Bytes& raw)
{
    auto made = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{raw.data(), raw.size()});
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) {
        return {};
    }
    auto session = std::move(made).unwrap();
    auto visits  = jbc::ir::collectCodeUnits(*session);
    for (const jbc::ir::CodeVisit& v : visits) {
        if (v.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::MethodUnit& mu = session->unit().methods[v.methodIdx];
        if (session->unit().symbols().str(mu.name) == "pick") {
            return normalizedInsns(*session, mu.id);
        }
    }
    ADD_FAILURE() << "baseline pick missing";
    return {};
}

/// N 的 Code 嵌套盖章行（结构断言用）：kOutlineStampAttr RawAttr 解
/// u16 计数 + (nSlot, parentSlot) 对序列。
std::vector<std::pair<ca::u32, ca::u32>> stampRowsOf(jbc::ir::ClassSession& session,
                                                     jbc::ir::MethodId id)
{
    std::vector<std::pair<ca::u32, ca::u32>> rows;
    jbc::core::ClassFile&      cf = session.backing();
    jbc::ir::MethodUnit*       mu = session.unit().method(id);
    if (mu == nullptr) {
        return rows;
    }
    for (jbc::core::AttrSlot& slot : cf.methods[mu->backingIndex].attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "Code")) {
            continue;
        }
        auto code = cf.decodeAttr(slot);
        if (code.is_err()) {
            return rows;
        }
        for (jbc::core::AttrSlot& nested :
             static_cast<jbc::core::CodeAttr*>(code.unwrap())->attrs) {
            if (!cf.cp.utf8Equals(nested.nameCp,
                                  mjbase::asm_sem::kOutlineStampAttr)) {
                continue;
            }
            auto body = cf.decodeAttr(nested);
            if (body.is_err()) {
                continue;
            }
            const std::vector<ca::u8>& b =
                static_cast<const jbc::core::RawAttr*>(body.unwrap())->bytes();
            if (b.size() < 2u) {
                continue;
            }
            const ca::u32 count = (static_cast<ca::u32>(b[0]) << 8) | b[1];
            for (ca::u32 i = 0; i < count && 2u + (i + 1u) * 4u <= b.size(); ++i) {
                rows.emplace_back(
                    (static_cast<ca::u32>(b[2u + i * 4u]) << 8) | b[3u + i * 4u],
                    (static_cast<ca::u32>(b[4u + i * 4u]) << 8) | b[5u + i * 4u]);
            }
        }
    }
    return rows;
}

}   // namespace

/// 往返（局部参数 + 自动发现）：outline → inline 后父方法规范化序列与原文
/// 一致，N 已删除，调用点消失。
TEST(MjAsmSemMergePlanTest, RoundtripThenArm)
{
    ClassBytesBuilder b("T");
    Bytes             raw = b.assemble({pickSpec()});
    const std::vector<NormInsn> before = normalizedBaseline(raw);

    std::shared_ptr<jbc::ir::ClassSession> session;
    jbc::ir::MethodId                      pickId{};
    roundtripOutlineInline(raw, session, pickId);

    expectSameInsns(before, normalizedInsns(*session, pickId));

    /// N 已删除：类内无 outline$ 方法残留。
    for (const jbc::ir::MethodUnit& m : session->unit().methods) {
        EXPECT_NE(session->unit().symbols().str(m.name), "outline$pick$1");
    }
}

/// 往返（指定 callee 模式）：InlineRequest.callee 指向 outline 产物。
TEST(MjAsmSemMergePlanTest, RoundtripTargetedCallee)
{
    ClassBytesBuilder b("T");
    Bytes             raw = b.assemble({pickSpec()});
    const std::vector<NormInsn> before = normalizedBaseline(raw);

    InlineContext ctx{raw};
    const jbc::ir::MethodId pickId = ctx.pickId();
    OutlinePlan   plan = outlineFirst(ctx);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, pickId, ctx.resolver, outlinedId});
    ASSERT_TRUE(inlined.is_ok()) << mjbase::asm_sem::to_string(inlined.unwrap_err());
    MergePlan merge = std::move(inlined).unwrap();
    EXPECT_FALSE(merge.retIsEntryCopy);
    ASSERT_TRUE(merge.apply(*ctx.fixture.session, ctx.resolver).is_ok());

    expectSameInsns(before, normalizedInsns(*ctx.fixture.session, pickId));
}

/// 往返（调用点侧栈形重排那一档的必测形态）：retIsEntryCopy 的调用点复位 DUP 消解，
/// 区域尾 DUP 随体回迁，合并后与原文一致。
TEST(MjAsmSemMergePlanTest, RoundtripBoundaryDupEntryCopy)
{
    ClassBytesBuilder b("T");
    Bytes             raw = b.assemble({boundaryDupSpec()});
    const std::vector<NormInsn> before = normalizedBaseline(raw);

    std::shared_ptr<jbc::ir::ClassSession> session;
    jbc::ir::MethodId                      pickId{};
    roundtripOutlineInline(raw, session, pickId);

    expectSameInsns(before, normalizedInsns(*session, pickId));
}

/// 往返（goto 入边 + 局部参数）：装载链尾锚定标签，回迁体插回标签边界。
/// 谓词钉住单块候选（regHead：参数槽的 reaching def 在区域外的形态）——
/// 自动发现会选中含 B 臂的双块候选，其中槽定义在区域内（自足）而非参数。
TEST(MjAsmSemMergePlanTest, RoundtripGotoEntry)
{
    ClassBytesBuilder b("T");
    Bytes             raw = b.assemble({gotoEntrySpec()});
    const std::vector<NormInsn> before = normalizedBaseline(raw);

    std::shared_ptr<jbc::ir::ClassSession> session;
    jbc::ir::MethodId                      pickId{};
    roundtripWhere(raw, [](const RegionCandidate& c) { return c.members.size() == 1u; },
                   session, pickId);

    expectSameInsns(before, normalizedInsns(*session, pickId));
}

/// 往返（多块区域 + glue goto + 自足槽盖章）：outline 剥离块间
/// goto-to-next，inline 回迁为直落（fallthrough ≡ goto-next，规范化两侧
/// 同删）。区域 = [istore t, goto][iload t, goto]：入口栈切片 [b] 成栈参
/// （体序幕 iload 0 承载，inline 侧按 k 档剥离）；t 先写后读为自足槽——
/// outline 侧盖章 (nSlot=1, parentSlot=2)，inline 消费后原槽号 exact 对回
/// （不靠 fresh 槽撞对原文槽号的形态巧合，合并产物与原文规范化逐位一致）。
TEST(MjAsmSemMergePlanTest, RoundtripMultiBlockGlueGoto)
{
    ClassBytesBuilder b("T");
    Bytes             raw = b.assemble({gotoEntrySpec()});
    const std::vector<NormInsn> before = normalizedBaseline(raw);

    InlineContext ctx{raw};
    const jbc::ir::MethodId pickId = ctx.pickId();
    OutlinePlan plan = outlineWhere(
        ctx, [](const RegionCandidate& c) { return c.members.size() == 2u; });
    ASSERT_EQ(plan.params.size(), 1u);
    EXPECT_TRUE(plan.params[0].kind == mjbase::asm_sem::SlotCopy::Kind::Stack);
    bool selfOld = false;
    for (const auto& row : plan.locals) {
        selfOld = selfOld || row.oldSlot == 2u;
    }
    EXPECT_TRUE(selfOld) << "自足槽 t（父槽 2）在重编号表内";
    /// 盖章处方：t 在 N 内占槽 1（栈参 b 占槽 0），原文父槽 2。
    ASSERT_EQ(plan.selfStamps.size(), 1u);
    EXPECT_EQ(plan.selfStamps[0].nSlot, 1u);
    EXPECT_EQ(plan.selfStamps[0].parentSlot, 2u);
    ASSERT_TRUE(plan.apply(*ctx.fixture.session, ctx.resolver).is_ok());

    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, pickId, ctx.resolver, {}});
    ASSERT_TRUE(inlined.is_ok()) << mjbase::asm_sem::to_string(inlined.unwrap_err());
    MergePlan merge = std::move(inlined).unwrap();
    ASSERT_EQ(merge.slotBacks.size(), 1u);
    EXPECT_TRUE(merge.slotBacks[0].selfSufficient);
    EXPECT_EQ(merge.slotBacks[0].nSlot, 1u);
    EXPECT_EQ(merge.slotBacks[0].parentSlot, 2u) << "盖章档 exact 恢复原文槽号";
    ASSERT_TRUE(merge.apply(*ctx.fixture.session, ctx.resolver).is_ok());

    /// 合并产物与原文规范化逐位一致（glue goto 与尾 goto 均折为直落，t 槽
    /// 原样对回——无需「fresh 槽撞对原文槽号」的形态豁免）。
    expectSameInsns(before, normalizedInsns(*ctx.fixture.session, pickId));
}

/// 往返（自足槽盖章 exact 恢复）：区域内声明局部 t（父槽 3）先写后读不进
/// 参数集——盖章行落 N 的 Code 嵌套属性，inline 按章 exact 恢复原槽号；
/// 无章形态回落 fresh 分配，父槽号无从对回（结构断言钉住盖章行）。
TEST(MjAsmSemMergePlanTest, RoundtripSelfSufficientExactSlot)
{
    ClassBytesBuilder b("T");
    Bytes             raw = b.assemble({selfSufficientSpec()});

    /// 处方侧自证：t 为自足槽（先写后读不进参数集，槽重编号非参数行）。
    /// 谓词钉住含 ISTORE 3 的写臂——纯值臂（iconst5）同样可 outline 但无
    /// 自足槽形态。
    InlineContext planCtx{raw};
    auto istoreArm = [&planCtx](const RegionCandidate& c) {
        for (const auto member : c.members) {
            const mjbase::asm_cfg::BasicBlock* blk = planCtx.view.cfg().block(member);
            if (blk == nullptr) {
                continue;
            }
            for (const jbc::ir::InsnId iid : blk->insns) {
                const jbc::ir::Insn* in = planCtx.unit->dom.peek(iid);
                if (in != nullptr && in->op == Op::ISTORE && in->a == 3) {
                    return true;
                }
            }
        }
        return false;
    };
    OutlinePlan   plan = outlineWhere(planCtx, istoreArm);
    ASSERT_TRUE(plan.params.empty());
    ASSERT_EQ(plan.locals.size(), 1u);
    /// 盖章处方：t 在 N 内占槽 0（无参数），原文父槽 3。
    ASSERT_EQ(plan.selfStamps.size(), 1u);
    EXPECT_EQ(plan.selfStamps[0].nSlot, 0u);
    EXPECT_EQ(plan.selfStamps[0].parentSlot, 3u);
    auto applied = plan.apply(*planCtx.fixture.session, planCtx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    /// 盖章行落 N 的 Code 嵌套属性：u16 计数 + (0, 3) 对。
    const auto stampRows = stampRowsOf(*planCtx.fixture.session, outlinedId);
    ASSERT_EQ(stampRows.size(), 1u);
    EXPECT_EQ(stampRows[0].first, 0u);
    EXPECT_EQ(stampRows[0].second, 3u);

    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*planCtx.fixture.session, planCtx.pickId(), planCtx.resolver, {}});
    ASSERT_TRUE(inlined.is_ok()) << mjbase::asm_sem::to_string(inlined.unwrap_err());
    MergePlan merge = std::move(inlined).unwrap();
    ASSERT_EQ(merge.slotBacks.size(), 1u);
    EXPECT_TRUE(merge.slotBacks[0].selfSufficient);
    EXPECT_EQ(merge.slotBacks[0].parentSlot, 3u) << "盖章档 exact 恢复原文槽号";
    ASSERT_TRUE(merge.apply(*planCtx.fixture.session, planCtx.resolver).is_ok());

    const std::vector<NormInsn> before = normalizedBaseline(raw);
    expectSameInsns(before, normalizedInsns(*planCtx.fixture.session, planCtx.pickId()));
}

/// 无盖章回落 fresh（外来 inline 形态兼容）：剥去 N 的 Code 嵌套
/// 盖章属性后 planInline 仍可识别，自足槽回落 maxLocals 起 fresh 分配，
/// 规范化对拍与原文一致。
TEST(MjAsmSemMergePlanTest, StampStrippedFallsBackFresh)
{
    ClassBytesBuilder b("T");
    Bytes             raw = b.assemble({selfSufficientSpec()});

    InlineContext planCtx{raw};
    auto istoreArm = [&planCtx](const RegionCandidate& c) {
        for (const auto member : c.members) {
            const mjbase::asm_cfg::BasicBlock* blk = planCtx.view.cfg().block(member);
            if (blk == nullptr) {
                continue;
            }
            for (const jbc::ir::InsnId iid : blk->insns) {
                const jbc::ir::Insn* in = planCtx.unit->dom.peek(iid);
                if (in != nullptr && in->op == Op::ISTORE && in->a == 3) {
                    return true;
                }
            }
        }
        return false;
    };
    OutlinePlan   plan = outlineWhere(planCtx, istoreArm);
    ASSERT_EQ(plan.selfStamps.size(), 1u);
    auto applied = plan.apply(*planCtx.fixture.session, planCtx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    /// 剥章：自 N 的 Code 嵌套属性表中移除 kOutlineStampAttr（模拟外来
    /// inline 形态——产物无盖章）。
    {
        jbc::core::ClassFile& cf = planCtx.fixture.session->backing();
        jbc::ir::MethodUnit*  nMu = planCtx.fixture.session->unit().method(outlinedId);
        ASSERT_NE(nMu, nullptr);
        bool stripped = false;
        for (jbc::core::AttrSlot& slot : cf.methods[nMu->backingIndex].attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) {
                continue;
            }
            auto code = cf.decodeAttr(slot);
            ASSERT_TRUE(code.is_ok());
            auto* codeAttr = static_cast<jbc::core::CodeAttr*>(code.unwrap());
            const ca::usize before = codeAttr->attrs.size();
            codeAttr->attrs.erase(
                std::remove_if(codeAttr->attrs.begin(), codeAttr->attrs.end(),
                               [&](const jbc::core::AttrSlot& s) {
                                   return cf.cp.utf8Equals(
                                       s.nameCp, mjbase::asm_sem::kOutlineStampAttr);
                               }),
                codeAttr->attrs.end());
            EXPECT_LT(codeAttr->attrs.size(), before) << "盖章属性存在";
            (void)slot.mutate();
            stripped = true;
            break;
        }
        ASSERT_TRUE(stripped);
    }

    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*planCtx.fixture.session, planCtx.pickId(), planCtx.resolver, {}});
    ASSERT_TRUE(inlined.is_ok()) << mjbase::asm_sem::to_string(inlined.unwrap_err());
    MergePlan merge = std::move(inlined).unwrap();
    ASSERT_EQ(merge.slotBacks.size(), 1u);
    EXPECT_TRUE(merge.slotBacks[0].selfSufficient);
    EXPECT_EQ(merge.slotBacks[0].parentSlot, merge.freshBase) << "无章回落 fresh";
    ASSERT_TRUE(merge.apply(*planCtx.fixture.session, planCtx.resolver).is_ok());

    const std::vector<NormInsn> before = normalizedBaseline(raw);
    expectSameInsns(before, normalizedInsns(*planCtx.fixture.session, planCtx.pickId()));
}

/// 往返（栈切片 + 原文装载链）：区域消费入口栈值，调用点前的原文 iload
/// 进入装载链并被剥离（边界滑动对偶档），合并后与原文一致。
TEST(MjAsmSemMergePlanTest, RoundtripStackSlice)
{
    ClassBytesBuilder b("T");
    Bytes             raw = b.assemble({stackSliceSpec()});
    const std::vector<NormInsn> before = normalizedBaseline(raw);

    std::shared_ptr<jbc::ir::ClassSession> session;
    jbc::ir::MethodId                      pickId{};
    roundtripOutlineInline(raw, session, pickId);

    expectSameInsns(before, normalizedInsns(*session, pickId));
}

/// LNT/LVT 行回收（outline 迁移表的逆操作）：迁入 N 的行在 inline 后
/// 回到父表（LNT 行号原样、LVT 槽按逆映射还原；区间长度按回迁窗口重建
/// ——迁入 N 时原长度已被体尾覆盖，属不可恢复项）。
TEST(MjAsmSemMergePlanTest, LntLvtRowMoveBack)
{
    ClassBytesBuilder b("T");
    InlineContext     ctx(b.assemble({pickSpec()}));
    const jbc::ir::MethodId pickId = ctx.pickId();

    ca::u32 imulPc     = 0;
    ca::u32 armFirstPc = 0;
    for (const jbc::ir::InsnId id : ctx.unit->dom.order()) {
        const jbc::ir::Insn* in = ctx.unit->dom.peek(id);
        if (in == nullptr) {
            continue;
        }
        if (in->op == Op::IMUL) {
            imulPc = in->srcPc;
        }
    }
    ASSERT_NE(imulPc, 0u);
    for (const RegionCandidate& c : ctx.candidates) {
        const mjbase::asm_cfg::BasicBlock* blk = ctx.view.cfg().block(c.members[0]);
        for (const jbc::ir::InsnId id : blk->insns) {
            const jbc::ir::Insn* in = ctx.unit->dom.peek(id);
            if (in != nullptr && in->op == Op::IMUL) {
                const jbc::ir::Insn* head = ctx.unit->dom.peek(blk->insns.front());
                ASSERT_NE(head, nullptr);
                armFirstPc = head->srcPc;
            }
        }
    }
    ASSERT_NE(armFirstPc, 0u);
    const ca::u32 joinPc = imulPc + 4u;   /// goto(3) + iconst_5(1) 之后

    jbc::core::ClassFile& cf = ctx.fixture.session->backing();
    const ca::u32 nameCp = cf.cp.addUtf8("a");
    attachRows(*ctx.fixture.session, pickId,
               {{0u, 10u}, {armFirstPc, 20u}, {joinPc, 30u}},
               {{armFirstPc, 7u, nameCp, cf.cp.addUtf8("I"), 0u}});

    OutlinePlan plan = outlineFirst(ctx);
    ASSERT_EQ(plan.lntMoves.size(), 1u);
    ASSERT_EQ(plan.lvtMoves.size(), 1u);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, pickId, ctx.resolver, {}});
    ASSERT_TRUE(inlined.is_ok()) << mjbase::asm_sem::to_string(inlined.unwrap_err());
    MergePlan merge = std::move(inlined).unwrap();
    ASSERT_EQ(merge.lntBacks.size(), 1u);
    ASSERT_EQ(merge.lvtBacks.size(), 1u);
    ASSERT_TRUE(merge.apply(*ctx.fixture.session, ctx.resolver).is_ok());

    /// 父表回收：三行 LNT 齐备（臂行行号 20 回到区域头 pc）、LVT 行槽 0。
    const auto parentLnt = lntRowsOf(*ctx.fixture.session, pickId);
    ASSERT_EQ(parentLnt.size(), 3u);
    bool armRowBack = false;
    for (const auto& r : parentLnt) {
        if (r.second == 20u) {
            EXPECT_EQ(r.first, armFirstPc);
            armRowBack = true;
        }
    }
    EXPECT_TRUE(armRowBack);
    const auto parentLvt = lvtRowsOf(*ctx.fixture.session, pickId, false);
    ASSERT_EQ(parentLvt.size(), 1u);
    EXPECT_EQ(parentLvt[0].startPc, armFirstPc);
    EXPECT_EQ(parentLvt[0].slot, 0u);
    EXPECT_EQ(parentLvt[0].nameCp, nameCp);
}

/// fail-closed：目标非 private|static|synthetic（撤销 ACC_SYNTHETIC）。
TEST(MjAsmSemMergePlanTest, FailClosedNotSynthetic)
{
    ClassBytesBuilder b("T");
    InlineContext     ctx(b.assemble({pickSpec()}));
    OutlinePlan       plan = outlineFirst(ctx);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    ctx.fixture.session->unit().method(outlinedId)->access = 0x0002u | 0x0008u;
    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, ctx.pickId(), ctx.resolver, {}});
    ASSERT_TRUE(inlined.is_err());
    EXPECT_EQ(inlined.unwrap_err(), OutlineError::NotOutlineSynthetic);
}

/// fail-closed：合成方法体异常表非空（outline 产物恒无）。
TEST(MjAsmSemMergePlanTest, FailClosedCalleeTryCovered)
{
    ClassBytesBuilder b("T");
    InlineContext     ctx(b.assemble({pickSpec()}));
    OutlinePlan       plan = outlineFirst(ctx);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    jbc::ir::CodeUnit* nUnit = ctx.fixture.session->unit().method(outlinedId)->code.get();
    ASSERT_NE(nUnit, nullptr);
    nUnit->exc.push_back(jbc::ir::CodeUnit::Exc{});
    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, ctx.pickId(), ctx.resolver, {}});
    ASSERT_TRUE(inlined.is_err());
    EXPECT_EQ(inlined.unwrap_err(), OutlineError::CalleeTryCovered);
}

/// fail-closed：合成方法体非直线（返回指令以外的控制转移）。
TEST(MjAsmSemMergePlanTest, FailClosedCalleeNotStraightLine)
{
    ClassBytesBuilder b("T");
    InlineContext     ctx(b.assemble({pickSpec()}));
    OutlinePlan       plan = outlineFirst(ctx);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    jbc::ir::CodeUnit* nUnit = ctx.fixture.session->unit().method(outlinedId)->code.get();
    ASSERT_NE(nUnit, nullptr);
    jbc::ir::CodeEditor ned{*nUnit};
    ASSERT_TRUE(ned.insertAfter(nUnit->dom.order().back(),
                                jbc::ir::CodeEditor::simple(Op::IRETURN)).is_ok());
    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, ctx.pickId(), ctx.resolver, {}});
    ASSERT_TRUE(inlined.is_err());
    EXPECT_EQ(inlined.unwrap_err(), OutlineError::CalleeNotStraightLine);
}

/// fail-closed：类内引用数 ≠ 1（向 N 体补一条自引用 → 2）。
TEST(MjAsmSemMergePlanTest, FailClosedCallSiteCount)
{
    ClassBytesBuilder b("T");
    InlineContext     ctx(b.assemble({pickSpec()}));
    OutlinePlan       plan = outlineFirst(ctx);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    jbc::ir::CodeUnit* nUnit = ctx.fixture.session->unit().method(outlinedId)->code.get();
    ASSERT_NE(nUnit, nullptr);
    jbc::ir::Insn extra;
    extra.op = Op::INVOKESTATIC;
    extra.cp = ctx.fixture.session->internMethodRef("T", "outline$pick$1", "(I)I");
    jbc::ir::CodeEditor ned{*nUnit};
    ASSERT_TRUE(ned.insertAfter(nUnit->dom.order().back(), extra).is_ok());
    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, ctx.pickId(), ctx.resolver, {}});
    ASSERT_TRUE(inlined.is_err());
    EXPECT_EQ(inlined.unwrap_err(), OutlineError::CallSiteCountUnsupported);
}

/// fail-closed：体形态残缺（删除序幕装载 → N 帧不可验证）。
TEST(MjAsmSemMergePlanTest, FailClosedCalleeShapeBroken)
{
    ClassBytesBuilder b("T");
    InlineContext     ctx(b.assemble({pickSpec()}));
    OutlinePlan       plan = outlineFirst(ctx);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    jbc::ir::CodeUnit* nUnit = ctx.fixture.session->unit().method(outlinedId)->code.get();
    ASSERT_NE(nUnit, nullptr);
    jbc::ir::CodeEditor ned{*nUnit};
    ASSERT_TRUE(ned.eraseBatch({nUnit->dom.order().front()},
                               jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed).is_ok());
    auto inlined = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, ctx.pickId(), ctx.resolver, {}});
    ASSERT_TRUE(inlined.is_err());
    EXPECT_EQ(inlined.unwrap_err(), OutlineError::CalleeShapeImpure);
}

/// fail-closed：调用方错配——指定 callee 的唯一调用点不在该消费方
/// （CallSiteShapeImpure）；无候选方法自动发现落空（NoInlineCandidate）。
TEST(MjAsmSemMergePlanTest, FailClosedWrongParentOrNoCandidate)
{
    ClassBytesBuilder b("T");
    InlineContext     ctx(b.assemble({pickSpec(), otherSpec()}));
    OutlinePlan       plan = outlineFirst(ctx);
    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const jbc::ir::MethodId outlinedId = std::move(applied).unwrap().outlined;

    const jbc::ir::MethodId otherId = ctx.fixture.session->unit().findMethodId("other");
    ASSERT_TRUE(ctx.fixture.session->unit().method(otherId) != nullptr);

    /// 指定 callee：类内引用数 = 1，但调用点不在 other。
    auto wrongParent = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, otherId, ctx.resolver, outlinedId});
    ASSERT_TRUE(wrongParent.is_err());
    EXPECT_EQ(wrongParent.unwrap_err(), OutlineError::CallSiteShapeImpure);

    /// 自动发现：other 内无 inline 候选调用点。
    auto noCandidate = mjbase::asm_sem::planInline(
        InlineRequest{*ctx.fixture.session, otherId, ctx.resolver, {}});
    ASSERT_TRUE(noCandidate.is_err());
    EXPECT_EQ(noCandidate.unwrap_err(), OutlineError::NoInlineCandidate);
}

/// validate 数据侧：回迁体被清空 → MalformedPlan。
TEST(MjAsmSemMergePlanTest, ValidateMalformed)
{
    MergePlan empty;
    EXPECT_EQ(empty.validate().unwrap_err(), OutlineError::MalformedPlan);
}

/// JVM 真跑门（javac 语料）：outline → inline 全程 -Xverify:all，行为与
/// 原文逐值一致；规范化序列亦一致（紧凑编码归一）。
TEST(MjAsmSemMergePlanTest, JvmGateRoundtripJavac)
{
    namespace fs = std::filesystem;
    static const auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path scratch =
        fs::temp_directory_path()
        / ("mjbase_inline_gate_" + std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(scratch, ec);
    ASSERT_FALSE(ec);
    const fs::path src = scratch / "OutlineSample.java";
    {
        std::ofstream out(src, std::ios::binary);
        out << "public class OutlineSample {\n"
               "    static int pick(int a, int b, boolean c) {\n"
               "        return (c ? a * 2 : b + 3) + 100;\n"
               "    }\n"
               "}\n";
    }
    ASSERT_TRUE(test::jdk::compileJava(tc, src, scratch));

    Bytes original;
    {
        std::ifstream in(scratch / "OutlineSample.class", std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    ASSERT_GT(original.size(), 0u);

    const std::string runnerBody =
        "System.out.println(OutlineSample.pick(3, 4, true));"
        "System.out.println(OutlineSample.pick(3, 4, false));";
    test::jdk::RunnerSpec before;
    before.classes     = {{"OutlineSample", original}};
    before.runnerBody  = runnerBody;
    std::string beforeOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, before, &beforeOut));

    const std::vector<NormInsn> normBefore = normalizedBaseline(original);

    auto made = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{original.data(), original.size()});
    ASSERT_TRUE(made.is_ok());
    auto session = std::move(made).unwrap();
    auto visits  = jbc::ir::collectCodeUnits(*session);
    jbc::ir::MethodId pickId{};
    for (const jbc::ir::CodeVisit& v : visits) {
        if (v.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::MethodUnit& mu = session->unit().methods[v.methodIdx];
        if (session->unit().symbols().str(mu.name) == "pick") {
            pickId = mu.id;
        }
    }
    ASSERT_TRUE(session->unit().method(pickId) != nullptr);

    auto builtView = RegionView::build(*session->unit().method(pickId)->code);
    ASSERT_TRUE(builtView.is_ok());
    RegionView view = std::move(builtView).unwrap();
    const std::vector<RegionCandidate> candidates = view.candidates();

    jbc::analysis::ShapeResolver resolver;
    OutlinePlan plan;
    bool plannedAny = false;
    OutlineError lastErr = OutlineError::Ok;
    for (const RegionCandidate& c : candidates) {
        auto planned = mjbase::asm_sem::planOutline(OutlineRequest{
            *session, pickId, sigOf(true, "(IIIZ)I", "pick", "OutlineSample"),
            resolver, view, c, 1});
        if (planned.is_ok()) {
            plan = std::move(planned).unwrap();
            plannedAny = true;
            break;
        }
        lastErr = planned.unwrap_err();
    }
    ASSERT_TRUE(plannedAny) << mjbase::asm_sem::to_string(lastErr);
    ASSERT_TRUE(plan.apply(*session, resolver).is_ok());

    auto inlined = mjbase::asm_sem::planInline(InlineRequest{*session, pickId, resolver, {}});
    ASSERT_TRUE(inlined.is_ok()) << mjbase::asm_sem::to_string(inlined.unwrap_err());
    ASSERT_TRUE(inlined.unwrap().apply(*session, resolver).is_ok());

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    const Bytes merged = std::move(written).unwrap();

    test::jdk::RunnerSpec after;
    after.classes     = {{"OutlineSample", merged}};
    after.runnerBody  = runnerBody;
    std::string afterOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, after, &afterOut));

    EXPECT_EQ(afterOut, "106\n107\n");
    EXPECT_EQ(afterOut, beforeOut);
    expectSameInsns(normBefore, normalizedInsns(*session, pickId));
    fs::remove_all(scratch, ec);
}

/// JVM 真跑门（builder 语料 + 调用点侧栈形重排的边界 DUP 必测形态）：往返后行为与
/// 拆分前逐字一致（对照组经 withStackMaps 补 SMT）。
TEST(MjAsmSemMergePlanTest, JvmGateRoundtripBoundaryDup)
{
    static const auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    ClassBytesBuilder b("T");
    Bytes             raw      = b.assemble({boundaryDupSpec()});
    Bytes             original = withStackMaps(raw);

    test::jdk::RunnerSpec before;
    before.classes    = {{"T", original}};
    before.runnerBody = "System.out.println(T.pick(0));"
                        "System.out.println(T.pick(3));"
                        "System.out.println(T.pick(9));";
    std::string beforeOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, before, &beforeOut));

    std::shared_ptr<jbc::ir::ClassSession> session;
    jbc::ir::MethodId                      pickId{};
    roundtripOutlineInline(raw, session, pickId);

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    const Bytes merged = std::move(written).unwrap();

    test::jdk::RunnerSpec after;
    after.classes    = {{"T", merged}};
    after.runnerBody = before.runnerBody;
    std::string afterOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, after, &afterOut));

    EXPECT_EQ(afterOut, "0\n3\n9\n");
    EXPECT_EQ(afterOut, beforeOut);
}
