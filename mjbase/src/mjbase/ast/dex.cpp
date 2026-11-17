#include "dex.hpp"

#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "dex/dex_file.hpp"
#include "dex/insn.hpp"
#include "jbc/type/descriptor.hpp"
#include "jstr/conversion.hpp"
#include "mjbase/util.hpp"

namespace mjbase::ast {
namespace {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using dex::core::DexFile;
using dex::core::Insn;

enum class Category : ca::u8 { Void, Single, Wide, Reference };

Category categoryOf(const jbc::type::TypeDesc& type)
{
    using jbc::type::BaseType;
    if (type.array_dims || type.kind == jbc::type::TypeDesc::Kind::Class) {
        return Category::Reference;
    }
    return type.primitive == BaseType::Long || type.primitive == BaseType::Double
        ? Category::Wide : Category::Single;
}

struct MethodInfo
{
    MemberKey key;
    Category result;
    std::vector<Category> params;
};

std::optional<MethodInfo> methodInfo(DexFile& file, ca::u32 index, KeyPool& keys)
{
    if (index >= file.methodIds.size()) return std::nullopt;
    const auto proto = file.methodIds[index].protoIdx;
    if (proto >= file.protoIds.size()) return std::nullopt;
    const auto owner = file.methodClassDescriptor(index);
    const auto name = file.methodName(index);
    const auto params = file.protoParameters(proto);
    const auto result = file.typeDescriptor(file.protoIds[proto].returnTypeIdx);
    if (owner.is_err() || name.is_err() || params.is_err() || result.is_err()) return std::nullopt;
    const auto internalName = util::descriptorToInternalName(owner.unwrap().as_std_view());
    const auto memberName = name.unwrap().as_std_view();
    if (internalName.empty() || memberName.empty()) return std::nullopt;

    std::string descriptor("(");
    MethodInfo method{};
    const auto& types = params.unwrap();
    for (ca::u32 i = 0; i < types.size; ++i) {
        auto param = file.typeDescriptor(types.at(i));
        if (param.is_err()) return std::nullopt;
        auto text = param.unwrap().as_std_view();
        auto type = jbc::type::parseFieldType(text);
        if (type.is_err()) return std::nullopt;
        method.params.push_back(categoryOf(type.unwrap()));
        descriptor.append(text);
    }
    const auto resultText = result.unwrap().as_std_view();
    if (resultText == "V") {
        method.result = Category::Void;
    } else {
        auto type = jbc::type::parseFieldType(resultText);
        if (type.is_err()) return std::nullopt;
        method.result = categoryOf(type.unwrap());
    }
    descriptor.append(")").append(resultText);
    method.key = memberKeyOf(keys, classKeyOf(keys, internalName), memberName, descriptor);
    return method;
}

constexpr ca::u8 kMove = 0x01;
constexpr ca::u8 kMoveWide = 0x04;
constexpr ca::u8 kMoveObject16 = 0x09;
constexpr ca::u8 kMoveResult = 0x0a;
constexpr ca::u8 kMoveResultWide = 0x0b;
constexpr ca::u8 kMoveResultObject = 0x0c;
constexpr ca::u8 kReturnVoid = 0x0e;
constexpr ca::u8 kReturn = 0x0f;
constexpr ca::u8 kReturnWide = 0x10;
constexpr ca::u8 kReturnObject = 0x11;
constexpr ca::u8 kConst4 = 0x12;
constexpr ca::u8 kConstHigh16 = 0x15;
constexpr ca::u8 kConstString = 0x1a;
constexpr ca::u8 kConstStringJumbo = 0x1b;
constexpr ca::u8 kThrow = 0x27;
constexpr ca::u8 kGoto = 0x28;
constexpr ca::u8 kGoto32 = 0x2a;
constexpr ca::u8 kIfEq = 0x32;
constexpr ca::u8 kIfEqz = 0x38;
constexpr ca::u8 kIfLez = 0x3d;
constexpr ca::u8 kIget = 0x52;
constexpr ca::u8 kIput = 0x59;
constexpr ca::u8 kSget = 0x60;
constexpr ca::u8 kSput = 0x67;
constexpr ca::u8 kSputShort = 0x6d;
constexpr ca::u8 kInvokeVirtual = 0x6e;
constexpr ca::u8 kInvokeInterface = 0x72;
constexpr ca::u8 kInvokeVirtualRange = 0x74;
constexpr ca::u8 kInvokeInterfaceRange = 0x78;
constexpr ca::u8 kAddInt = 0x90;
constexpr ca::u8 kUshrInt = 0x9a;
constexpr ca::u8 kAddInt2Addr = 0xb0;
constexpr ca::u8 kUshrInt2Addr = 0xba;
constexpr ca::u8 kAddIntLit16 = 0xd0;
constexpr ca::u8 kXorIntLit16 = 0xd7;
constexpr ca::u8 kAddIntLit8 = 0xd8;
constexpr ca::u8 kUshrIntLit8 = 0xe2;

bool isJump(ca::u8 op) { return op >= kGoto && op <= kGoto32; }
bool isConditional(ca::u8 op) { return op >= kIfEq && op <= kIfLez; }
bool isReturn(ca::u8 op) { return op >= kReturnVoid && op <= kReturnObject; }
bool isMoveResult(ca::u8 op) { return op >= kMoveResult && op <= kMoveResultObject; }

std::optional<Dispatch> dispatchOf(ca::u8 op)
{
    static constexpr Dispatch dispatches[] = {
        Dispatch::Virtual, Dispatch::Super, Dispatch::Direct, Dispatch::Static, Dispatch::Interface
    };
    if (op >= kInvokeVirtual && op <= kInvokeInterface) return dispatches[op - kInvokeVirtual];
    if (op >= kInvokeVirtualRange && op <= kInvokeInterfaceRange) {
        return dispatches[op - kInvokeVirtualRange];
    }
    return std::nullopt;
}

std::optional<ca::u32> branchAddress(const Insn& insn)
{
    const ca::i64 address = static_cast<ca::i64>(insn.address) + insn.branchOffset;
    if (address < 0 || address > std::numeric_limits<ca::u32>::max()) return std::nullopt;
    return static_cast<ca::u32>(address);
}

struct BlockLayout
{
    std::vector<ca::usize> starts;
    std::unordered_map<ca::u32, BlockId> blocks;

