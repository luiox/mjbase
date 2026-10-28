/// @file        member_remap_plan.hpp
/// @brief       成员重映射规则计划：类/字段/方法的精确旧值到新值规则。
///
/// 驻留存储换成 mjbase::KeyPool
/// （内容寻址 u32 id），规则键统一为 @ref mjbase::MemberKey——remap 计划的
/// 键类型与 alias/reference 消费契约对齐；文本入参 intern 进计划自有池，
/// 返回的 string_view 均指向池驻留文本，计划存活期内有效。
///
/// `MemberRemapPlan` 只保存 class、field、method 的精确旧值到新值规则，并
/// 提供名称与 descriptor 查询。它不遍历 classfile、不识别反射调用、也不
/// 修改容器资源。应用规则请使用 member_remapper.hpp 的 remap 入口。
#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/keys.hpp"

namespace mjbase::asm_remapper {

/// @brief 字段声明/引用重写规则（键 MemberKey 化）。
///
/// oldKey 三段（owner/name/desc）是映射前的成员身份；oldKey.desc 为空 =
/// 无描述符规则（映射文件形态），按 owner/name 匹配并保留现状描述符。
/// newKey 三段是结构 pass 之后的声明/调用点目标。
struct FieldRemapRule
{
    MemberKey oldKey;
    MemberKey newKey;
};

/// @brief 方法声明/引用重写规则（精确匹配，含描述符）。
struct MethodRemapRule
{
    MemberKey oldKey;
    MemberKey newKey;
};

/// @brief 累积的类/成员重映射计划（结构优化器间的确定性契约）。
///
/// 持有类名映射 + 精确字段/方法规则。查询入口收文本三段，经计划自有
/// KeyPool 驻留后比较；全部返回的 string_view 指向池驻留文本，视图寿命
/// = 计划寿命，消费者不得把视图带过本对象死亡点。
class MemberRemapPlan
{
public:
    /// @brief 添加或替换一个内部类名映射。
    void mapClass(std::string_view oldName, std::string_view newName);

    /// @brief 添加一条精确字段规则（键已驻留的 MemberKey 形态）。
    void mapField(const FieldRemapRule& rule);

    /// @brief 添加一条精确方法规则（键已驻留的 MemberKey 形态）。
    void mapMethod(const MethodRemapRule& rule);

    /// @brief 文本便捷形态：六段文本经本计划池驻留后建规则。
    void mapField(std::string_view owner, std::string_view name, std::string_view desc,
                  std::string_view newOwner, std::string_view newName,
                  std::string_view newDesc);

    /// @brief 文本便捷形态（方法）。
    void mapMethod(std::string_view owner, std::string_view name, std::string_view desc,
                   std::string_view newOwner, std::string_view newName,
                   std::string_view newDesc);

    /// @brief 映射类内部名；未命中原样返回输入。
    std::string_view mapInternalName(std::string_view name) const;

    /// @brief 重写字段描述符中内嵌的对象类型。
    ///
    /// 恒等快路：classMappings_ 为空时返回输入视图本身（零分配，与
    /// mapInternalName 未命中同语义）；非空时无论是否改写均返回池驻留视图。
    /// 重复输入经描述符缓存记忆化。消费者即时消费，不得跨期持有（寿命=计划）。
    std::string_view mapFieldDescriptor(std::string_view desc) const;

    /// @brief 重写方法描述符中内嵌的对象类型（缓存语义同上）。
    std::string_view mapMethodDescriptor(std::string_view desc) const;

    /// @brief 重写泛型签名文本：结构化签名树读写（libjbc
    ///        jbc::type::signature 消费面）。解析树定界全部类类型出现，逐类
    ///        类型按完整内部名查映射，段级 span 替换拼装（扁平段链跨 '$'
    ///        形态整段替换；真嵌套段链部件对齐逐段替换，新名部件数不一致
    ///        即整链坍缩为扁平新名——改名 pass 给嵌套类配独立新名，二元名
    ///        整体易主，链间 '.' 与链上实参随旧链结构一并消失）；类型实参/
    ///        通配符/数组维度/类型变量原样保留。解析失败原样返回输入
    ///        （fail-closed，畸形签名清洗归 IllegalSignatureRemover）。单遍
    ///        同时替换语义：每个类类型出现只查一次映射，映射结果不再串链。
    std::string_view mapSignature(std::string_view signature) const;

    /// @brief 精确字段规则查询；退化匹配描述符为空的 owner/name 规则。
    const FieldRemapRule* findField(std::string_view owner, std::string_view name,
                                    std::string_view desc) const;

    /// @brief 精确方法规则查询（owner/name/desc 全匹配）。
    const MethodRemapRule* findMethod(std::string_view owner, std::string_view name,
                                      std::string_view desc) const;

    /// @brief 名称规则查询：全部 owner/name 匹配改名一致才返回规则，否则 nullptr。
    const FieldRemapRule* findFieldByName(std::string_view owner,
                                          std::string_view name) const;

    /// @brief 名称规则查询（方法；歧义重载返回 nullptr）。
    const MethodRemapRule* findMethodByName(std::string_view owner,
                                            std::string_view name) const;

    /// @brief owner 槽统一映射入口：直接类名走 mapInternalName；`[L...;`/
    ///        `[[L...;` 数组型 owner（enum values() 的 clone 引用等）按字段
    ///        描述符整体重映射。
    std::string_view mapOwnerRef(std::string_view owner) const;

    /// @brief 无任何类/字段/方法映射时为 true。
    bool empty() const;

    /// @brief 计划自有驻留池（调用方经 memberKeyOf 构造 MemberKey 规则键用）。
    KeyPool&       pool() noexcept { return pool_; }
    const KeyPool& pool() const noexcept { return pool_; }

private:
    /// @brief 描述符重写工作体：classMappings_ 非空时解析并重拼，结果驻留。
    std::string_view remapDescriptorCached(std::string_view desc, bool method) const;

    mutable KeyPool pool_;   ///< 查询期文本 intern 落点（const 查询惰性驻留）

    /// 类映射（驻留 id 键值）；描述符重映射的唯一输入面。
    std::unordered_map<ca::u32, ca::u32> classMappings_;

    /// desc→desc 结果缓存（键值均驻留 id）：mapClass 是唯一失效点。
    mutable std::unordered_map<ca::u32, ca::u32> descCache_;

    std::vector<FieldRemapRule>  fieldRules_;
    std::vector<MethodRemapRule> methodRules_;
};

}   // namespace mjbase::asm_remapper
