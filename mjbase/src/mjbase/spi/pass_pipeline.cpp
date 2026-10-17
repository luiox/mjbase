#include "mjbase/spi/pass_pipeline.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <cstring>

#include "jbc/core/bytes.hpp"
#include "jbc/core/error.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/progress_sink.hpp"

namespace mjbase::spi {

namespace {

using ca::core::Result;

/// 方法诊断目标："<classInternalName>.<name><descriptor>"（faultReport 契约格式）。
/// 输入视图来自会话符号池/驻留池，拼接产物按原始字节保真收进 Utf8String。
ca::str::Utf8String methodTargetText(std::string_view classInternalName,
                                     std::string_view name, std::string_view desc)
{
    std::string text;
    text.reserve(classInternalName.size() + name.size() + desc.size() + 1);
    text.append(classInternalName);
    text.push_back('.');
    text.append(name);
    text.append(desc);
    return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(text.data()),
                                                    text.size());
}

/// 异常文本 → 诊断 detail（ASCII what() 一次性入 Utf8String）。
ca::str::Utf8String detailText(const char* what)
{
    const auto* begin = reinterpret_cast<const ca::u8*>(what != nullptr ? what : "");
    return ca::str::Utf8String::from_data_unchecked(begin, what != nullptr ? std::strlen(what) : 0);
}

}   // namespace

// ── runMethodPipelineOverSession：方法级管线的统一驱动 ──

PreservedAnalyses runMethodPipelineOverSession(jbc::ir::ClassSession&  session,
                                               MethodPassManager&     mpm,
                                               MethodAnalysisManager& mam,
                                               mjbase::KeyPool&       identityPool,
                                               const MethodPredicate& predicate,
                                               const PassPipelineOptions& options)
{
    auto pa = PreservedAnalyses::all();

    jbc::ir::ClassUnit& unit = session.unit();
    const std::string_view thisName = unit.symbols().str(unit.thisClass);
    const mjbase::ClassKey ownerKey = mjbase::classKeyOf(identityPool, thisName);

    // 逐方法堆分配的 scope：全部持有到本函数返回（地址互异且贯穿全程——
    // AnalysisManager 按 scope 地址缓存，栈上复用地址会伪造跨方法命中）。
    std::vector<std::unique_ptr<MethodScope>> scopes;
    std::vector<jbc::ir::CodeUnitRef>         promoted;   // CodeUnit 保活副本

    const ca::usize methodCount = unit.methods.size();
    for (ca::usize idx = 0; idx < methodCount; ++idx) {
        const jbc::ir::MethodUnit& mu = unit.methods[idx];
        const std::string_view     name = unit.symbols().str(mu.name);
        const std::string_view     desc = unit.symbols().str(mu.desc);
        const mjbase::MemberKey    key  = mjbase::memberKeyOf(identityPool, ownerKey, name, desc);

        // 谓词在 promote 前按身份面求值：被过滤方法不付解码成本（-m 语义）。
        if (predicate) {
            const MethodTarget target{key, mu.id, name, desc, mu.access, session};
            if (!predicate(target)) continue;
        }

        // 无 Code 属性（abstract/native）是预期形态：直接跳过，不进错误路径。
        if (!jbc::ir::hasCode(session, idx)) continue;

        try {
            auto promotedResult = jbc::ir::promoteMethod(session, idx);
            if (promotedResult.is_err()) {
                throw std::runtime_error(std::string("promote failed: ") +
                                         jbc::ir::to_string(promotedResult.unwrap_err()));
            }
            promoted.push_back(std::move(promotedResult).unwrap());
            auto scope = std::make_unique<MethodScope>(
                MethodScope{MethodTarget{key, mu.id, name, desc, mu.access, session},
                            *promoted.back().get()});
            scopes.push_back(std::move(scope));

            PreservedAnalyses methodPA = mpm.run(*scopes.back(), mam);
            pa.intersect(methodPA);

            // 管线统一驱动 lower（契约见 runMethodPipelineOverSession 头注）：
            // 仅当本次方法管线声明了 IR 改动才写回 backing。lower 前做嵌套
            // 属性布局适配（LNT/LVT 悬垂行重映射 / SMT 悬垂摘除），lower 后
            // 按 resolver 注入与否走 A1 帧链再接线（见 PassPipelineOptions::
            // typeResolver 契约）。
            if (methodPA.changed()) {
                const bool smtDropped =
                    frame_rewire::prepareNestedAttrsForLayout(session, scopes.back()->code);
                if (smtDropped && options.typeResolver == nullptr) {
                    throw std::runtime_error(
                        "stack map frames dangle after transform and no ITypeResolver "
                        "is injected; refusing to emit a frameless class");
                }
                auto lowered = jbc::ir::lowerMethod(session, scopes.back()->method);
                if (lowered.is_err()) {
                    throw std::runtime_error(std::string("lower failed: ") +
                                             jbc::ir::to_string(lowered.unwrap_err()));
                }
                if (options.typeResolver != nullptr) {
                    frame_rewire::rewireFramesAfterLower(session, *scopes.back(),
                                                         *options.typeResolver);
                }
            }
        }
        catch (const std::exception& e) {
            if (!options.perMethodFaultIsolation) throw;
            // 容错档位：PassManager 已在重抛前对该方法保守失效全部缓存（该路径
            // 无任何保留声明），此处无需再清理；按 none() 参与聚合使整体视为
            // 已改动，驱动外层保留其余方法的成功变换并重序列化。
            pa.intersect(PreservedAnalyses::none());
            if (options.faultReport) {
                options.faultReport->add({/*scope*/ ca::str::Utf8String::from_cstr("method"),
                                          methodTargetText(thisName, name, desc),
                                          /*reason*/ ca::str::Utf8String::from_cstr("pass_error"),
                                          detailText(e.what())});
            }
        }
    }
    return pa;
}

