/// @file        reference_index_analysis.hpp
/// @brief       ReferenceIndex 的 GlobalAnalysisManager 包装。
///
/// 全局 pass 经 getResult<ReferenceIndexAnalysis> 请求直接引用事实，而不是
/// 自行重扫 container。索引自有键池随结果存活（「图自有池」纪律）。
#pragma once

#include "mjbase/model/resource_container.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::reference {

using mjbase::spi::GlobalAnalysisManager;

/// @brief Global analysis wrapper for ReferenceIndex.
///
/// Passes can request this from GlobalAnalysisManager when they need direct
/// method/field/type reference facts without owning the scan themselves.
struct ReferenceIndexAnalysis
{
    using Result = ReferenceIndex;

    Result run(mjbase::model::ResourceContainer& container, GlobalAnalysisManager& gam)
    {
        (void)gam;   // mjbase 索引经自有 KeyPool 驻留
        ReferenceIndex index;
        index.build(container);
        return index;
    }
};

}   // namespace mjbase::reference
