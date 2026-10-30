/// @file        shadow_token.hpp
/// @brief       #1156 shadow 引用 token —— resolve(s, pos) 单一收口 + name 槽文法。
///
/// 规范：shadow-reference-token-spec（唯一事实源）
///   §2 语法   ClassRef  := "${Class_" 1*DIGIT "." 8*HEXDIG "}"
///             MemberRef := ClassRef "." 1*(shadow 段) "." 8*HEXDIG
///             转义      $${...} 表示字面 "${"
///   §3 位置三分 解析是文法驱动的：展开只发生在文法标识槽；字符串常量/注释
///             恒等、永不解析（文法上不是标识槽，本收口根本不会被调用——
///             该性质由测试钉住，而非运行时检查）。
///   §4 中段裁定 `${` 出现在类型名/操作数中段（非段首）不展开、不静默：
///             二进制名文法不含花括号，未标记的引用形状混入裸名 =
///             调用方笔误 → Descriptor 类型段与 JasmBody 裸引用部
///             fail-loud（malformed-handle）；name 槽保持整串二选一
///             （raw 零漂移，verifyRaw 提供核表响亮路径）；`$${` 显式
///             转义两槽均保留字面（调用方自证字面意图）。
///   §6 错误四分类 unknown-handle / malformed-handle / raw-not-found /
///             ambiguous-member，各自带固定修复动作；缺 sidecar 时任何引用
///             形状 → 硬报错（missing-sidecar），不静默回落裸名。
///
/// 边界：Name/Descriptor/JasmBody 槽已实现。实修：查表命中面一律按值/optional 交付——
/// findClassByHandle 返回指向 findByShadowName 临时 vector 内部的指针（悬垂
/// UB，mjt 仓 9e72e9f 已修），本移植自始不带该形状。
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "mjbase/renamer/shadow_alias.hpp"