    std::optional<BlockId> at(ca::u32 address) const
    {
        const auto it = blocks.find(address);
        if (it == blocks.end()) return std::nullopt;
        return it->second;
    }
};

/// Split the supported instruction stream at transfer targets and their successors.
Result<BlockLayout, Error> blockLayout(const std::vector<Insn>& insns)
{
    if (insns.empty()) return Err(Error{ErrorKind::InvalidCode});
    std::unordered_set<ca::u32> leaders{0};
    for (ca::usize i = 0; i < insns.size(); ++i) {
        const auto& insn = insns[i];
        if (insn.isPayload() || (insn.isBranch() && !isJump(insn.opcode)
                                 && !isConditional(insn.opcode))) {
            return Err(Error{ErrorKind::Unsupported, insn.address});
        }
        if (insn.isBranch()) {
            auto target = branchAddress(insn);
            if (!target) return Err(Error{ErrorKind::InvalidCode, insn.address});
            leaders.insert(*target);
        }
        if (i + 1 < insns.size() &&
            (insn.isBranch() || isReturn(insn.opcode) || insn.opcode == kThrow)) {
            leaders.insert(insns[i + 1].address);
        }
    }
    BlockLayout layout;
    for (ca::usize i = 0; i < insns.size(); ++i) {
        const auto address = insns[i].address;
        if (leaders.count(address) != 0) {
            layout.blocks.emplace(address, BlockId{layout.starts.size()});
            layout.starts.push_back(i);
        }
    }
    if (layout.starts.size() != leaders.size()) return Err(Error{ErrorKind::InvalidCode});
    layout.starts.push_back(insns.size());
    return Ok(std::move(layout));
}

struct DexBlock
{
    DexFile& file;
    KeyPool& keys;
    const std::vector<LocalId>& locals;
    Block& block;

    bool valid(ca::usize reg, ca::usize width = 1) const
    {
        return reg < locals.size() && width <= locals.size() - reg;
    }

    ValueId let(Expression expression)
    {
        const ValueId id{block.statements.size()};
        block.statements.emplace_back(Let{std::move(expression)});
        return id;
    }

