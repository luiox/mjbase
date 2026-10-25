/// @file        fix_pattern_fixture.hpp
/// @brief       FixPatternSimplifier 测试夹具：CodeDom 直构指令序列。
///
/// 按 mjbase AGENTS.md「构造指令序列直接用 jbc::ir::CodeDom（append +
/// createLabel 组合，不需要 classfile）」：种子会话 → addMethod(withCode) →
/// promote → CodeEditor append——分支目标是 LabelId 句柄（偏移由 lower 重算，
/// 不手算）、常量经 session.intern* 落位（不手拼 CP 下标）、异常表边界是标签
/// 句柄。类字节只在「跑生产管线 + 写回断言」时经 writeClass 产出，全程无裸
/// 字节输入。
///
/// 前向分支（目标指令后于分支出现）走 CodeDom 的受控延后绑定构建态
/// （begin/endDeferredTargets——为「整体方法体重放」场景设计，本夹具的整体
/// 构建正是该场景），收尾全量补验目标锚定。
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"

#include "mjbase/asm/insn/insn_factory.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace mjbase::test {

using Bytes = std::vector<ca::u8>;

/// 方法体构建器：CodeDom append 的薄封装（标签/装载/分支惯用形态）。
class Body
{
public:
    Body(jbc::ir::ClassSession& session, jbc::ir::CodeEditor& editor,
         jbc::ir::CodeUnit& code)
        : session_(session), editor_(editor), code_(code)
    {
    }

    /// 追加一条无操作数指令（ICONST_*/DUP/SWAP/IADD/IRETURN 等）。
    ///
    /// 只收无操作数形态：工厂层校验拒绝一切带操作数形状（局部/分支/CP/
    /// iinc/switch 族），误传时 ADD_FAILURE 提示改用 typed 助手——带操作数
    /// 指令经此路径只会得到全零操作数（静默产错指令），必须走
    /// iload/istore/branch/lookupSwitch/ldcInt 等助手给全参数。
    Body& op(jbc::core::Op op)
    {
        auto made = mjbase::asm_insn::simpleInsn(op);
        if (made.is_err()) {
            ADD_FAILURE() << "op() 只收无操作数形态，带操作数的用 typed 助手: "
                          << static_cast<int>(op);
            return *this;
        }
        last_ = editor_.append(std::move(made).unwrap()).unwrap();
        return *this;
    }

    // ---- 无操作数快捷（InsnBuilder 风格直写，等价 op(Op::X)）----

    Body& nop() { return op(jbc::core::Op::NOP); }
    Body& aconstNull() { return op(jbc::core::Op::ACONST_NULL); }
    Body& iconst_m1() { return op(jbc::core::Op::ICONST_M1); }
    Body& iconst_0() { return op(jbc::core::Op::ICONST_0); }
    Body& iconst_1() { return op(jbc::core::Op::ICONST_1); }
    Body& iconst_2() { return op(jbc::core::Op::ICONST_2); }
    Body& iconst_3() { return op(jbc::core::Op::ICONST_3); }
    Body& iconst_4() { return op(jbc::core::Op::ICONST_4); }
    Body& iconst_5() { return op(jbc::core::Op::ICONST_5); }
    Body& iadd() { return op(jbc::core::Op::IADD); }
    Body& isub() { return op(jbc::core::Op::ISUB); }
    Body& imul() { return op(jbc::core::Op::IMUL); }
    Body& idiv() { return op(jbc::core::Op::IDIV); }
    Body& irem() { return op(jbc::core::Op::IREM); }
    Body& ineg() { return op(jbc::core::Op::INEG); }
    Body& ishl() { return op(jbc::core::Op::ISHL); }
    Body& ishr() { return op(jbc::core::Op::ISHR); }
    Body& iushr() { return op(jbc::core::Op::IUSHR); }
    Body& iand() { return op(jbc::core::Op::IAND); }
    Body& ior() { return op(jbc::core::Op::IOR); }
    Body& ixor() { return op(jbc::core::Op::IXOR); }
    Body& l2i() { return op(jbc::core::Op::L2I); }
    Body& i2l() { return op(jbc::core::Op::I2L); }
    Body& pop() { return op(jbc::core::Op::POP); }
    Body& pop2() { return op(jbc::core::Op::POP2); }
    Body& dup() { return op(jbc::core::Op::DUP); }
    Body& dup_x1() { return op(jbc::core::Op::DUP_X1); }
    Body& dup_x2() { return op(jbc::core::Op::DUP_X2); }
    Body& dup2() { return op(jbc::core::Op::DUP2); }
    Body& swap() { return op(jbc::core::Op::SWAP); }
    Body& ireturn() { return op(jbc::core::Op::IRETURN); }
    Body& lreturn() { return op(jbc::core::Op::LRETURN); }
    Body& freturn() { return op(jbc::core::Op::FRETURN); }
    Body& dreturn() { return op(jbc::core::Op::DRETURN); }
    Body& areturn() { return op(jbc::core::Op::ARETURN); }
    Body& vreturn() { return op(jbc::core::Op::RETURN); }
    Body& athrow() { return op(jbc::core::Op::ATHROW); }
    Body& monitorenter() { return op(jbc::core::Op::MONITORENTER); }
    Body& monitorexit() { return op(jbc::core::Op::MONITOREXIT); }

