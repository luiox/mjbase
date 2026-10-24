#include "mjbase/pass/devirtualize_single_target.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/classpath/resolved_call_graph.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using mjbase::reference::MethodDeclaration;
using mjbase::reference::MethodReference;
using mjbase::reference::MethodReferenceKind;
using mjbase::reference::MethodRefKey;

constexpr ca::u16 kAccPrivate   = 0x0002;
constexpr ca::u16 kAccStatic    = 0x0008;
constexpr ca::u16 kAccBridge    = 0x0040;
constexpr ca::u16 kAccSynthetic = 0x1000;
constexpr ca::u16 kAccAbstract  = 0x0400;

/// 调用者类版本下界：major ≥ 52（V1_8）。依据见 pass 头注 §4.1 条——
/// SE8+ JVM 恒视 ACC_SUPER 置位；更旧版本类文件在旧宿主上按实际位取语义。
constexpr ca::u16 kMinClassMajor = 52;

/// 一处待改写调用点（分析面事实；字节码面复核在装载后进行）。
struct PlannedSite
{
    std::string_view method;      ///< 调用者方法名（索引池视图）
    std::string_view desc;        ///< 调用者方法描述符（索引池视图）
    ca::i32          opcode    = -1;
    ca::i32          insnIndex = -1;
    MethodRefKey     encoded{};    ///< 站点编码目标（索引池视图）
    MethodRefKey     target{};     ///< 唯一落解目标（索引池视图）
};

/// 按类分组的待改写计划（类内站点保持引用扫描序）。
struct PlannedClass
{
    std::string              name;
    std::vector<PlannedSite> sites;
};

/// 该 opcode 是否为去虚拟化的目标形态（虚/接口调用）。
bool isDevirtualizableOpcode(ca::i32 opcode) noexcept
{
    const auto op = static_cast<Op>(opcode);
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKEINTERFACE;
}

/// 站点边收集：与该引用同坐标（insnIndex + opcode + 编码目标）的全部 CHA 边。
/// 返回 nullptr 表示非唯一（0 或 ≥2 条）——唯一目标证明的直接判据。
const mjbase::asm_classpath::ResolvedCallEdge* singleSiteEdge(
    const mjbase::asm_classpath::ResolvedCallGraph& graph, const MethodReference& ref)
{
    const auto& edges =
        graph.edgesFrom(ref.site.sourceClass, ref.site.sourceMethod, ref.site.sourceDesc);
    const mjbase::asm_classpath::ResolvedCallEdge* found = nullptr;
    ca::usize                                      count = 0;
    for (const auto& edge : edges) {
        if (edge.site.insnIndex != ref.site.insnIndex || edge.site.opcode != ref.site.opcode) {
            continue;
        }
        if (!(edge.encodedTarget.owner == ref.target.owner &&
              edge.encodedTarget.name == ref.target.name &&
              edge.encodedTarget.desc == ref.target.desc)) {
            continue;
        }
        ++count;
        if (count > 1) return nullptr;
        found = &edge;
    }
    return count == 1 ? found : nullptr;
}

