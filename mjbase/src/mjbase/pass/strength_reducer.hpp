#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 强度削减器：把「常量 2^k 的乘/除」改写为移位与移位-加法修正
///        序列（强度削减族；仅 int/long——float/double 的对应形态不在
///        本 pass 面，见输入边界）。
///
/// @par 策略（匹配矩阵）
///
/// 对 CodeUnit 指令序列做一趟快照扫描。游标 i 处指令为 IMUL/IDIV/LMUL/
/// LDIV，且前驱（i-1）是同宽度编译期常量装载（int：ICONST/BIPUSH/SIPUSH/
/// LDC(Int)，经 method_rewrite_util::intValueOf；long：LCONST/ldc 族
/// (Long)，经 longValueOf）时开启候选对：
///   - 常量值为正 2 的幂、指数 k 满足 1<=k<宽度 → 改写；
///   - 其余（0、负数、非 2 的幂、k==0、k>=宽度）→ 保守不动。
///
/// 改写形态（int 侧列出；long 侧同构替换：DUP→DUP2、31→63、ISHR→LSHR、
/// IAND→LAND、IADD→LADD）：
///   - `[x][2^k] IMUL` → `[x] BIPUSH k; ISHL`（指令数不变、栈形不变）；
///   - `[x][2^k] IDIV` → `[x] DUP; BIPUSH 31; ISHR; <mask=2^k-1>; IAND;
///     IADD; BIPUSH k; ISHR`（除数常量装载整条删除，序列峰值栈深 +1）。
/// 原运算指令就地改写为序列首指令（InsnId 不变，锚定其边界的标签自然
/// 跟随）；除数常量装载最后删除（kRetargetToFallthrough；边界守卫已
/// 保证两成员均无引用）。
///
/// @par 精确语义依据
///
/// - 乘法：Java `x * 2^k` 与 `x << k` 同在 mod 2^width 域回绕，逐位一致
///   （JVMS §6.5 imul/lmul 与 ishl/lshl 的「低 32/64 位」语义）；k<宽度
///   时移位量掩码（int 掩 5 位 / long 掩 6 位）不改变量。
/// - 除法：Java 除法向零取整（JLS §15.17.2）。序列计算
///   `q = (x + ((x >>s 31) & (2^k - 1))) >>s k`：x>=0 时符号项为 0，
///   q = x >>s k = x / 2^k（非负时向零=向下）；x<0 时符号项为 2^k-1，
///   q = floor((x + 2^k - 1) / 2^k) = trunc(x / 2^k)。1<=k<宽度时
///   x + 2^k - 1 不溢出（x >= -2^(width-1)，和落在 [-2^(width-1),
///   2^k-2] ⊂ 值域）。负被除数样例：-7/2 = (-7 + ((-7>>31)&1)) >> 1
///   = -6>>1 = -3 ✓；INT_MIN/2 = (INT_MIN+1)>>1 = -1073741824 ✓。
/// - 移位量 31/63 与 k 均取 BIPUSH 立即数（k<64 在 signed byte 域内）；
///   long 侧移位量仍是 int 类别（BIPUSH k），符合 JVMS lshl/lshr 的
///   「int 移位量」形状。
///
/// @par 输入边界
///
/// 只吃完整解码的 CodeUnit（驱动方 promoteMethod 之后）；decode 失败
/// （AttrNotDecoded / BadInstructions）由管线容错档位处置，本 pass 不见
/// 半解码状态。float/double 的 fmul/fdiv/dmul/ddiv 不匹配：IEEE 乘除
/// 2^k 的强度削减形态与整数不同（且常量折叠族已覆盖其精确面），列为
/// 后续。irem/lrem 不在本 pass 面。
///
/// @par 保守停止清单（任一命中 → 该候选对原样不动）
///
/// - 除数常量不是正 2 的幂、k==0（乘/除 1，留给 nop 清理家族）、
///   k>=宽度（防御性守卫：正常量实际最大 2^30 / 2^62，k=宽度-1 不可达，
///   守卫防的是常量读取面未来放宽到无符号域）；
/// - 常量装载或运算指令的前置边界有标签锚定（分支目标/异常表边界——
///   旧栈 LabelNode 阻断语义的 CodeDom 等价）：入边携带除数常量上栈，
///   改写后期望栈形已变，必须整体不动；
/// - div 改写遇 maxStack 距 u16 顶不足序列峰值预算（int +1 / long +2，
///   即 maxStack >= 0xFFFD 时不再改写）；
/// - mask 需物化 LDC/LDC2_W 时 CP 容量预检失败（cp.size()+待追加槽数
///   > 0xFFFF）；
/// - 任一编辑步骤失败：停止全盘扫描，已发生的修改保留、按 changed 交出
///   （失败语义归管线容错档位——与 junk_code_inserter /
///   char_arr_to_str_simplifier 同款纪律）。
///
/// @par 帧与 maxes
///
/// 本 pass 不触碰 SMT / 帧链接线（pass 不得自驱帧重算，写回权归管线）。
/// div 序列按宽度抬升模式处峰值栈深：int 侧原 `[x][c]` 峰 d+2 → 序列峰
/// d+3；long 侧原 `[x long][c long]` 峰 d+4 → DUP2/BIPUSH/mask-long 峰
/// d+6。maxStack 相应 +1 / +2（原类可验证 ⇒ maxStack>=原峰，抬升后恒覆盖
/// 新峰）；注入 typeResolver 的管线会在 lower 后用 Strict 重算值覆盖
/// maxes，本抬升只为未接线路径保底。mul 改写栈形不变，不动 maxStack。
///
/// @par PreservedAnalyses
///
/// 发生改写返回 none()（指令序列与 CP 均可能变），否则 all()。本 pass
/// 不注册也不消费任何分析结果（mam 未使用）。
///
/// @par 标签
/// - `scope:method`
/// - `action:simplify`
/// - `stability:opt-in`
class StrengthReducer : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