    /// 通用 ILOAD slot。
    Body& iload(ca::u32 slot)
    {
        last_ = editor_.append(jbc::ir::CodeEditor::local(jbc::core::Op::ILOAD, slot)).unwrap();
        return *this;
    }

    /// 通用 ISTORE slot。
    Body& istore(ca::u32 slot)
    {
        last_ = editor_.append(jbc::ir::CodeEditor::local(jbc::core::Op::ISTORE, slot)).unwrap();
        return *this;
    }

    /// IINC（a = slot，b = 增量）。
    Body& iinc(ca::u32 slot, ca::i32 delta)
    {
        jbc::ir::Insn in;
        in.op = jbc::core::Op::IINC;
        in.a  = static_cast<ca::i32>(slot);
        in.b  = delta;
        last_ = editor_.append(in).unwrap();
        return *this;
    }

    /// BIPUSH 立即数。
    Body& bipush(ca::i32 v)
    {
        last_ = editor_.append(jbc::ir::CodeEditor::local(jbc::core::Op::BIPUSH,
                                                          static_cast<ca::u32>(v)))
                    .unwrap();
        return *this;
    }

    /// int 常量装载（intern 去重后 LDC/LDC_W）。
    Body& ldcInt(ca::i32 v)
    {
        last_ = editor_.append(jbc::ir::CodeEditor::loadConst(session_.internInteger(v))).unwrap();
        return *this;
    }

    /// int 常量装载的 LDC_W 形态（宽索引；旧栈 tree 把 LDC_W 折叠为 LDC 节点，
    /// 常量读取面对两者同权）。
    Body& ldcWInt(ca::i32 v)
    {
        jbc::ir::Insn in;
        in.op = jbc::core::Op::LDC_W;
        in.cp = session_.internInteger(v);
        last_ = editor_.append(in).unwrap();
        return *this;
    }

    /// long 常量装载（0/1 -> LCONST，其余 LDC2_W）。
    Body& ldcLong(ca::i64 v)
    {
        last_ = editor_.append(mjbase::pass::method_rewrite_util::materializeLongConst(
                                   session_, v))
                    .unwrap();
        return *this;
    }

    /// float 常量装载（+0/1/2 -> FCONST，其余 LDC/LDC_W）。
    Body& ldcFloat(float v)
    {
        last_ = editor_.append(mjbase::pass::method_rewrite_util::materializeFloatConst(
                                   session_, v))
                    .unwrap();
        return *this;
    }

    /// double 常量装载（+0/1 -> DCONST，其余 LDC2_W）。
    Body& ldcDouble(double v)
    {
        last_ = editor_.append(mjbase::pass::method_rewrite_util::materializeDoubleConst(
                                   session_, v))
                    .unwrap();
        return *this;
    }

    /// invokestatic（Methodref 形态；owner/name/desc 自动 intern）。
    Body& invokeStatic(std::string_view owner, std::string_view name, std::string_view desc)
    {
        last_ = editor_.append(jbc::ir::CodeEditor::cpInsn(
                                   jbc::core::Op::INVOKESTATIC,
                                   session_.internMethodRef(owner, name, desc)))
                    .unwrap();
        return *this;
    }

    /// 字符串常量装载。
    Body& ldcString(std::string_view s)
    {
        last_ =
            editor_.append(jbc::ir::CodeEditor::loadConst(session_.internString(s))).unwrap();
        return *this;
    }