/// 阶段一：分析面规划。逐引用过分析门，产出按类分组的站点集合。
/// 门语义见 pass 头注「改写与放弃清单」；任何一门不过即放弃该站点。
std::vector<PlannedClass> planSites(const mjbase::reference::ReferenceIndex&      index,
                                    const mjbase::asm_classpath::ResolvedCallGraph& graph,
                                    const mjbase::hierarchy::ClassHierarchy&        ch)
{
    std::unordered_map<std::string_view, ca::usize> byName;
    std::vector<PlannedClass>                       planned;

    for (const MethodReference& ref : index.methodReferences()) {
        if (ref.kind != MethodReferenceKind::Direct) continue;
        if (!isDevirtualizableOpcode(ref.site.opcode)) continue;
        if (ref.site.insnIndex < 0 || ref.target.desc.empty()) continue;

        const auto* edge = singleSiteEdge(graph, ref);
        if (edge == nullptr) continue;   // 多目标 / 零边：非唯一

        const MethodRefKey&     target = edge->resolvedTarget;
        const MethodDeclaration* decl  = index.declaredMethod(target.owner, target.name, target.desc);
        if (decl == nullptr) continue;   // 目标不在容器（降级编码目标/外部）

        const ca::u32 access = decl->access;
        if ((access & (kAccStatic | kAccAbstract | kAccBridge | kAccSynthetic)) != 0) continue;
        if (!target.name.empty() && target.name.front() == '<') {
            continue;   // <init>/<clinit>：虚拟族语义外的防御位
        }
        if ((access & kAccPrivate) != 0 && target.owner != ref.site.sourceClass) {
            continue;   // 跨 nest 私有：invokespecial 选择扫描可能命中中间类同签名声明
        }

        const mjbase::ClassKey targetKey = ch.findClass(target.owner);
        const mjbase::ClassKey callerKey = ch.findClass(ref.site.sourceClass);
        if (!targetKey.valid() || !callerKey.valid()) continue;
        if (ch.isInterface(targetKey)) continue;   // 接口目标：类链优先选择使其不可证
        bool onCallerChain = false;
        for (const mjbase::ClassKey ancestor : ch.ancestors(callerKey)) {
            if (ancestor.id == targetKey.id) {
                onCallerChain = true;
                break;
            }
        }
        if (!onCallerChain) continue;   // 命名类须在调用者 super 链上（verifier isAssignable）

        const std::string_view caller = ref.site.sourceClass;
        const auto [it, inserted]    = byName.emplace(caller, planned.size());
        if (inserted) {
            PlannedClass entry;
            entry.name.assign(caller);
            planned.push_back(std::move(entry));
        }
        PlannedSite site;
        site.method    = ref.site.sourceMethod;
        site.desc      = ref.site.sourceDesc;
        site.opcode    = ref.site.opcode;
        site.insnIndex = ref.site.insnIndex;
        site.encoded   = ref.target;
        site.target    = target;
        planned[it->second].sites.push_back(std::move(site));
    }

    // 类处理序按内部名固定（容器/扫描序不进输出契约，跨平台逐位稳定）。
    std::sort(planned.begin(), planned.end(),
              [](const PlannedClass& a, const PlannedClass& b) { return a.name < b.name; });
    return planned;
}