    ValueId read(ca::usize reg) { return let(ReadLocal{locals[reg]}); }
    void assign(ca::usize reg, ValueId value)
    {
        block.statements.emplace_back(Assign{locals[reg], value});
    }

    std::optional<ErrorKind> call(const Insn& in, Dispatch dispatch, const Insn* moveResult)
    {
        auto target = methodInfo(file, in.index, keys);
        if (!target) return ErrorKind::InvalidCode;
        Call invoke{target->key, dispatch, std::nullopt, {}};
        ca::usize word = 0;
        const auto argReg = [&](ca::usize index) -> ca::usize {
            return in.format == dex::core::InsnFormat::F3rc
                ? static_cast<ca::usize>(in.startRegister) + index : in.registers[index];
        };
        if (in.format != dex::core::InsnFormat::F3rc && in.registerCount > 5) {
            return ErrorKind::InvalidCode;
        }
        if (dispatch != Dispatch::Static) {
            if (in.registerCount == 0 || !valid(argReg(0))) return ErrorKind::InvalidCode;
            invoke.receiver = read(argReg(word++));
        }
        for (const auto param : target->params) {
            const ca::usize width = param == Category::Wide ? 2 : 1;
            if (word + width > in.registerCount) return ErrorKind::InvalidCode;
            const auto reg = argReg(word);
            if (!valid(reg, width) || (width == 2 && argReg(word + 1) != reg + 1)) {
                return ErrorKind::InvalidCode;
            }
            invoke.arguments.push_back(read(reg));
            word += width;
        }
        if (word != in.registerCount) return ErrorKind::InvalidCode;
        if (!moveResult) {
            block.statements.emplace_back(CallStatement{std::move(invoke)});
            return std::nullopt;
        }
        const auto op = target->result == Category::Wide ? kMoveResultWide
            : target->result == Category::Reference ? kMoveResultObject : kMoveResult;
        const auto reg = moveResult->registers[0];
        if (target->result == Category::Void || moveResult->opcode != op
            || !valid(reg, target->result == Category::Wide ? 2 : 1)) {
            return ErrorKind::InvalidCode;
        }
        assign(reg, let(std::move(invoke)));
        return std::nullopt;
    }

    std::optional<ErrorKind> binary(const Insn& in, ca::u8 operation,
                                    ca::usize leftReg, std::optional<ca::usize> rightReg)
    {
        static constexpr IntOp operations[] = {
            IntOp::Add, IntOp::Subtract, IntOp::Multiply, IntOp::Divide, IntOp::Remainder,
            IntOp::And, IntOp::Or, IntOp::Xor, IntOp::ShiftLeft, IntOp::ShiftRight,
            IntOp::UnsignedShiftRight
        };
        const auto dest = in.registers[0];
        if (!valid(dest) || !valid(leftReg) || (rightReg && !valid(*rightReg))) {
            return ErrorKind::InvalidCode;
        }
        auto left = read(leftReg);
        auto right = rightReg ? read(*rightReg) : let(Literal{static_cast<ca::i32>(in.literal)});
        // rsub-int and rsub-int/lit8 subtract the register operand from the literal.
        if (!rightReg && operation == 1) std::swap(left, right);
        assign(dest, let(IntBinary{operations[operation], left, right}));
        return std::nullopt;
    }

