#include "rename_engine.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "mjbase/analysis/alias_analysis.hpp"
#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/mapping/identifier_util.hpp"

namespace mjbase::renamer {
namespace {

using mjbase::ClassKey;

// ── 管线内部字符串工作表示：std::string 键（对齐 workspace 管线惯例），
//    模块边界（plan/record/proposal）一律驻留视图。 ──

std::string toStd(std::string_view value)
{
    return std::string(value);
}

ca::str::Utf8StringRef utf8Ref(std::string_view value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

bool isJavaKeyword(std::string_view name)
{
    for (const auto& keyword : mjbase::mapping::IdentifierUtil::javaKeyWords()) {
        if (std::string_view(reinterpret_cast<const char*>(keyword.data()),
                             keyword.byte_length()) == name) {
            return true;
        }
    }
    return false;
}

bool isReservedMethodName(std::string_view name)
{
    return name == "<init>" || name == "<clinit>";
}

/// 内部名路径合法性（类名/包路径）：按 '/' 分段，每段须为合法标识符且非关键字。
/// 段合法性复用 IdentifierUtil::isValidJavaIdentifier（口径与 mapping 线一致）。
enum class NamePathVerdict
{
    Ok,
    Illegal,
    Keyword,
};

NamePathVerdict checkNamePath(std::string_view path)
{
    if (path.empty()) return NamePathVerdict::Illegal;
    ca::usize begin = 0;
    while (begin <= path.size()) {
        const ca::usize slash = path.find('/', begin);
        const auto segment =
            slash == std::string_view::npos ? path.substr(begin) : path.substr(begin, slash - begin);
        if (segment.empty()) return NamePathVerdict::Illegal;
        const auto segmentRef = utf8Ref(segment);
        if (!mjbase::mapping::IdentifierUtil::isValidJavaIdentifier(segmentRef)) {
            return NamePathVerdict::Illegal;
        }
        if (isJavaKeyword(segment)) return NamePathVerdict::Keyword;
        if (slash == std::string_view::npos) break;
        begin = slash + 1;
    }
    return NamePathVerdict::Ok;
}

std::string trimTrailingSlash(std::string path)
{
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

/// owner 内的既有成员占用表（撞名检测用，值 = (name, desc)；字段槽忽略 desc）。
/// 方法 desc 以 plan 类映射预映像（mapped-desc 空间）：类改名
/// 会同步改写方法描述符中的类类型，最终类文件状态在 mapped-desc 空间，方法槽查撞
/// 必须与之同空间。调用时机：类映射已全部入 plan 之后。
/// 层级与池经入参注入（自由函数不占 RenameContext 的友元通道）。
struct OwnerSlots
{
    std::set<std::string>                         fieldNames;
    std::set<std::pair<std::string, std::string>> methodSlots;
};

std::unordered_map<std::string, OwnerSlots> collectOwnerSlots(
    const hierarchy::ClassHierarchy& hierarchy, const KeyPool& pool, const RenamePlan& plan)
{
    std::unordered_map<std::string, OwnerSlots> slots;
    for (const ClassKey classKey : hierarchy.registrationOrder()) {
        const auto* entry = hierarchy.summary(classKey);
        if (entry == nullptr) continue;
        auto&    owner = slots[toStd(hierarchy.nameOf(classKey))];
        for (const auto& field : entry->fields) {
            owner.fieldNames.insert(toStd(pool.str(field.name)));
        }
        for (const auto& method : entry->methods) {
            owner.methodSlots.emplace(toStd(pool.str(method.name)),
                                      plan.mapMethodDescriptor(pool.str(method.desc)));
        }
    }
    return slots;
}

/// proposal 管线条目：指向 run() 内存活的建议向量。
struct PipelineClassChange
{
    ca::usize   proposalIdx = 0;
    std::string oldName;
    std::string newName;
    bool        fromPackage = false;
};

struct PipelineFieldChange
{
    ca::usize   proposalIdx = 0;
    std::string owner;
    std::string oldName;
    std::string desc;
    std::string newName;
};

struct PipelineMethodChange
{
    ca::usize   proposalIdx = 0;
    std::string owner;
    std::string oldName;
    std::string desc;
    std::string newName;
    bool        fromClosure = false;
    bool        dropped     = false;   ///< 所属闭包组整组被拒时置位
};

struct MethodGroup
{
    std::vector<ca::usize> memberIndices;   // → methodChanges 下标
};

/// 同 target 裁决键：class/package = 新名全局槽；field = owner+新名（名字槽）；
/// method = owner+新名+desc（重载是合法的不同槽）。
using TargetKey = std::tuple<ca::u8, std::string, std::string, std::string>;

TargetKey targetKeyOf(const NameProposal& proposal)
{
    switch (proposal.kind) {
    case ProposalKind::Class:
    case ProposalKind::Package: return {static_cast<ca::u8>(proposal.kind), {}, toStd(proposal.newName), {}};
    case ProposalKind::Field:
        return {static_cast<ca::u8>(proposal.kind), toStd(proposal.owner), toStd(proposal.newName), {}};
    case ProposalKind::Method:
        return {static_cast<ca::u8>(proposal.kind), toStd(proposal.owner), toStd(proposal.newName),
                toStd(proposal.desc)};
    }
    return {static_cast<ca::u8>(proposal.kind), {}, toStd(proposal.newName), {}};
}

/// 同 old slot 键：同一成员/类只允许一个终名——两条幸存建议改同一位 → 裁决。
TargetKey oldSlotKeyOf(const NameProposal& proposal)
{
    switch (proposal.kind) {
    case ProposalKind::Class:
    case ProposalKind::Package: return {static_cast<ca::u8>(proposal.kind), {}, toStd(proposal.oldName), {}};
    case ProposalKind::Field:
    case ProposalKind::Method:
        return {static_cast<ca::u8>(proposal.kind), toStd(proposal.owner), toStd(proposal.oldName),
                toStd(proposal.desc)};
    }
    return {static_cast<ca::u8>(proposal.kind), {}, toStd(proposal.oldName), {}};
}

/// proposal 的槽位定位串（撞名报错面）：kind + owner + 原位 + 终名，让拒绝流水
/// 的 detail 自含「谁撞了谁」两侧坐标。
std::string proposalSlotRepr(const NameProposal& proposal)
{
    const std::string kind = proposal.kind == ProposalKind::Class    ? "class"
                             : proposal.kind == ProposalKind::Package ? "package"
                             : proposal.kind == ProposalKind::Field   ? "field"
                                                                     : "method";
    if (proposal.kind == ProposalKind::Class || proposal.kind == ProposalKind::Package) {
        return kind + " " + toStd(proposal.oldName) + " -> " + toStd(proposal.newName);
    }
    return kind + " " + toStd(proposal.owner) + "." + toStd(proposal.oldName) +
           toStd(proposal.desc) + " -> " + toStd(proposal.newName);
}

/// proposal 的来源定位串：source + confidence + 槽位坐标。
std::string proposalOriginRepr(const NameProposal& proposal)
{
    return "source '" + toStd(proposal.source) + "' confidence " +
           std::to_string(proposal.confidence) + ": " + proposalSlotRepr(proposal);
}

/// 裁决结果：key 首次认领 / 在位者保位（挑战者出局）/ 挑战者夺位（在位者出局）。
enum class Adjudication
{
    Claimed,
    IncumbentKept,
    ChallengerWon,
};

/// 同 target 裁决：高位 confidence 者胜；同 confidence 先到者胜（后到拒）。
/// 出局者下标经 loserIdx 返回，由调用方写流水。
Adjudication adjudicate(std::map<TargetKey, ca::usize>&      winnerOfKey,
                        std::vector<ca::usize>&             winners,
                        const std::vector<NameProposal>&    proposals,
                        const TargetKey&                    key,
                        ca::usize                           index,
                        ca::usize&                          loserIdx)
{
    auto it = winnerOfKey.find(key);
    if (it == winnerOfKey.end()) {
        winnerOfKey.emplace(key, index);
        winners.push_back(index);
        return Adjudication::Claimed;
    }
    const double incumbentConfidence  = proposals[it->second].confidence;
    const double challengerConfidence = proposals[index].confidence;
    if (challengerConfidence > incumbentConfidence) {
        loserIdx   = it->second;
        it->second = index;
        std::replace(winners.begin(), winners.end(), loserIdx, index);
        return Adjudication::ChallengerWon;
    }
    loserIdx = index;
    return Adjudication::IncumbentKept;
}

}   // namespace

// ─────────────────────────── NameChangeRecord ───────────────────────────

void NameChangeRecord::addAccepted(AcceptedChange change)
{
    accepted_.push_back(std::move(change));
}

void NameChangeRecord::addRejected(NameProposal proposal, RejectionReason reason,
                                   std::string detail)
{
    rejected_.push_back({std::move(proposal), reason, std::move(detail)});
}

/// MappingError 的短标签（renamer 导出通道的拼串用）。
static std::string_view mappingErrorName(mjbase::mapping::MappingError error)
{
    switch (error) {
    case mjbase::mapping::MappingError::InvalidText: return "invalid text";
    case mjbase::mapping::MappingError::UnknownFormat: return "unknown format";
    case mjbase::mapping::MappingError::Io: return "io";
    }
    return "?";
}

ca::core::Result<ca::str::Utf8String, std::string> NameChangeRecord::exportMapping(
    const mjbase::mapping::IntermediateMapping&   mapping,
    std::string_view                              formatName,
    const mjbase::mapping::MappingFormatRegistry& registry)
{
    if (!registry.hasFormat(utf8Ref(formatName))) {
        return ca::core::Err("unknown mapping format: " + std::string(formatName));
    }
    auto formatted = registry.format(utf8Ref(formatName), mapping);
    if (formatted.is_err()) {
        return ca::core::Err("mapping export failed: " +
                             std::string(mappingErrorName(std::move(formatted).unwrap_err())));
    }
    return ca::core::Ok(std::move(formatted).unwrap());
}

ca::core::Result<ca::str::Utf8String, std::string> NameChangeRecord::exportMapping(
    std::string_view formatName, const mjbase::mapping::MappingFormatRegistry& registry) const
{
    return exportMapping(mapping_, formatName, registry);
}

// ─────────────────────────── RenamerEngine ───────────────────────────

RenamerRunResult RenamerEngine::run(mjbase::model::ResourceContainer&   container,
                                    const std::vector<NameProvider*>&   providers,
                                    const RenamerOptions&               options)
{
    RenamerRunResult result;

    // ── 0. 只读视图：类扫描 + 类/方法层级（一次构建，全程只读）──
    auto contextResult = RenameContext::build(container);
    if (contextResult.is_err()) {
        result.ok    = false;
        result.error = std::move(contextResult).unwrap_err();
        return result;
    }
    std::unique_ptr<RenameContext> context = std::move(contextResult).unwrap();

    // ── 1. 收集各 provider 建议流 ──
    std::vector<NameProposal> proposals;
    for (auto* provider : providers) {
        if (provider == nullptr) continue;
        auto stream = provider->propose(*context);
        if (stream.is_err()) {
            const auto failure = std::move(stream).unwrap_err();
            ++result.stats.rejected;
            result.record.addRejected(NameProposal{},
                                      RejectionReason::ProviderError,
                                      "provider '" + std::string(provider->name()) +
                                          "' propose failed: " + failure);
            continue;
        }
        auto batch = std::move(stream).unwrap();
        for (auto& proposal : batch) {
            if (proposal.source.empty()) {
                proposal.source = mjbase::analysis::aliasInterned(provider->name());
            }
            proposals.push_back(std::move(proposal));
        }
    }
    result.stats.proposed = proposals.size();

    // ── 2. 逐条 schema + 合法性 + 存在性过滤（fail-closed：逐条拒，不中断）──
    std::vector<ca::usize> validated;
    for (ca::usize i = 0; i < proposals.size(); ++i) {
        const auto&                                                     proposal = proposals[i];
        std::optional<std::pair<RejectionReason, std::string>> verdict;

        const auto reject = [&verdict](RejectionReason reason, std::string detail) {
            verdict = {reason, std::move(detail)};
        };
        const auto requireOwnerShape = [&]() {
            if (!proposal.owner.empty())
                reject(RejectionReason::Schema, "class/package proposal must not set owner");
            else if (!proposal.desc.empty())
                reject(RejectionReason::Schema, "class/package proposal must not set desc");
            else if (proposal.oldName.empty())
                reject(RejectionReason::Schema, "empty old name");
            else if (proposal.oldName == proposal.newName)
                reject(RejectionReason::Schema, "no-op rename");
        };

        if (!(proposal.confidence >= 0.0 && proposal.confidence <= 1.0)) {
            reject(RejectionReason::Schema,
                   "confidence out of [0,1]: " + std::to_string(proposal.confidence));
        }
        else if (proposal.newName.empty()) {
            reject(RejectionReason::Schema, "empty new name");
        }
        else if (proposal.kind == ProposalKind::Class) {
            requireOwnerShape();
            if (!verdict && !context->classExists(proposal.oldName)) {
                reject(RejectionReason::NotFound,
                       "class not in container: " + toStd(proposal.oldName));
            }
        }
        else if (proposal.kind == ProposalKind::Package) {
            requireOwnerShape();
            if (!verdict) {
                const auto oldPrefix = trimTrailingSlash(toStd(proposal.oldName));
                const auto newPrefix = trimTrailingSlash(toStd(proposal.newName));
                if (oldPrefix.empty() || newPrefix.empty()) {
                    reject(RejectionReason::Schema, "empty package path");
                }
                else if (oldPrefix == newPrefix) {
                    reject(RejectionReason::Schema, "no-op rename");
                }
                else {
                    // 包前缀下至少要有一个容器内真实类
                    bool hasClass = false;
                    for (const ClassKey classKey : context->hierarchy_.registrationOrder()) {
                        const auto* entry = context->hierarchy_.summary(classKey);
                        if (entry == nullptr) continue;
                        const auto name = toStd(context->hierarchy_.nameOf(classKey));
                        if (name == oldPrefix || name.rfind(oldPrefix + "/", 0) == 0) {
                            hasClass = true;
                            break;
                        }
                    }
                    if (!hasClass) {
                        reject(RejectionReason::NotFound,
                               "no container class under package prefix: " + oldPrefix);
                    }
                }
            }
        }
        else if (proposal.kind == ProposalKind::Field) {
            if (proposal.owner.empty() || proposal.oldName.empty() || proposal.desc.empty()) {
                reject(RejectionReason::Schema,
                       "field proposal requires owner, old name and desc");
            }
            else if (proposal.oldName == proposal.newName) {
                reject(RejectionReason::Schema, "no-op rename");
            }
            else if (!context->classExists(proposal.owner)) {
                reject(RejectionReason::NotFound,
                       "owner class not in container: " + toStd(proposal.owner));
            }
        }
        else if (proposal.kind == ProposalKind::Method) {
            if (proposal.owner.empty() || proposal.oldName.empty() || proposal.desc.empty()) {
                reject(RejectionReason::Schema,
                       "method proposal requires owner, old name and desc");
            }
            else if (isReservedMethodName(proposal.oldName)) {
                reject(RejectionReason::ReservedName,
                       toStd(proposal.oldName) + " is exempt from renaming");
            }
            else if (isReservedMethodName(proposal.newName)) {
                reject(RejectionReason::ReservedName,
                       toStd(proposal.newName) + " cannot be a new name");
            }
            else if (proposal.oldName == proposal.newName) {
                reject(RejectionReason::Schema, "no-op rename");
            }
            else if (!context->classExists(proposal.owner)) {
                reject(RejectionReason::NotFound,
                       "owner class not in container: " + toStd(proposal.owner));
            }
            else if (!context->methodExists(proposal.owner, proposal.oldName, proposal.desc)) {
                reject(RejectionReason::NotFound,
                       "method not declared in owner: " + toStd(proposal.owner) + "." +
                           toStd(proposal.oldName) + toStd(proposal.desc));
            }
        }

        // 新名合法性（与 kind 相关的口径）
        if (!verdict) {
            if (proposal.kind == ProposalKind::Class || proposal.kind == ProposalKind::Package) {
                switch (checkNamePath(proposal.newName)) {
                case NamePathVerdict::Illegal:
                    reject(RejectionReason::IllegalIdentifier,
                           "new name is not a legal internal name path: " +
                               toStd(proposal.newName));
                    break;
                case NamePathVerdict::Keyword:
                    reject(RejectionReason::Keyword,
                           "new name segment is a java keyword: " + toStd(proposal.newName));
                    break;
                case NamePathVerdict::Ok: break;
                }
            }
            else {
                if (!mjbase::mapping::IdentifierUtil::isValidJavaIdentifier(
                        utf8Ref(proposal.newName))) {
                    reject(RejectionReason::IllegalIdentifier,
                           "new name is not a legal java identifier: " + toStd(proposal.newName));
                }
                else if (isJavaKeyword(proposal.newName)) {
                    reject(RejectionReason::Keyword,
                           "new name is a java keyword: " + toStd(proposal.newName));
                }
            }
        }

        if (verdict) {
            ++result.stats.rejected;
            result.record.addRejected(proposals[i], verdict->first, verdict->second);
            continue;
        }
        validated.push_back(i);
    }

    // ── 3. 同 target confidence 裁决（高者胜；同 confidence 先到先得，后到拒）──
    // detail 自含两侧坐标（撞在哪个槽、双方 owner/old/new）。
    const auto recordAdjudication =
        [&](Adjudication verdict, ca::usize loserIdx, ca::usize winnerIdx, const char* slotLabel) {
            if (verdict == Adjudication::Claimed) return;
            ++result.stats.rejected;
            const auto& winner     = proposals[winnerIdx];
            const auto& loser      = proposals[loserIdx];
            const auto  winnerRepr = proposalOriginRepr(winner);
            result.record.addRejected(
                proposals[loserIdx],
                RejectionReason::Conflict,
                verdict == Adjudication::ChallengerWon
                    ? std::string(slotLabel) +
                          " superseded by higher confidence proposal (" + winnerRepr +
                          "); rejected: " + proposalSlotRepr(loser)
                    : std::string(slotLabel) +
                          " already claimed by arrival-earlier proposal (" + winnerRepr +
                          "); rejected: " + proposalSlotRepr(loser));
        };

    // 3a. 新名槽：同终名同槽的候选二选一。
    std::map<TargetKey, ca::usize> winnerOfKey;
    std::vector<ca::usize>         winners;
    for (const auto index : validated) {
        const auto key     = targetKeyOf(proposals[index]);
        ca::usize  loser   = index;
        const auto verdict = adjudicate(winnerOfKey, winners, proposals, key, index, loser);
        recordAdjudication(verdict, loser, winnerOfKey.at(key), "target");
    }
    // 3b. 旧位槽：同一成员/类被两条幸存建议改到不同新名 → 再裁决一次。
    {
        std::map<TargetKey, ca::usize> winnerOfOldSlot;
        std::vector<ca::usize>         survivors;
        for (const auto index : winners) {
            const auto key   = oldSlotKeyOf(proposals[index]);
            ca::usize  loser = index;
            const auto verdict =
                adjudicate(winnerOfOldSlot, survivors, proposals, key, index, loser);
            recordAdjudication(verdict, loser, winnerOfOldSlot.at(key), "old slot");
        }
        winners = std::move(survivors);   // 到达序保持
    }
    std::sort(winners.begin(), winners.end());

    // 4-pre. 同一 override 组件被多条建议分别点名（如 interface.m→foo 与
    //        impl.m→bar）时 3a/3b 均挡不住 → 展开前按组件规范根去重：
    //        同组件多建议走 confidence 裁决（高者胜；同分先到），败者记 Conflict
    //        留 ledger。去重后各组件至多一条建议 → 6c 的旧槽占用恰好各让位/回插一次。
    {
        std::unordered_map<MemberKey, ca::usize, MemberKeyHash> componentWinnerOf;
        std::set<ca::usize>                                     componentLosers;
        for (const auto index : winners) {
            const auto& proposal = proposals[index];
            if (proposal.kind != ProposalKind::Method) continue;

            auto root = context->overrideComponentRootOf(proposal.owner, proposal.oldName,
                                                         proposal.desc);
            if (root.is_err()) continue;   // 防御：step 2 已验存在性，不可达则交单例降级
            const auto key       = std::move(root).unwrap();
            const auto incumbent = componentWinnerOf.find(key);
            if (incumbent == componentWinnerOf.end()) {
                componentWinnerOf.emplace(key, index);
                continue;
            }
            const bool challengerWins =
                proposal.confidence > proposals[incumbent->second].confidence;
            const auto loserIdx  = challengerWins ? incumbent->second : index;
            const auto winnerIdx = challengerWins ? index : incumbent->second;
            ++result.stats.rejected;
            const auto& winner = proposals[winnerIdx];
            // 两侧建议指向同一 override 组件，组件定位任取其一（胜者坐标）。
            const auto componentRepr = "override component of " + proposalSlotRepr(winner);
            result.record.addRejected(
                proposals[loserIdx],
                RejectionReason::Conflict,
                challengerWins
                    ? componentRepr + " superseded by higher confidence proposal (" +
                          proposalOriginRepr(winner) + "); rejected: " +
                          proposalSlotRepr(proposals[loserIdx])
                    : componentRepr + " already claimed by arrival-earlier proposal (" +
                          proposalOriginRepr(winner) + "); rejected: " +
                          proposalSlotRepr(proposals[loserIdx]));
            componentLosers.insert(loserIdx);
            if (challengerWins) incumbent->second = index;
        }
        if (!componentLosers.empty()) {
            std::vector<ca::usize> kept;
            kept.reserve(winners.size());
            for (const auto index : winners) {
                if (componentLosers.count(index) == 0) kept.push_back(index);
            }
            winners = std::move(kept);
        }
    }

    // ── 4. method override 闭包整组同改 ──
    std::vector<PipelineMethodChange> methodChanges;
    std::vector<MethodGroup>          methodGroups;
    for (const auto index : winners) {
        const auto& proposal = proposals[index];
        if (proposal.kind != ProposalKind::Method) continue;

        std::vector<OverrideComponentMember> members;
        auto component = context->overrideComponentOf(proposal.owner, proposal.oldName,
                                                      proposal.desc);
        if (component.is_err()) {
            // 不在容器/不在方法层级（罕见：owner 校验已过，图构建缺类）→ 单例组降级
            members.push_back({proposal.owner, proposal.oldName, proposal.desc});
        }
        else {
            members = std::move(component).unwrap();
        }

        MethodGroup group;
        bool        primary = true;
        for (const auto& member : members) {
            if (member.name == proposal.newName) continue;   // 已是目标名：跳过该成员
            PipelineMethodChange change{index,
                                        toStd(member.owner),
                                        toStd(member.name),
                                        toStd(member.desc),
                                        toStd(proposal.newName),
                                        !primary,
                                        false};
            group.memberIndices.push_back(methodChanges.size());
            methodChanges.push_back(std::move(change));
            primary = false;
        }
        methodGroups.push_back(std::move(group));
    }

    // ── 5. class/field 赢家落管线；package 前缀展开（到达序，all-or-reject）──
    std::vector<PipelineClassChange>      classChanges;
    std::vector<PipelineFieldChange>      fieldChanges;
    std::unordered_map<std::string, bool> claimedOldClass;
    std::unordered_map<std::string, bool> claimedTargetClass;
    for (const auto index : winners) {
        const auto& proposal = proposals[index];
        if (proposal.kind == ProposalKind::Class) {
            classChanges.push_back(
                {index, toStd(proposal.oldName), toStd(proposal.newName), false});
            claimedOldClass[toStd(proposal.oldName)]    = true;
            claimedTargetClass[toStd(proposal.newName)] = true;
        }
        else if (proposal.kind == ProposalKind::Field) {
            fieldChanges.push_back({index,
                                    toStd(proposal.owner),
                                    toStd(proposal.oldName),
                                    toStd(proposal.desc),
                                    toStd(proposal.newName)});
        }
    }
    for (const auto index : winners) {
        const auto& proposal = proposals[index];
        if (proposal.kind != ProposalKind::Package) continue;

        const auto oldPrefix = trimTrailingSlash(toStd(proposal.oldName));
        const auto newPrefix = trimTrailingSlash(toStd(proposal.newName));
        std::vector<std::pair<std::string, std::string>> expansion;
        std::string                                      clashedClass;
        bool                                             clash = false;
        for (const ClassKey classKey : context->hierarchy_.registrationOrder()) {
            const auto* entry = context->hierarchy_.summary(classKey);
            if (entry == nullptr) continue;
            const auto name = toStd(context->hierarchy_.nameOf(classKey));
            if (name != oldPrefix && name.rfind(oldPrefix + "/", 0) != 0) continue;
            const auto mapped = newPrefix + name.substr(oldPrefix.size());
            if (claimedOldClass.count(name) != 0 || claimedTargetClass.count(mapped) != 0) {
                clashedClass = name + " -> " + mapped;
                clash        = true;
                break;
            }
            expansion.emplace_back(name, mapped);
        }
        if (expansion.empty() || clash) {
            ++result.stats.rejected;
            result.record.addRejected(
                proposals[index],
                clash ? RejectionReason::Conflict : RejectionReason::NotFound,
                clash ? "package expansion collides with claimed class rename: " + clashedClass
                      : "no container class under package prefix: " + oldPrefix);
            continue;
        }
        for (const auto& [oldName, mapped] : expansion) {
            classChanges.push_back({index, oldName, mapped, true});
            claimedOldClass[oldName] = true;
            claimedTargetClass[mapped] = true;
        }
    }

    // ── 6. 残余撞名 `_N` 后缀消解（共享一张既有成员占用表；被改名旧位让位）──

    // 6a. 类名空间：被改名旧名让位，接受侧逐一占用；撞名 `_1`、`_2`… 递增。
    {
        std::unordered_set<std::string> renamedOlds;
        for (const auto& change : classChanges) renamedOlds.insert(change.oldName);
        std::unordered_set<std::string> occupied;
        for (const ClassKey classKey : context->hierarchy_.registrationOrder()) {
            const auto* entry = context->hierarchy_.summary(classKey);
            if (entry == nullptr) continue;
            const auto name = toStd(context->hierarchy_.nameOf(classKey));
            if (renamedOlds.count(name) == 0) occupied.insert(name);
        }
        for (auto& change : classChanges) {
            auto      candidate = change.newName;
            ca::usize suffix    = 1;
            while (occupied.count(candidate) != 0) {
                candidate = change.newName + "_" + std::to_string(suffix++);
            }
            if (candidate != change.newName) ++result.stats.suffixResolved;
            change.newName = std::move(candidate);
            occupied.insert(change.newName);
        }
    }

    // 6-pre. 类映射先行入 plan：6c 的方法槽查撞在 mapped-desc 空间进行（类改名
    //        同步改写方法描述符中的类类型），desc 预映像依赖 plan 中的类映射——
    //        与 step 7 成员规则 newOwner/newDesc 预映像同机制。
    for (const auto& change : classChanges) {
        result.plan.mapClass(change.oldName, change.newName);
    }
    auto occupancy = collectOwnerSlots(context->hierarchy_, context->pool_, result.plan);

    // 6b. 字段名空间（名字槽，同 owner 内唯一；纯名无 desc，不涉查撞空间切换）。
    {
        for (auto& change : fieldChanges) occupancy[change.owner].fieldNames.erase(change.oldName);
        for (auto& change : fieldChanges) {
            auto&     occupied  = occupancy[change.owner].fieldNames;
            auto      candidate = change.newName;
            ca::usize suffix    = 1;
            while (occupied.count(candidate) != 0) {
                candidate = change.newName + "_" + std::to_string(suffix++);
            }
            if (candidate != change.newName) ++result.stats.suffixResolved;
            change.newName = std::move(candidate);
            occupied.insert(change.newName);
        }
    }

    // 6c. 方法闭包组：(owner, name, desc) 槽位；组内统一后缀保闭包一致性，
    //     找不到一致后缀 → 整组拒（fail-closed，不破坏虚分派）。
    //     查撞空间 = mapped-desc：占用表收集时已按 plan 类映射预映像，此处
    //     erase/查撞/回插/落位四路 desc 同口径经 mapMethodDescriptor 预映像。
    {
        const auto mappedMethodDesc = [&result](const std::string& desc) {
            return result.plan.mapMethodDescriptor(desc);
        };
        for (auto& change : methodChanges) {
            occupancy[change.owner].methodSlots.erase(
                {change.oldName, mappedMethodDesc(change.desc)});
        }
        for (auto& group : methodGroups) {
            if (group.memberIndices.empty()) continue;
            const auto& base   = methodChanges[group.memberIndices.front()].newName;
            bool        resolved = false;
            std::string chosen   = base;
            for (ca::usize suffix = 0; suffix <= 64 && !resolved; ++suffix) {
                const auto candidate = suffix == 0 ? base : base + "_" + std::to_string(suffix);
                bool       freeEverywhere = true;
                for (const auto memberIndex : group.memberIndices) {
                    const auto& member = methodChanges[memberIndex];
                    if (occupancy[member.owner].methodSlots.count(
                            {candidate, mappedMethodDesc(member.desc)}) != 0) {
                        freeEverywhere = false;
                        break;
                    }
                }
                if (freeEverywhere) {
                    chosen   = candidate;
                    resolved = true;
                }
            }
            if (!resolved) {
                ++result.stats.rejected;
                const auto proposalIdx = methodChanges[group.memberIndices.front()].proposalIdx;
                std::string owners;
                for (const auto memberIndex : group.memberIndices) {
                    if (!owners.empty()) owners += ", ";
                    owners += methodChanges[memberIndex].owner;
                }
                result.record.addRejected(
                    proposals[proposalIdx],
                    RejectionReason::Conflict,
                    "no consistent _N suffix fits override component of '" +
                        methodChanges[group.memberIndices.front()].oldName +
                        "' (tried base and _1.._64; members: " + owners + ")");
                for (const auto memberIndex : group.memberIndices) {
                    auto& member = methodChanges[memberIndex];
                    member.dropped = true;
                    // 整组拒 = 该方法实际没改走：必须回插旧槽占用，否则后续组
                    // 可把新名撞上这个仍在的 (owner, name, desc) 槽 → 同 owner
                    // 重复成员名。组件根去重后各组件不相交，旧槽恰好回插一次。
                    occupancy[member.owner].methodSlots.insert(
                        {member.oldName, mappedMethodDesc(member.desc)});
                }
                continue;
            }
            if (chosen != base) ++result.stats.suffixResolved;
            for (const auto memberIndex : group.memberIndices) {
                auto& member   = methodChanges[memberIndex];
                member.newName = chosen;
                occupancy[member.owner].methodSlots.insert(
                    {chosen, mappedMethodDesc(member.desc)});
            }
        }
    }

    // ── 7. 构造 RenamePlan + NameChangeRecord ──
    std::set<ca::usize> acceptedProposals;
    // 成员改名的 owner 若无类名映射，补恒等类行：Tiny-V2 等类嵌套格式的导出以
    // 类行为挂载点，缺行会静默丢成员映射（往返恒等验收基准）。
    const auto ensureOwnerRow = [&result](const std::string& owner) {
        if (result.record.mapping().getClassMapping(utf8Ref(owner)) == nullptr) {
            result.record.mappingMut().addClass(utf8Ref(owner), utf8Ref(owner));
        }
    };
    const auto acceptedOf = [](const std::vector<NameProposal>& all, ca::usize idx,
                               ProposalKind kind, const std::string& owner,
                               const std::string& oldName, const std::string& desc,
                               const std::string& newName, bool fromClosure) {
        const auto& proposal = all[idx];
        return AcceptedChange{kind,
                              mjbase::analysis::aliasInterned(owner),
                              mjbase::analysis::aliasInterned(oldName),
                              mjbase::analysis::aliasInterned(desc),
                              mjbase::analysis::aliasInterned(newName),
                              proposal.source,
                              proposal.confidence,
                              fromClosure,
                              proposal.rationale,
                              proposal.evidence};
    };

    for (const auto& change : classChanges) {
        // 类映射已在 6-pre 提前入 plan，此处只落 record 与审计流水。
        result.record.mappingMut().addClass(utf8Ref(change.oldName), utf8Ref(change.newName));
        result.record.addAccepted(acceptedOf(proposals, change.proposalIdx, ProposalKind::Class,
                                             "", change.oldName, "", change.newName,
                                             change.fromPackage));
        acceptedProposals.insert(change.proposalIdx);
        ++result.stats.renamedMembers;
    }
    // package 建议：包级映射本身入 record（类级展开已逐条入上）。
    {
        std::set<ca::usize> packageDone;
        for (const auto& change : classChanges) {
            if (!change.fromPackage) continue;
            const auto proposalIdx = change.proposalIdx;
            if (!packageDone.insert(proposalIdx).second) continue;
            const auto& proposal   = proposals[proposalIdx];
            const auto  oldPackage = trimTrailingSlash(toStd(proposal.oldName));
            const auto  newPackage = trimTrailingSlash(toStd(proposal.newName));
            result.record.mappingMut().addPackage(utf8Ref(oldPackage), utf8Ref(newPackage));
            result.record.addAccepted(acceptedOf(proposals, proposalIdx, ProposalKind::Package,
                                                 "", oldPackage, "", newPackage, false));
            acceptedProposals.insert(proposalIdx);
        }
    }
    for (const auto& change : fieldChanges) {
        ensureOwnerRow(change.owner);
        // 规则的 newOwner/newDesc 必须预映像（类映射已全部入 plan）。
        const auto newOwner = toStd(result.plan.mapInternalName(change.owner));
        const auto newDesc  = result.plan.mapFieldDescriptor(change.desc);
        result.plan.mapField({change.owner,
                              change.oldName,
                              change.desc,
                              newOwner,
                              change.newName,
                              newDesc});
        result.record.mappingMut().addField(
            utf8Ref(change.owner), utf8Ref(change.desc), utf8Ref(change.oldName),
            utf8Ref(change.newName));
        result.record.addAccepted(acceptedOf(proposals, change.proposalIdx, ProposalKind::Field,
                                             change.owner, change.oldName, change.desc,
                                             change.newName, false));
        acceptedProposals.insert(change.proposalIdx);
        ++result.stats.renamedMembers;

        // 字段继承别名（plan-only，不入 record/导出）：javac 可能以子类为 owner
        // 发出继承字段的 field 指令（getfield B.f，f 实际声明于超类 A），而
        // 应用面只按精确 (owner,name,desc) 查规则。沿旧栈 rename_core 同款口径
        // 补齐：子类 B 沿 super 链解析出的声明 owner 恰为本字段 owner 时，逐子类
        // 补 (B, oldName, desc) → 新名别名规则。SRG/mapping 记录仍只含声明处一条。
        const ClassKey ownerKey = context->hierarchy_.findClass(change.owner);
        if (!ownerKey.valid()) continue;
        for (const ClassKey classKey : context->hierarchy_.registrationOrder()) {
            if (classKey == ownerKey ||
                !context->hierarchy_.isSubtype(classKey, ownerKey)) {
                continue;
            }
            if (!(context->resolveInstanceFieldOwner(classKey, change.oldName, change.desc) ==
                  ownerKey)) {
                continue;
            }
            const auto* aliasSummary = context->hierarchy_.summary(classKey);
            if (aliasSummary == nullptr) continue;
            const auto aliasOwner = toStd(context->hierarchy_.nameOf(classKey));
            result.plan.mapField({aliasOwner,
                                  change.oldName,
                                  change.desc,
                                  toStd(result.plan.mapInternalName(aliasOwner)),
                                  change.newName,
                                  result.plan.mapFieldDescriptor(change.desc)});
        }
    }
    for (const auto& change : methodChanges) {
        if (change.dropped) continue;
        ensureOwnerRow(change.owner);
        // 同 field：newOwner/newDesc 预映像，避免同 plan 类改名时调用点写回旧值。
        const auto newOwner = toStd(result.plan.mapInternalName(change.owner));
        const auto newDesc  = result.plan.mapMethodDescriptor(change.desc);
        result.plan.mapMethod({change.owner,
                               change.oldName,
                               change.desc,
                               newOwner,
                               change.newName,
                               newDesc});
        result.record.mappingMut().addMethod(
            utf8Ref(change.owner), utf8Ref(change.desc), utf8Ref(change.oldName),
            utf8Ref(change.newName));
        result.record.addAccepted(acceptedOf(proposals, change.proposalIdx, ProposalKind::Method,
                                             change.owner, change.oldName, change.desc,
                                             change.newName, change.fromClosure));
        acceptedProposals.insert(change.proposalIdx);
        ++result.stats.renamedMembers;
    }

    // 方法继承别名（plan-only，不入 record/导出）：闭世界子类继承了改名方法但
    // 未自行声明时，其继承引用（invokevirtual B.m，m 实际声明于超类 A）在应用面
    // 的精确规则查找下会漏改。沿旧栈 rename_core 同款口径补齐：
    //   1) C 未声明 (oldName, desc)（声明者由闭包规则覆盖）；
    //   2) C 的继承闭包封闭（不延伸到容器外类）；
    //   3) C 是组内任一成员 owner 的子类型；
    // 满足则补 (C, oldName, desc) → 组新名别名规则。同一 (C, oldName, desc)
    // 被两组以不同新名称命时歧义——整条放弃（旧核心同口径）。
    {
        struct MethodAliasCandidate
        {
            std::string owner;
            std::string oldName;
            std::string desc;
            std::string newName;
            bool        ambiguous = false;
        };
        std::map<std::tuple<std::string, std::string, std::string>, MethodAliasCandidate>
            methodAliases;

        for (const auto& group : methodGroups) {
            if (group.memberIndices.empty()) continue;
            const auto& first = methodChanges[group.memberIndices.front()];
            if (first.dropped) continue;   // 整组被拒：方法未改走，无别名可补

            for (const ClassKey classKey : context->hierarchy_.registrationOrder()) {
                const auto* entry = context->hierarchy_.summary(classKey);
                if (entry == nullptr ||
                    context->classDeclaresMethod(*entry, first.oldName, first.desc) ||
                    !context->hierarchyClosedForVirtualRename(classKey)) {
                    continue;
                }

                bool inheritsGroup = false;
                for (const auto memberIndex : group.memberIndices) {
                    const auto& member = methodChanges[memberIndex];
                    const ClassKey memberOwnerKey =
                        context->hierarchy_.findClass(member.owner);
                    if (memberOwnerKey.valid() &&
                        context->hierarchy_.isSubtype(classKey, memberOwnerKey)) {
                        inheritsGroup = true;
                        break;
                    }
                }
                if (!inheritsGroup) continue;

                const auto aliasOwner = toStd(context->hierarchy_.nameOf(classKey));
                const auto key = std::make_tuple(aliasOwner, first.oldName, first.desc);
                auto [it, inserted] = methodAliases.try_emplace(
                    key, MethodAliasCandidate{aliasOwner, first.oldName, first.desc,
                                              first.newName});
                if (!inserted && it->second.newName != first.newName) {
                    it->second.ambiguous = true;
                }
            }
        }
        for (const auto& [key, alias] : methodAliases) {
            (void)key;
            if (alias.ambiguous) continue;
            result.plan.mapMethod({alias.owner,
                                   alias.oldName,
                                   alias.desc,
                                   toStd(result.plan.mapInternalName(alias.owner)),
                                   alias.newName,
                                   result.plan.mapMethodDescriptor(alias.desc)});
        }
    }
    result.stats.accepted = acceptedProposals.size();

    // ── 8. plan-only 或应用（应用面复用 asm/remapper 域，见 rename_engine.hpp）──
    if (!options.planOnly) {
        if (result.plan.empty()) {
            // 空 plan + 非 plan-only：无可应用内容即成功（zero-applied 旧口径；
            // applyResourceRemapPlan 对空 plan 返回 false，直接调用会被误判失败）。
            result.applied = true;
        }
        else {
            // 引擎计划 → remapper 域计划：类映射按登记序先行，成员规则随后
            //（newOwner/newDesc 已在 step 7 按类映射预映像，直接搬运即可）。
            mjbase::asm_remapper::MemberRemapPlan memberPlan;
            for (const auto& [oldName, newName] : result.plan.classMappings()) {
                memberPlan.mapClass(oldName, newName);
            }
            for (const auto& rule : result.plan.fieldRules()) {
                memberPlan.mapField(rule.owner, rule.oldName, rule.desc, rule.newOwner,
                                    rule.newName, rule.newDesc);
            }
            for (const auto& rule : result.plan.methodRules()) {
                memberPlan.mapMethod(rule.owner, rule.oldName, rule.desc, rule.newOwner,
                                     rule.newName, rule.newDesc);
            }
            auto applied = mjbase::asm_remapper::applyResourceRemapPlan(container, memberPlan,
                                                                        options.policy);
            const bool committed = applied.is_ok() && std::move(applied).unwrap();
            if (!committed) {
                // ：非空计划应用后零变化（逐类失败被 remapper 面按类
                // 跳过后的聚合信号）= ApplyFailed，全量不落盘口径。
                ++result.stats.rejected;
                result.record.addRejected(
                    NameProposal{},
                    RejectionReason::ApplyFailed,
                    "applyResourceRemapPlan failed; nothing committed");
                result.ok    = false;
                result.error = "renamer apply failed; container untouched per remapper atomicity";
                return result;
            }
            result.applied = true;
        }
    }

    result.ok = true;
    return result;
}

}   // namespace mjbase::renamer
