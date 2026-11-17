#include "jvm.hpp"

#include <utility>

#include "jbc/core/writer.hpp"
#include "jbc/type/descriptor.hpp"
#include "jstr/conversion.hpp"
#include "mjbase/asm/cfg/control_flow_graph.hpp"
#include "mjbase/asm/sem/insert_prim.hpp"

namespace mjbase::ast {
namespace {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::type::TypeDesc;

using Family = asm_sem::ValueFamily;

Family familyOf(const TypeDesc& type)
{
    if (type.array_dims || type.kind == TypeDesc::Kind::Class) return Family::Ref;
    using jbc::type::BaseType;
    switch (type.primitive) {
    case BaseType::Long: return Family::Long;
    case BaseType::Float: return Family::Float;
    case BaseType::Double: return Family::Double;
    default: return Family::Int;
    }
}

ca::usize width(Family type)
{
    return type == Family::Long || type == Family::Double ? 2 : 1;
}

std::optional<IntOp> binaryOp(Op op)
{
    switch (op) {
    case Op::IADD: return IntOp::Add;
    case Op::ISUB: return IntOp::Subtract;
    case Op::IMUL: return IntOp::Multiply;
    case Op::IDIV: return IntOp::Divide;
    case Op::IREM: return IntOp::Remainder;
    case Op::IAND: return IntOp::And;
    case Op::IOR: return IntOp::Or;
    case Op::IXOR: return IntOp::Xor;
    case Op::ISHL: return IntOp::ShiftLeft;
    case Op::ISHR: return IntOp::ShiftRight;
    case Op::IUSHR: return IntOp::UnsignedShiftRight;
    default: return std::nullopt;
    }
}

std::optional<Dispatch> dispatchOf(Op op)
{
    switch (op) {
    case Op::INVOKESTATIC: return Dispatch::Static;
    case Op::INVOKEVIRTUAL: return Dispatch::Virtual;
    case Op::INVOKEINTERFACE: return Dispatch::Interface;
    case Op::INVOKESPECIAL: return Dispatch::Special;
    default: return std::nullopt;
    }
}

struct LiteralSite
{
    BlockId block;
    ValueId value;
    InsnId instruction;
};

/// Order all evaluations, including local reads and ignored call results.
struct BlockLift
{
    struct Operand { ValueId value; Family family; };

    ClassSession& session;
    KeyPool& keys;
    const std::vector<LocalId>& locals;
    Block& block;
    bool entry;
    std::vector<Operand> stack;

    ErrorKind stackError() const
    {
        return entry ? ErrorKind::InvalidCode : ErrorKind::Unsupported;
    }

    ValueId let(Expression expression)
    {
        const ValueId id{block.statements.size()};
        block.statements.emplace_back(Let{std::move(expression)});
        return id;
    }

    void push(Expression expression, Family family)
    {
        stack.push_back({let(std::move(expression)), family});
    }

    std::optional<ValueId> pop(Family family)
    {
        if (stack.empty() || stack.back().family != family) return std::nullopt;
        const ValueId value = stack.back().value;
        stack.pop_back();
        return value;
    }

    std::optional<ErrorKind> call(const Insn& in, Dispatch dispatch)
    {
        ClassSession::MemberSem member;
        if (!session.memberOf(in, member) || member.owner.empty() || member.name.empty()) {
            return ErrorKind::InvalidCode;
        }
        auto parsed = jbc::type::parseMethodDescriptor(member.desc);
        if (parsed.is_err()) return ErrorKind::InvalidCode;
        auto signature = std::move(parsed).unwrap();
        Call invoke{memberKeyOf(keys, classKeyOf(keys, member.owner), member.name, member.desc),
                    dispatch, std::nullopt, std::vector<ValueId>(signature.params.size())};
        for (ca::usize i = signature.params.size(); i > 0; --i) {
            auto value = pop(familyOf(signature.params[i - 1]));
            if (!value) return stackError();
            invoke.arguments[i - 1] = *value;
        }
        if (dispatch != Dispatch::Static) {
            invoke.receiver = pop(Family::Ref);
            if (!invoke.receiver) return stackError();
        }
        if (signature.ret_void) block.statements.emplace_back(CallStatement{std::move(invoke)});
        else push(std::move(invoke), familyOf(signature.ret));
        return std::nullopt;
    }

