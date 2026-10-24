/// @file        class_scan.hpp
/// @brief       analysis 模块内共享的类集合扫描底座：资源容器 → 会话集合。
///
/// 各全局分析共享的类扫描入口（ResourceContainer
/// 迭代 + ClassReader 解析 + 畸形跳过）。mjbase 的字节底座是 libjbc：类字节经
/// makeSessionFromBytes 建会话；解析失败/空字节的类不产出会话（fail-open，与
/// 「malformed classes contribute no facts」语义）。会话按容器迭代序
/// 交付（跨类分析的确定性扫描序）。
///
/// 会话寿命：shared_ptr 由扫描集合持有；消费者在扫描集合存活期内使用会话。
/// 本模块的既有分析约定「一个分析任务持有一份扫描集合」，建图期全程借用。
#pragma once

#include <memory>
#include <vector>

#include "jbc/ir/units.hpp"

#include "mjbase/model/resource_container.hpp"

namespace mjbase::analysis {

/// @brief 一个被扫描类：会话（骨架 + 按需 promote 的方法体）+ 来源类名。
struct ScannedClass
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    std::string_view                       name;   ///< 借用会话符号池；会话存活期内有效
};

/// @brief 扫描资源容器中的全部类资源（保持容器迭代序）。
///
/// ClassResource 之外的资源类型跳过；字节解析失败的类跳过（不产出会话）。
std::vector<ScannedClass> scanClassResources(const mjbase::model::ResourceContainer& container);

}   // namespace mjbase::analysis
