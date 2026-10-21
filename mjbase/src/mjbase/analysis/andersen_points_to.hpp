/// @file        andersen_points_to.hpp
/// @brief       Field-sensitive Andersen 包含约束求解器（纯算法件直迁）。
///
/// 约束图求解不含任何 jbytecode 语义，逐语义直迁：变量与抽象分配点是两个
/// 独立命名空间；copy/load/store 约束单调，solve() 计算最小不动点；堆字段
/// 以（分配点，字段名）为键，无关接收者的字段事实互不污染。
///
/// 名字内码：约束图节点/分配点/堆字段以 NameId（u32 句柄）寻址，名字串只在
/// intern 边界物化一次（求解热路径零串拷贝、零再哈希——(object, fieldId)
/// 数值对直接寻址 fieldNode）。pointsTo(string) 字符串出口仅为诊断/单测保留。
#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjbase::analysis {

class AndersenPointsToSolver
{
public:
    using NameId = ca::u32;

    void clear();

    /// @brief Intern a name once; identical contents map to the same id.
    NameId intern(std::string_view name);

    /// @brief Materialized name for a handle（merge 节点文本重建用）。
    std::string_view nameOf(NameId id) const { return nameTable_[id]; }

    /// @brief Intern `prefix + num` without materializing the suffix twice.
    NameId internIndexed(std::string_view prefix, ca::i64 num);

    /// @brief Add `target = &allocation` (both by name id).
    void addAddressOf(NameId target, NameId allocation);

    /// @brief Add the inclusion constraint `target` includes `source`.
    void addCopy(NameId target, NameId source);

    /// @brief Add `target = base.field` (field by name id).
    void addLoad(NameId target, NameId base, NameId field);

    /// @brief Add `base.field = source` (field by name id).
    void addStore(NameId base, NameId field, NameId source);

    /// @brief Compute the least fixed point. Safe to call after adding more constraints.
    void solve();

    /// @brief Return stable, sorted allocation identities for a variable (诊断/单测出口).
    std::vector<std::string> pointsTo(std::string_view variable) const;

private:
    using NodeIndex   = ca::u32;
    using ObjectIndex = ca::u32;

    struct FieldKey
    {
        ObjectIndex object  = 0;
        NameId      fieldId = 0;
        bool        operator==(const FieldKey& other) const
        {
            return object == other.object && fieldId == other.fieldId;
        }
    };
    struct FieldKeyHash
    {
        std::size_t operator()(const FieldKey& key) const
        {
            return (static_cast<std::size_t>(key.object) << 32) ^ key.fieldId;
        }
    };

    NodeIndex   variableByName(NameId name);
    NodeIndex   appendNode();
    ObjectIndex allocationByName(NameId name);
    NodeIndex   fieldNode(ObjectIndex object, NameId fieldId);
    bool        addPointsTo(NodeIndex node, ObjectIndex object);
    void        addCopyEdge(NodeIndex source, NodeIndex target);
    void        enqueue(NodeIndex node);

    std::unordered_map<std::string, NameId>               nameIds_;
    std::deque<std::string>                               nameTable_;
    std::vector<NodeIndex>                                variableById_;
    std::vector<ObjectIndex>                              allocationById_;
    std::unordered_map<FieldKey, NodeIndex, FieldKeyHash> fieldNodes_;
    std::deque<std::string>                               allocationNames_;
    std::vector<std::unordered_set<ObjectIndex>>          pointsTo_;
    std::vector<std::vector<NodeIndex>>                   copyTargets_;
    std::vector<std::vector<std::pair<NodeIndex, NameId>>> loads_;
    std::vector<std::vector<std::pair<NameId, NodeIndex>>> stores_;
    std::deque<NodeIndex>                                 worklist_;
    std::vector<ca::u8>                                   queued_;
};

}   // namespace mjbase::analysis