    std::optional<ErrorKind> field(const Insn& in)
    {
        const auto owner = file.fieldClassDescriptor(in.index);
        const auto name = file.fieldName(in.index);
        const auto type = file.fieldTypeDescriptor(in.index);
        if (owner.is_err() || name.is_err() || type.is_err()) return ErrorKind::InvalidCode;
        const auto internalName = util::descriptorToInternalName(owner.unwrap().as_std_view());
        const auto memberName = name.unwrap().as_std_view();
        const auto descriptor = type.unwrap().as_std_view();
        auto parsed = jbc::type::parseFieldType(descriptor);
        if (parsed.is_err() || internalName.empty() || memberName.empty()) {
            return ErrorKind::InvalidCode;
        }
        const auto family = categoryOf(parsed.unwrap());
        const bool isStatic = in.opcode >= kSget;
        const bool store = isStatic ? in.opcode >= kSput : in.opcode >= kIput;
        const auto base = isStatic ? (store ? kSput : kSget) : (store ? kIput : kIget);
        const auto reg = in.registers[0];
        // Each access family is ordered narrow, wide, object, boolean, byte, char, short.
        ca::u8 variant = 0;
        if (family == Category::Wide) variant = 1;
        else if (family == Category::Reference) variant = 2;
        else if (descriptor.front() == 'Z') variant = 3;
        else if (descriptor.front() == 'B') variant = 4;
        else if (descriptor.front() == 'C') variant = 5;
        else if (descriptor.front() == 'S') variant = 6;
        if (in.opcode - base != variant || !valid(reg, family == Category::Wide ? 2 : 1)) {
            return ErrorKind::InvalidCode;
        }
        std::optional<ValueId> receiver;
        if (!isStatic) {
            if (!valid(in.registers[1])) return ErrorKind::InvalidCode;
            receiver = read(in.registers[1]);
        }
        const auto key = memberKeyOf(keys, classKeyOf(keys, internalName), memberName, descriptor);
        if (store) block.statements.emplace_back(FieldWrite{key, receiver, read(reg)});
        else assign(reg, let(FieldRead{key, receiver}));
        return std::nullopt;
    }

    std::optional<ErrorKind> step(const Insn& in)
    {
        const auto op = in.opcode;
        const auto dest = in.registers[0];
        const auto src = in.registers[1];
        if (op == 0) return std::nullopt;
        if (op >= kMove && op <= kMoveObject16) {
            const ca::usize width = op >= kMoveWide && op < kMoveWide + 3 ? 2 : 1;
            if (!valid(dest, width) || !valid(src, width)) return ErrorKind::InvalidCode;
            assign(dest, read(src));
        } else if (op >= kConst4 && op <= kConstHigh16) {
            if (!valid(dest)) return ErrorKind::InvalidCode;
            assign(dest, let(Literal{Bits32{static_cast<ca::u32>(in.literal)}}));
        } else if (op == kConstString || op == kConstStringJumbo) {
            if (!valid(dest)) return ErrorKind::InvalidCode;
            auto string = file.string(in.index);
            if (string.is_err()) return ErrorKind::InvalidCode;
            auto decoded = jstr::mutf8_to_utf16(string.unwrap());
            if (decoded.is_err()) return ErrorKind::InvalidCode;
            assign(dest, let(Literal{std::move(decoded).unwrap()}));
        } else if (op >= kAddInt && op <= kUshrInt) {
            return binary(in, op - kAddInt, src, in.registers[2]);
        } else if (op >= kAddInt2Addr && op <= kUshrInt2Addr) {
            return binary(in, op - kAddInt2Addr, dest, src);
        } else if (op >= kAddIntLit16 && op <= kXorIntLit16) {
            return binary(in, op - kAddIntLit16, src, std::nullopt);
        } else if (op >= kAddIntLit8 && op <= kUshrIntLit8) {
            return binary(in, op - kAddIntLit8, src, std::nullopt);
        } else if (op >= kIget && op <= kSputShort) {
            return field(in);
        } else {
            return isMoveResult(op) ? ErrorKind::InvalidCode : ErrorKind::Unsupported;
        }
        return std::nullopt;
    }