namespace mjbase::renamer {

/// @brief 解析位置（§3 位置三分）。单一收口按位置分派，后续扩展在此枚举上加
///        分支，不再新开解析入口——保证「展开只发生在文法标识槽」有唯一守门点。
enum class ShadowSlotPosition
{
    /// 整串参数槽：class/owner/name/class_uri/uri 等表意"类或成员引用"的
    /// 参数。整串 = Ref 或 raw 二选一（无子串扫描）。
    Name,
    /// 方法/字段描述符：词法走查 L…; 完整段，类型位展开（泛型签名
    /// fail-loud，Sig 属性 v2 另批）。
    Descriptor,
    /// JASM body 指令操作数（引用型）：类名 / owner.name 复合 + 尾随描述符。
    JasmBody,
};

/// @brief §2 文法形状分类（纯词法，无表参与；形状判定先于任何查表，
///        这是「两错误码永不 fall-through」的结构保证）。
enum class ShadowRefForm
{
    /// 非 `${`/`$${` 前缀：裸名。
    Raw,
    /// `$${` 前缀：字面名（剥前导 `$` 后按 Raw 语义）。
    EscapedLiteral,
    /// `${Class_N.token8}`。
    ClassRef,
    /// `${Class_N.token8}.{member影子名}.{token8}`。
    MemberRef,
    /// `${` 前缀但不合文法（缺后缀/坏十六进制/裸 `${`/坏段/尾随垃圾）。
    Malformed,
};

/// @brief parseShadowRef 的产物：形状 + 各段位（未命中的段为空）。
struct ShadowRefParts
{
    ShadowRefForm form        = ShadowRefForm::Raw;
    std::string   index;        ///< Class_ 序号（十进制串，保原样；不补齐不规范化）
    std::string   classToken;   ///< 8 位小写 hex
    std::string   memberShadow; ///< MemberRef 的成员影子名段
    std::string   memberToken;  ///< MemberRef 的 8 位小写 hex
    std::string   literal;      ///< EscapedLiteral：剥前导 `$` 后的字面名
};

/// @brief 纯文法解析（无表）：整串 → 形状 + 段位。
///
/// 口径：
/// - `${` 前缀但后续不合文法 → Malformed（含 `${Class_1}` 缺 token、坏 hex、
///   `Class_` 前缀大小写不符、段数不符、`}` 后尾随字符）；
/// - `${` 不在串首 → 整串按 Raw（name 槽无子串扫描，mid-string `${` 只是
///   裸名字节）；
/// - `$${` 前缀 → EscapedLiteral，literal = 剥一个 `$`；
/// - 影子名段接受 `[A-Za-z0-9_]+`：既覆盖规范示例 `handle_3`，也覆盖生成器
///   实际产物 `Method_N`/`Field_N`（大写开头）；合法性最终由表查证兜底
///   （段形状 ≠ 表内存在）；
/// - token8 段只认小写 hex（生成口径即小写；大写 = 形状违约 → Malformed，
///   手滑响亮报错）。
ShadowRefParts parseShadowRef(std::string_view input);

/// @brief §6 错误分类（四分类 + 缺 sidecar 硬报错）。
enum class ShadowSlotError
{
    None,
    /// 形状对但表里没有（陈旧 sidecar / 抄错号 / token 不匹配）。
    /// 修复动作：调一次回显工具（raw_to_shadow / shadow_to_raw）重新取引用。
    UnknownHandle,
    /// 形状坏（缺后缀/坏十六进制/裸 `${`）。修复动作：按语法重拼。
    MalformedHandle,
    /// 标识位给了裸名但不存在。修复动作：别手打脏名，用回显工具取 token。
    RawNotFound,
    /// 二元 handle（类 + 成员影子名）命中多条重载、无 desc 区分不了。
    /// 修复动作：补 raw desc。
    AmbiguousMember,
    /// 缺 sidecar 而输入为引用形状（§6 末行硬报错，不静默回落裸名）。
    /// 修复动作：加载 sidecar（MCP shadow_generate / `mjt shadow -i <jar>`）。
    MissingSidecar,
};

/// @brief 错误码字（消息前缀，与报告处方口径一致）：
///        "unknown-handle" / "malformed-handle" / "raw-not-found" /
///        "ambiguous-member" / "missing-sidecar"；None → 空串。
const char* shadowSlotErrorCode(ShadowSlotError error);

/// @brief 解析查询参数：位置 + 成员槽上下文。
struct ShadowSlotQuery
{
    ShadowSlotPosition position = ShadowSlotPosition::Name;
    /// true = 成员槽（name/member_name/method_name）：raw 分支按
    /// (owner, rawName) 定位核验，Ref 分支回填表内 desc。
    bool        member = false;
    /// member=true 时必填：已解析的 owner raw 内部名。
    std::string owner;
    /// member=true 时可选：调用方 raw desc（仅用于回传给接线层做一致性核对，
    /// 本收口不因 desc 不匹配改判四分类）。
    std::string descHint;
    /// false = raw 形状恒等直通、不做存在性核验（xref 族零漂移口径：表外
    /// 合法目标如 java/lang/Object 不因核表被拒）。Ref 形状不受此位影响，
    /// 照常全量解析。缺省 true（全量门控槽）。
    bool verifyRaw = true;
};

/// @brief 解析结果。error != None 时 message 含「码字: 诊断 + owner 上下文 +
///        §6 固定修复动作」；error == None 时 raw/descriptor/kind 有效。
struct ShadowSlotResult
{
    ShadowSlotError  error = ShadowSlotError::None;
    std::string      message;    ///< 诊断消息（错误时）；成功时为空
    std::string      raw;        ///< 解析出的真实名（Ref 展开）或原样（raw 恒等）
    std::string      descriptor; ///< MemberRef 命中的表内 desc；其余为空
    ShadowKind       kind = ShadowKind::Class; ///< 命中条目种类（raw 恒等路径 = 表内查得种类）
    bool             fromHandle = false;       ///< true = Ref 展开发生；false = raw 恒等
};

/// @brief resolve(s, pos) 单一收口（§3 name 槽行；唯一解析入口）。
///
/// 整串 = Ref 或 raw 二选一，永不 fall-through：
/// - Ref 形状（ClassRef/MemberRef）：只查表。命中 → 展开为真实名；
///   表里没有（含类/成员 token 不匹配）→ UnknownHandle；形状坏 →
///   MalformedHandle；成员影子名命中多条重载 → AmbiguousMember。
///   **Ref 形状永不产生 RawNotFound，raw 形状永不产生 UnknownHandle**——
///   形状判定先于查表（parseShadowRef 纯词法），结构上排除串码。
/// - raw 形状（Raw/EscapedLiteral）：verifyRaw 时查表核验存在性，不存在 →
///   RawNotFound；存在或缺表 → 恒等放行（缺 sidecar 的 raw 路径逐字节
///   零漂移：无表可核即原样通过）。
/// - 缺 sidecar（空表）+ 引用形状（ClassRef/MemberRef/Malformed）→
///   MissingSidecar 硬报错，不静默回落裸名。
/// - Descriptor 位置：走 resolveShadowDescriptor（见下）。
/// - JasmBody 位置：走 resolveShadowJasmBody（见下）。
ShadowSlotResult resolveShadowSlot(const ShadowAliasTable& table,
                                   const std::string&      input,
                                   const ShadowSlotQuery&  query = {});

/// @brief Descriptor 槽（§3 位置三分之描述符行）：字段/方法描述符内
///        `L…;` 段的类型位展开。
///
/// 文法（纯描述符，泛型签名不在内）：
/// - 词法走查 `(`/`)`/`[`/基类型（B C D F I J S Z V）逐字节直通；
/// - `L…;` 段：段内容按 parseShadowRef 分形——
///   Raw 恒等直通（描述符类型位任指任意类型，零漂移不核表，
///   query.verifyRaw 在本槽不适用）；EscapedLiteral 剥 `$` 后直通；
///   ClassRef 查表展开为真实内部名（同 resolveShadowHandleEntry 四分类，
///   UnknownHandle/MissingSidecar 原样上抛）；MemberRef → MalformedHandle
///   （类型位不收成员引用）；Malformed → MalformedHandle；
/// - 其余字节（含 `<`/`>`）不合描述符文法 → MalformedHandle（泛型签名
///   fail-loud，v2 另批；不静默替换产生垃圾输出）。
///
/// 结果：raw = 替换后的完整描述符；fromHandle = 发生过 ≥1 次展开；
/// descriptor/kind 不承载（类型位没有成员 desc 语义）。
ShadowSlotResult resolveShadowDescriptor(const ShadowAliasTable& table,
                                         const std::string&      input);

/// @brief JasmBody 槽（§3 位置三分之操作数行）：引用型指令操作数展开。
///
/// 输入 = 单条指令的引用型操作数（助记符之后；iinc/数字等非引用操作数由
/// 调用方不分派本槽）。文法（对齐 rewriteJasmForDisplay 的显示形态）：
///
/// - 尾随描述符切分：以 ")" 结尾 → 取最后一个 " (" 之前的引用部 + 括号内
///   方法描述符；否则取最后一个空格之后的字段描述符；无空格 = 纯引用部
///   （类操作数 new/checkcast/instanceof/anewarray/ldc 类常量）；
/// - 引用部：整体 ClassRef → 类内部名；整体 MemberRef → `rawOwner.rawName`
///   复合；`OWNER.RAWNAME` 复合（OWNER = ClassRef 或裸内部名）→ owner 展开/
///   恒等；纯裸名 → 恒等直通（显示文本的 shadow 名如 `Class_1.m_1` 不含
///   `\${` 形状，按 Raw 零漂移直通——shadow→raw 的通路是回显工具，不在本槽）；
///   形状冲突（owner 不同的嵌套 MemberRef 等）→ MalformedHandle；
/// - 描述符部：经 resolveShadowDescriptor 展开（类 handle 可内嵌）；
/// - query.verifyRaw/query.member/query.descHint 在本槽不适用（raw 零漂移：
///   java/lang/System.out 等表外目标恒等放行）。
///
/// 结果：raw = 替换后的完整操作数；fromHandle = 发生过 ≥1 次展开；
/// descriptor/kind 不承载。
ShadowSlotResult resolveShadowJasmBody(const ShadowAliasTable& table,
                                       const std::string&      input);

/// @brief 引用句柄（ClassRef/MemberRef）全量核验 → 命中条目（Ref 分支的条目面，
///        供回显工具 handle→条目反查；与 resolveShadowSlot 的 Ref 分支同判同错）。
///
/// - 形状非 ClassRef/MemberRef（Raw/EscapedLiteral/Malformed）：返回 nullopt 且
///   `result.error == None`（调用方按非句柄输入自行分派；Malformed 的分类归
///   resolveShadowSlot）；
/// - 形状对但核验失败（缺 sidecar / 类或成员查无 / token 不匹配 / 重载歧义）：
///   返回 nullopt，`result` 携带四分类错误与诊断消息；
/// - 命中：返回表条目拷贝，`result` 填 raw/descriptor/kind 且 fromHandle=true。
std::optional<ShadowAliasEntry> resolveShadowHandleEntry(const ShadowAliasTable& table,
                                                         const std::string&      handle,
                                                         ShadowSlotResult&       result);

/// @brief 条目的引用句柄（§2 形态，供回显工具发放）。
///
/// class 条目 → `${Class_N.token8}`；成员条目 →
/// `${Class_N.token8}.{shadow}.{token8}`（需 owner 类条目存在）。
/// 不可构造时返回空：缺 token8（v1 遗留）、类影子名非 `Class_<序号>` 形
/// （手工名不在 §2 文法内，通路是 rename）、成员影子名段出文法字符、
/// 成员条目的 owner 类条目缺失。
std::string shadowRefHandleOf(const ShadowAliasTable& table, const ShadowAliasEntry& entry);

}   // namespace mjbase::renamer
