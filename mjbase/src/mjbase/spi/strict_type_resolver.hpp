#pragma once

#include "jbc/analysis/frame.hpp"

#include "mjbase/spi/common_super_resolver.hpp"

namespace mjbase::spi {

/// @brief Strict 帧合并的 resolver 适配器：CommonSuperClassResolver →
///        libjbc ITypeResolver（design §10.1 边界：实现体在 mjbase，注入
///        libjbc 计算域）。
///
/// 职责面 = merge 单方法：两个具体引用类型的公共父类求解，委托
/// CommonSuperClassResolver::getCommonSuperClass（super-chain-only，接口
/// 维度不参与，与帧域契约一致）。结构性规则（Top 支配、Null 底、
/// Uninitialized 按 offset 判等）由 libjbc 计算域内置，不经此适配器。
///
/// 非具体引用对的防御语义：任一侧缺精确名（BestEffort 形态值/无名引用）
/// 返回 Err(Unresolvable)——Strict 域据此显式拒绝并给指令定位，不伪造帧。
///
/// 结果视图寿命：返回 VType 的 name 指向层次图驻留池或 resolver 查询池，
/// 寿命 = 两者中先死亡者（pass-contract §7）；帧计算期内调用方须保持
/// resolver 与层次图存活。
class StrictTypeResolverAdapter : public jbc::analysis::ITypeResolver
{
public:
    /// @param resolver 已挂层次图的公共父类求解器（借用，须存活）。
    explicit StrictTypeResolverAdapter(CommonSuperClassResolver& resolver) noexcept
        : resolver_(&resolver)
    {}

    ca::core::Result<jbc::analysis::VType, jbc::analysis::TypeMergeError>
        merge(jbc::analysis::VType a, jbc::analysis::VType b) override;

private:
    CommonSuperClassResolver* resolver_;
};

}   // namespace mjbase::spi
