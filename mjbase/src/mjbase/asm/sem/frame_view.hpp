/// @file        frame_view.hpp
/// @brief       FrameView：方法体逐指令到达帧的只读语义视图（asm/sem 设施层首批）。
///
/// 设计依据 luiox/mjbase #115 §2.2（D0 设计稿）：FrameView 是设施层的帧
/// 查询收口——Strict 档（类型精确）+ MethodSig 播种 + 注入 ITypeResolver，
/// 不可算显式 Err（携带 FrameSummary 诊断），不伪造帧。
///
/// 引擎装配与 FrameEngine(FramePolicy::Strict) 同源同档：DomainEngine +
/// StrictDomain + 签名播种（FrameEngine 本身即 DomainEngine<VType> 的壳）；
/// 本视图保留 DomainResult 的 SlotV 保真形状（cat2 垫底行带 bottom 位），
/// 值行查询按「值」索引（cat2 底槽不计，与 asm/analyzer 的边 index 口径
/// 一致），比 FrameAt 交付（底槽读作 Top）多一层底槽可辨性。
///
/// 红线（#115 §2.1）：只读、值语义、不驻留编辑状态。视图是构建时快照，
/// 方法体任何结构变更后须重建（与 asm/cfg 同口径，不做增量维护）；过期
/// 句柄的查询探测式返回空，不抛错。
///
/// VType::name 是借用视图（CP / 域内驻留表），寿命随常量池——视图存活期
/// 内常量池须存活（同 FrameAt 的借用契约）。
#pragma once

#include <functional>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::asm_sem {

/// @brief callable 适配的引用合并策略：ITypeResolver 的最小注入面。
///
/// #115 §2.2 的「ITypeResolver 默认装配为 hierarchy 求解器」由调用方
/// 完成——层次求解器在 hierarchy/spi 层，asm/sem 不反向依赖；本适配器让
/// 装配点收敛为一行 lambda（spi::CommonSuperClassResolver::getCommonSuperClass
/// 直连即可），测试则以自由函数注入。
class FunctionTypeResolver final : public jbc::analysis::ITypeResolver
{
public:
    using MergeFn = ca::core::Result<jbc::analysis::VType, jbc::analysis::TypeMergeError>(
        const jbc::analysis::VType&, const jbc::analysis::VType&);

    explicit FunctionTypeResolver(std::function<MergeFn> merge)
        : merge_(std::move(merge))
    {}

    ca::core::Result<jbc::analysis::VType, jbc::analysis::TypeMergeError>
    merge(jbc::analysis::VType a, jbc::analysis::VType b) override
    {
        return merge_(a, b);
    }

private:
    std::function<MergeFn> merge_;
};

/// @brief 方法体逐指令到达帧的只读视图（值语义、自持有）。
///
/// frames() 与 CodeUnit::dom.order() 按下标对齐；不可达指令（死代码）的
/// 帧 reachable=false 且 locals/stack 为空。构建失败（数据流不可算）返回
/// Err(FrameSummary)，不交付部分帧。
class FrameView
{
public:
    /// @brief 按 Strict 档计算方法帧（签名播种入参；不修改 unit）。
    ///
    /// @param cp       常量池（引用借用；VType::name 视图寿命随它）。
    /// @param u        参与方法（只读；句柄在视图存活期内保持解析语义）。
    /// @param sig      签名（isStatic/desc 必填；name/thisClass 供构造器
    ///                 UninitThis 与 this 槽精确类名）。
    /// @param resolver 引用合并策略（Strict 必须注入；借用，构建期存活）。
    /// @return Err 携带引擎 FrameSummary（fail/failAt 定位），frames 为空。
    static ca::core::Result<FrameView, jbc::analysis::FrameSummary>
    build(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
          const jbc::analysis::MethodSig& sig, jbc::analysis::ITypeResolver& resolver);

    /// 引擎 summary（ok 时 fail == None）。
    const jbc::analysis::FrameSummary& summary() const noexcept { return summary_; }

    /// 方法体含 jsr/ret（判据 5 的语料级事实；源自引擎 summary）。
    bool hasSubroutine() const noexcept { return summary_.hasSubroutine; }

    /// 逐指令到达帧（与 dom.order() 下标对齐；ok 构建保证非空对齐）。
    const std::vector<jbc::analysis::FrameV<jbc::analysis::VType>>&
    frames() const noexcept
    {
        return frames_;
    }

    /// 指令是否可达（死代码无到达帧；句柄无效/不属于本方法同为 false）。
    bool reachable(jbc::ir::InsnId id) const noexcept { return frameOf(id) != nullptr; }

    /// @brief 到达帧；不可达/无效句柄返回 nullptr。
    const jbc::analysis::FrameV<jbc::analysis::VType>*
    frameOf(jbc::ir::InsnId id) const noexcept;

    /// @brief 到达帧的局部变量槽值。
    /// @return nullptr：不可达 / 越界 / cat2 垫底行（第二槽不承载值）。
    const jbc::analysis::VType* localAt(jbc::ir::InsnId id, ca::u32 idx) const noexcept;

    /// @brief 到达帧的操作数栈值（fromTop 按值计：0 = 栈顶值，cat2 底槽
    ///        不计——与 asm/analyzer 边 index 同口径）。
    /// @return nullptr：不可达 / 越过栈深。
    const jbc::analysis::VType* stackFromTop(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept;

    /// @brief 到达帧的栈上「值」个数（cat2 垫底行不计）。
    ///        不可达返回 0。
    ca::u32 stackValueCount(jbc::ir::InsnId id) const noexcept;

    /// @brief 帧内 UninitializedThis 是否存活（判据 6 探针：构造器超调用
    ///        前该槽不可作参数传递，该时段区域 v1 拒绝——消费方据此过滤）。
    ///        不可达返回 false。
    bool hasUninitThis(jbc::ir::InsnId id) const noexcept;

private:
    jbc::analysis::FrameSummary summary_{};
    std::vector<jbc::analysis::FrameV<jbc::analysis::VType>> frames_;
    std::unordered_map<ca::u32, ca::usize> posOf_;   ///< InsnId.slot → frames 下标
};

}   // namespace mjbase::asm_sem
