/// @file        cp_refs.hpp
/// @brief       analysis 模块内共享的 CP 成员/类解析（域钩子与走查件共用）。
///
/// 解析原语已下沉 libjbc（jbc/core/cp_resolve.hpp，PR #14）：本文件自 B8 8.4
/// 起只是转发头，域钩子与走查件的既有调用面（mjbase::analysis::resolveMemberRef
/// / resolveClassName / MemberRefSem）零改动。libjbc 会话的 memberOf/classOf
/// 是「指令语义层」入口；域钩子只拿到 Insn + ConstantPool（与 DomainEngine
/// 的接线面一致），此处共享的即同语义的纯池解析。解析失败返回 false/空，
/// 不抛错（MemberRefSem 相比旧本地版多出 tag 字段记原引用条目 tag，成功时
/// 置位，既有消费方只读 owner/name/desc，不受影响）。
#pragma once

#include "jbc/core/cp_resolve.hpp"

namespace mjbase::analysis {

/// @brief 成员引用三段（owner/name/desc + 原 tag）；即 jbc::core::MemberRefSem。
using MemberRefSem = jbc::core::MemberRefSem;

using jbc::core::resolveClassName;
using jbc::core::resolveMemberRef;

}   // namespace mjbase::analysis
