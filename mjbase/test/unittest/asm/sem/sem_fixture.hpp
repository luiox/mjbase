/// @file        sem_fixture.hpp
/// @brief       asm/sem 测试夹具：手拼 class 字节 + 会话升格 + 按名取方法体。
///
/// 与 asm/analyzer def_use_test 的 Fixture 同款纪律：会话保活（CodeUnit
/// 句柄解析依赖 backing），code() 返回非 const 指针（ControlFlowGraph::build
/// 的签名要求）。
#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/jbc.hpp"

#include "analysis/class_bytes_builder.hpp"

/// asm/sem 合成语料夹具：会话保活 + 按名取升格后的 CodeUnit。
struct SemFixture
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    std::vector<jbc::ir::CodeVisit>        visits;

    explicit SemFixture(const mjbase::analysis::test_support::Bytes& bytes)
    {
        auto made = jbc::ir::makeSessionFromBytes(
            jbc::core::ByteView{bytes.data(), bytes.size()});
        EXPECT_TRUE(made.is_ok());
        session = std::move(made).unwrap();
        visits  = jbc::ir::collectCodeUnits(*session);
    }

    const jbc::core::ConstantPool& cp() const { return session->backing().cp; }

    /// 按方法名取升格后的 CodeUnit；非 const（CFG 构建签名要求）。
    jbc::ir::CodeUnit* code(std::string_view name)
    {
        for (const jbc::ir::CodeVisit& visit : visits) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) {
                continue;
            }
            const jbc::ir::MethodUnit& unit = session->unit().methods[visit.methodIdx];
            if (session->unit().symbols().str(unit.name) == name) {
                return visit.unit.get();
            }
        }
        return nullptr;
    }
};

/// MethodSig 速记（name/thisClass 缺省不播）。
inline jbc::analysis::MethodSig sigOf(bool isStatic, std::string_view desc,
                                      std::string_view name = {},
                                      std::string_view thisClass = {})
{
    jbc::analysis::MethodSig sig;
    sig.isStatic  = isStatic;
    sig.desc      = desc;
    sig.name      = name;
    sig.thisClass = thisClass;
    return sig;
}

/// builder 语料补 SMT：类版本 ≥50 且含分支目标时 JVM 校验要求栈映射表
/// （javac 产物自带而 builder 语料不带；与 asm/sem applier 步骤 5 同一流水
/// 再生）。仅用于前后对拍的对照组字节——变换会话仍从裸语料起步，applier
/// 全表再生不与既有 SMT 叠挂。
inline mjbase::analysis::test_support::Bytes withStackMaps(
    const mjbase::analysis::test_support::Bytes& in)
{
    auto made = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{in.data(), in.size()});
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) {
        return in;
    }
    auto                   session = std::move(made).unwrap();
    jbc::ir::ClassSession& sess    = *session;
    const jbc::ir::ClassUnit& cu   = sess.unit();
    if ((cu.version >> 16) < 50u) {
        return in;
    }
    const std::string_view thisClass = cu.symbols().str(cu.thisClass);
    auto visits = jbc::ir::collectCodeUnits(sess);
    for (const jbc::ir::CodeVisit& v : visits) {
        if (v.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::MethodId mid = sess.unit().methods[v.methodIdx].id;
        if (jbc::ir::lowerMethod(sess, mid).is_err()) {
            ADD_FAILURE() << "withStackMaps: lower 失败";
            continue;
        }
        jbc::ir::CodeUnit* unit = v.unit.get();
        jbc::analysis::ShapeResolver resolver;
        jbc::analysis::MethodSig     sig;
        sig.isStatic  = true;   /// builder 语料方法恒 static
        sig.desc      = cu.symbols().str(sess.unit().method(mid)->desc);
        sig.name      = cu.symbols().str(sess.unit().method(mid)->name);
        sig.thisClass = thisClass;
        jbc::analysis::FrameEngine engine(sess.backing().cp, resolver,
                                          jbc::analysis::FramePolicy::Strict);
        std::vector<jbc::analysis::FrameAt> frames;
        const jbc::analysis::FrameSummary   summary =
            engine.compute(*unit, sig, frames);
        EXPECT_TRUE(summary.ok) << sig.name;
        if (!summary.ok) {
            continue;
        }
        if (jbc::ir::lowerMethod(sess, mid).is_err()) {
            ADD_FAILURE() << "withStackMaps: 二次 lower 失败";
            continue;
        }
        auto smt = jbc::analysis::buildStackMapTable(sess, *unit, frames);
        EXPECT_TRUE(smt.is_ok());
        if (smt.is_err()) {
            continue;
        }
        auto result = std::move(smt).unwrap();
        if (!result.needed) {
            continue;
        }
        jbc::core::ClassFile& cf   = sess.backing();
        jbc::core::AttrSlot*  code = nullptr;
        for (jbc::core::AttrSlot& slot : cf.methods[sess.unit().method(mid)->backingIndex].attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, "Code")) {
                code = &slot;
                break;
            }
        }
        if (code == nullptr) {
            ADD_FAILURE() << "withStackMaps: Code 缺失";
            continue;
        }
        auto body = cf.decodeAttr(*code);
        if (body.is_err()) {
            ADD_FAILURE() << "withStackMaps: Code 解码失败";
            continue;
        }
        static_cast<jbc::core::CodeAttr*>(body.unwrap())
            ->attrs.push_back(jbc::core::AttrSlot::makeNew(
                sess.internUtf8("StackMapTable"),
                std::unique_ptr<jbc::core::AttrBody>(std::move(result.attr))));
        (void)code->mutate();
    }
    auto written = jbc::core::writeClass(sess.backing());
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) {
        return in;
    }
    return std::move(written).unwrap();
}

