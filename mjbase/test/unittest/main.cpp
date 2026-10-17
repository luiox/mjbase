/// @file        main.cpp
/// @brief       mjbase 单测入口。
#include <gtest/gtest.h>

#include "test_helper.hpp"

int main(int argc, char** argv)
{
    // 注册 mjbase 子项目根（mjbase/.project_root_file）：test_resource 资源访问
    // （golden 语料/基线）经 test_helper 统一入口解析。
    test::setup("mjbase");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