    /// invoke 族指令（cp 经 session.internMethodRef 等取得）。
    Body& invoke(jbc::core::Op op, ca::u32 cp)
    {
        last_ = editor_.append(jbc::ir::CodeEditor::cpInsn(op, cp)).unwrap();
        return *this;
    }

    /// newarray（atype：T_INT = 4 等）。
    Body& newarray(ca::u32 atype)
    {
        last_ = editor_.append(jbc::ir::CodeEditor::local(jbc::core::Op::NEWARRAY, atype))
                    .unwrap();
        return *this;
    }

    /// 条件/无条件分支（目标是 LabelId，偏移不手算；前向目标在延后绑定态下
    /// 允许尚未锚定，收尾由 endDeferredTargets 全量补验）。
    Body& branch(jbc::core::Op op, jbc::ir::LabelId target)
    {
        last_ = editor_.append(jbc::ir::CodeEditor::branch(op, target)).unwrap();
        return *this;
    }

    /// lookupswitch（default 标签 + keys/targets 平行数组；经 asm_insn 工厂
    /// 校验，keys 须严格递增）。
    Body& lookupSwitch(jbc::ir::LabelId def, std::vector<ca::i32> keys,
                       std::vector<jbc::ir::LabelId> targets)
    {
        last_ = editor_
                    .append(mjbase::asm_insn::lookupSwitchInsn(def, std::move(keys),
                                                               std::move(targets))
                                .unwrap())
                    .unwrap();
        return *this;
    }

    /// tableswitch（targets[i] 对应 key = low + i，长度须等于 high-low+1）。
    Body& tableSwitch(jbc::ir::LabelId def, ca::i32 low, ca::i32 high,
                      std::vector<jbc::ir::LabelId> targets)
    {
        last_ = editor_
                    .append(mjbase::asm_insn::tableSwitchInsn(def, low, high,
                                                              std::move(targets))
                                .unwrap())
                    .unwrap();
        return *this;
    }

    /// 新标签（默认锚方法末尾；bindHere/bindAt 后改锚到指令前置边界）。
    jbc::ir::LabelId label()
    {
        return code_.dom.createLabel().unwrap();
    }

    /// 把标签锚到最近追加的指令前置边界。
    void bindHere(jbc::ir::LabelId label)
    {
        (void)editor_.bindLabel(label, last_);
    }

    /// 把标签锚到指定指令前置边界。
    void bindAt(jbc::ir::LabelId label, jbc::ir::InsnId at)
    {
        (void)editor_.bindLabel(label, at);
    }

    /// 异常表条目（边界全部是标签句柄；catchCp = 0 表示 any）。
    void tryCatch(jbc::ir::LabelId start, jbc::ir::LabelId end, jbc::ir::LabelId handler,
                  ca::u32 catchCp = 0)
    {
        code_.exc.push_back({start, end, handler, catchCp});
    }

    jbc::ir::InsnId last() const { return last_; }

    /// 所属会话（常量/成员引用的 intern 面）。
    jbc::ir::ClassSession& session() { return session_; }

private:
    jbc::ir::ClassSession& session_;
    jbc::ir::CodeEditor&   editor_;
    jbc::ir::CodeUnit&     code_;
    jbc::ir::InsnId        last_{};
};

/// 从既有类字节建会话（幂等复跑/写回断言用；gtest 断言失败即停）。
inline std::shared_ptr<jbc::ir::ClassSession> sessionFromBytes(const Bytes& bytes)
{
    auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{bytes});
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return nullptr;
    return std::move(made).unwrap();
}

/// 对既有会话跑一组方法级 pass（默认管线选项）。
template<class... PassTs>
mjbase::spi::PreservedAnalyses runPassesOn(std::shared_ptr<jbc::ir::ClassSession> session,
                                           PassTs&&... passes)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::MethodAnalysisManager mam(arena);
    mjbase::KeyPool                    identityPool;
    mjbase::spi::MethodPassManager     mpm;
    (mpm.addPass(std::make_unique<PassTs>(std::forward<PassTs>(passes))), ...);
    return mjbase::spi::runMethodPipelineOverSession(*session, mpm, mam, identityPool, nullptr,
                                                     {});
}

