#include "mjbase/analysis/exception_flow_analysis.hpp"

#include <algorithm>

#include "jbc/core/opcode.hpp"

#include "class_scan.hpp"
#include "cp_refs.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"

namespace mjbase::analysis {

namespace {

using jbc::core::Op;

/// 旧栈 isBuiltinSubtype：JDK Throwable 族小表（项目内层次不可达时的兜底）。
bool isBuiltinSubtype(std::string_view type, std::string_view handler)
{
    if (handler == "java/lang/Throwable") return true;
    if (handler == "java/lang/Exception") {
        return type == "java/lang/RuntimeException" || type == "java/lang/IllegalStateException" ||
               type == "java/lang/NullPointerException" ||
               type == "java/lang/IllegalArgumentException";
    }
    if (handler == "java/lang/RuntimeException") {
        return type == "java/lang/IllegalStateException" ||
               type == "java/lang/NullPointerException" ||
               type == "java/lang/IllegalArgumentException";
    }
    return false;
}

/// 旧栈 catches：内置表优先，未命中走项目内层次（invalid 键自然落空）。
bool catches(const std::vector<std::string_view>& handlers, std::string_view type,
             std::string_view& handler, const mjbase::hierarchy::ClassHierarchy* hierarchy)
{
    for (const auto candidate : handlers) {
        bool subtype = candidate.empty() || candidate == type || isBuiltinSubtype(type, candidate);
        if (!subtype && hierarchy != nullptr) {
            const auto child  = hierarchy->findClass(type);
            const auto parent = hierarchy->findClass(candidate);
            subtype = child.valid() && parent.valid() && hierarchy->isSubtype(child, parent);
        }
        if (subtype) {
            handler = candidate.empty() ? std::string_view("java/lang/Throwable") : candidate;
            return true;
        }
    }
    return false;
}

void appendUnique(std::vector<std::string_view>& values, std::string_view value, KeyPool& pool)
{
    if (value.empty()) return;
    if (std::find(values.begin(), values.end(), value) != values.end()) return;
    values.push_back(pool.str(pool.intern(value)));
}

/// 一个方法的扫描产物：抛出站点 + 调用站点（处理器列表按范围收集）。
struct ScannedMethod
{
    struct ThrowSite
    {
        std::string_view              type;   ///< 不可判定时为 "java/lang/Throwable"（旧栈同口径）
        std::vector<std::string_view> handlers;
    };

    struct CallSite
    {
        MemberKey                     callee;
        std::vector<std::string_view> handlers;
    };

    MemberKey              method;
    std::vector<ThrowSite> localThrows;
    std::vector<CallSite>  calls;
};

/// 异常表覆盖判定：指令程序序位置落在 [start, end) 内的处理器的捕获类型。
/// end 绑定方法末尾标签时视为代码长度（越过最后一条指令）。
std::vector<std::string_view> handlersAt(const jbc::ir::CodeUnit& u,
                                         const jbc::core::ConstantPool& cp,
                                         const std::vector<ca::usize>& orderPos,
                                         ca::usize instructionPos, KeyPool& pool)
{
    std::vector<std::string_view> handlers;
    for (const auto& block : u.exc) {
        const auto startPos = u.dom.resolveLabel(block.start);
        if (startPos.is_err() || startPos.unwrap().isEnd() ||
            orderPos[startPos.unwrap().insn.slot] == 0) {
            continue;
        }
        const ca::usize begin = orderPos[startPos.unwrap().insn.slot] - 1;
        ca::usize       end   = orderPos.size();   // 方法末尾标签 = 越过末条指令
        const auto      endPos = u.dom.resolveLabel(block.end);
        if (!endPos.is_err() && !endPos.unwrap().isEnd() &&
            orderPos[endPos.unwrap().insn.slot] != 0) {
            end = orderPos[endPos.unwrap().insn.slot] - 1;
        }
        if (instructionPos < begin || instructionPos >= end) continue;
        // catchCp = 0 表示 any（旧栈空 type → catches 里的 Throwable 兜底）。
        std::string_view type;
        if (block.catchCp != 0) type = resolveClassName(cp, block.catchCp);
        handlers.push_back(pool.str(pool.intern(type)));
    }
    return handlers;
}

/// slot → order 下标 + 1（0 = 不在序内；异常表标签解析出的句柄都以此定位）。
std::vector<ca::usize> buildOrderPos(const jbc::ir::CodeUnit& u)
{
    const auto& order = u.dom.order();
    ca::u32     maxSlot = 0;
    for (const auto& id : order) maxSlot = std::max(maxSlot, id.slot);
    std::vector<ca::usize> orderPos(static_cast<ca::usize>(maxSlot) + 1, 0);
    for (ca::usize i = 0; i < order.size(); ++i) {
        orderPos[order[i].slot] = i + 1;
    }
    return orderPos;
}

}   // namespace

void ExceptionFlowModel::clear()
{
    pool_       = KeyPool{};
    summaries_.clear();
    byKey_.clear();
    edges_.clear();
}

void ExceptionFlowModel::build(const mjbase::model::ResourceContainer& container)
{
    clear();
    auto classes = scanClassResources(container);

    // 项目内层次（供 catches 的通用路径；未登记名走 invalid 键落空）。
    mjbase::hierarchy::ClassHierarchy hierarchy(pool_);
    for (const auto& clazz : classes) {
        const mjbase::hierarchy::ClassSummary s =
            mjbase::hierarchy::fillSummary(*clazz.session, pool_);
        (void)hierarchy.addClass(s);   // 畸形骨架（环/重复）跳过：fail-open 扫描
    }

    std::vector<ScannedMethod> methods;
    for (const auto& clazz : classes) {
        jbc::ir::ClassSession& session = *clazz.session;
        // 扫描前就地升格（scanClassResources 只建会话不升格）。
        (void)jbc::ir::collectCodeUnits(session);
        const jbc::ir::ClassUnit& unit = session.unit();
        for (const auto& m : unit.methods) {
            const std::string_view owner = unit.symbols().str(unit.thisClass);
            const std::string_view name  = unit.symbols().str(m.name);
            const std::string_view desc  = unit.symbols().str(m.desc);
            ScannedMethod scanned;
            scanned.method = memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc);
            if (m.code != nullptr) {
                const jbc::ir::CodeUnit& u   = *m.code;
                const jbc::core::ConstantPool& cp = session.backing().cp;
                const auto               orderPos = buildOrderPos(u);
                const auto&              order    = u.dom.order();

                std::string_view lastAllocation;
                for (ca::usize pos = 0; pos < order.size(); ++pos) {
                    const jbc::ir::Insn* in = u.dom.peek(order[pos]);
                    if (in == nullptr) continue;
                    if (in->op == Op::NEW) {
                        lastAllocation = resolveClassName(cp, in->cp);
                    }
                    else if (in->op == Op::ATHROW) {
                        ScannedMethod::ThrowSite site;
                        site.type = lastAllocation.empty() ? std::string_view("java/lang/Throwable")
                                                           : lastAllocation;
                        site.handlers = handlersAt(u, cp, orderPos, pos, pool_);
                        scanned.localThrows.push_back(std::move(site));
                        lastAllocation = std::string_view{};
                    }
                    else {
                        jbc::ir::ClassSession::MemberSem sem;
                        if ((in->op == Op::INVOKEVIRTUAL || in->op == Op::INVOKESPECIAL ||
                             in->op == Op::INVOKESTATIC || in->op == Op::INVOKEINTERFACE) &&
                            session.memberOf(*in, sem)) {
                            ScannedMethod::CallSite call;
                            call.callee =
                                memberKeyOf(pool_, classKeyOf(pool_, sem.owner), sem.name, sem.desc);
                            call.handlers = handlersAt(u, cp, orderPos, pos, pool_);
                            scanned.calls.push_back(std::move(call));
                        }
                    }
                }
            }
            methods.push_back(std::move(scanned));
        }
    }

