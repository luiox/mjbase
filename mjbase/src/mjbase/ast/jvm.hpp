/// @file jvm.hpp
/// @brief classfile 适配器，见 doc:semantic-ast-jvm。
#pragma once

#include <functional>
#include <string_view>

#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"

#include "method_body.hpp"

namespace mjbase::ast {

/// 读取现有 CodeUnit，尚未升格的 JVM 方法按需升格。
ca::core::Result<MethodBody, Error>
liftJvmMethod(jbc::ir::ClassSession& session, jbc::ir::MethodId method, KeyPool& keys);

/// Positional entry for a caller already iterating ClassUnit::methods.
ca::core::Result<MethodBody, Error>
liftJvmMethod(jbc::ir::ClassSession& session, ca::usize methodIndex, KeyPool& keys);

/// Instruction anchors for the statements and terminator of one lifted block.
struct JvmBlockSources
{
    jbc::ir::InsnId entry = jbc::ir::kNoInsn;
    /// Aligned with Block::statements. Synthetic statements have kNoInsn.
    std::vector<jbc::ir::InsnId> statements;
    jbc::ir::InsnId terminator = jbc::ir::kNoInsn;
};

struct JvmMethodAst
{
    MethodBody body;
    /// Aligned with MethodBody::blocks. Anchors describe the code at lift time.
    std::vector<JvmBlockSources> sources;
};

/// Lift a method and retain its JVM instruction anchors for adapters and passes.
ca::core::Result<JvmMethodAst, Error>
liftJvmMethodWithSources(jbc::ir::ClassSession& session, jbc::ir::MethodId method, KeyPool& keys);

/// Replace a JVM local load in the current code with a proven int or null value of the same kind.
/// The caller proves the value at this site before editing. The original instruction anchor
/// and source PC are retained. False leaves the instruction and constant pool intact.
bool replaceJvmLocalReadWithConstant(jbc::ir::ClassSession& session, jbc::ir::CodeUnit& code,
                                     jbc::ir::InsnId site, const Literal& value);

/// 修改单个常量节点。返回空值保留旧值。替换值须属于同一种字面量。
using LiteralRewriter = std::function<std::optional<Literal>(
    const MethodBody&, const KeyPool&, BlockId, ValueId, const Literal&)>;

/// 从独立会话一次提交方法体内的常量改写，失败时原输入仍可用。
/// 回调按块与语句顺序访问原始 AST 中有源指令的常量，成功时返回完整 classfile。
ca::core::Result<std::vector<ca::u8>, Error>
rewriteJvmLiterals(jbc::core::ByteView bytes, std::string_view name, std::string_view descriptor,
                   const LiteralRewriter& rewrite);

}   // namespace mjbase::ast