/// 在会话里加一个 withCode 方法并经 CodeDom 构建（前向分支走受控延后绑定，
/// 收尾全量补验 + 一次 lower 落 backing）。
inline void addBodyMethod(jbc::ir::ClassSession& s, std::string_view name,
                          std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals,
                          const std::function<void(Body&)>& fill, ca::u32 access = 0x0009)
{
    auto added = s.addMethod({name, desc, access, true, maxStack, maxLocals});
    ASSERT_TRUE(added.is_ok());
    const ca::usize idx = s.unit().methods.size() - 1;
    auto promoted = jbc::ir::promoteMethod(s, idx);
    ASSERT_TRUE(promoted.is_ok());
    auto code = std::move(promoted).unwrap();

    code->dom.beginDeferredTargets();
    jbc::ir::CodeEditor editor(*code);
    Body                body(s, editor, *code);
    fill(body);
    auto closed = code->dom.endDeferredTargets();
    ASSERT_TRUE(closed.is_ok()) << "方法体分支目标未全部锚定: " << std::string(name);

    auto lowered = jbc::ir::lowerMethod(s, idx);
    ASSERT_TRUE(lowered.is_ok());
}

/// 单方法种子会话夹具：CodeDom 构建 → 一次 lower（backing 与 IR 一致，之后
/// 任何 promote 路径都拿到同一份体）。
struct DomFixture
{
    std::shared_ptr<jbc::ir::ClassSession> session;

    DomFixture(std::string_view thisClass, std::string_view name, std::string_view desc,
               ca::u16 maxStack, ca::u16 maxLocals, const std::function<void(Body&)>& fill,
               ca::u32 access = 0x0009)
    {
        // 构造函数内不能用 ASSERT_*（其隐含 return 与构造语义冲突，MSVC C2534）；
        // 失败面用 ADD_FAILURE + 提前返回。
        jbc::ir::NewClassSeed seed;
        seed.thisName  = thisClass;
        seed.superName = "java/lang/Object";
        auto made = jbc::ir::makeSession(seed);
        if (made.is_err()) {
            ADD_FAILURE() << "makeSession failed";
            return;
        }
        session = std::move(made).unwrap();
        addBodyMethod(*session, name, desc, maxStack, maxLocals, fill, access);
    }

    /// 方法 0 的 CodeUnit（run 之后即 pass 变换产物）。
    jbc::ir::CodeUnit& unit()
    {
        auto* code = session->unit().methods[0].code.get();
        EXPECT_NE(code, nullptr);
        return *code;
    }

    /// 跑生产管线（默认选项），返回聚合保留集合。
    template<class... PassTs>
    mjbase::spi::PreservedAnalyses run(PassTs&&... passes)
    {
        return runPassesOn(session, std::forward<PassTs>(passes)...);
    }

    /// 跑生产管线（显式选项，如注入 typeResolver 的 A1 帧链）。
    template<class... PassTs>
    mjbase::spi::PreservedAnalyses runWith(mjbase::spi::PassPipelineOptions options,
                                           PassTs&&... passes)
    {
        ca::str::Utf8StringArena           arena;
        mjbase::spi::MethodAnalysisManager mam(arena);
        mjbase::KeyPool                    identityPool;
        mjbase::spi::MethodPassManager     mpm;
        (mpm.addPass(std::make_unique<PassTs>(std::forward<PassTs>(passes))), ...);
        return mjbase::spi::runMethodPipelineOverSession(*session, mpm, mam, identityPool,
                                                         nullptr, options);
    }

    /// 写回类字节（backing → classfile）。
    Bytes writeBack() const
    {
        auto written = jbc::core::writeClass(session->backing());
        EXPECT_TRUE(written.is_ok());
        return std::move(written).unwrap();
    }

