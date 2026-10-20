#pragma once

// mjbase 全局 i18n 兜底入口。
//
// 各产品线 main 通常在启动时调用 ca::i18n::init 注册各自的
// 翻译表;但 mjbase 的静态库代码可能被编辑器、插件宿主等“未显式 init i18n”
// 的环境消费。若这些层直接调用 ca::i18n::tr,未注册 key 会原样返回(裸 key
// 泄漏到用户界面)。
//
// 本头提供 ensureI18nBoot():首次调用时用 mjbase 自带表兜底注册,保证描述/
// 错误消息始终可解析(默认 zh_CN)。key 空间与 旧栈 保持一致(mbase.* 全部
// key,文案自 旧栈 单源表 cli/translations 原样带入)——tr() 按字面 key 查
// 表,迁移期 key 前缀不改。已 init 的宿主不受影响——init 后注册的表格仍按
// “后注册者胜”覆盖 mjbase 默认文案。

#include "i18n_mjbase_translations.generated.hpp"
#include "libca/i18n/i18n.hpp"

namespace mjbase {

/// 若宿主未注册任何翻译表,用 mjbase 自带表兜底注册(不覆盖已 init 的宿主)。
inline void ensureI18nBoot()
{
    if (!ca::i18n::initialized()) {
        ca::i18n::init({kI18nTableSet_mjbase}, {});
    }
}

/// mjbase 文案查询:先确保兜底注册,再按当前语言查询。
inline const char* tr(const char* key)
{
    ensureI18nBoot();
    return ca::i18n::tr(key);
}

}   // namespace mjbase
