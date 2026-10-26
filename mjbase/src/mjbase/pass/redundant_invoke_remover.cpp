#include "mjbase/pass/redundant_invoke_remover.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/analysis/method_effect_analysis.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/remove_method_calls.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::InsnId;

/// 容器内成员三元组（比较/序全按字典序，确定性）。
struct MemberTriple
{
    std::string owner;
    std::string name;
    std::string desc;

    bool operator<(const MemberTriple& o) const
    {
        return owner != o.owner ? owner < o.owner
               : name != o.name ? name < o.name
                                : desc < o.desc;
    }
};

/// 装载条目：会话 + 类内部名。
struct LoadedClass
{
    std::string                            name;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 容器内全部 class 资源装载；任一空 bytes / 不可解析 / 无类名返回 false
/// （普查全量性前提：缺一类即定义集/调用面不完整，整件放弃）。
bool loadAllClasses(mjbase::model::ResourceContainer& container, std::vector<LoadedClass>& out)
{
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr) continue;
        if (cls->get().empty()) return false;
        auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{cls->get()});
        if (made.is_err()) return false;
        LoadedClass entry;
        entry.session = std::move(made).unwrap();
        entry.name.assign(entry.session->backing().thisName());
        if (entry.name.empty()) return false;
        out.push_back(std::move(entry));
    }
    return true;
}

/// 描述符返回类别：2 = cat2（J/D），1 = 其余非 void，0 = void。
ca::i32 returnCategory(std::string_view desc)
{
    const ca::usize close = desc.find(')');
    if (close == std::string_view::npos || close + 1 >= desc.size()) return -1;
    const char c = desc[close + 1];
    if (c == 'V') return 0;
    return (c == 'J' || c == 'D') ? 2 : 1;
}

}   // namespace

mjbase::spi::PreservedAnalyses RedundantInvokeRemover::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    std::vector<LoadedClass> loaded;
    if (!loadAllClasses(container, loaded)) return mjbase::spi::PreservedAnalyses::all();
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 定义集：全部类声明的方法三元组（含 abstract/native——调用点解析按
    // 引用匹配，定义与实现分离的形态同样提名）。
    std::set<MemberTriple> defined;
    for (const LoadedClass& lc : loaded) {
        const jbc::ir::ClassUnit&    u   = lc.session->unit();
        const jbc::ir::SymbolPool&   sym = u.symbols();
        for (const jbc::ir::MethodUnit& m : u.methods) {
            defined.insert(MemberTriple{lc.name, std::string(sym.str(m.name)),
                                        std::string(sym.str(m.desc))});
        }
    }

    // 纯度模型：容器重建后逐候选查询（Pure 档，与旧栈对齐）。
    mjbase::analysis::MethodEffectModel effects;
    effects.build(container);

    // 普查：任一「调用 + 紧随宽度匹配弹弃」且目标入定义集且纯 → 提名。
    std::set<MemberTriple> nominated;
    for (const LoadedClass& lc : loaded) {
        jbc::ir::ClassSession& session = *lc.session;
        jbc::ir::ClassUnit&    unit    = session.unit();
        for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
            if (!jbc::ir::hasCode(session, idx)) continue;
            auto promoted = jbc::ir::promoteMethod(session, idx);
            if (promoted.is_err()) continue;
            auto cuRef = std::move(promoted).unwrap();
            jbc::ir::CodeUnit&         cu    = *cuRef.get();
            const std::vector<InsnId>& order = cu.dom.order();
            for (ca::usize i = 0; i + 1 < order.size(); ++i) {
                const jbc::ir::Insn* in = cu.dom.peek(order[i]);
                if (in == nullptr) continue;
                const Op op = in->op;
                if (op != Op::INVOKEVIRTUAL && op != Op::INVOKESPECIAL
                    && op != Op::INVOKESTATIC && op != Op::INVOKEINTERFACE) {
                    continue;
                }
                jbc::ir::ClassSession::MemberSem sem;
                if (!session.memberOf(*in, sem)) continue;
                if (sem.owner.empty()) continue;
                const ca::i32 cat = returnCategory(sem.desc);
                if (cat <= 0) continue;   // void / 畸形描述符：无弹弃面
                const jbc::ir::Insn* next = cu.dom.peek(order[i + 1]);
                if (next == nullptr) continue;
                const Op want = cat == 2 ? Op::POP2 : Op::POP;
                if (next->op != want) continue;
                const auto it = defined.find(MemberTriple{std::string(sem.owner),
                                                           std::string(sem.name),
                                                           std::string(sem.desc)});
                if (it == defined.end()) continue;
                if (!effects.isPure(it->owner, it->name, it->desc)) continue;
                nominated.insert(*it);
            }
        }
    }
    if (nominated.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 委派点名面：栈等价改写（可删形态判定/实参弹宽/句柄钉死/两阶段提交）
    // 全部复用 MethodCallRemover。
    MethodCallRemovalConfig config;
    config.includeCalls.reserve(nominated.size());
    for (const MemberTriple& m : nominated) {
        config.includeCalls.push_back(MethodCallSpec{m.owner, m.name, m.desc});
    }
    MethodCallRemover remover{std::move(config)};
    return remover.run(container, am);
}

}   // namespace mjbase::pass
