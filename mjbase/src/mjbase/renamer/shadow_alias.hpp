/// @file        shadow_alias.hpp
/// @brief       shadow name 表：真名 → 显示名映射，仅服务显示层，永不写回字节码。
///
/// 机械占位名 + 引用
/// token）。
///
/// - 成员键走跨类身份契约："owner\x1fname\x1fdesc" 拼接串键换
///   mjbase::asm_analyzer::aliasMemberKey 建的 MemberKey（进程级内容寻址驻留，
///   等值即内容等值）；类键用表私有 KeyPool 驻留 id。不发明跨模块字符串键。
/// - 字符串面按 mjbase 约定改 std::string_view 借用视图（表私有 KeyPool
///   驻留，寿命 = 表对象寿命）；渲染产物（printReadable 拼接）是值返回。
/// - token 补号的随机源失败由 throw 改 Result 值语义（不抛异常不吞错）。
///
/// 与真实 remap 数据零混写（不进 RenamePlan）；sidecar 经
/// ShadowAliasFormat（注册名 "Shadow-Alias"）读写，解析产物
/// IntermediateMapping（oldName=raw、newName=shadow）由 fromMapping 收编。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/model/resource_container.hpp"

namespace mjbase::renamer {

/// @brief shadow 别名条目的符号类别。
enum class ShadowKind : ca::u8
{
    Class,
    Field,
    Method,
};

/// @brief 一条 shadow 别名：真名（raw）→ 仅显示层名字（shadow）。
///
/// 所有字段都是 ShadowAliasTable 私有池的驻留视图；寿命 = 表对象寿命。
struct ShadowAliasEntry
{
    ShadowKind       kind       = ShadowKind::Class;
    std::string_view owner      = {};  ///< class 条目为空
    std::string_view desc       = {};  ///< class 条目为空
    std::string_view rawName    = {};
    std::string_view shadowName = {};
    /// #1156 引用 token：8 个小写 hex 字符（uint32 %08x 语义），shadow 名分配时
    /// 由 CSPRNG 生成、表内唯一。旧格式 sidecar（v1）加载的条目为空。
    std::string_view token8     = {};
};

/// @brief 机械占位名生成统计（cmd shadow 摘要用）。
struct ShadowGenStats
{
    ca::usize assignedClasses = 0;
    ca::usize assignedMethods = 0;
    ca::usize assignedFields  = 0;
    ca::usize keptEntries     = 0;  ///< 生成前已登记、本次保留的条目数
};

/// @brief shadow name 表（登记/查询/渲染/双向范围查询；显示层专用）。
///
/// -- 视图寿命声明 --
/// 本成员持有表私有的 KeyPool。指向本池的视图包括：entries_ 各字段、
/// classByRaw_/byShadowName_/byRawName_ 的 key 与查询返回值。视图寿命 =
/// 本对象寿命；死亡点：clear()、析构。消费者不得把 entries()/查询返回的
/// 视图带过本对象的死亡点。成员唯一性键是 aliasMemberKey 的 MemberKey
/// （进程级池驻留，与表池解耦，只作等值判别）。
class ShadowAliasTable
{
public:
    /// @brief 从 sidecar 解析产物构造（同符号重复登记以先到者优先）。
    ///        v2 产物条目的 token 列随行收编（#1156）；token 缺省为空。
    static ShadowAliasTable fromMapping(const mjbase::mapping::IntermediateMapping& mappings);

    /// @brief 导出为 sidecar 产物（writeShadowAliasText / registry "Shadow-Alias" 消费）。
    ///        token8 随行导出到产物条目 token 列（#1156）。
    mjbase::mapping::IntermediateMapping toMapping() const;

    /// @brief 登记 class 别名；同一 raw 符号已登记时返回 false（保留先登记者）。
    ///        token8 缺省为空（旧格式加载/无 token 场景）。
    bool addClass(std::string_view raw, std::string_view shadow,
                  std::string_view token8 = {});

    /// @brief 登记 field 别名；同一 (owner, desc, raw) 已登记时返回 false。
    bool addField(std::string_view owner, std::string_view desc, std::string_view raw,
                  std::string_view shadow, std::string_view token8 = {});

    /// @brief 登记 method 别名；同一 (owner, desc, raw) 已登记时返回 false。
    bool addMethod(std::string_view owner, std::string_view desc, std::string_view raw,
                   std::string_view shadow, std::string_view token8 = {});

    bool    empty() const noexcept { return entries_.empty(); }
    ca::usize size() const noexcept { return entries_.size(); }

    /// @brief 全部条目（登记序；视图寿命见类注释）。
    const std::vector<ShadowAliasEntry>& entries() const { return entries_; }

