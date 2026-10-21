// 资源类型换 mjbase::model。
#include <gtest/gtest.h>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_resource_pending_commit.hpp"

using namespace mjbase::spi;

namespace {

std::vector<ca::u8> bytesWith(char marker)
{
    return {0xCA, 0xFE, static_cast<ca::u8>(marker)};
}

}   // namespace

// commit 之前资源必须保持原样（阶段一不写回）。
TEST(ClassResourcePendingCommitTest, stageDoesNotWriteBackBeforeCommit)
{
    mjbase::model::ClassResource resource("A.class", bytesWith(1));
    ClassResourcePendingCommit   pending;

    pending.stage(&resource, bytesWith(2));
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(resource.get(), bytesWith(1));
}

TEST(ClassResourcePendingCommitTest, commitWritesBackAllStaged)
{
    mjbase::model::ClassResource a("A.class", bytesWith(1));
    mjbase::model::ClassResource b("B.class", bytesWith(2));
    ClassResourcePendingCommit   pending;

    pending.stage(&a, bytesWith(9));
    pending.stage(&b, bytesWith(8));
    pending.commit();

    EXPECT_EQ(a.get(), bytesWith(9));
    EXPECT_EQ(b.get(), bytesWith(8));
    EXPECT_TRUE(pending.empty());
}

// 同一资源重复 stage 以最后一次为准。
TEST(ClassResourcePendingCommitTest, restageSameResourceLastWins)
{
    mjbase::model::ClassResource resource("A.class", bytesWith(1));
    ClassResourcePendingCommit   pending;

    pending.stage(&resource, bytesWith(2));
    pending.stage(&resource, bytesWith(3));
    EXPECT_EQ(pending.size(), 1u);
    pending.commit();
    EXPECT_EQ(resource.get(), bytesWith(3));
}

// pass 循环中途抛出：未 commit 即析构，所有资源保持原样。
TEST(ClassResourcePendingCommitTest, exceptionBeforeCommitLeavesResourcesUntouched)
{
    mjbase::model::ClassResource first("First.class", bytesWith(1));
    mjbase::model::ClassResource second("Second.class", bytesWith(2));
    ClassResourcePendingCommit   pending;

    ASSERT_THROW(
        {
            pending.stage(&first, bytesWith(9));
            throw std::runtime_error("serialize failure");
        },
        std::runtime_error);
    EXPECT_EQ(first.get(), bytesWith(1));
    EXPECT_EQ(second.get(), bytesWith(2));
}

TEST(ClassResourcePendingCommitTest, commitTwiceIsNoOp)
{
    mjbase::model::ClassResource resource("A.class", bytesWith(1));
    ClassResourcePendingCommit   pending;

    pending.stage(&resource, bytesWith(2));
    pending.commit();
    pending.commit();
    EXPECT_EQ(resource.get(), bytesWith(2));
    EXPECT_TRUE(pending.empty());
}
