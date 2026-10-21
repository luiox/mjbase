/// @file        log_init_test.cpp
/// @brief       log_init 编译冒烟 + 幂等抽检。
///
/// 抽检仅验证幂等语义不崩溃。
#include <gtest/gtest.h>

#include "mjbase/log/log_init.hpp"

TEST(MjLogTest, initDefaultLoggerIdempotent)
{
    mjbase::log::initDefaultLogger();
    mjbase::log::initDefaultLogger();   // 幂等：重复注册不崩溃
    SUCCEED();
}
