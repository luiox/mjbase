/// LineNumberRemover 单测：只摘 Code 嵌套 LineNumberTable，保 LVT 与指令载荷；
/// 条件 PA（无 LNT 时 all）。
///
/// 夹具纪律：类骨架一律 jasm 文本装配（JvmJasmService），指令体片段
/// R"JASM 定界；LineNumberTable 走 jasm 原生 .line 行号指令。LocalVariableTable
/// 不在 jasm 覆盖面——纪律例外②：装配产物上以 CF 写入面挂 RawAttr 形空
/// LVT，构造仅限属性本体，模拟上游 javac 的 LVT 发射工序。
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>

#include "jbc/core/class_file.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/line_number_remover.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "shared/jasm_fixture.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

/// 装配 → 建会话 → Code 嵌套挂空 LVT（RawAttr {count=0}）。
std::shared_ptr<jbc::ir::ClassSession> makeSessionWithLvt(const Bytes& assembled)
{
    auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(assembled));
    EXPECT_TRUE(made.is_ok());
    if (!made.is_ok()) return nullptr;
    auto                session = std::move(made).unwrap();
    jbc::core::ClassFile& cf = session->backing();
    EXPECT_EQ(cf.methods.size(), 1u);
    if (cf.methods.size() != 1u) return nullptr;
    auto decoded = cf.decodeCode(cf.methods[0]);
    EXPECT_TRUE(decoded.is_ok());
    if (decoded.is_err()) return nullptr;
    jbc::core::CodeAttr* code = std::move(decoded).unwrap();
    EXPECT_NE(code, nullptr);
    if (code == nullptr) return nullptr;
    code->attrs.push_back(jbc::core::AttrSlot::makeNew(
        cf.cp.addUtf8("LocalVariableTable"),
        std::make_unique<jbc::core::RawAttr>(std::vector<ca::u8>{0x00, 0x00})));
    return session;
}

std::set<std::string> codeAttrNames(jbc::core::ClassFile& cf, jbc::core::MemberInfo& m)
{
    std::set<std::string> out;
    auto                  decoded = cf.decodeCode(m);
    if (decoded.is_err()) return out;
    const jbc::core::CodeAttr* code = std::move(decoded).unwrap();
    if (code == nullptr) return out;
    for (const jbc::core::AttrSlot& slot : code->attrs) {
        out.emplace(std::string(cf.utf8(slot.nameCp)));
    }
    return out;
}

}   // namespace

TEST(MjPassLineNumberRemoverTest, StripsLineNumbersKeepsLocalVariables)
{
    // .line 42 产原生 LNT（1 行：pc0 → 42，与原夹具 LNT 字节同形）。
    const Bytes assembled = assembleJasm(R"JASM(.super java/lang/Object
.class public super T {
  .method public static m ()V {
    code: {
      line 42
      nop
      return
    }
  }
}
)JASM");
    auto      session     = makeSessionWithLvt(assembled);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::LineNumberRemover   pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());

    jbc::core::ClassFile& cf = session->backing();
    ASSERT_EQ(cf.methods.size(), 1u);
    const std::set<std::string> names = codeAttrNames(cf, cf.methods[0]);
    EXPECT_EQ(names.count("LineNumberTable"), 0u);
    EXPECT_EQ(names.count("LocalVariableTable"), 1u);
}

TEST(MjPassLineNumberRemoverTest, KeepsMethodWithoutLineNumbers)
{
    // 无 .line → 无 LNT，仅挂 LVT。
    const Bytes assembled = assembleJasm(R"JASM(.super java/lang/Object
.class public super T {
  .method public static m ()V {
    code: {
      nop
      return
    }
  }
}
)JASM");
    auto      session     = makeSessionWithLvt(assembled);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::LineNumberRemover   pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_FALSE(pa.changed());

    jbc::core::ClassFile& cf = session->backing();
    ASSERT_EQ(cf.methods.size(), 1u);
    EXPECT_EQ(codeAttrNames(cf, cf.methods[0]).count("LocalVariableTable"), 1u);
}
