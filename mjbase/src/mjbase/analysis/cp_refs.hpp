/// @file        cp_refs.hpp
/// @brief       analysis 模块内共享的 CP 成员/类解析（域钩子与走查件共用）。
///
/// 解析原语在 libjbc：本文件只是转发头，对域钩子与走查件提供调用面
/// mjbase::analysis::resolveMemberRef / resolveClassName / MemberRefSem。
/// libjbc 会话的 memberOf/classOf
/// 是「指令语义层」入口；域钩子只拿到 Insn + ConstantPool（与 DomainEngine
/// 的接线面一致），此处共享的即同语义的纯池解析。解析失败返回 false/空，
/// 不抛错（MemberRefSem 的 tag 字段记录原引用条目 tag，成功时
/// 置位，消费方只读 owner/name/desc）。
#pragma once

#include "jbc/core/cp_resolve.hpp"

namespace mjbase::analysis {

/// @brief 成员引用三段（owner/name/desc + 原 tag）；即 jbc::core::MemberRefSem。
using MemberRefSem = jbc::core::MemberRefSem;

using jbc::core::resolveClassName;
using jbc::core::resolveMemberRef;

}   // namespace mjbase::analysis