    std::optional<ErrorKind> local(const Insn& in, Family type, bool store)
    {
        const auto slot = asm_sem::localOperandSlot(in);
        if (slot >= locals.size() || width(type) > locals.size() - slot) {
            return ErrorKind::InvalidCode;
        }
        if (store) {
            auto value = pop(type);
            if (!value) return stackError();
            block.statements.emplace_back(Assign{locals[slot], *value});
        } else {
            push(ReadLocal{locals[slot]}, type);
        }
        return std::nullopt;
    }

    std::optional<ErrorKind> field(const Insn& in)
    {
        ClassSession::MemberSem member;
        if (!session.memberOf(in, member) || member.kind != ClassSession::MemberSem::Kind::Field
            || member.owner.empty() || member.name.empty()) {
            return ErrorKind::InvalidCode;
        }
        auto parsed = jbc::type::parseFieldType(member.desc);
        if (parsed.is_err()) return ErrorKind::InvalidCode;
        const auto family = familyOf(parsed.unwrap());
        const auto key = memberKeyOf(keys, classKeyOf(keys, member.owner), member.name, member.desc);
        std::optional<ValueId> value;
        if (in.op == Op::PUTFIELD || in.op == Op::PUTSTATIC) {
            value = pop(family);
            if (!value) return stackError();
        }
        std::optional<ValueId> receiver;
        if (in.op == Op::GETFIELD || in.op == Op::PUTFIELD) {
            receiver = pop(Family::Ref);
            if (!receiver) return stackError();
        }
        if (value) block.statements.emplace_back(FieldWrite{key, receiver, *value});
        else push(FieldRead{key, receiver}, family);
        return std::nullopt;
    }

