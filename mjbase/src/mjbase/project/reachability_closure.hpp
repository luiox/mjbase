/// @file        reachability_closure.hpp
/// @brief       全程序方法可达闭包（CHA）：计算面 + 批次落库（schema v9，#159）。
///
/// 输入图是分析库持久事实（refs 整数对边 + classes/members/interfaces 层次
/// 基底），不是容器字节码：闭包是派生事实，失效判定吃输入图指纹（v9
/// closure_batches.graph_fingerprint），指纹必须锚定持久状态。CHA 口径见
/// doc_ai/reachability-closure-design.md §3：
///
/// - refs 无 opcode 位，方法边一律保守族展开（M5 实测与带 opcode 口径差集
///   +927 行 / +3.1%，方向恒为超集）；静态/私有/构造器目标族退化为自身，
///   语义不变；
/// - clinit 触发边进闭包口径：Type/Field 引用与方法边（refs 无 opcode，
///   「可能首触」保守处理）触发目标类 `<clinit>`，沿 super 链传递（JVM
///   初始化次序）；
/// - default 方法冲突不做 most-specific 裁决，族内具体声明全入（保守并集，
///   宁过近似不丢真实执行边）；
/// - 闭包域 = 库内已登记方法（refs 文本分支的未登记目标是外部符号，只吃
///   其已登记 owner 类的 clinit 触发，不产成员节点）。
///
/// 种子三档（可叠加，seed_mask 位词表）：entry_points（`public static void
/// main(String[])` 发现，对齐 reference::isJavaMainMethod 谓词）、keep_roots
/// （KeepTier::PublicProtected 档位投影：public 类 × public/protected 具体
/// 方法）、显式 member_id 清单（未知 id 拒绝，Invalid）。种子级归属不落库
/// （「从哪个种子可达」不是本批维度，见设计稿 §4）。
///
/// 失效与复用：指纹 = SHA-256(closureFingerprintPayload)，payload 覆盖
/// inputs 投影 + classes/interfaces/members/refs 变更敏感聚合（ref_id 单调
/// 不复用，任何整库替换或切片替换必改聚合；聚合是变更敏感定位非内容寻址，
/// 见设计稿 §5）。指纹命中既有已收尾批 → 复用不重算（reused=true）；不匹配
/// → 整体重算落新批，不增量修补。
///
/// 原子写：批次生命周期由 AnalysisDb::storeClosureBatch 托管（开 closure_write
/// 批 → 事实行 closure_batches + closure_members + batch_scopes 类域单事务
/// 落库 → 收尾批）；事实段中途失败留残批不写事实——批次是逻辑运行记录，
/// 残批是合法审计状态。确定性：种子升序入队、邻接按 target 升序、FIFO
/// worklist、族展开升序去重，同输入双算成员集逐字节一致。
///
/// indy / MethodHandle 展开边不在本批射入（coverage_mask bit1 恒 0，接线随
/// 后置批）；lambda / method_handle / reflection 形态的边在 refs 无 kind
/// 分类，闭包在这些点欠近似，coverage_mask bit0（refs direct-only）恒置位
/// 自报覆盖度。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/project/analysis_db.hpp"

namespace mjbase::project {

/// @brief 闭包批次覆盖度位（closure_batches.coverage_mask 词表）。
enum class ClosureCoverageMask : ca::u32
{
    RefsDirectOnly = 0x1u, ///< refs direct-only（v9 固有覆盖度，恒置位）
    IndyEdges      = 0x2u, ///< indy 补充边已射入（本批恒 0，接线随后置批）
};

/// @brief 种子档位位（closure_batches.seed_mask 词表；可叠加）。
enum class ClosureSeedMask : ca::u32
{
    EntryPoints = 0x1u, ///< main 发现（isJavaMainMethod 谓词在 members 表的投影）
    KeepRoots   = 0x2u, ///< keep_roots 档位投影（public 类 × public/protected 具体方法）
    Explicit    = 0x4u, ///< 显式 member_id 清单（options.explicitSeeds）
};

/// @brief 闭包计算选项：种子三档叠加；全空 = 无种子，Invalid。
struct ReachabilityClosureOptions
{
    bool                 entryPoints = false;
    bool                 keepRoots   = false;
    std::vector<ca::i64> explicitSeeds; ///< 显式种子（未知 member_id → Invalid）
};

/// @brief 一次闭包计算 / 复用的统计。
struct ReachabilityClosureStats
{
    ca::i64   batchId           = 0;
    ca::i64   memberCount       = 0;      ///< 闭包成员行数（复用时取既有批行数）
    ca::u32   seedMask          = 0;      ///< 种子档位位词表（seedMaskOf 投影）
    ca::usize seedCount         = 0;
    ca::usize clinitMarks       = 0;      ///< clinit 触发入队次数（诊断面）
    ca::usize familyExpansions  = 0;      ///< 保守族展开次数（诊断面）
    ca::usize textBranchClasses = 0;      ///< 文本分支已登记 owner 类的 clinit 触发数
    bool      reused            = false;  ///< 指纹 + 档位命中既有批，未重算
};

/// @brief 计算可达闭包并落库（指纹 + 种子档位命中已收尾批时复用不重算）。
///
/// producer 必须已注册（建议 key `mjbase.reachability.cha`，trusted 档）；
/// 批次 kind = closure_write。库为只读连接 → ReadOnly；种子集为空 →
/// Invalid；方法边邻接经 AnalysisDb::closureGraphEdges 批量快照，层次基底
/// 经 classes/interfaces/members 读取面装载。
ca::Result<ReachabilityClosureStats, AnalysisDbError> computeAndStoreReachabilityClosure(
    AnalysisDb&                          db,
    ca::i64                              producerId,
    const ReachabilityClosureOptions&    options);

/// @brief 闭包输入图指纹（32 字节 SHA-256）。
///
/// 载荷 = AnalysisDb::closureFingerprintPayload()（canonical 序列化：fp_version
/// 字节 + inputs 投影 + 四表变更敏感聚合，设计稿 §5）。独立暴露给测试钉
/// 失效往返（载荷逐字节对不上即指纹必不同）。
ca::Result<std::vector<ca::u8>, AnalysisDbError> reachabilityClosureFingerprint(
    const AnalysisDb& db);

}   // namespace mjbase::project
