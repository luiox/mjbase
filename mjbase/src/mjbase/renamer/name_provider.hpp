/// @file        name_provider.hpp
/// @brief       改名引擎只读上下文（RenameContext）+ 建议来源抽象（NameProvider）。
///
/// 旧栈对应物：renamer/name_provider（ClassHierarchy+MethodHierarchy 借
/// ClassNode 活在 旧栈 树上）。mjbase 重写线：容器经 class_scan 建libjbc 会话，
/// fillSummary 抽骨架值建 mjbase::hierarchy 图；override 闭包组件 =
/// MethodHierarchy::overrideFamily（并查集等价语义已在 hierarchy 批对拍）。
///
/// 建键纪律：provider/引擎侧一律以 (owner, name, desc) 文本查询，本对象经
/// 自有 KeyPool 驻留后落 MemberKey（跨类身份原语）；不发明跨模块字符串键。
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/method_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/renamer/name_proposal.hpp"

namespace mjbase::renamer {

using ca::core::Result;

class RenamerEngine;   // 引擎友元通道前置声明（rename_engine.hpp）

/// @brief override 闭包组件中的一个声明（驻留视图快照；寿命 = 本上下文）。
struct OverrideComponentMember
{
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

/// @brief 改名引擎的只读上下文视图（旧栈 Q2 结论照旧：只暴露存在性/层级/闭包
///        查询，不暴露可变句柄）。
///
/// 由引擎在 run() 入口从容器一次性构建（类会话扫描 + 类/方法层级），provider
/// 在 propose() 中只读查询。所有返回的 string_view 借用本对象私有 KeyPool，
/// 寿命 = 本对象寿命；消费者不得把视图带过本对象的死亡点。
///
/// RenamerEngine 是友元：引擎读内部层级做成员枚举与撞名检测；provider 侧只走
/// 公开只读查询。
class RenameContext
{
public:
    /// @brief 从容器构建只读视图（扫描全部类资源并建类/方法层级）。
    ///
    /// 堆上构造、以 unique_ptr 交付：ClassHierarchy 借用本对象 pool_ 的地址，
    /// 对象一经构造不得移动/拷贝。畸形类跳过（class_scan fail-open 口径，
    /// 与 reference_index 一致）；重复类名/环属数据灾难，Err 拒绝整次 run。
    static Result<std::unique_ptr<RenameContext>, std::string> build(
        const mjbase::model::ResourceContainer& container);

    RenameContext(const RenameContext&)            = delete;
    RenameContext& operator=(const RenameContext&) = delete;
    RenameContext(RenameContext&&)                 = delete;
    RenameContext& operator=(RenameContext&&)      = delete;

    /// @brief 类是否真实存在于容器（mjbase 层级只登记真实类：无 phantom 形态）。
    bool classExists(std::string_view internalName) const;

    /// @brief sub 是否（传递）子类型（JVM 语义：extends 边 + 接口闭包）。
    bool isSubType(std::string_view sub, std::string_view super) const;

    /// @brief 返回直接父类内部名；不存在 / 无 super 返回空视图。
    std::string_view superClassOf(std::string_view internalName) const;

    /// @brief owner 类内是否存在精确 (name, desc) 方法声明（含 <init>/<clinit>）。
    bool methodExists(std::string_view owner, std::string_view name,
                      std::string_view desc) const;

    /// @brief 方法 override 闭包组件（等价族全部声明，含自身；登记序）。
    ///
    /// 目标方法不在容器中时 Err（NotFound 语义，调用方自行决定降级为单例组）。
    /// mjbase 层级只含容器内声明：旧栈「组件延伸至容器外」的成员本就不参与
    /// 改名，新图上天然不出现。
    Result<std::vector<OverrideComponentMember>, std::string> overrideComponentOf(
        std::string_view owner, std::string_view name, std::string_view desc) const;

    /// @brief 方法所属 override 组件的规范根（族内最浅声明，run 内稳定）。
    ///
    /// 旧栈并查集根下标的替代品：同一族内任意成员的 sourceDeclaration 恒等，
    /// 引擎据此按组件去重。目标方法不在容器/未声明时 Err。
    Result<MemberKey, std::string> overrideComponentRootOf(std::string_view owner,
                                                           std::string_view name,
                                                           std::string_view desc) const;

    /// @brief 上下文私有驻留池（视图出处；池存活期 = 本对象存活期）。
    const KeyPool& pool() const noexcept { return pool_; }

private:
    RenameContext() = default;

    // 引擎内部通道：读层级原始数据做类枚举、成员枚举与撞名检测。
    friend class RenamerEngine;

    /// @brief 文本三段 → 本池驻留 MemberKey（内容寻址；与摘要/闭包查询对键）。
    MemberKey keyOf(std::string_view owner, std::string_view name,
                    std::string_view desc) const;

    /// @brief 类内是否存在精确 (name, desc) 方法/字段声明。
    bool classDeclaresMethod(const hierarchy::ClassSummary& summary, std::string_view name,
                             std::string_view desc) const;
    bool classDeclaresField(const hierarchy::ClassSummary& summary, std::string_view name,
                            std::string_view desc) const;

    /// @brief 沿 super 链上溯，返回声明 (name, desc) 字段的最近类；无处声明返回
    ///        invalid（旧栈 resolveInstanceFieldOwnerIndex 口径）。
    ClassKey resolveInstanceFieldOwner(ClassKey start, std::string_view name,
                                       std::string_view desc) const;

    /// @brief 闭世界判定：自 start 沿 super + interfaces 闭包可达的全部类名都在
    ///        容器中有真实类（java/lang/Object 豁免）。false = 虚分派闭包延伸
    ///        到容器外，继承引用不可安全补别名（旧栈同口径）。
    bool hierarchyClosedForVirtualRename(ClassKey start) const;

    /// 查询按需 intern（keyOf/声明查证把入参文本驻留进池），故为 mutable。
    mutable KeyPool            pool_;
    hierarchy::ClassHierarchy  hierarchy_{pool_};
    hierarchy::MethodHierarchy methods_;
};

/// @brief 名字建议 provider 抽象（旧栈语义照旧：只产建议流、不触字节码）。
///
/// 判定与消解都在 RenamerEngine：provider 只回答"改成什么"，引擎管
/// "怎么改才合法"。单条建议非法由引擎逐条拒并记录；provider 整体硬失败走
/// Err 返回，该 provider 建议流全弃（记 ProviderError）。
class NameProvider
{
public:
    virtual ~NameProvider() = default;

    /// @brief provider 标签：建议缺省 source、审计流水来源列（驻留/静态寿命）。
    virtual std::string_view name() const = 0;

    /// @brief 产出一批改名建议。context 只读，禁止缓存跨 run 引用（视图寿命 = run）。
    virtual Result<std::vector<NameProposal>, std::string> propose(
        const RenameContext& context) = 0;
};

}   // namespace mjbase::renamer
