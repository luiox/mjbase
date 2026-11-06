/// @file        lvt_display_policy.hpp
/// @brief       LVT（LocalVariableTable）显示名策略：逐名形态判定 + jar 级
///              兜底 + `var<槽位号>` 呈现。
///
/// 纯函数面：输入 LVT 行集/形态统计，输出呈现名；无全局状态、无正则引擎
/// （禁 std::regex，形态判定手写逐字节扫描）。只作用于显示层
/// 输出——调用方（mjt workspace 查询面）把原始 LVT 行喂进来、拿呈现名渲染
/// 出去；字节码与 LVT 原文一律不改写，也不入库。
///
/// 判定规则（阈值细节测试钉死）：
///   - 合法标识符（ASCII 字母/数字/下划线/$、首字符非数字、≤64 字节）保留
///     原名——obf 没清 LVT 时原名是 deobf 证据；
///   - 空、首字符数字、非 ASCII、控制字符、超长怪名 → `var<槽位号>`
///     （`\uXXXX` 字面形态含反斜杠，不是合法字符，逐字符扫描一并拒绝）；
///   - cat2（long/double）占两槽：呈现名直接用行槽位号，编号按物理槽自然
///     跳号（var2/var4，无 var3）；
///   - 同槽出现 ≥2 个不同描述符（跨作用域复用）→ 该槽 var 形态行加类型
///     后缀 `var<槽>_<类型>`；
///   - jar 级质量兜底：非法名占比 ≥ 1/2 判整 jar 名字空间不可信，全 jar
///     归 `var<槽位号>`，不逐名纠结短名。
#pragma once

#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::analysis {

/// @brief 单条 LVT 行的呈现判定输入（字节码 LVT/LVTT 行的最小视图）。
struct LvtDisplayRow final
{
    std::string_view name;       ///< LVT 原名（classfile Utf8 原文字节）
    std::string_view descriptor; ///< 类型描述符；LVTT 行为签名文本
    ca::u32          slot = 0;   ///< 槽位号（cat2 占首槽）
};

/// @brief LVT 名字形态统计（jar 级兜底判定的输入）。
struct LvtShapeStats final
{
    ca::usize total   = 0; ///< 行总数
    ca::usize invalid = 0; ///< 原名不可保留（非法形态）的行数
};

/// @brief 原名可保留的字节长度上限；超过按超长怪名处理（`var<槽位号>`）。
inline constexpr ca::usize kLvtMaxTrustedNameBytes = 64;

/// @brief 逐名形态判定：原名是否可保留（显示层直接透出的资格）。
///
/// 合法 = 非空、全 ASCII、每字符属于 [A-Za-z0-9_$]、首字符非数字、字节长度
/// ≤ kLvtMaxTrustedNameBytes。逐字节扫描：任一字节 ≥0x80（非 ASCII）、
/// <0x20 或 0x7F（控制字符）即不可信；`\uXXXX` 字面形态的反斜杠不是合法
/// 字符，随同一扫描一并拒绝，无需单列。
bool lvtNameTrustworthy(std::string_view name) noexcept;

/// @brief jar 级质量兜底：非法名占比 ≥ 1/2（total>0）判整 jar 不可信。
///
/// 阈值取 1/2：兜底宁枉勿纵——漏放对抗名进模型上下文的代价大于丢原名
/// （`var<槽位号>` 仍保留槽位结构信息）。阈值由测试钉死。
bool lvtJarFallback(const LvtShapeStats& stats) noexcept;

/// @brief 核心呈现：输出与 @p rows 一一对应的呈现名。
///
/// jarFallback=false 且原名可保留 → 原名；否则 `var<槽位号>`。同槽 ≥2 个不同
/// 描述符时 var 形态行加类型后缀：基本类型全名（int/long/…）、对象类型取
/// 最后一段简单名（泛型签名在 '<'/';' 处截断，LVT 行与 LVTT 行同变量收敛
/// 同后缀）、数组元素后缀加 Array、畸形描述符按该槽去重描述符首现序落
/// `t<序号>`；保留原名的行不加后缀（原名本身即区分）。输出恒为 ASCII 安全
/// 文本（保留原名即已判可信 = 纯 ASCII；var 形态生成即 ASCII），对抗字节
/// 不会沿此面流向文本出口。两个不同描述符推导出同一后缀时共用同名——
/// 显示层近似，LVT/LVTT 成对行因此收敛。
std::vector<ca::str::Utf8String> lvtDisplayNames(const std::vector<LvtDisplayRow>& rows,
                                                 bool                              jarFallback);

}   // namespace mjbase::analysis