/// 阶段二（单类）：会话装载 + 逐站点复核与原位改写 + 逐方法 lower。
/// 返回改写后的类字节；任一物化步骤失败返回 nullopt（整类放弃，容器不动）。
std::optional<std::vector<ca::u8>> rewriteOneClass(const std::vector<ca::u8>& corpus,
                                                   const PlannedClass&        entry)
{
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
    if (loaded.is_err()) return std::nullopt;   // 装载失败：整类放弃
    auto&                  session = *loaded.unwrap();
    jbc::ir::ClassUnit&    unit    = session.unit();

    // 版本门：major < 52 的调用者类不改写（§4.1 ACC_SUPER 宿主无关口径）。
    if (session.backing().major < kMinClassMajor) return std::nullopt;

    bool rewrittenAny = false;
    for (const PlannedSite& site : entry.sites) {
        // 定位调用者方法。
        std::optional<ca::usize> methodIdx;
        for (ca::usize i = 0; i < unit.methods.size(); ++i) {
            const jbc::ir::MethodUnit& mu = unit.methods[i];
            if (unit.symbols().str(mu.name) == site.method &&
                unit.symbols().str(mu.desc) == site.desc) {
                methodIdx = i;
                break;
            }
        }
        if (!methodIdx.has_value()) continue;
        if (!jbc::ir::hasCode(session, *methodIdx)) continue;

        auto promoted = jbc::ir::promoteMethod(session, *methodIdx);
        if (promoted.is_err()) return std::nullopt;   // 升格失败：整类放弃
        jbc::ir::CodeUnit&                  cu    = *promoted.unwrap().get();
        jbc::ir::CodeDom&                   dom   = cu.dom;
        const std::vector<jbc::ir::InsnId>& order = dom.order();

        // 坐标复核：分析下标必须落在当前程序序内且指令事实一致。
        if (site.insnIndex <= 0 || static_cast<ca::usize>(site.insnIndex) >= order.size()) {
            continue;   // insnIndex == 0 亦放弃：接收者装载必在调用点之前
        }
        const jbc::ir::InsnId siteId = order[static_cast<ca::usize>(site.insnIndex)];
        const jbc::ir::Insn*  in     = dom.peek(siteId);
        if (in == nullptr || static_cast<Op>(in->op) != static_cast<Op>(site.opcode)) {
            continue;
        }
        jbc::ir::ClassSession::MemberSem member;
        if (!session.memberOf(*in, member) || member.owner != site.encoded.owner ||
            member.name != site.encoded.name || member.desc != site.encoded.desc) {
            continue;   // 分析-字节码漂移：放弃该站点
        }
        // 接收者守卫：前一条指令必须是 ALOAD_0（verifier 接收者约束的保守
        // 充分形态，见 pass 头注 §4.10.1.9 条）。
        const jbc::ir::Insn* prev =
            dom.peek(order[static_cast<ca::usize>(site.insnIndex) - 1]);
        if (prev == nullptr || prev->op != Op::ALOAD_0) continue;
        // 锚定窗守卫：站点前置边界有标签（分支目标/异常表/调试边界）放弃。
        if (method_rewrite_util::labelsAnchorAt(dom, siteId)) continue;

        // 原位改写：INVOKEVIRTUAL/INVOKEINTERFACE → INVOKESPECIAL + 指向目标
        // 声明类的 Methodref（同长改写，帧与 SMT 逐点不变）。
        auto mutableInsn = dom.get(siteId);
        if (mutableInsn.is_err()) continue;
        const Op        savedOp = in->op;
        const ca::i32   savedA  = in->a;
        jbc::ir::Insn*  insn    = mutableInsn.unwrap();
        insn->a                = 0;   // invokeinterface count 字段随形态作废
        insn->op               = Op::INVOKESPECIAL;
        if (!session.setMember(*insn, jbc::ir::ClassSession::MemberSem::Kind::Method,
                               site.target.owner, site.target.name, site.target.desc)) {
            insn->op = savedOp;   // 改写拒绝：恢复原形，放弃该站点
            insn->a  = savedA;
            continue;
        }
        if (jbc::ir::lowerMethod(session, *methodIdx).is_err()) {
            return std::nullopt;   // 重定位失败：整类放弃（会话弃置，容器不动）
        }
        rewrittenAny = true;
    }

    if (!rewrittenAny) return std::nullopt;
    auto encoded = jbc::core::writeClass(session.backing());
    if (encoded.is_err()) return std::nullopt;   // 序列化失败：整类放弃
    return std::move(encoded).unwrap();
}

}   // namespace

mjbase::spi::PreservedAnalyses DevirtualizeSingleTarget::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    auto&     index  = am.getResult<mjbase::reference::ReferenceIndexAnalysis>(container);
    auto&     graph  = am.getResult<mjbase::asm_classpath::ResolvedCallGraphAnalysis>(container);
    auto&     bundle = am.getResult<mjbase::asm_classpath::HierarchyBundleAnalysis>(container);
    const mjbase::hierarchy::ClassHierarchy& ch = bundle->classHierarchy();

    std::vector<PlannedClass> planned = planSites(index, graph, ch);
    if (planned.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 阶段二 + 提交：单次资源扫描，按 thisName 匹配计划类（EmptyClassRemover
    // 同款寻址纪律，不猜测 uri 与类内部名的映射关系）。每类独立两阶段：
    // 改写产物全部就绪后统一 replaceResource——容器只在成功路径上被触碰。
    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
    };
    std::vector<PendingWrite> pending;

    for (auto& [uri, resource] : container.getResources()) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr || cls->get().empty()) continue;
        auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
        if (read.is_err()) continue;
        const std::string name(std::move(read).unwrap().file->thisName());
        const auto        it = std::find_if(planned.begin(), planned.end(),
                                            [&name](const PlannedClass& c) {
                                                return c.name == name;
                                            });
        if (it == planned.end()) continue;

        auto rewritten = rewriteOneClass(cls->get(), *it);
        if (!rewritten.has_value()) continue;   // 整类放弃：原资源保持原样
        PendingWrite item;
        item.uri.assign(uri);
        item.bytes = std::move(*rewritten);
        pending.push_back(std::move(item));
    }

    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();
    for (PendingWrite& item : pending) {
        container.replaceResource(item.uri,
                                  std::make_unique<mjbase::model::ClassResource>(
                                      item.uri, std::move(item.bytes)));
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
