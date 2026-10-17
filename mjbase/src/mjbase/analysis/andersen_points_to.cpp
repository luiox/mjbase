#include "mjbase/analysis/andersen_points_to.hpp"

#include <algorithm>

namespace mjbase::analysis {

void AndersenPointsToSolver::clear()
{
    nameIds_.clear();
    nameTable_.clear();
    variableById_.clear();
    allocationById_.clear();
    fieldNodes_.clear();
    allocationNames_.clear();
    pointsTo_.clear();
    copyTargets_.clear();
    loads_.clear();
    stores_.clear();
    worklist_.clear();
    queued_.clear();
}

AndersenPointsToSolver::NameId AndersenPointsToSolver::intern(std::string_view name)
{
    const auto found = nameIds_.find(std::string(name));
    if (found != nameIds_.end()) return found->second;
    // deque 节点地址稳定：id → 名字表供诊断出口与分配点驻留复用。
    nameTable_.push_back(std::string(name));
    const auto id = static_cast<NameId>(nameTable_.size() - 1);
    nameIds_.emplace(nameTable_.back(), id);
    return id;
}

AndersenPointsToSolver::NameId AndersenPointsToSolver::internIndexed(std::string_view prefix,
                                                                     ca::i64     num)
{
    thread_local std::string scratch;
    scratch.assign(prefix);
    scratch += std::to_string(num);
    return intern(scratch);
}

AndersenPointsToSolver::NodeIndex AndersenPointsToSolver::appendNode()
{
    const auto index = static_cast<NodeIndex>(pointsTo_.size());
    pointsTo_.emplace_back();
    copyTargets_.emplace_back();
    loads_.emplace_back();
    stores_.emplace_back();
    queued_.push_back(0);
    return index;
}

AndersenPointsToSolver::NodeIndex AndersenPointsToSolver::variableByName(NameId name)
{
    if (name < variableById_.size() && variableById_[name] != 0) {
        return variableById_[name] - 1;
    }
    const auto index = appendNode();
    if (variableById_.size() <= name) variableById_.resize(static_cast<ca::usize>(name) + 1, 0);
    variableById_[name] = index + 1;
    return index;
}

AndersenPointsToSolver::ObjectIndex AndersenPointsToSolver::allocationByName(NameId name)
{
    if (name < allocationById_.size() && allocationById_[name] != 0) {
        return allocationById_[name] - 1;
    }
    const auto index = static_cast<ObjectIndex>(allocationNames_.size());
    allocationNames_.push_back(nameTable_[name]);
    if (allocationById_.size() <= name) allocationById_.resize(static_cast<ca::usize>(name) + 1, 0);
    allocationById_[name] = index + 1;
    return index;
}

AndersenPointsToSolver::NodeIndex AndersenPointsToSolver::fieldNode(ObjectIndex object,
                                                                    NameId      fieldId)
{
    const FieldKey key{object, fieldId};
    const auto     found = fieldNodes_.find(key);
    if (found != fieldNodes_.end()) return found->second;
    const auto index = appendNode();
    fieldNodes_.emplace(key, index);
    return index;
}

void AndersenPointsToSolver::enqueue(NodeIndex node)
{
    if (queued_[node] != 0) return;
    queued_[node] = 1;
    worklist_.push_back(node);
}

bool AndersenPointsToSolver::addPointsTo(NodeIndex node, ObjectIndex object)
{
    if (!pointsTo_[node].insert(object).second) return false;
    enqueue(node);
    return true;
}

void AndersenPointsToSolver::addCopyEdge(NodeIndex source, NodeIndex target)
{
    auto& targets = copyTargets_[source];
    if (std::find(targets.begin(), targets.end(), target) != targets.end()) return;
    targets.push_back(target);
    const auto objects = pointsTo_[source];
    for (const auto object : objects) addPointsTo(target, object);
}

void AndersenPointsToSolver::addAddressOf(NameId target, NameId allocationName)
{
    addPointsTo(variableByName(target), allocationByName(allocationName));
}

void AndersenPointsToSolver::addCopy(NameId target, NameId source)
{
    addCopyEdge(variableByName(source), variableByName(target));
}

void AndersenPointsToSolver::addLoad(NameId target, NameId base, NameId field)
{
    const auto baseNode   = variableByName(base);
    const auto targetNode = variableByName(target);
    loads_[baseNode].push_back({targetNode, field});
    if (!pointsTo_[baseNode].empty()) enqueue(baseNode);
}

void AndersenPointsToSolver::addStore(NameId base, NameId field, NameId source)
{
    const auto baseNode   = variableByName(base);
    const auto sourceNode = variableByName(source);
    stores_[baseNode].push_back({field, sourceNode});
    if (!pointsTo_[baseNode].empty()) enqueue(baseNode);
}

void AndersenPointsToSolver::solve()
{
    while (!worklist_.empty()) {
        const auto node = worklist_.front();
        worklist_.pop_front();
        queued_[node] = 0;

        const auto objects = pointsTo_[node];
        const auto targets = copyTargets_[node];
        for (const auto target : targets) {
            for (const auto object : objects) addPointsTo(target, object);
        }

        const auto loads = loads_[node];
        for (const auto& [targetNode, fieldId] : loads) {
            for (const auto object : objects) {
                addCopyEdge(fieldNode(object, fieldId), targetNode);
            }
        }

        const auto stores = stores_[node];
        for (const auto& [fieldId, sourceNode] : stores) {
            for (const auto object : objects) {
                addCopyEdge(sourceNode, fieldNode(object, fieldId));
            }
        }
    }
}

std::vector<std::string> AndersenPointsToSolver::pointsTo(std::string_view variable) const
{
    const auto found = nameIds_.find(std::string(variable));
    if (found == nameIds_.end() || found->second >= variableById_.size() ||
        variableById_[found->second] == 0) {
        return {};
    }
    std::vector<std::string> result;
    const auto&              objects = pointsTo_[variableById_[found->second] - 1];
    result.reserve(objects.size());
    for (const auto object : objects) {
        result.push_back(allocationNames_[object]);
    }
    std::sort(result.begin(), result.end());
    return result;
}

}   // namespace mjbase::analysis