/// 方法 Code 嵌套属性里的 LNT 行（结构断言用）。
inline std::vector<std::pair<ca::u32, ca::u32>> lntRowsOf(jbc::ir::ClassSession& session,
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
        auto* codeAttr = static_cast<jbc::core::CodeAttr*>(code.unwrap());
        for (jbc::core::AttrSlot& nested : codeAttr->attrs) {
            if (!cf.cp.utf8Equals(nested.nameCp, "LineNumberTable")) {
                continue;
            }
            auto body = cf.decodeAttr(nested);
            if (body.is_err()) {
                continue;
            }
            for (const jbc::core::LineNumberTableAttr::Row& r :
                 static_cast<jbc::core::LineNumberTableAttr*>(body.unwrap())->rows) {
                rows.emplace_back(r.startPc, r.line);
            }
        }
    }
    return rows;
}

/// 方法 Code 嵌套属性是否挂有 SMT。
inline bool hasSmt(jbc::ir::ClassSession& session, jbc::ir::MethodId id)
{
    jbc::core::ClassFile& cf = session.backing();
    jbc::ir::MethodUnit*  mu = session.unit().method(id);
    if (mu == nullptr) {
        return false;
    }
    for (jbc::core::AttrSlot& slot : cf.methods[mu->backingIndex].attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "Code")) {
            continue;
        }
        auto code = cf.decodeAttr(slot);
        if (code.is_err()) {
            return false;
        }
        for (const jbc::core::AttrSlot& nested :
             static_cast<jbc::core::CodeAttr*>(code.unwrap())->attrs) {
            if (cf.cp.utf8Equals(nested.nameCp, "StackMapTable")) {
                return true;
            }
        }
    }
    return false;
}

/// 方法 Code 嵌套 LVT 首表行（LNT/LVT 搬运断言用）。
inline std::vector<jbc::core::LocalVariableTableAttr::Row> lvtRowsOf(
    jbc::ir::ClassSession& session, jbc::ir::MethodId id, bool typeTable)
{
    std::vector<jbc::core::LocalVariableTableAttr::Row> rows;
    jbc::core::ClassFile& cf = session.backing();
    jbc::ir::MethodUnit*  mu = session.unit().method(id);
    if (mu == nullptr) {
        return rows;
    }
    const char* want = typeTable ? "LocalVariableTypeTable" : "LocalVariableTable";
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
            if (!cf.cp.utf8Equals(nested.nameCp, want)) {
                continue;
            }
            auto body = cf.decodeAttr(nested);
            if (body.is_err()) {
                continue;
            }
            for (const jbc::core::LocalVariableTableAttr::Row& r :
                 static_cast<jbc::core::LocalVariableTableAttr*>(body.unwrap())->rows) {
                rows.push_back(r);
            }
        }
    }
    return rows;
}

/// 给方法 Code 挂嵌套 LNT/LVT 行（合成行测试用：builder 产物无调试属性）。
inline void attachRows(jbc::ir::ClassSession& session, jbc::ir::MethodId id,
                       const std::vector<jbc::core::LineNumberTableAttr::Row>& lnt,
                       const std::vector<jbc::core::LocalVariableTableAttr::Row>& lvt)
{
    jbc::core::ClassFile& cf = session.backing();
    jbc::ir::MethodUnit* mu = session.unit().method(id);
    ASSERT_NE(mu, nullptr);
    for (jbc::core::AttrSlot& slot : cf.methods[mu->backingIndex].attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "Code")) {
            continue;
        }
        auto code = cf.decodeAttr(slot);
        ASSERT_TRUE(code.is_ok());
        auto* codeAttr = static_cast<jbc::core::CodeAttr*>(code.unwrap());
        if (!lnt.empty()) {
            auto body = std::make_unique<jbc::core::LineNumberTableAttr>();
            body->rows = lnt;
            codeAttr->attrs.push_back(
                jbc::core::AttrSlot::makeNew(cf.cp.addUtf8("LineNumberTable"), std::move(body)));
        }
        if (!lvt.empty()) {
            auto body = std::make_unique<jbc::core::LocalVariableTableAttr>();
            body->rows = lvt;
            codeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(
                cf.cp.addUtf8("LocalVariableTable"), std::move(body)));
        }
        (void)slot.mutate();
        return;
    }
    ADD_FAILURE() << "Code attribute missing";
}
