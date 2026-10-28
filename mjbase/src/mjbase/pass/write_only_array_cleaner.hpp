#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 只写数组清理器：分配后仅被元素存储消费、无任何读取/逃逸的数组
///        连同存储窗整体删除（#37 参数装箱还原第三件——无用数组分配清理）。
///
/// 覆盖形态（快照序连续窗形状匹配，均以紧邻前驱纯 int 常量为 size 生产者）：
///   S1 直通形 [size; NEWARRAY/ANEWARRAY; (DUP; int常量; 元素常量; XASTORE)*; POP]
///   S2 暂存形 [size; NEWARRAY/ANEWARRAY; ASTORE n; (ALOAD n; int常量; 元素
///             常量; XASTORE)*]
/// 元素生产者须为单条纯常量装载（int/long/float/double/string 常量装载 +
/// ACONST_NULL，与 DeadLocalStoreRemover/UselessPopCleaner 帧面同族口径）；
/// 下标须为 int 常量；元素组 k >= 0（k=0 即 `分配; pop` 与 `分配; astore` 的
/// 死分配特形）。
///
/// 安全门（任一命中整窗放弃）：
///   - size 生产者非紧邻前驱或非纯 int 常量（删除分配会孤儿化其输入值）；
///   - 窗内任一指令（含窗首与 size 生产者）锚有标签（回边重执行分配语义
///     改变）；
///   - 任一异常范围与窗相交（分配/存储的抛出可观测，全含亦弃）；
///   - S2：局部 n 在窗外有任何读写出现（各类型 load/store 族 + IINC +
///     RET 全扫）——同一槽位复用/逃逸面保守拒收；
///   - MULTIANEWARRAY 不收（多维嵌套构造形状不闭包）。
///
/// 纯删除：maxStack/maxLocals 不变，lower 重算帧。发生删除返回 none()，
/// 否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class WriteOnlyArrayCleaner : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