    /// 给方法 0 挂一条 LineNumberTable 行（锚在 anchorOp 首次出现指令的 pc）。
    void addLineNumberRow(jbc::core::Op anchorOp, ca::u32 line)
    {
        jbc::core::ClassFile& cf = session->backing();
        const auto&           mu = session->unit().methods[0];
        jbc::core::AttrSlot*  codeSlot = nullptr;
        for (auto& slot : cf.methods[mu.backingIndex].attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, "Code")) codeSlot = &slot;
        }
        ASSERT_NE(codeSlot, nullptr);
        auto decoded = cf.decodeAttr(*codeSlot);
        ASSERT_TRUE(decoded.is_ok());
        auto* codeAttr = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());

        ca::u32 pc = jbc::ir::kNoPc;
        for (const jbc::ir::InsnId id : unit().dom.order()) {
            const jbc::ir::Insn* in = unit().dom.peek(id);
            if (in != nullptr && in->op == anchorOp) {
                pc = in->srcPc;
                break;
            }
        }
        ASSERT_NE(pc, jbc::ir::kNoPc);

        auto lnt = std::make_unique<jbc::core::LineNumberTableAttr>();
        lnt->rows.push_back({pc, line});
        ca::u32 nameCp = cf.cp.findUtf8("LineNumberTable");
        if (nameCp == 0) nameCp = cf.cp.addUtf8("LineNumberTable");
        codeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(nameCp, std::move(lnt)));
        // 嵌套 New 槽要随 Code 属性一起重编码（lower 已置脏过 Code，这里显式
        // 再钉一次，防后续无 lower 路径 copy 源 span 丢掉新行）。
        codeSlot->state = jbc::core::AttrSlot::State::Modified;
    }
};

/// 方法体操作码序列。
inline std::vector<jbc::core::Op> opsOf(const DomFixture& fx)
{
    std::vector<jbc::core::Op> ops;
    for (const jbc::ir::InsnId id : fx.session->unit().methods[0].code->dom.order()) {
        const jbc::ir::Insn* in = fx.session->unit().methods[0].code->dom.peek(id);
        EXPECT_NE(in, nullptr);
        ops.push_back(in->op);
    }
    return ops;
}

/// 方法体内指定 opcode 的唯一指令（多个或缺失返回 nullptr）。
inline const jbc::ir::Insn* uniqueInsn(const DomFixture& fx, jbc::core::Op op)
{
    const jbc::ir::Insn* found = nullptr;
    for (const jbc::ir::InsnId id : fx.session->unit().methods[0].code->dom.order()) {
        const jbc::ir::Insn* in = fx.session->unit().methods[0].code->dom.peek(id);
        if (in != nullptr && in->op == op) {
            if (found != nullptr) return nullptr;
            found = in;
        }
    }
    return found;
}

/// 指令 id 在顺序中的下标（找不到返回 -1）。
inline ca::i64 indexOfId(const DomFixture& fx, jbc::ir::InsnId id)
{
    const std::vector<jbc::ir::InsnId>& order = fx.session->unit().methods[0].code->dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (order[i] == id) return static_cast<ca::i64>(i);
    }
    return -1;
}

/// 输出类字节的 Code 嵌套 LineNumberTable 行（无属性返回空）。
inline std::vector<jbc::core::LineNumberTableAttr::Row> lntRowsOf(const Bytes& bytes)
{
    std::vector<jbc::core::LineNumberTableAttr::Row> rows;
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return rows;
    auto cf = std::move(read).unwrap();
    for (jbc::core::MemberInfo& m : cf.file->methods) {
        auto code = cf.file->decodeCode(m);
        if (code.is_err()) continue;
        for (jbc::core::AttrSlot& slot : code.unwrap()->attrs) {
            auto decoded = cf.file->decodeAttr(slot);
            if (decoded.is_err()) continue;
            auto* body = std::move(decoded).unwrap();
            if (body->kind() != jbc::core::AttrKind::LineNumberTable) continue;
            rows = static_cast<jbc::core::LineNumberTableAttr*>(body)->rows;
        }
    }
    return rows;
}

/// 给会话内全部方法补 StackMapTable（fixture 面专用）：真 JVM 验证器对 V50+
/// 的分支方法要求帧表；变换管线自己会对 changed 方法走同一条 A1 链
/// （prepare → lower → rewire），未 changed 方法的帧原样保留。
inline void attachFrames(jbc::ir::ClassSession& session,
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
        auto        scope = std::make_unique<mjbase::spi::MethodScope>(
            mjbase::spi::MethodScope{
                mjbase::spi::MethodTarget{mjbase::MemberKey{}, mu.id,
                                          unit.symbols().str(mu.name),
                                          unit.symbols().str(mu.desc), mu.access, session},
                *code.get()});
        mjbase::spi::frame_rewire::rewireFramesAfterLower(session, *scope, resolver);
    }
}

}   // namespace mjbase::test
