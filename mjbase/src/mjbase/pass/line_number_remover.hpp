#pragma once

#include "jbc/ir/units.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 删除每个方法 Code 嵌套的 LineNumberTable。
///
/// 混淆产物的行号不对应任何真实源码位置，且方法体改写后原行首 pc 可能
/// 悬垂（lowerMethod 报 RelocationBadTarget）。本 pass 只摘 LNT 槽，
/// 保留 LocalVariableTable/LocalVariableTypeTable、指令载荷、异常表与
/// StackMapTable；改动经父 Code 槽置脏驱动重编码。无 LNT 时零改动。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class LineNumberRemover : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

}   // namespace mjbase::pass
