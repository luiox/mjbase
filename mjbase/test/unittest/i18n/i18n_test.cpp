/// @file        i18n_test.cpp
/// @brief       i18n 兜底门面编译冒烟 + 兜底注册行为抽检。
///
/// 对 i18n.hpp 的单测以编译通过为主，
/// 抽检验证兜底注册后 key 可解析。
#include <gtest/gtest.h>

#include <string>

#include "libca/i18n/i18n.hpp"

#include "mjbase/i18n.hpp"

TEST(MjI18nTest, ensureI18nBootRegistersFallback)
{
    mjbase::ensureI18nBoot();
    EXPECT_TRUE(ca::i18n::initialized());

    const char* text = mjbase::tr("mbase.cli.usage");
    EXPECT_NE(text, nullptr);
    // 兜底表已注册：不再裸 key 泄漏（key 空间保持 mbase.*，文案 zh_CN）。
    EXPECT_NE(std::string(text), "mbase.cli.usage");
}
