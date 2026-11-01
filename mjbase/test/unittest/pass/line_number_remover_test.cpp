/// LineNumberRemover 单测：只摘 Code 嵌套 LineNumberTable，保 LVT 与指令载荷；
/// 条件 PA（无 LNT 时 all）。
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>

#include "jbc/core/class_file.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/line_number_remover.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

std::shared_ptr<jbc::ir::ClassSession> makeSession(FixtureBuilder builder)
{
    const Bytes bytes  = builder.assemble();
    auto        result = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(result.is_ok());
    return std::move(result).unwrap();
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
    FixtureBuilder builder("T");
    builder.addMethod({"m", "()V", nopReturn(), 0x0009, {}, 1, 1, {},
                       {{"LineNumberTable", Bytes{0x00, 0x01, 0x00, 0x00, 0x00, 0x2A}},
                        {"LocalVariableTable", Bytes{0x00, 0x00}}}});
    auto session = makeSession(builder);

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
    FixtureBuilder builder("T");
    builder.addMethod({"m", "()V", nopReturn(), 0x0009, {}, 1, 1, {},
                       {{"LocalVariableTable", Bytes{0x00, 0x00}}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::LineNumberRemover   pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_FALSE(pa.changed());

    jbc::core::ClassFile& cf = session->backing();
    ASSERT_EQ(cf.methods.size(), 1u);
    EXPECT_EQ(codeAttrNames(cf, cf.methods[0]).count("LocalVariableTable"), 1u);
}