// ── ClassToMethodPassAdaptor ──

PreservedAnalyses ClassToMethodPassAdaptor::run(jbc::ir::ClassSession& session,
                                                ClassAnalysisManager&  cam)
{
    // 经 pinned proxy 取方法级 AM（不受 class 级 pass 的 invalidate 影响）。
    auto* proxyResult = cam.getPinned<MethodAnalysisManagerProxy>();
    if (!proxyResult || !proxyResult->am) {
        throw std::logic_error(
            "ClassToMethodPassAdaptor: MethodAnalysisManagerProxy is not pinned");
    }
    return runMethodPipelineOverSession(session, mpm_, *proxyResult->am, identityPool_,
                                        predicate_, options_);
}

// ── GlobalToClassPassAdaptor ──

PreservedAnalyses GlobalToClassPassAdaptor::run(mjbase::model::ResourceContainer& container,
                                                GlobalAnalysisManager&            gam)
{
    using mjbase::model::ClassResource;

    auto pa = PreservedAnalyses::all();

    // 进度反馈：先数 ClassResource 总数，循环里计数刷新。
    // total 在循环前一次性算出（container 已 import 完毕，非流式）。
    int classTotal = 0;
    if (progress_) {
        for (const auto& [uri, resource] : container.getResources()) {
            (void)uri;
            if (ca::core::tag_cast::isa<ClassResource>(resource.get())) ++classTotal;
        }
    }
    int classIndex = 0;

    // 作业级原子性：两阶段提交。阶段一逐 class 读+跑+序列化到 pending，任何失败
    // 抛出且容器保持原样（无部分写回）；阶段二全部成功后统一提交。前序 class
    // 的进度反馈照常发出（只读过程），但写回被推迟到提交点。
    std::vector<std::pair<ClassResource*, std::vector<ca::u8>>> pending;
    pending.reserve(container.getResources().size());

    for (auto& [uri, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto*       cls   = ca::core::tag_cast::cast<ClassResource>(resource.get());
        const auto& bytes = cls->get();
        // 空 bytes 资源沿既有契约原样跳过。
        if (bytes.empty()) continue;

        // 每类作用域：AM 一造（arena/缓存随迭代消亡，类间无指针跨越）。
        // mam 先于 cam 构造，析构序 cam→mam，pinned proxy 里的 &mam 在 cam
        // 析构时仍有效。pinned proxy 不受 class pass 的 invalidate 影响，
        // ClassToMethodPassAdaptor 始终可经 getPinned 取到。
        ca::str::Utf8StringArena classArena;
        MethodAnalysisManager mam(classArena);
        ClassAnalysisManager  cam(classArena);
        cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});

        ++classIndex;   // 1-based
        if (progress_) {
            ProgressSink::State s;
            s.phase = ProgressSink::Phase::Pipeline;
            s.total = classTotal;
            s.index = classIndex;
            // 展示名转义由 CLI 层负责（mjbase::spi 无转义设施），此处原样透传 URI。
            s.currentName = ca::str::Utf8String::from_data_unchecked(
                reinterpret_cast<const ca::u8*>(uri.data()), uri.size());
            // pass 名未透传（passIndex/passTotal 留 0，进度行不显 pass 段）。
            progress_->update(std::move(s));
        }

        // 失败语义分档：默认档位任一阶段失败即向上抛出（作业级原子），由于写回
        // 推迟到提交点，前序 class 也未写回——整个作业保持原子。
        // perClassFaultIsolation 档位仅捕获 std::exception：失败类记录诊断（reason
        // 按失败阶段 reader/pass/writer 区分）后跳过——不进 pending，原 bytes 保持
        // 原样，其余类继续；作业结束照常统一提交全部成功类。非 std 异常两档都不吞。
        const char* failStage = "reader_error";
        try {
            // 读字节 → 会话（readClass + 骨架物化，读段成功但解析降级不算失败）。
            auto sessionResult = jbc::ir::makeSessionFromBytes(
                jbc::core::ByteView(bytes));
            if (sessionResult.is_err()) {
                const jbc::ir::SessionError se = sessionResult.unwrap_err();
                const char* stageText =
                    se.stage == jbc::ir::SessionError::Stage::Read
                        ? jbc::core::to_string(se.read)
                        : jbc::ir::to_string(se.session);
                throw std::runtime_error(std::string("read failed: ") + stageText);
            }
            auto session = std::move(sessionResult).unwrap();

            // 跑 class pipeline（ClassToMethodPassAdaptor 内部经 pinned proxy 取 mam）
            failStage = "pass_error";
            PreservedAnalyses classPA = cpm_.run(*session, cam);
            pa.intersect(classPA);

            // 序列化到 pending：仅当 class pipeline 改动了 IR 时重新序列化。
            // changed() 与分析保留集合解耦；libjbc copy-through 保证未触碰区段
            // 原样保真，无 COMPUTE_* 旗标需求。
            failStage = "writer_error";
            if (classPA.changed()) {
                auto written = jbc::core::writeClass(session->backing());
                if (written.is_err()) {
                    throw std::runtime_error(std::string("write failed: ") +
                                             jbc::core::to_string(written.unwrap_err()));
                }
                pending.emplace_back(cls, std::move(written).unwrap());
            }

            // E2 ChangeSet 观察点（§35.4 sink 契约）：管线成功且台账非空时
            // 回调——会话仍存活，身份视图在回调内消费。排在写回提交点之前；
            // 默认档位任一后续类失败整作业抛出、容器无写回，调用方按契约
            // 整体作废已折叠状态。
            if (options_.changeSink && !session->changeSet().empty()) {
                const jbc::ir::ClassUnit& unit = session->unit();
                const std::string_view thisName = unit.symbols().str(unit.thisClass);
                options_.changeSink(thisName, *session, session->changeSet());
            }
        }
        catch (const std::exception& e) {
            if (!options_.perClassFaultIsolation) {
                throw std::runtime_error(std::string("GlobalToClassPassAdaptor: failed at '") +
                                         std::string(uri) + "': " + e.what());
            }
            if (options_.faultReport) {
                options_.faultReport->add({/*scope*/ ca::str::Utf8String::from_cstr("class"),
                                           ca::str::Utf8String::from_data_unchecked(
                                               reinterpret_cast<const ca::u8*>(uri.data()),
                                               uri.size()),
                                           /*reason*/
                                           ca::str::Utf8String::from_cstr(failStage),
                                           detailText(e.what())});
            }
            continue;   // 跳过失败类：不写回，其余类继续。
        }
        catch (...) {
            // 非 std 异常不吞：默认档位转 runtime_error 保持原语义；容错档位同样
            // 向上传播终止作业（不静默跳过来源不明的异常）。
            throw std::runtime_error(std::string("GlobalToClassPassAdaptor: failed at '") +
                                     std::string(uri) + "': unknown exception");
        }
    }

    // 阶段二：全部成功，统一提交写回。
    for (auto& [cls, newBytes] : pending) {
        cls->set(std::move(newBytes));
    }

    return pa;
}

}   // namespace mjbase::spi
