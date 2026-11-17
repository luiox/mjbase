/// @file method_body.hpp
/// @brief JVM 与 DEX 共用的方法体 AST，见 doc:semantic-ast。
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "libca/core/datatype.hpp"
#include "mjbase/keys.hpp"

namespace mjbase::ast {

/// 基本块在方法内的下标，0 是入口。
struct BlockId { ca::usize index; };
/// 值在当前块内定义它的语句下标。
struct ValueId { ca::usize index; };
/// 可变变量在方法内的逻辑编号。
struct LocalId { ca::usize index; };

struct Null {};
/// DEX 的无类型 32 位立即数，由用法解释为 int、float 或 null。
struct Bits32 { ca::u32 bits; };
/// Java 字符串用 UTF-16 码元表示，保留 NUL 和未配对代理码元。
using Literal = std::variant<ca::i32, std::u16string, Null, Bits32>;

struct ReadLocal { LocalId local; };
enum class IntOp : ca::u8 { Add, Subtract, Multiply, Divide, Remainder, And, Or, Xor,
                            ShiftLeft, ShiftRight, UnsignedShiftRight };
struct IntBinary { IntOp op; ValueId left; ValueId right; };

/// 空接收者表示静态字段。字段类型由 MemberKey 的描述符给出。
struct FieldRead { MemberKey field; std::optional<ValueId> receiver; };
struct FieldWrite { MemberKey field; std::optional<ValueId> receiver; ValueId value; };

enum class Dispatch : ca::u8 { Static, Virtual, Interface, Direct, Super, Special };
struct Call
{
    MemberKey target;
    Dispatch dispatch;
    std::optional<ValueId> receiver;
    std::vector<ValueId> arguments;
};

using Expression = std::variant<Literal, ReadLocal, IntBinary, FieldRead, Call>;
/// 求值恰好一次，结果可由本块后续语句和终结符引用。
struct Let { Expression expression; };
struct Assign { LocalId local; ValueId value; };
/// 忽略调用结果，保留副作用和异常。
struct CallStatement { Call call; };
using Statement = std::variant<Let, Assign, FieldWrite, CallStatement>;

enum class Comparison : ca::u8 { Equal, NotEqual, Less, GreaterEqual, Greater, LessEqual };
struct Jump { BlockId target; };
struct Branch
{
    Comparison comparison;
    ValueId left;
    ValueId right;
    BlockId whenTrue;
    BlockId whenFalse;
};
struct Return { std::optional<ValueId> value; };
struct Throw { ValueId exception; };
using Terminator = std::variant<Jump, Branch, Return, Throw>;

struct Block
{
    std::vector<Statement> statements;
    Terminator terminator;
};

/// AST 自持。符号身份由调用者提供的 KeyPool 解析。
struct MethodBody
{
    MemberKey method;
    /// 实例方法的接收者变量。
    std::optional<LocalId> receiver;
    /// 描述符顺序的参数变量。
    std::vector<LocalId> parameters;
    ca::usize localCount = 0;
    std::vector<Block> blocks;
};

/// 表示一次符号调用，包含调用点在 AST 内的位置。
struct CallSite
{
    BlockId block;
    ca::usize statement;
    MemberKey target;
    Dispatch dispatch;
};

/// 按块和语句顺序提取静态可见的调用点，含忽略返回值的调用。
std::vector<CallSite> directCalls(const MethodBody& body);

/// 适配器返回完整方法体，失败时返回原因与可用的原始指令位置。
enum class ErrorKind : ca::u8 { Read, MissingMethod, NoCode, InvalidCode, Unsupported,
                                 InvalidLiteralEdit, Write };
struct Error
{
    ErrorKind kind;
    /// JVM 指令序号或 DEX code unit 地址。
    std::optional<ca::usize> position = std::nullopt;
};

}   // namespace mjbase::ast
