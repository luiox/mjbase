#include "mjbase/analysis/cycle_class_scan.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "libca/core/datatype.hpp"

namespace mjbase::analysis {

namespace {

/// 未访问序号哨兵。
constexpr ca::usize kNoIndex = static_cast<ca::usize>(-1);

/// 图节点：登记序即扫描序；自环单独记（不成 SCC ≥2，单独判腐坏）。
struct CycleNode
{
    std::string_view       name     = {};   ///< 类内部名（借用会话符号池）
    bool                   selfLoop = false;
    std::vector<ca::usize> edges;           ///< 出边（super + 接口，集合内解析）
};

}   // namespace

std::vector<std::string_view> findInheritanceCycleClasses(
    const std::vector<ScannedClass>& scanned)
{
    // 节点登记：同名重复扫描类首个成节点，后续同名类不另立节点（判据按名
    // 报告，视角下是同一类身份；其边不再并入，口径确定且与登记序无关）。
    std::unordered_map<std::string_view, ca::usize> byName;
    std::vector<CycleNode>                          nodes;
    nodes.reserve(scanned.size());
    for (const ScannedClass& cls : scanned) {
        if (byName.emplace(cls.name, nodes.size()).second) {
            CycleNode node;
            node.name = cls.name;
            nodes.push_back(std::move(node));
        }
    }

    // 边构建：super + 接口（extends 与 implements 双边，Recaf isLoop 口径），
    // 只解析到扫描集合内的名字——悬挂键不可能闭合环，落空即截断。
    for (ca::usize i = 0; i < scanned.size(); ++i) {
        const ScannedClass& cls = scanned[i];
        const auto          selfIt = byName.find(cls.name);
        const ca::usize     self   = selfIt->second;
        if (self != i) continue;   // 同名后续类：节点属首个，边不再并入
        const jbc::ir::ClassUnit&   unit    = cls.session->unit();
        const jbc::ir::SymbolPool&  symbols = unit.symbols();
        const auto edgeTo = [&](std::string_view target) {
            if (target.empty()) return;
            const auto it = byName.find(target);
            if (it == byName.end()) return;   // 悬挂键：截断
            if (it->second == self) {
                nodes[self].selfLoop = true;
                return;
            }
            nodes[self].edges.push_back(it->second);
        };
        edgeTo(symbols.str(unit.superClass));
        for (const jbc::ir::SymbolId iface : unit.interfaces) {
            edgeTo(symbols.str(iface));
        }
    }

    // 迭代 Tarjan 强连通分量：环成员 = SCC ≥2 的全体成员或自环节点。
    // 显式栈帧（节点 + 出边游标）防深链递归爆栈，访问集防环死循环。
    const ca::usize         n = nodes.size();
    std::vector<ca::usize>  order(n, kNoIndex);
    std::vector<ca::usize>  low(n, 0);
    std::vector<ca::u8>     onStack(n, 0);
    std::vector<ca::u8>     onCycle(n, 0);
    std::vector<ca::usize>  stack;
    std::vector<ca::usize>  scc;
    std::vector<std::pair<ca::usize, ca::usize>> frames;
    stack.reserve(n);
    ca::usize counter = 0;
    for (ca::usize root = 0; root < n; ++root) {
        if (order[root] != kNoIndex) continue;
        frames.push_back({root, 0});
        while (!frames.empty()) {
            const ca::usize v     = frames.back().first;
            const ca::usize stage = frames.back().second;
            if (stage == 0) {
                order[v] = low[v] = counter++;
                stack.push_back(v);
                onStack[v] = 1;
            }
            if (stage < nodes[v].edges.size()) {
                frames.back().second = stage + 1;
                const ca::usize w = nodes[v].edges[stage];
                if (order[w] == kNoIndex) {
                    frames.push_back({w, 0});
                }
                else if (onStack[w] != 0) {
                    low[v] = std::min(low[v], order[w]);
                }
                continue;
            }
            // v 收官：SCC 弹栈定环；父帧低链合并。
            if (low[v] == order[v]) {
                scc.clear();
                ca::usize w = 0;
                do {
                    w = stack.back();
                    stack.pop_back();
                    onStack[w] = 0;
                    scc.push_back(w);
                } while (w != v);
                if (scc.size() >= 2 || nodes[v].selfLoop) {
                    for (const ca::usize member : scc) {
                        onCycle[member] = 1;
                    }
                }
            }
            frames.pop_back();
            if (!frames.empty()) {
                const ca::usize parent = frames.back().first;
                low[parent] = std::min(low[parent], low[v]);
            }
        }
    }

    std::vector<std::string_view> out;
    for (ca::usize i = 0; i < n; ++i) {
        if (onCycle[i] != 0) out.push_back(nodes[i].name);
    }
    return out;
}

}   // namespace mjbase::analysis
