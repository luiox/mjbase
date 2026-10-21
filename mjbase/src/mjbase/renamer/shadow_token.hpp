/// @file        shadow_token.hpp
/// @brief       #1156 shadow 引用 token —— resolve(s, pos) 单一收口 + name 槽文法。
///
/// 规范：旧栈 doc_ai/shadow-reference-token-spec.md（唯一事实源）
///   §2 语法   ClassRef  := "${Class_" 1*DIGIT "." 8*HEXDIG "}"
///             MemberRef := ClassRef "." 1*(shadow 段) "." 8*HEXDIG
///             转义      $${...} 表示字面 "${"
///   §3 位置三分 解析是文法驱动的：展开只发生在文法标识槽；字符串常量/注释
///             恒等、永不解析（文法上不是标识槽，本收口根本不会被调用——
///             该性质由测试钉住，而非运行时检查）。
///   §6 错误四分类 unknown-handle / malformed-handle / raw-not-found /
///             ambiguous-member，各自带固定修复动作；缺 sidecar 时任何引用
///             形状 → 硬报错（missing-sidecar），不静默回落裸名。
///
/// 边界：仅 Name 槽实现；Descriptor/JasmBody 分支未接线（传入即 fail-loud，
/// 不做文法）。相对旧栈的实修：查表命中面一律按值/optional 交付——旧栈
/// findClassByHandle 返回指向 findByShadowName 临时 vector 内部的指针（悬垂
/// UB），本移植自始不带该形状。
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
    /// 方法/字段描述符、Sig 属性 v2：词法走查 L…; 完整段。未接线。
    Descriptor,
    /// JASM body 指令操作数中的内部名/描述符。未接线。
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
    /// 修复动作：加载 sidecar（由项目 shadow 工具生成）。
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
/// - Descriptor/JasmBody 位置：未接线，fail-loud（MalformedHandle + 指引），
///   防意外提前使用。
ShadowSlotResult resolveShadowSlot(const ShadowAliasTable& table,
                                   const std::string&      input,
                                   const ShadowSlotQuery&  query = {});

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
