/// 测试面 table-driven 假 resolver（indy SPI/单测/JVM 测试共用）：站点名 →
/// 预设应答（目标四段 + 尾部元数据数 / ResolveFailed / 不应答=NoTarget）。
/// 文本一律字符串字面量视图——Spec 以值存储，视图指向调用方字面量。
#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include "mjbase/spi/indy_resolver.hpp"

/// indy 测试共用假 resolver（::TestTable）。
class TableIndyResolver : public mjbase::spi::IndyResolver
{
public:
    struct Spec
    {
        std::string_view         site;      ///< 匹配 IndySiteFacts::siteName
        ResolvedIndyTarget::Form form = ResolvedIndyTarget::Form::Static;
        std::string_view         owner;
        std::string_view         name;
        std::string_view         desc;
        ca::u32                  trailing = 0;   ///< 尾部元数据实参个数
        bool                     fail     = false;   ///< true = 应答 ResolveFailed
    };

    explicit TableIndyResolver(std::vector<Spec> specs) : specs_(std::move(specs)) {}

    std::string_view name() const override { return "TestTable"; }

    ca::core::Result<ResolvedIndyTarget, mjbase::spi::IndyResolverError>
        resolve(const mjbase::spi::IndySiteFacts& site) override
    {
        for (const Spec& spec : specs_) {
            if (spec.site != site.siteName) continue;
            if (spec.fail) {
                return ca::core::Err(mjbase::spi::IndyResolverError::ResolveFailed);
            }
            mjbase::spi::ResolvedIndyTarget target;
            target.form                 = spec.form;
            target.owner                = spec.owner;
            target.name                 = spec.name;
            target.desc                 = spec.desc;
            target.trailingMetadataArgs = spec.trailing;
            return ca::core::Ok(std::move(target));
        }
        return ca::core::Err(mjbase::spi::IndyResolverError::NoTarget);
    }

private:
    std::vector<Spec> specs_;
};
