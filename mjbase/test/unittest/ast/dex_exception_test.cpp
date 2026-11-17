#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <utility>

#include "dex/builder.hpp"
#include "dex/reader.hpp"
#include "smali/assemble.hpp"

#include "mjbase/ast/control_flow.hpp"
#include "mjbase/ast/dex.hpp"

namespace {

using namespace mjbase::ast;
using dex::core::DexFile;

std::unique_ptr<DexFile> assembleDex(std::string_view smali, bool combineCatches = false)
{
    auto file = DexFile::createEmpty(35);
    dex::smali::SmaliAssembler assembler(*file);
    const auto assembled = assembler.assembleClass(smali);
    if (assembled.is_err()) {
        const auto& error = assembled.unwrap_err();
        ADD_FAILURE() << "smali line " << error.line << ": " << error.detail;
        return nullptr;
    }
    auto& options = assembler.options();
    if (combineCatches) {
        // The assembler produces one try_item per .catch line. Encode the
        // single shared interval with one ordered handler list.
        auto& code = options.methodAdditions.front().item;
        dex::core::CatchHandler combined;
        for (const auto& handler : code.catchHandlers) {
            combined.typed.insert(combined.typed.end(), handler.typed.begin(), handler.typed.end());
            if (handler.hasCatchAll) {
                combined.hasCatchAll = true;
                combined.catchAllAddress = handler.catchAllAddress;
            }
        }
        code.tryBlocks.resize(1);
        code.tryBlocks.front().handlerIndex = 0;
        code.triesSize = 1;
        code.catchHandlers = {std::move(combined)};
    }
    auto bytes = dex::core::buildDex(*file, options);
    EXPECT_TRUE(bytes.is_ok());
    if (bytes.is_err()) return nullptr;
    const auto encoded = std::move(bytes).unwrap();
    auto parsed = dex::core::readDex({encoded.data(), encoded.size()});
    EXPECT_TRUE(parsed.is_ok());
    return parsed.is_ok() ? std::move(parsed).unwrap() : nullptr;
}

TEST(MjAstDexExceptionTest, SplitsProtectedRangeAndPassesCaughtExceptionInTableOrder)
{
    auto file = assembleDex(R"SMALI(
.class public LTryAst;
.super Ljava/lang/Object;
.method public static divide(I)I
    .registers 3
    const/4 v1, 0x0
  :try_start
    div-int v1, p0, v1
  :try_end
    return v1
  :typed
    move-exception v0
    const/4 v1, 0x3
    return v1
  :ignore
    const/4 v1, 0x4
    return v1
  :catch_all
    move-exception v0
    throw v0
    .catch Ljava/lang/ArithmeticException; { :try_start .. :try_end } :typed
    .catch Ljava/lang/RuntimeException; { :try_start .. :try_end } :ignore
    .catchall { :try_start .. :try_end } :catch_all
.end method
)SMALI", true);
    ASSERT_TRUE(file);
    const auto data = file->classData(0);
    ASSERT_TRUE(data.is_ok());
    ASSERT_TRUE(data.unwrap());
    ASSERT_EQ(data.unwrap()->directMethods.size(), 1u);
    mjbase::KeyPool keys;
    auto lifted = liftDexMethod(*file, data.unwrap()->directMethods.front(), keys);
    if (lifted.is_err()) {
        const auto error = lifted.unwrap_err();
        FAIL() << "lift error " << static_cast<int>(error.kind)
               << ", code-unit address " << error.position.value_or(0);
    }
    const auto body = std::move(lifted).unwrap();
    ASSERT_EQ(body.blocks.size(), 6u);
    EXPECT_TRUE(body.blocks[0].handlers.empty());
    EXPECT_TRUE(body.blocks[2].handlers.empty());

    const auto& handlers = body.blocks[1].handlers;
    ASSERT_EQ(handlers.size(), 3u);
    ASSERT_TRUE(handlers[0].type);
    EXPECT_EQ(keys.str(handlers[0].type->id), "java/lang/ArithmeticException");
    ASSERT_TRUE(handlers[1].type);
    EXPECT_EQ(keys.str(handlers[1].type->id), "java/lang/RuntimeException");
    EXPECT_FALSE(handlers[2].type);
    EXPECT_EQ(handlers[0].target.index, 3u);
    EXPECT_EQ(handlers[1].target.index, 4u);
    EXPECT_EQ(handlers[2].target.index, 5u);

    const auto graph = ControlFlowGraph::build(body);
    ASSERT_TRUE(graph.is_ok());
    const DominatorTree dominators(graph.unwrap());
    EXPECT_TRUE(dominators.dominates(BlockId{1}, handlers[0].target));

    const std::vector<LocalKind> exception{LocalKind::Reference};
    for (const auto& handler : handlers) {
        const auto& target = body.blocks[handler.target.index];
        EXPECT_EQ(target.parameters, exception);
        EXPECT_TRUE(target.handlers.empty());
    }
    const auto& typed = body.blocks[3].statements;
    ASSERT_GE(typed.size(), 2u);
    const auto* let = std::get_if<Let>(&typed[0]);
    ASSERT_NE(let, nullptr);
    const auto* caught = std::get_if<BlockParameter>(&let->expression);
    ASSERT_NE(caught, nullptr);
    EXPECT_EQ(caught->index, 0u);
    const auto* assignment = std::get_if<Assign>(&typed[1]);
    ASSERT_NE(assignment, nullptr);
    EXPECT_EQ(assignment->value.index, 0u);
    EXPECT_TRUE(std::holds_alternative<Throw>(body.blocks[5].terminator));
}

TEST(MjAstDexExceptionTest, RejectsNormalEntryToMoveExceptionHandler)
{
    auto file = assembleDex(R"SMALI(
.class public LBadTryAst;
.super Ljava/lang/Object;
.method public static fail(I)V
    .registers 2
    goto :handler
  :try_start
    div-int/lit8 v0, p0, 0x0
  :try_end
    return-void
  :handler
    move-exception v0
    return-void
    .catchall { :try_start .. :try_end } :handler
.end method
)SMALI");
    ASSERT_TRUE(file);
    const auto data = file->classData(0);
    ASSERT_TRUE(data.is_ok());
    ASSERT_TRUE(data.unwrap());
    ASSERT_EQ(data.unwrap()->directMethods.size(), 1u);
    mjbase::KeyPool keys;
    auto lifted = liftDexMethod(*file, data.unwrap()->directMethods.front(), keys);
    ASSERT_TRUE(lifted.is_err());
    EXPECT_EQ(lifted.unwrap_err().kind, ErrorKind::Unsupported);
}

}   // namespace