    // 未捕获异常不动点：本地抛出先行，调用边沿容器内摘要传播（Jacobi）。
    std::unordered_map<MemberKey, std::vector<std::string_view>, MemberKeyHash> uncaught;
    for (const auto& method : methods) {
        for (const auto& site : method.localThrows) {
            std::string_view handler;
            if (!catches(site.handlers, site.type, handler, &hierarchy)) {
                appendUnique(uncaught[method.method], site.type, pool_);
            }
        }
    }
    for (ca::usize round = 0; round < methods.size() + 1; ++round) {
        bool changed = false;
        for (const auto& method : methods) {
            auto& current = uncaught[method.method];
            for (const auto& call : method.calls) {
                auto it = uncaught.find(call.callee);
                if (it == uncaught.end()) continue;
                for (const auto type : it->second) {
                    std::string_view handler;
                    if (!catches(call.handlers, type, handler, &hierarchy)) {
                        const auto before = current.size();
                        appendUnique(current, type, pool_);
                        changed |= current.size() != before;
                    }
                }
            }
        }
        if (!changed) break;
    }

    for (const auto& method : methods) {
        ExceptionMethodSummary summary;
        summary.method        = method.method;
        summary.uncaughtTypes = uncaught[method.method];
        std::sort(summary.uncaughtTypes.begin(), summary.uncaughtTypes.end());
        byKey_[summary.method] = summaries_.size();
        summaries_.push_back(std::move(summary));
        for (const auto& call : method.calls) {
            auto it = uncaught.find(call.callee);
            if (it == uncaught.end()) continue;
            for (const auto type : it->second) {
                std::string_view handler;
                const bool       handled = catches(call.handlers, type, handler, &hierarchy);
                ExceptionFlowEdge edge;
                edge.source        = method.method;
                edge.callee        = call.callee;
                edge.exceptionType = type;
                edge.handlerType   = handled ? handler : std::string_view{};
                edge.handled       = handled;
                edges_.push_back(edge);
            }
        }
    }
}

const ExceptionMethodSummary* ExceptionFlowModel::summaryOf(std::string_view owner,
                                                            std::string_view name,
                                                            std::string_view desc) const
{
    // 查询键驻留进池（只增不改摘要）；键结构与 build 期完全同构。
    const auto it = byKey_.find(
        MemberKey{classKeyOf(pool_, owner).id, pool_.intern(name), pool_.intern(desc)});
    return it == byKey_.end() ? nullptr : &summaries_[it->second];
}

}   // namespace mjbase::analysis
