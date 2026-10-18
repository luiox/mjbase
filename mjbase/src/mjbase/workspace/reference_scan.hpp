/// @file        reference_scan.hpp
/// @brief       工作区内直写引用扫描：字节码编码事实（方法/字段/类型）。
///
/// mbase ReferenceIndex 的 mjbase 直迁切片：只回答「哪些扫描过的 class 和方法体
/// 直接引用给定的 method/field/type」。旧栈索引的反射事实族中 class-string 形态
/// 事实已随迁（字符串常量按 remapper 反射语法识别为类型引用候选）；常量反射
/// 成员查找、method handle、lambda bootstrap、别名传播与注解/签名/结构元数据
/// 类型引用未随迁——那些是独立的共享分析设施，待主线落（见批次报告）。
/// 本扫描件覆盖：方法调用（invoke 家族）、字段访问（get/put 家族）、显式类型指令
/// （new/checkcast/instanceof/anewarray/multianewarray）、LDC class 常量、
/// LDC class-string 反射候选、member owner 类型引用与 class 头声明
/// （super/interfaces）。
///
/// 底座：类字节经 jbc 会话建面，逐方法 promote 后遍历 CodeDom 程序顺序，invoke/
/// 字段族操作数经 ClassSession::memberOf 解析。site.insnIndex 为方法体内代码指令
/// 序号；class 级引用（头声明）site 的 sourceMethod 为空且 insnIndex 为 -1。
#pragma once

#include <string>
#include <vector>

#include "mjbase/model/resource_container.hpp"
#include "mjbase/workspace/workspace_types.hpp"

namespace mjbase::workspace {

/// @brief 稳定的 JVM 方法引用标识（自持拷贝，build 返回后独立于扫描中间态）。
struct MethodRefKey
{
    std::string owner;
    std::string name;
    std::string descriptor;
};

/// @brief 稳定的 JVM 字段引用标识。
struct FieldRefKey
{
    std::string owner;
    std::string name;
    std::string descriptor;
};

/// @brief 引用位置（方法级引用带指令序号；class 级引用 method/desc 为空、index 为 -1）。
struct ReferenceSite
{
    std::string sourceClass;
    std::string sourceMethod;
    std::string sourceDescriptor;
    ca::i32     insnIndex = -1;
};

/// @brief 方法引用种类。本切片只产出 Direct（编码事实）。
enum class MethodReferenceKind : ca::u8
{
    Direct,
};

/// @brief 字段引用种类。本切片只产出 Direct（编码事实）。
enum class FieldReferenceKind : ca::u8
{
    Direct,
};

/// @brief 一条方法引用事实。
struct MethodReference
{
    MethodRefKey        target;
    ReferenceSite       site;
    MethodReferenceKind kind = MethodReferenceKind::Direct;
};

/// @brief 一条字段引用事实。
struct FieldReference
{
    FieldRefKey        target;
    ReferenceSite      site;
    FieldReferenceKind kind = FieldReferenceKind::Direct;
};

/// @brief 一条类型引用事实。
struct TypeReference
{
    std::string   targetType;
    ReferenceSite site;
};

/// @brief 引用扫描结果与查询面（值语义；build 可重复调用全量重建）。
class ReferenceScan
{
public:
    /// @brief 扫描容器内全部 ClassResource；畸形 class 跳过（容错 CLI 纪律）。
    void build(const model::ResourceContainer& container);

    /// 精确目标查询：编码目标为 owner/name/descriptor 的方法引用。
    std::vector<MethodReference> methodReferencesTo(const std::string& owner, const std::string& name,
                                                    const std::string& descriptor) const;
    /// 来源方法查询：owner.name(descriptor) 方法体发出的全部方法引用。
    std::vector<MethodReference> methodReferencesFrom(const std::string& owner, const std::string& name,
                                                      const std::string& descriptor) const;
    /// 来源类查询：class 声明的全部方法发出的方法引用。
    std::vector<MethodReference> methodReferencesFromClass(const std::string& owner) const;
    /// 精确目标查询：编码目标为 owner/name/descriptor 的字段访问。
    std::vector<FieldReference> fieldReferencesTo(const std::string& owner, const std::string& name,
                                                  const std::string& descriptor) const;
    /// 来源类查询：class 声明的全部方法发出的字段访问。
    std::vector<FieldReference> fieldReferencesFromClass(const std::string& owner) const;
    /// 精确目标查询：编码引用 targetType 的全部位置（指令级 + class 头声明）。
    std::vector<TypeReference> typeReferencesTo(const std::string& targetType) const;
    /// 来源类查询：class 发出的全部类型引用。
    std::vector<TypeReference> typeReferencesFromClass(const std::string& owner) const;

private:
    std::vector<MethodReference> methods_;
    std::vector<FieldReference>  fields_;
    std::vector<TypeReference>   types_;
};

/// @brief MethodReferenceKind → DTO kind 文本（本切片恒 "direct"）。
std::string methodReferenceKind(MethodReferenceKind kind);

/// @brief FieldReferenceKind → DTO kind 文本（本切片恒 "direct"）。
std::string fieldReferenceKind(FieldReferenceKind kind);

/// @brief ReferenceInfo 组装与稳定排序（xref 查询共用）。
ReferenceInfo referenceInfo(const ReferenceSite& site, const std::string& target, const std::string& kind);
void sortReferences(std::vector<ReferenceInfo>& references);

}   // namespace mjbase::workspace