    std::optional<ErrorKind> step(const Insn& in)
    {
        if (in.op >= Op::ICONST_M1 && in.op <= Op::ICONST_5) {
            push(Literal{static_cast<ca::i32>(in.op) - static_cast<ca::i32>(Op::ICONST_0)},
                 Family::Int);
            return std::nullopt;
        }
        if (auto op = binaryOp(in.op)) {
            auto right = pop(Family::Int);
            auto left = pop(Family::Int);
            if (!left || !right) return stackError();
            push(IntBinary{*op, *left, *right}, Family::Int);
            return std::nullopt;
        }
        if (auto dispatch = dispatchOf(in.op)) return call(in, *dispatch);

        switch (asm_sem::explicitLocalOp(in.op)) {
        case Op::ILOAD: return local(in, Family::Int, false);
        case Op::LLOAD: return local(in, Family::Long, false);
        case Op::FLOAD: return local(in, Family::Float, false);
        case Op::DLOAD: return local(in, Family::Double, false);
        case Op::ALOAD: return local(in, Family::Ref, false);
        case Op::ISTORE: return local(in, Family::Int, true);
        case Op::LSTORE: return local(in, Family::Long, true);
        case Op::FSTORE: return local(in, Family::Float, true);
        case Op::DSTORE: return local(in, Family::Double, true);
        case Op::ASTORE: return local(in, Family::Ref, true);
        case Op::GETFIELD:
        case Op::GETSTATIC:
        case Op::PUTFIELD:
        case Op::PUTSTATIC: return field(in);
        case Op::BIPUSH:
        case Op::SIPUSH:
            push(Literal{in.a}, Family::Int);
            return std::nullopt;
        case Op::LDC:
        case Op::LDC_W: {
            ClassSession::ConstSem constant;
            if (!session.constOf(in, constant)) return ErrorKind::InvalidCode;
            if (constant.kind == ClassSession::ConstSem::Kind::Int) {
                push(Literal{static_cast<ca::i32>(constant.num)}, Family::Int);
                return std::nullopt;
            }
            if (constant.kind == ClassSession::ConstSem::Kind::String) {
                auto decoded = jstr::mutf8_to_utf16(jstr::MUtf8StringRef(constant.str));
                if (decoded.is_err()) return ErrorKind::InvalidCode;
                push(Literal{std::move(decoded).unwrap()}, Family::Ref);
                return std::nullopt;
            }
            return ErrorKind::Unsupported;
        }
        case Op::ACONST_NULL:
            push(Literal{Null{}}, Family::Ref);
            return std::nullopt;
        case Op::IINC: {
            const auto slot = asm_sem::localOperandSlot(in);
            if (slot >= locals.size()) return ErrorKind::InvalidCode;
            const auto left = let(ReadLocal{locals[slot]});
            const auto right = let(Literal{in.b});
            const auto sum = let(IntBinary{IntOp::Add, left, right});
            block.statements.emplace_back(Assign{locals[slot], sum});
            return std::nullopt;
        }
        case Op::POP:
            if (stack.empty() || width(stack.back().family) != 1) return stackError();
            stack.pop_back();
            return std::nullopt;
        case Op::NOP: return std::nullopt;
        default: return ErrorKind::Unsupported;
        }
    }
};

/// A zero or null comparison in bytecode uses an explicit literal in the AST.
std::optional<Branch> conditional(BlockLift& lift, Op op, BlockId taken, BlockId fallthrough)
{
    const bool againstZero = op >= Op::IFEQ && op <= Op::IFLE;
    const bool againstNull = op == Op::IFNULL || op == Op::IFNONNULL;
    const bool reference = againstNull || op == Op::IF_ACMPEQ || op == Op::IF_ACMPNE;
    const Family family = reference ? Family::Ref : Family::Int;
    std::optional<ValueId> right;
    if (againstZero) right = lift.let(Literal{ca::i32{0}});
    else if (againstNull) right = lift.let(Literal{Null{}});
    else right = lift.pop(family);
    auto left = lift.pop(family);
    if (!left || !right) return std::nullopt;

    static constexpr Comparison comparisons[] = {
        Comparison::Equal, Comparison::NotEqual, Comparison::Less,
        Comparison::GreaterEqual, Comparison::Greater, Comparison::LessEqual
    };
    const auto base = againstZero ? Op::IFEQ : Op::IF_ICMPEQ;
    const Comparison comparison = reference
        ? (op == Op::IFNULL || op == Op::IF_ACMPEQ ? Comparison::Equal : Comparison::NotEqual)
        : comparisons[static_cast<ca::u8>(op) - static_cast<ca::u8>(base)];
    return Branch{comparison, *left, *right, taken, fallthrough};
}

std::optional<ErrorKind> terminate(BlockLift& lift, Op op,
                                   const std::vector<asm_cfg::Edge>& successors,
                                   std::optional<Family> returnType)
{
    std::optional<BlockId> fallthrough;
    std::optional<BlockId> taken;
    for (const auto& edge : successors) {
        switch (edge.kind) {
        case asm_cfg::EdgeKind::FallThrough: fallthrough = BlockId{edge.target.index}; break;
        case asm_cfg::EdgeKind::Branch:
        case asm_cfg::EdgeKind::Condition: taken = BlockId{edge.target.index}; break;
        default: return ErrorKind::Unsupported;
        }
    }
    if ((op >= Op::IFEQ && op <= Op::IF_ACMPNE) || op == Op::IFNULL || op == Op::IFNONNULL) {
        if (!taken || !fallthrough) return ErrorKind::InvalidCode;
        auto branch = conditional(lift, op, *taken, *fallthrough);
        if (!branch) return lift.stackError();
        lift.block.terminator = *branch;
    } else if (op == Op::ATHROW) {
        auto exception = lift.pop(Family::Ref);
        if (!exception) return lift.stackError();
        lift.block.terminator = Throw{*exception};
    } else if (op >= Op::IRETURN && op <= Op::RETURN) {
        if (op == Op::RETURN) {
            if (returnType) return ErrorKind::InvalidCode;
            lift.block.terminator = Return{};
        } else {
            if (!returnType || op != asm_sem::returnOpOf(*returnType)) {
                return ErrorKind::InvalidCode;
            }
            auto value = lift.pop(*returnType);
            if (!value) return lift.stackError();
            lift.block.terminator = Return{*value};
        }
    } else {
        const auto target = op == Op::GOTO || op == Op::GOTO_W ? taken : fallthrough;
        if (!target) return ErrorKind::InvalidCode;
        lift.block.terminator = Jump{*target};
    }
    if (!lift.stack.empty()) return ErrorKind::Unsupported;
    return std::nullopt;
}

Result<MethodBody, Error> lift(ClassSession& session, jbc::ir::MethodId id, KeyPool& keys,
                               std::vector<LiteralSite>* sites)
{
    const auto& cls = session.unit();
    const auto* method = cls.method(id);
    if (!method) return Err(Error{ErrorKind::MissingMethod});
    const auto desc = cls.symbols().str(method->desc);
    auto parsed = jbc::type::parseMethodDescriptor(desc);
    if (parsed.is_err()) return Err(Error{ErrorKind::InvalidCode});
    auto signature = std::move(parsed).unwrap();

    auto* unit = method->code.get();
    if (!unit) {
        if (!jbc::ir::hasCode(session, cls.indexOf(id))) return Err(Error{ErrorKind::NoCode});
        auto promoted = jbc::ir::promoteMethod(session, id);
        if (promoted.is_err()) return Err(Error{ErrorKind::InvalidCode});
        unit = &*promoted.unwrap();
    }
    if (!unit->exc.empty()) return Err(Error{ErrorKind::Unsupported});
    auto cfgResult = asm_cfg::ControlFlowGraph::build(*unit);
    if (cfgResult.is_err()) return Err(Error{ErrorKind::InvalidCode});
    const auto cfg = std::move(cfgResult).unwrap();
    if (cfg.blockCount() == 0) return Err(Error{ErrorKind::InvalidCode});

    MethodBody body;
    body.method = memberKeyOf(keys, classKeyOf(keys, cls.symbols().str(cls.thisClass)),
                              cls.symbols().str(method->name), desc);
    std::vector<LocalId> locals(unit->maxLocals);
    ca::usize slot = 0;
    if ((method->access & 0x0008u) == 0) {
        if (locals.empty()) return Err(Error{ErrorKind::InvalidCode});
        body.receiver = locals[slot++] = LocalId{body.localCount++};
    }
    for (const auto& param : signature.params) {
        const auto size = width(familyOf(param));
        if (size > locals.size() - slot) return Err(Error{ErrorKind::InvalidCode});
        locals[slot] = LocalId{body.localCount++};
        body.parameters.push_back(locals[slot]);
        slot += size;
    }
    // Reserve independent identities for all remaining physical slots, including
    // the high slot of a wide parameter, which bytecode may reuse after entry.
    slot = body.receiver ? 1 : 0;
    for (const auto& param : signature.params) {
        if (width(familyOf(param)) == 2) locals[++slot] = LocalId{body.localCount++};
        ++slot;
    }
    for (; slot < locals.size(); ++slot) locals[slot] = LocalId{body.localCount++};

    const auto returnType = signature.ret_void ? std::nullopt
                                               : std::optional<Family>{familyOf(signature.ret)};
    body.blocks.resize(cfg.blockCount());
    ca::usize position = 0;
    for (const auto& source : cfg.blocks()) {
        Block& block = body.blocks[source.id.index];
        BlockLift liftBlock{session, keys, locals, block, source.id.index == 0, {}};
        for (const InsnId insn : source.insns) {
            const Insn& in = *unit->dom.peek(insn);
            const bool last = insn == source.insns.back();
            const bool control = asm_sem::isControlTransfer(in.op) && in.op != Op::JSR
                && in.op != Op::JSR_W && in.op != Op::RET;
            if (!last || !control) {
                const ValueId start{block.statements.size()};
                if (auto error = liftBlock.step(in)) {
                    return Err(Error{*error, position});
                }
                if (sites && block.statements.size() == start.index + 1) {
                    const auto* let = std::get_if<Let>(&block.statements.back());
                    if (let && std::holds_alternative<Literal>(let->expression)) {
                        sites->push_back({BlockId{source.id.index}, start, insn});
                    }
                }
            }
            if (last) {
                if (auto error = terminate(liftBlock, in.op, cfg.succEdgesOf(source.id), returnType)) {
                    return Err(Error{*error, position});
                }
            }
            ++position;
        }
    }
    return Ok(std::move(body));
}

bool encodeLiteral(ClassSession& session, Insn& in, const Literal& literal)
{
    if (const auto* number = std::get_if<ca::i32>(&literal)) {
        Insn replacement;
        replacement.srcPc = in.srcPc;
        if (*number >= -1 && *number <= 5) {
            replacement.op = static_cast<Op>(static_cast<ca::i32>(Op::ICONST_0) + *number);
        } else if (*number >= -128 && *number <= 127) {
            replacement.op = Op::BIPUSH;
            replacement.a = *number;
        } else if (*number >= -32768 && *number <= 32767) {
            replacement.op = Op::SIPUSH;
            replacement.a = *number;
        } else {
            replacement.cp = session.internInteger(*number);
            if (replacement.cp == 0 || replacement.cp >= 65535u) return false;
            replacement.op = replacement.cp <= 255u ? Op::LDC : Op::LDC_W;
        }
        in = std::move(replacement);
        return true;
    }
    if (const auto* string = std::get_if<std::u16string>(&literal)) {
        auto encoded = jstr::utf16_to_mutf8(*string);
        if (encoded.is_err()) return false;
        const auto text = std::move(encoded).unwrap();
        const auto bytes = text.view().as_std_view();
        return bytes.size() <= 65535u && session.setString(in, bytes)
            && in.cp != 0 && in.cp < 65535u;
    }
    return std::holds_alternative<Null>(literal);
}

bool sameLiteral(const Literal& before, const Literal& after)
{
    if (const auto* number = std::get_if<ca::i32>(&before)) return *number == std::get<ca::i32>(after);
    if (const auto* string = std::get_if<std::u16string>(&before)) {
        return *string == std::get<std::u16string>(after);
    }
    return true;
}

}   // namespace

ca::core::Result<MethodBody, Error>
liftJvmMethod(ClassSession& session, jbc::ir::MethodId method, KeyPool& keys)
{
    return lift(session, method, keys, nullptr);
}

ca::core::Result<std::vector<ca::u8>, Error>
rewriteJvmLiterals(jbc::core::ByteView bytes, std::string_view name, std::string_view descriptor,
                   const LiteralRewriter& rewrite)
{
    auto read = jbc::ir::makeSessionFromBytes(bytes);
    if (read.is_err()) return Err(Error{ErrorKind::Read});
    auto sessionOwner = std::move(read).unwrap();
    auto& session = *sessionOwner;
    const auto method = session.unit().findMethodId(name, descriptor);
    KeyPool keys;
    std::vector<LiteralSite> sites;
    auto lifted = lift(session, method, keys, &sites);
    if (lifted.is_err()) return Err(lifted.unwrap_err());
    if (!rewrite) return Err(Error{ErrorKind::InvalidLiteralEdit});

    const auto body = std::move(lifted).unwrap();
    auto& dom = session.unit().method(method)->code->dom;
    bool changed = false;
    for (const auto& site : sites) {
        const auto& statement = body.blocks[site.block.index].statements[site.value.index];
        const auto& old = std::get<Literal>(std::get<Let>(statement).expression);
        auto replacement = rewrite(body, keys, site.block, site.value, old);
        if (!replacement) continue;
        if (replacement->index() != old.index()) return Err(Error{ErrorKind::InvalidLiteralEdit});
        if (sameLiteral(old, *replacement)) continue;
        auto dest = dom.get(site.instruction);
        if (dest.is_err() || !encodeLiteral(session, *dest.unwrap(), *replacement)) {
            return Err(Error{ErrorKind::InvalidLiteralEdit});
        }
        changed = true;
    }
    if (!changed) return Ok(std::vector<ca::u8>(bytes.data(), bytes.data() + bytes.size()));
    if (jbc::ir::lowerMethod(session, method).is_err()) return Err(Error{ErrorKind::Write});
    auto written = jbc::core::writeClass(session.backing());
    if (written.is_err()) return Err(Error{ErrorKind::Write});
    return Ok(std::move(written).unwrap());
}

}   // namespace mjbase::ast
