/// @file        class_source.hpp
/// @brief       E3 类源抽象：IClassSource 协议 + 内存/容器两个实现。
///
/// 消费者对接协议（E 系）的类字节供给面：产品线（开源 mjt 等）经本面按
/// 内部名取 class 字节、枚举类集合，不直接触碰 ResourceContainer/Jar 细节。
/// 设计承自 libjbc design.md §9 的 IClassSource 草案，按 mjbase 底座重定型：
/// 键是 JVM 内部名字符串（协议面的调用方还没有 KeyPool），错误走 Result 值
/// 语义，fail-closed——按名取类缺名显式 Err(NotFound)，不静默空。
///
/// 两个实现：
/// - MemoryClassSource：进程内 map（测试/内联装配）；
/// - ContainerClassSource：ResourceContainer 适配（目录/JAR 经 model 批的
///   导入器装载后即为容器，本类是其协议面投影）。
///
/// 借用纪律：bytes/listClasses 返回的视图借用来源自持存储，来源存活期内
/// 有效；ContainerClassSource 另要求容器在来源存活期内结构冻结（不增删/
/// 替换资源），否则字节视图可能悬垂。
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "libca/core/result.hpp"

#include "jbc/core/bytes.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"

namespace mjbase::consumer {

/// @brief consumer 模块错误码。
enum class ErrorCode : ca::u8
{
    NotFound,     ///< 按名取类：名字不在源内（fail-closed，不静默空）
    BadClassFile, ///< class 字节解析失败（libjbc 读侧 Err，消息带来源标识）
};

/// @brief 错误码 → 短名（断言输出/日志用）。
const char* to_string(ErrorCode code) noexcept;

/// @brief consumer 模块错误值：码 + 消息文本（模块边界错误形态，消息落
///        std::string——与 workspace::WorkspaceErrorInfo 同型，mjbase 总约定
///        中「std::string 只出现在输出/异常文本等外部边界」的豁免位）。
///        本值只经 Result 的 Err 通道出现，不承载 Ok 态。
struct ConsumerError
{
    ErrorCode   code = ErrorCode::NotFound;
    std::string message;
};

/// @brief consumer 模块 Result 别名。
template <typename T>
using ConsumerResult = ca::core::Result<T, ConsumerError>;

/// @brief 类字节供给抽象（E3 协议面）。
///
/// 实现保证：类名集合与字节内容在来源存活期内稳定（不随查询变化）；
/// 枚举序恒为内部名字典序（确定性，与底层容器遍历序解耦）。
class IClassSource
{
public:
    virtual ~IClassSource() = default;

    /// @brief 按内部名取 class 字节。
    ///
    /// 缺名返回 Err(NotFound)（fail-closed：调用方显式决定如何处理缺失，
    /// 库不代为跳过）。视图借用来源自持字节，来源存活期内有效。
    virtual ConsumerResult<jbc::core::ByteView> bytes(std::string_view internalName) const = 0;

    /// @brief 已知类名数。
    virtual ca::usize classCount() const noexcept = 0;

    /// @brief 枚举全部类名（内部名字典序，追加到 out）。
    ///
    /// 视图借用来源自持文本，来源存活期内有效。
    virtual void listClasses(std::vector<std::string_view>& out) const = 0;
};

/// @brief 内存 map 版类源：内部名 → class 字节（进程内装配/测试夹具用）。
///
/// add 为 upsert 语义（同名覆盖既有字节）；枚举序恒为名字字典序。
/// move-only（池驻留文本随源存活，复制无意义）。
class MemoryClassSource final : public IClassSource
{
public:
    MemoryClassSource() = default;
    MemoryClassSource(const MemoryClassSource&)            = delete;
    MemoryClassSource& operator=(const MemoryClassSource&) = delete;
    MemoryClassSource(MemoryClassSource&&)                 = default;
    MemoryClassSource& operator=(MemoryClassSource&&)      = default;

    /// @brief 插入或覆盖一个类（视图入参，字节与名字均转自有存储）。
    void add(std::string_view internalName, std::vector<ca::u8> bytes);

    ConsumerResult<jbc::core::ByteView> bytes(std::string_view internalName) const override;
    ca::usize classCount() const noexcept override { return classes_.size(); }
    void      listClasses(std::vector<std::string_view>& out) const override;

private:
    /// 键为 KeyPool 驻留 id（名字驻留作业池纪律）；map 按 id 的字典序排列
    /// 由 sortedIds_ 显式承载，map 只做 id 寻址。
    std::map<ca::u32, std::vector<ca::u8>> classes_;
    std::vector<ca::u32>                   sortedIds_;   ///< 名字字典序缓存（add 后重建）
    /// 查询入参按名 intern 进本池（内容寻址、幂等），故 pool 为 mutable
    /// （与 mjbase::reference::ReferenceIndex 同款纪律）。
    mutable KeyPool pool_;
};

/// @brief ResourceContainer 适配版类源（目录/JAR 容器投影）。
///
/// make 时逐 ClassResource 解析内部名（libjbc 骨架读）：
/// - 任一 class 资源解析失败 → Err(BadClassFile)（fail-closed：源若带病
///   构建，后续按名取类会静默缺类）。消费方可先经 sanitize 闸门过滤畸形
///   资源再建源。
/// - 同一内部名出现多次（不同 URI）按 first-wins 保留首条（JVM 类加载
///   语义，与容器 addResource 的 URI first-wins 同型），重复项记录在
///   duplicateNames()。
/// - 非 Class 资源（Manifest/Unknown）忽略。
class ContainerClassSource final : public IClassSource
{
public:
    ContainerClassSource(ContainerClassSource&&)            = default;
    ContainerClassSource& operator=(ContainerClassSource&&) = default;

    /// @brief 从容器构建类源；container 必须在本源存活期内结构冻结。
    static ConsumerResult<ContainerClassSource> make(const model::ResourceContainer& container);

    /// @brief 构建期发现的重复内部名（first-wins 保留首条），名字序。
    ///
    /// 视图借用源自持池，源存活期内有效。与容器 duplicateResourceUris()
    /// （URI 级）互补：本表是解析后的内部名级重复。
    const std::vector<std::string_view>& duplicateNames() const noexcept { return duplicates_; }

    ConsumerResult<jbc::core::ByteView> bytes(std::string_view internalName) const override;
    ca::usize classCount() const noexcept override { return resources_.size(); }
    void      listClasses(std::vector<std::string_view>& out) const override;

private:
    ContainerClassSource() = default;

    /// 查询入参按名 intern 进本池（内容寻址、幂等），故 pool 为 mutable
    /// （与 mjbase::reference::ReferenceIndex 同款纪律）。
    mutable KeyPool pool_;
    std::map<ca::u32, const model::ClassResource*> resources_;   ///< 内部名 id → 资源（借用）
    std::vector<ca::u32>                           sortedIds_;   ///< 名字字典序缓存
    std::vector<std::string_view>                  duplicates_;
};

}   // namespace mjbase::consumer
