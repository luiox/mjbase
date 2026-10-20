/// @file        summary_scanner.hpp
/// @brief       E3 骨架扫描：SummaryScanner 经 libjbc 读侧产出 ClassSummary。
///
/// 消费者对接协议（E 系）的类摘要面：对一份 class 字节做骨架扫描（类名/父/
/// 接口/成员签名/hasCode/access/version），产出可跨类携带的值对象。语义与
/// 旧栈 `WorkspaceBase::listClassSummaries`（libjbc 薄切片口径）对齐：
/// internalName/superName/access/version/interfaceCount/fieldCount/methodCount
/// 逐字段可对拍（version 编码同口径：`(minor << 16) | major`）；在计数之外
/// 再交付成员签名（名字/描述符/access）与方法 hasCode，供消费方在不开会话
/// 的前提下决定 promote 分流。
///
/// 载体纪律：ClassSummary 是纯值；名字/描述符/接口名以 mjbase::ClassKey 与
/// KeyPool 驻留 id 表达（名字驻留调用方传入的作业池，池存活期内键稳定）。
/// 失败 fail-closed：结构损坏（libjbc 读侧 Err）显式报错，不产出半截摘要；
/// 降级读入（Recovered，design.md §29 结构可确定族）照常出骨架。
#pragma once

#include <vector>

#include "jbc/core/bytes.hpp"

#include "mjbase/consumer/class_source.hpp"
#include "mjbase/keys.hpp"

namespace mjbase::consumer {

/// @brief 方法摘要：名字/描述符为 KeyPool 驻留 id + hasCode 分流位。
struct SummaryMethod
{
    ca::u32 name    = 0;   ///< 名字（KeyPool id）
    ca::u32 desc    = 0;   ///< 描述符（KeyPool id）
    ca::u32 access  = 0;   ///< JVM access flags 原样
    bool    hasCode = false;   ///< 是否带 Code 属性（abstract/native 为 false）
};

/// @brief 字段摘要：名字/描述符为 KeyPool 驻留 id。
struct SummaryField
{
    ca::u32 name   = 0;   ///< 名字（KeyPool id）
    ca::u32 desc   = 0;   ///< 描述符（KeyPool id）
    ca::u32 access = 0;   ///< JVM access flags 原样
};

/// @brief 类摘要值对象：一个类对协议消费方可见的全部骨架事实。
///
/// 成员序 = classfile 声明序。self/super/interfaces 驻留调用方 KeyPool；
/// super 为 invalid 键表示无父类条目（java/lang/Object 自身或 module-info，
/// 与 hierarchy::fillSummary 的空串映射同口径）。interfaceCount/
/// fieldCount/methodCount 不另设字段：即三个容器的 size（与 旧栈 口径的
/// 计数逐字段相等）。
struct ClassSummary
{
    ClassKey                   self;       ///< 本类（内部名驻留键）
    ClassKey                   super;      ///< 父类；无父类条目时 invalid
    std::vector<ClassKey>      interfaces; ///< 直接接口（superinterfaces）
    std::vector<SummaryMethod> methods;
    std::vector<SummaryField>  fields;
    ca::u32                    access  = 0;   ///< 类 access flags 原样
    ca::u32                    version = 0;   ///< `(minor << 16) | major`（旧栈 口径）
};

/// @brief E3 骨架扫描器（无状态；池由调用方持有）。
class SummaryScanner
{
public:
    /// @brief 扫描单类字节骨架。
    ///
    /// 经 libjbc readClass 纯扫描（preserveSourceBytes=false，不解属性体；
    /// hasCode 按成员属性表有无 "Code" 槽判定，不解码 Code 内容）。读侧
    /// Err（结构损坏）转 Err(BadClassFile)；Recovered 照常出骨架。
    ConsumerResult<ClassSummary> scan(jbc::core::ByteView bytes, KeyPool& pool) const;

    /// @brief 扫描一个类源全量（按 listClasses 序）。
    ///
    /// fail-closed：任一类扫描失败即整批 Err（消息带类名），不静默跳过、
    /// 不返回部分结果——调用方要容忍缺失请逐类自行 scan。
    ConsumerResult<std::vector<ClassSummary>> scanSource(const IClassSource& source,
                                                         KeyPool&            pool) const;
};

}   // namespace mjbase::consumer
