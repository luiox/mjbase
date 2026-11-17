#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <utility>

#include "dex/builder.hpp"
#include "dex/reader.hpp"
#include "smali/assemble.hpp"

#include "mjbase/ast/dex.hpp"
#include "mjbase/ast/jvm.hpp"
#include "shared/jasm_fixture.hpp"

namespace {

using namespace mjbase::ast;
using dex::core::DexFile;
using dex::core::EncodedMethod;

std::unique_ptr<DexFile> assembleDex(std::string_view smali)
{
    auto fresh = DexFile::createEmpty(35);
    dex::smali::SmaliAssembler assembler(*fresh);
    auto assembled = assembler.assembleClass(smali);
    if (assembled.is_err()) {
        const auto& error = assembled.unwrap_err();
        ADD_FAILURE() << "smali line " << error.line << ": " << error.detail;
        return nullptr;
    }
    auto bytes = dex::core::buildDex(*fresh, assembler.options());
    EXPECT_TRUE(bytes.is_ok());
    if (bytes.is_err()) return nullptr;
    const auto encoded = std::move(bytes).unwrap();
    auto file = dex::core::readDex({encoded.data(), encoded.size()});
    EXPECT_TRUE(file.is_ok());
    return file.is_ok() ? std::move(file).unwrap() : nullptr;
}

const EncodedMethod* staticMethod(DexFile& file, std::string_view name)
{
    auto data = file.classData(0);
    if (data.is_err() || !data.unwrap()) return nullptr;
    for (const auto& method : data.unwrap()->directMethods) {
        const auto methodName = file.methodName(method.methodIndex);
        if (methodName.is_ok() && methodName.unwrap().as_std_view() == name) return &method;
    }
    return nullptr;
}

TEST(MjAstDexTest, SharesSymbolicCallsWithJvmAndPairsWideRangeResult)
{
    auto file = assembleDex(R"SMALI(
.class public LSharedAst;
.super Ljava/lang/Object;
.method public static select(IJ)J
    .registers 5
    if-eqz p0, :zero
    invoke-static/range {v3 .. v4}, LSharedAst;->sink(J)J
    move-result-wide v0
    return-wide v0
  :zero
    return-wide p1
.end method
.method public static sink(J)J
    .registers 2
    return-wide p0
.end method
)SMALI");
    ASSERT_TRUE(file);
    const auto* method = staticMethod(*file, "select");
    ASSERT_NE(method, nullptr);
    mjbase::KeyPool keys;
    auto dex = liftDexMethod(*file, *method, keys);
    ASSERT_TRUE(dex.is_ok());
    file.reset();

    const auto bytes = mjbase::test::assembleClass("SharedAst", "java/lang/Object", R"JASM(
    .method public static select (IJ)J {
      code: {
        iload_0
        ifeq Lzero
        lload_1
        invokestatic SharedAst.sink (J)J
        lreturn
      Lzero:
        lload_1
        lreturn
      }
    }
    .method public static sink (J)J {
      code: {
        lload_0
        lreturn
      }
    }
)JASM");
    auto session = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(session.is_ok());
    auto& source = *session.unwrap();
    auto jvm = liftJvmMethod(source, source.unit().findMethodId("select", "(IJ)J"), keys);
    ASSERT_TRUE(jvm.is_ok());

    const auto& body = dex.unwrap();
    ASSERT_EQ(body.blocks.size(), 3u);
    ASSERT_EQ(body.parameters.size(), 2u);
    EXPECT_EQ(body.method, jvm.unwrap().method);
    const auto* branch = std::get_if<Branch>(&body.blocks.front().terminator);
    ASSERT_NE(branch, nullptr);
    EXPECT_EQ(branch->comparison, Comparison::Equal);
    EXPECT_EQ(branch->whenTrue.index, 2u);
    EXPECT_EQ(branch->whenFalse.index, 1u);
    const auto& condition = body.blocks.front().statements;
    EXPECT_EQ(std::get<ReadLocal>(std::get<Let>(condition[branch->left.index]).expression)
                  .local.index, body.parameters[0].index);

    const auto dexCalls = directCalls(body);
    const auto jvmCalls = directCalls(jvm.unwrap());
    ASSERT_EQ(dexCalls.size(), 1u);
    ASSERT_EQ(jvmCalls.size(), 1u);
    EXPECT_EQ(dexCalls.front().target, jvmCalls.front().target);
    EXPECT_EQ(dexCalls.front().dispatch, jvmCalls.front().dispatch);
    EXPECT_EQ(keys.str(dexCalls.front().target.desc), "(J)J");
    const auto& callBlock = body.blocks[branch->whenFalse.index];
    const auto callIndex = dexCalls.front().statement;
    const auto& call = std::get<Call>(std::get<Let>(callBlock.statements[callIndex]).expression);
    ASSERT_EQ(call.arguments.size(), 1u);
    EXPECT_EQ(std::get<ReadLocal>(
        std::get<Let>(callBlock.statements[call.arguments[0].index]).expression).local.index,
        body.parameters[1].index);
    const auto& assignment = std::get<Assign>(callBlock.statements[callIndex + 1]);
    EXPECT_EQ(assignment.value.index, callIndex);
    const auto& result = std::get<Return>(callBlock.terminator);
    ASSERT_TRUE(result.value);
    EXPECT_EQ(std::get<ReadLocal>(
        std::get<Let>(callBlock.statements[result.value->index]).expression).local.index,
        assignment.local.index);
}

TEST(MjAstDexTest, RejectsUnsupportedInstructionWithCodeUnitAddress)
{
    auto file = assembleDex(R"SMALI(
.class public LSharedAst;
.super Ljava/lang/Object;
.method public static lock(Ljava/lang/Object;)V
    .registers 1
    nop
    monitor-enter p0
    monitor-exit p0
    return-void
.end method
)SMALI");
    ASSERT_TRUE(file);
    const auto* method = staticMethod(*file, "lock");
    ASSERT_NE(method, nullptr);
    mjbase::KeyPool keys;
    auto lifted = liftDexMethod(*file, *method, keys);
    ASSERT_TRUE(lifted.is_err());
    EXPECT_EQ(lifted.unwrap_err().kind, ErrorKind::Unsupported);
    EXPECT_EQ(lifted.unwrap_err().position, 1u);
}

TEST(MjAstDexTest, LiftsSymbolicFieldsAndPreservesReverseSubtractOrder)
{
    auto file = assembleDex(R"SMALI(
.class public LSharedAst;
.super Ljava/lang/Object;
.field public static source:I
.field public destination:I
.method public static update(LSharedAst;)V
    .registers 2
    sget v0, LSharedAst;->source:I
    rsub-int/lit8 v0, v0, 0x7
    iput v0, p0, LSharedAst;->destination:I
    return-void
.end method
)SMALI");
    ASSERT_TRUE(file);
    const auto* method = staticMethod(*file, "update");
    ASSERT_NE(method, nullptr);
    mjbase::KeyPool keys;
    auto lifted = liftDexMethod(*file, *method, keys);
    ASSERT_TRUE(lifted.is_ok());
    const auto& body = lifted.unwrap();
    ASSERT_EQ(body.blocks.size(), 1u);
    ASSERT_EQ(body.parameters.size(), 1u);
    const auto& statements = body.blocks[0].statements;
    const auto expressionAt = [&](ValueId value) -> const Expression& {
        return std::get<Let>(statements.at(value.index)).expression;
    };
    const FieldRead* field = nullptr;
    const IntBinary* arithmetic = nullptr;
    const FieldWrite* write = nullptr;
    for (const auto& statement : statements) {
        if (const auto* let = std::get_if<Let>(&statement)) {
            if (auto* read = std::get_if<FieldRead>(&let->expression)) field = read;
            if (auto* binary = std::get_if<IntBinary>(&let->expression)) arithmetic = binary;
        }
        if (auto* store = std::get_if<FieldWrite>(&statement)) write = store;
    }
    ASSERT_NE(field, nullptr);
    EXPECT_FALSE(field->receiver);
    EXPECT_EQ(keys.str(field->field.name), "source");
    ASSERT_NE(arithmetic, nullptr);
    EXPECT_EQ(arithmetic->op, IntOp::Subtract);
    EXPECT_EQ(std::get<ca::i32>(std::get<Literal>(expressionAt(arithmetic->left))), 7);
    EXPECT_TRUE(std::holds_alternative<ReadLocal>(expressionAt(arithmetic->right)));
    ASSERT_NE(write, nullptr);
    ASSERT_TRUE(write->receiver);
    EXPECT_EQ(keys.str(write->field.name), "destination");
    EXPECT_EQ(std::get<ReadLocal>(expressionAt(*write->receiver)).local.index,
              body.parameters[0].index);

    const auto bytes = mjbase::test::assembleClass("SharedAst", "java/lang/Object", R"JASM(
    .field public static source I
    .field public destination I
    .method public static update (LSharedAst;)V {
      code: {
        aload_0
        bipush 7
        getstatic SharedAst.source I
        isub
        putfield SharedAst.destination I
        return
      }
    }
)JASM");
    auto session = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(session.is_ok());
    auto& source = *session.unwrap();
    auto jvm = liftJvmMethod(source, source.unit().findMethodId("update", "(LSharedAst;)V"), keys);
    ASSERT_TRUE(jvm.is_ok());
    const auto& jvmBody = jvm.unwrap();
    EXPECT_EQ(body.method, jvmBody.method);
    ASSERT_EQ(jvmBody.blocks.size(), 1u);
    const auto& jvmStatements = jvmBody.blocks[0].statements;
    ASSERT_EQ(jvmStatements.size(), 5u);
    const auto* jvmRead = std::get_if<FieldRead>(&std::get<Let>(jvmStatements[2]).expression);
    ASSERT_NE(jvmRead, nullptr);
    EXPECT_EQ(jvmRead->field, field->field);
    EXPECT_FALSE(jvmRead->receiver);
    const auto* jvmWrite = std::get_if<FieldWrite>(&jvmStatements.back());
    ASSERT_NE(jvmWrite, nullptr);
    EXPECT_EQ(jvmWrite->field, write->field);
    ASSERT_TRUE(jvmWrite->receiver);
    const auto& receiver = std::get<Let>(jvmStatements[jvmWrite->receiver->index]).expression;
    EXPECT_EQ(std::get<ReadLocal>(receiver).local.index, jvmBody.parameters[0].index);
}

}   // namespace