    // ── 正向查询：未登记返回空视图 ──────────────────────────────────
    std::string_view classShadow(std::string_view raw) const;
    std::string_view fieldShadow(std::string_view owner, std::string_view raw,
                                 std::string_view desc) const;
    std::string_view methodShadow(std::string_view owner, std::string_view raw,
                                  std::string_view desc) const;

    // ── 显示渲染（#1072 钉死：shadow 命中 → "shadow (原始名)"，否则原样；
    //    两段均经 IdentifierUtil::printReadable 转义，与既有打印口径逐字节一致）──
    /// @brief class 内部名渲染（拼接产物值返回）。
    std::string renderClass(std::string_view raw) const;

    /// @brief 成员名字片段渲染（不含 owner/desc；方法标签等分离打印点用）。
    std::string renderMemberName(std::string_view owner, std::string_view raw,
                                 std::string_view desc) const;

    /// @brief 完整成员形式 `owner.name:desc`；未命中输出与既有 printReadable
    ///        拼接逐字节一致。命中时 owner/name 段按 `shadow (raw)` 并列；
    ///        尾随 ` (完整原始形式)` 只在尾段含前段外信息时输出（#1106 P0-2）。
    std::string renderMember(std::string_view owner, std::string_view name,
                             std::string_view desc) const;

    // ── 反向 / 范围查询（MCP 双向查询、jasm legend 用）────────────────
    /// @brief shadow 名 → 条目列表（机械占位名跨类重号，可多条）。
    std::vector<ShadowAliasEntry> findByShadowName(std::string_view shadow) const;

    /// @brief 真名 → 条目列表（class 为内部名；成员为裸名，跨 owner 可多条）。
    std::vector<ShadowAliasEntry> findByRawName(std::string_view raw) const;

    /// @brief 某 class 名下全部条目（class 条目自身 + 以它为 owner 的成员条目）。
    std::vector<ShadowAliasEntry> entriesOfClass(std::string_view owner) const;

    /// @brief class 条目精确查询；未登记返回 nullptr（指向 entries_ 内部）。
    const ShadowAliasEntry* classEntry(std::string_view raw) const;

    /// @brief 给缺 token8 的条目补 CSPRNG 随机号（#1156）：8 位小写 hex、表内
    ///        唯一（碰撞重生成）；已有 token 的条目原样保留（幂等，重跑不换号）。
    ///        随机源 = libca ca::crypto::secure_random_bytes（系统 CSPRNG）；
    ///        系统 RNG 不可用时报 Err（不自造随机源、不静默降级）。
    ///        返回本次补号条数。
    ca::core::Result<ca::usize, std::string> backfillTokens();

    // ── 生成期占用检查 ─────────────────────────────────────────────
    /// @brief shadow 名是否已被其它 class 占用（除 exceptRaw 外）。
    bool classShadowTaken(std::string_view shadow, std::string_view exceptRaw) const;

    /// @brief shadow 名是否已被该 owner 作用域内的其它成员占用。
    bool memberShadowTaken(std::string_view owner, std::string_view shadow) const;

    /// @brief 清空全表（视图全部失效；prune 重建路径用）。
    void clear();

private:
    /// 成员唯一性键：aliasMemberKey 建键（进程级驻留，等值 = 内容等值）。
    static mjbase::MemberKey memberKeyOf(std::string_view owner, std::string_view raw,
                                         std::string_view desc);

