/// @file        log_init_test.cpp
/// @brief       log_init 编译冒烟 + 幂等抽检。
///
/// mbase 对 log_init.hpp 无单测；迁移验收以编译通过为主，
/// 抽检仅验证幂等语义不崩溃。
#include <gtest/gtest.h>

#include "mjbase/log/log_init.hpp"

TEST(MjLogTest, initDefaultLoggerIdempotent)
{
    mjbase::log::initDefaultLogger();
    mjbase::log::initDefaultLogger();   // 幂等：重复注册不崩溃
    SUCCEED();
}
