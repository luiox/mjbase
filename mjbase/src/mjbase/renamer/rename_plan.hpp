/// @file        rename_plan.hpp
/// @brief       改名计划：类映射 + 字段/方法规则（旧栈 MemberRemapPlan 的
///              mjbase renamer 内部等价物）。
///
/// 旧栈对应物：mbase/asm/remapper/member_remap_plan（旧栈引擎直接产出
/// MemberRemapPlan）。remapper 批并行迁移期间 renamer 自持同名语义的计划
/// 结构（类映射 + 成员规则 newOwner/newDesc 预映像 + 描述符预映像查询）；
/// 应用面接线后本结构仍是引擎产物面，应用时按登记序搬运成 asm/remapper 域
/// MemberRemapPlan 消费（见 rename_engine step 8）。语义对旧实现：
///
/// - 规则的 newOwner/newDesc 必须**预映像**（类映射先入 plan）：留原值会让
///   应用面的指令同步路径把调用点 owner/描述符写回旧名（旧栈 P1 实证）；
/// - 方法槽查撞在 mapped-desc 空间进行（luiox/morpher#1037）：类改名同步改写
///   方法描述符中的类类型，最终类文件状态在 mapped-desc 空间；
/// - mapInternalName / mapFieldDescriptor / mapMethodDescriptor 对未命中的
///   输入恒等返回（描述符只改写 L...; 段内类名，数组与前缀原样）。
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mjbase/keys.hpp"

namespace mjbase::renamer {

/// @brief 一条字段/方法改名规则（三段旧位 + 三段终态，全部池驻留视图）。
struct RenameMemberRule
{
    std::string_view owner;
    std::string_view oldName;
    std::string_view desc;
    std::string_view newOwner;
    std::string_view newName;
    std::string_view newDesc;
};

/// @brief 改名计划值簇：引擎 run 的产物之一，供应用面（remapper 批）消费。
///
/// 字符串一律经本计划私有 KeyPool 驻留（入参多为 run 局部 std::string，必须
/// 驻留才能以视图交付）；视图寿命 = 本对象寿命。
class RenamePlan
{
public:
    /// @brief 登记类映射（old → new；视图驻留）。
    void mapClass(std::string_view oldName, std::string_view newName);
    /// @brief 登记字段规则（调用方保证 newOwner/newDesc 已预映像）。
    void mapField(const RenameMemberRule& rule);
    /// @brief 登记方法规则（调用方保证 newOwner/newDesc 已预映像）。
    void mapMethod(const RenameMemberRule& rule);

    /// @brief 计划是否为空（无类映射且无成员规则）。
    bool empty() const noexcept;

    /// @brief 类映射表（登记序；first=old, second=new）。
    const std::vector<std::pair<std::string_view, std::string_view>>& classMappings() const
    {
        return classMappings_;
    }
    const std::vector<RenameMemberRule>& fieldRules() const { return fieldRules_; }
    const std::vector<RenameMemberRule>& methodRules() const { return methodRules_; }

    /// @brief 精确 (owner, oldName, desc) 查字段规则；未命中返回 nullptr。
    const RenameMemberRule* findField(std::string_view owner, std::string_view oldName,
                                      std::string_view desc) const;
    /// @brief 精确 (owner, oldName, desc) 查方法规则；未命中返回 nullptr。
    const RenameMemberRule* findMethod(std::string_view owner, std::string_view oldName,
                                       std::string_view desc) const;

    /// @brief 内部名经类映射预映像；未命中恒等返回入参。
    std::string_view mapInternalName(std::string_view internalName) const;

    /// @brief 字段描述符预映像（L...; 段内类名经类映射改写；拼接产物值返回）。
    std::string mapFieldDescriptor(std::string_view desc) const;
    /// @brief 方法描述符预映像（同上；mapped-desc 查撞空间的数据源）。
    std::string mapMethodDescriptor(std::string_view desc) const;

private:
    /// 描述符遍历核心：逐段复制，L...; 段内类名经 mapInternalName 改写。
    void mapDescriptorInto(std::string_view desc, std::string& out) const;

    /// 查询面（mapInternalName/描述符预映像）按需 intern 未命中键，故为 mutable。
    mutable KeyPool                                     pool_;
    std::vector<std::pair<std::string_view, std::string_view>> classMappings_;
    std::unordered_map<std::string_view, std::string_view>     classByOld_;
    std::vector<RenameMemberRule>                              fieldRules_;
    std::vector<RenameMemberRule>                              methodRules_;
    std::unordered_map<MemberKey, ca::usize, MemberKeyHash>    fieldByKey_;
    std::unordered_map<MemberKey, ca::usize, MemberKeyHash>    methodByKey_;
};

}   // namespace mjbase::renamer