    bool terminate(const Insn& in, const BlockLayout& layout, Category result)
    {
        const ca::u8 op = in.opcode;
        const auto reg = in.registers[0];
        if (isReturn(op)) {
            if (result == Category::Void) {
                if (op != kReturnVoid) return false;
                block.terminator = Return{};
            } else {
                const auto expected = result == Category::Wide ? kReturnWide
                    : result == Category::Reference ? kReturnObject : kReturn;
                if (op != expected || !valid(reg, result == Category::Wide ? 2 : 1)) return false;
                block.terminator = Return{read(reg)};
            }
            return true;
        }
        if (op == kThrow) {
            if (!valid(reg)) return false;
            block.terminator = Throw{read(reg)};
            return true;
        }
        const auto fallthrough = layout.at(in.address + in.width);
        if (isJump(op) || isConditional(op)) {
            const auto address = branchAddress(in);
            const auto target = address ? layout.at(*address) : std::nullopt;
            if (!target) return false;
            if (isJump(op)) {
                block.terminator = Jump{*target};
                return true;
            }
            const auto second = in.registers[1];
            const bool againstZero = op >= kIfEqz;
            if (!fallthrough || !valid(reg) || (!againstZero && !valid(second))) return false;
            static constexpr Comparison comparisons[] = {
                Comparison::Equal, Comparison::NotEqual, Comparison::Less,
                Comparison::GreaterEqual, Comparison::Greater, Comparison::LessEqual
            };
            const auto left = read(reg);
            const auto right = againstZero ? let(Literal{Bits32{0}}) : read(second);
            block.terminator = Branch{comparisons[op - (againstZero ? kIfEqz : kIfEq)],
                                      left, right, *target, *fallthrough};
            return true;
        }
        if (!fallthrough) return false;
        block.terminator = Jump{*fallthrough};
        return true;
    }
};

}   // namespace

Result<MethodBody, Error>
liftDexMethod(DexFile& file, const dex::core::EncodedMethod& method, KeyPool& keys)
{
    if (method.codeOff == 0) return Err(Error{ErrorKind::NoCode});
    auto signature = methodInfo(file, method.methodIndex, keys);
    if (!signature) return Err(Error{ErrorKind::InvalidCode});
    const auto loaded = file.codeItem(method.codeOff);
    if (loaded.is_err() || !loaded.unwrap()) return Err(Error{ErrorKind::Read});
    const auto& code = *loaded.unwrap();
    if (!code.tryBlocks.empty()) return Err(Error{ErrorKind::Unsupported});
    auto decoded = dex::core::decodeInsns(dex::core::subOrEmpty(
        file.source(), code.insns.offset, code.insns.length));
    if (decoded.is_err()) return Err(Error{ErrorKind::InvalidCode});
    const auto insns = std::move(decoded).unwrap();
    auto split = blockLayout(insns);
    if (split.is_err()) return Err(split.unwrap_err());
    const auto layout = std::move(split).unwrap();

    MethodBody body;
    body.method = signature->key;
    std::vector<std::optional<LocalId>> parameterRegisters(code.registersSize);
    if (code.insSize > code.registersSize) return Err(Error{ErrorKind::InvalidCode});
    ca::usize reg = code.registersSize - code.insSize;
    if ((method.accessFlags & 0x0008u) == 0) {
        if (reg == code.registersSize) return Err(Error{ErrorKind::InvalidCode});
        body.receiver = LocalId{body.localCount++};
        parameterRegisters[reg++] = body.receiver;
    }
    for (const auto param : signature->params) {
        const ca::usize width = param == Category::Wide ? 2 : 1;
        if (width > code.registersSize - reg) return Err(Error{ErrorKind::InvalidCode});
        const LocalId id{body.localCount++};
        parameterRegisters[reg] = id;
        body.parameters.push_back(id);
        reg += width;
    }
    if (reg != code.registersSize) return Err(Error{ErrorKind::InvalidCode});
    std::vector<LocalId> locals;
    locals.reserve(code.registersSize);
    for (auto id : parameterRegisters) {
        locals.push_back(id ? *id : LocalId{body.localCount++});
    }

    body.blocks.resize(layout.blocks.size());
    for (ca::usize b = 0; b < body.blocks.size(); ++b) {
        DexBlock block{file, keys, locals, body.blocks[b]};
        const auto end = layout.starts[b + 1];
        for (ca::usize i = layout.starts[b]; i < end; ++i) {
            const auto& in = insns[i];
            if (in.isBranch() || isReturn(in.opcode) || in.opcode == kThrow) continue;
            if (auto dispatch = dispatchOf(in.opcode)) {
                const Insn* moveResult = i + 1 < end && isMoveResult(insns[i + 1].opcode)
                    ? &insns[i + 1] : nullptr;
                if (auto error = block.call(in, *dispatch, moveResult)) {
                    return Err(Error{*error, in.address});
                }
                if (moveResult) ++i;
            } else if (auto error = block.step(in)) {
                return Err(Error{*error, in.address});
            }
        }
        const auto& last = insns[end - 1];
        if (!block.terminate(last, layout, signature->result)) {
            return Err(Error{ErrorKind::InvalidCode, last.address});
        }
    }
    return Ok(std::move(body));
}

}   // namespace mjbase::ast