    /// 私有驻留池：登记写入 + 查询按需 intern 探测键（classShadow/findBy* 等
    /// const 查询也驻留），故为 mutable。
    mutable mjbase::KeyPool            pool_;
    std::vector<ShadowAliasEntry>      entries_;
    std::unordered_map<ca::u32, ca::usize> classByRaw_;   ///< 类内部名池 id → 条目下标
    std::unordered_map<mjbase::MemberKey, ca::usize, mjbase::MemberKeyHash>
        memberByOwnerKey_;
    std::unordered_map<std::string_view, std::vector<ca::usize>> byShadowName_;
    std::unordered_map<std::string_view, std::vector<ca::usize>> byRawName_;
};

/// @brief 一键批量生成机械指代名（#1072 钉死规则）。
///
/// - class：全 jar 按内部名字典序全局排序，位置 i（1 起）→ `Class_i`；
/// - method/field：类内按 (name, desc) 字典序排序，位置 N（1 起）→
///   `Method_N` / `Field_N`（作用域 = 所属 class）；
/// - 已登记 shadow 的符号保留原条目（幂等：重跑不换号，手工名不丢）；
/// - 生成的名字如与作用域内已有 shadow 名撞车则 N 递增跳过；
/// - 收尾 backfillTokens()：新条目与历史缺号条目一律补 CSPRNG token8，已有
///   token 原样保留（随机源失败 → Err；已分配编号保留在表，重跑幂等补号）。
/// 确定性：jar 不变则影子名编号输出不变（token8 为随机列，不参与编号与排序）；
/// 容器遍历序无关（先排序后编号）。
ca::core::Result<ShadowGenStats, std::string> generateMechanicalShadowNames(
    const mjbase::model::ResourceContainer& container, ShadowAliasTable& table);

/// @brief 重载拆分生成统计（同名异签方法的视图层拆分）。
struct OverloadSplitStats
{
    ca::usize splitGroups     = 0;  ///< 拆分的同名异签组数（size >= 2）
    ca::usize assignedMethods = 0;  ///< 本次新分配 shadow 的方法数
    ca::usize keptEntries     = 0;  ///< 已登记 shadow 保留（幂等）的方法数
};

/// @brief 重载拆分（视图层同名异签拆分）：同名方法组内按描述符字典序生成
///        `<原名>$N`（N 从 0 递增）shadow 名，写入本表；字节码零写入。
///
/// - 拆分键 = (owner, name, desc) 成员三元组（表既有身份键；descriptor 只
///   参与组内排序与反查，不另立身份面）；
/// - 仅 size >= 2 的同名组拆分（无重载不分配）；组内已登记成员保留原
///   shadow（幂等：重跑不换号、手工名不丢），其占用照常参与撞号递增；
/// - 候选名撞车（类内真实方法/字段名或作用域内已分配 shadow）→ N 递增
///   跳过；
/// - 类按内部名字典序、组按名字字典序、组内按 desc 字典序——容器遍历序
///   无关，jar 不变重跑输出不变；
/// - 收尾 backfillTokens()（同机械占位名生成；随机源失败 → Err，已分配
///   编号保留在表，重跑幂等补号）。
ca::core::Result<OverloadSplitStats, std::string> generateOverloadSplitShadows(
    const mjbase::model::ResourceContainer& container, ShadowAliasTable& table);

/// @brief 批量 shadow 指派规格（成员身份 = (owner, name, desc) 三元组）。
struct OverloadShadowSpec
{
    std::string owner;    ///< class 内部名
    std::string name;     ///< 方法真名
    std::string desc;     ///< 方法描述符
    std::string shadow;   ///< 指派 shadow 名（显示层）
};

/// @brief 批量方法 shadow 指派（单事务）：全部规格校验通过才落表，任一
///        失败整批拒绝（表零改动，Result 单错误通道）。返回本次新登记条数。
///
/// 校验面（逐规格）：
/// - owner 类存在且声明了 (name, desc) 方法，否则 Err；
/// - shadow 非空；撞类内真实方法/字段名 → Err；撞作用域内其它成员的
///   shadow（含同批先到规格）→ Err；
/// - 成员已登记同 shadow → 幂等保留（不计新增）；已登记异 shadow → Err
///   （先到者优先契约；撤销/重算走 clear/prune 重建路径）；
/// - 同成员重复规格：同 shadow 视为一条，异 shadow → Err。
/// 落表后收尾 backfillTokens()（随机源失败 → Err）。
ca::core::Result<ca::usize, std::string> assignMethodShadowBatch(
    const mjbase::model::ResourceContainer& container, ShadowAliasTable& table,
    const std::vector<OverloadShadowSpec>& specs);

/// @brief 清理指向不存在符号的过期条目（jar 变更后的失配容忍补充，--shadow-prune）。
///
/// 存在性口径：class 条目要求容器内有该 class；成员条目要求 owner 存在且
/// 声明了同 (desc, name) 成员。返回移除条数。
ca::usize pruneShadowAliases(const mjbase::model::ResourceContainer& container,
                             ShadowAliasTable&                       table);

/// @brief jasm 文本显示层 shadow 改写（#1072 显示层；输出不再是可回编译 jasm）。
///
/// 整 token 替换：class 条目按转义后内部名全 jar 范围替换（含描述符内
/// `L<name>;` 的 L 前缀连带）；成员条目仅替换 ownerClass 名下的裸名 token 与
/// `owner.name` 复合 token。引号字符串字面量内部不动。配合
/// entriesOfClass 在文本前打 legend（`shadow = raw` 对照）。
std::string rewriteJasmForDisplay(std::string_view                  jasmText,
                                  const ShadowAliasTable&           table,
                                  std::string_view                  ownerClass);

}   // namespace mjbase::renamer
