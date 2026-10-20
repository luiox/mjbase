/// @file        indy_test.cpp
/// @brief       asm/classpath indy 解析面测试：BSM 分族、lambda 展开产物、
///              altMetafactory 桥类型、已知无目标族、未知族 fail-closed、
///              VarHandle 访问读写分类，以及 CallGraph 的展开边织入与外部目标过滤。
///
/// 类字节全部手工拼装（CP/BSM/Code 逐字节受控），不依赖本地语料。
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/bytes.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/asm/classpath/indy.hpp"

namespace {

using Bytes = std::vector<ca::u8>;
using jbc::core::Op;
using mjbase::asm_classpath::BootstrapFamily;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::asm_classpath::CallGraphOptions;
using mjbase::asm_classpath::CallSite;
using mjbase::asm_classpath::HandleDispatch;
using mjbase::asm_classpath::IndyIndex;
using mjbase::asm_classpath::IndyIndexBuilder;
using mjbase::asm_classpath::IndyResolution;
using mjbase::asm_classpath::InvokeKind;
using mjbase::asm_classpath::VarHandleAccess;
using mjbase::MemberKey;

void putU8(Bytes& b, ca::u8 v) { b.push_back(v); }

void putU16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

void putU32(Bytes& b, ca::u32 v)
{
    b.push_back(static_cast<ca::u8>(v >> 24));
    b.push_back(static_cast<ca::u8>(v >> 16));
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

void putUtf8Raw(Bytes& b, std::string_view s)
{
    for (const char c : s) {
        b.push_back(static_cast<ca::u8>(c));
    }
}

constexpr std::string_view kMetafactoryDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;"
    "Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)"
    "Ljava/lang/invoke/CallSite;";
constexpr std::string_view kAltMetafactoryDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;";
constexpr std::string_view kUnknownBsmDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;";
constexpr std::string_view kObjectMethodsDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/TypeDescriptor;Ljava/lang/Class;Ljava/lang/String;"
    "[Ljava/lang/invoke/MethodHandle;)Ljava/lang/Object;";

/// MethodHandle 载体规格。
struct HandleSpec
{
    ca::u8           kind = 6;
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

/// BSM 静态参数规格（loadable 子集：int/string/class/method type/handle）。
struct BsmArgSpec
{
    enum class Kind : ca::u8
    {
        Int,
        String,
        Class,
        MethodType,
        MethodHandle,
    };

    Kind             kind = Kind::MethodType;
    std::string_view text;
    ca::i32          num = 0;
    HandleSpec       handle;
};

BsmArgSpec methodTypeArg(std::string_view desc)
{
    BsmArgSpec arg;
    arg.kind = BsmArgSpec::Kind::MethodType;
    arg.text = desc;
    return arg;
}

BsmArgSpec classArg(std::string_view internalName)
{
    BsmArgSpec arg;
    arg.kind = BsmArgSpec::Kind::Class;
    arg.text = internalName;
    return arg;
}

BsmArgSpec stringArg(std::string_view value)
{
    BsmArgSpec arg;
    arg.kind = BsmArgSpec::Kind::String;
    arg.text = value;
    return arg;
}

BsmArgSpec intArg(ca::i32 value)
{
    BsmArgSpec arg;
    arg.kind = BsmArgSpec::Kind::Int;
    arg.num  = value;
    return arg;
}

BsmArgSpec handleArg(ca::u8 kind, std::string_view owner, std::string_view name,
                     std::string_view desc)
{
    BsmArgSpec arg;
    arg.kind   = BsmArgSpec::Kind::MethodHandle;
    arg.handle = HandleSpec{kind, owner, name, desc};
    return arg;
}

struct InsnSpec
{
    enum class Kind : ca::u8
    {
        Simple,
        Indy,
        InvokeVirtual,
    };

    Kind             kind = Kind::Simple;
    ca::u8           opcode = static_cast<ca::u8>(Op::RETURN);
    std::string_view name;
    std::string_view desc;
    std::string_view owner;
    ca::u16          bsm = 0;
};

InsnSpec simple(Op op)
{
    InsnSpec insn;
    insn.kind   = InsnSpec::Kind::Simple;
    insn.opcode = static_cast<ca::u8>(op);
    return insn;
}

InsnSpec indy(std::string_view name, std::string_view desc, ca::u16 bsm)
{
    InsnSpec insn;
    insn.kind = InsnSpec::Kind::Indy;
    insn.name = name;
    insn.desc = desc;
    insn.bsm  = bsm;
    return insn;
}

InsnSpec invokeVirtual(std::string_view owner, std::string_view name,
                       std::string_view desc)
{
    InsnSpec insn;
    insn.kind  = InsnSpec::Kind::InvokeVirtual;
    insn.owner = owner;
    insn.name  = name;
    insn.desc  = desc;
    return insn;
}

struct MethodSpec
{
    std::string_view      name;
    std::string_view      desc;
    ca::u16               access = 0x0009;   // public static
    std::vector<InsnSpec> insns;
};

struct BsmSpec
{
    HandleSpec               handle;
    std::vector<BsmArgSpec>  args;
};

/// 手工拼最小 classfile（V1_8 / public super / super=java/lang/Object）。
///
/// CP 先全量物化（字段/BSM/方法声明），再写头与主体；写阶段全部命中缓存，
/// cpCount 不会漏条目。
class ClassBuilder
{
public:
    explicit ClassBuilder(std::string_view thisClass)
    {
        thisIdx_  = cls(thisClass);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
        bsmName_  = utf8("BootstrapMethods");
    }

    ca::u16 addBootstrap(const HandleSpec& handle, std::vector<BsmArgSpec> args)
    {
        bsms_.push_back(BsmSpec{handle, std::move(args)});
        return static_cast<ca::u16>(bsms_.size() - 1);
    }

    void addField(std::string_view name, std::string_view desc)
    {
        fields_.push_back(std::make_pair(name, desc));
    }

    void addMethod(MethodSpec method) { methods_.push_back(std::move(method)); }

    Bytes assemble(std::string_view thisClass)
    {
        for (const auto& field : fields_) {
            utf8(field.first);
            utf8(field.second);
        }
        for (const BsmSpec& bsm : bsms_) {
            declareBsm(bsm);
        }
        for (const MethodSpec& method : methods_) {
            declareMethod(method);
        }

        Bytes out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);   // V1_8
        putU16(out, next_);  // cpCount（无 long/double 双槽）
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, 0x0021);   // public super
        putU16(out, thisIdx_);
        putU16(out, superIdx_);
        putU16(out, 0);   // interfaces
        putU16(out, static_cast<ca::u16>(fields_.size()));
        for (const auto& field : fields_) {
            putU16(out, 0x0002);   // private
            putU16(out, utf8(field.first));
            putU16(out, utf8(field.second));
            putU16(out, 0);   // field attrs
        }
        putU16(out, static_cast<ca::u16>(methods_.size()));
        for (const MethodSpec& method : methods_) {
            writeMethod(out, method);
        }
        writeClassAttrs(out);
        (void)thisClass;
        return out;
    }

private:
    ca::u16 utf8(std::string_view s)
    {
        std::string key(s);
        const auto  it = utf8s_.find(key);
        if (it != utf8s_.end()) {
            return it->second;
        }
        putU8(cp_, 0x01);
        putU16(cp_, static_cast<ca::u16>(s.size()));
        putUtf8Raw(cp_, s);
        const ca::u16 idx = next_++;
        utf8s_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 cls(std::string_view name)
    {
        std::string key(name);
        const auto  it = classes_.find(key);
        if (it != classes_.end()) {
            return it->second;
        }
        const ca::u16 nameIdx = utf8(name);
        putU8(cp_, 0x07);
        putU16(cp_, nameIdx);
        const ca::u16 idx = next_++;
        classes_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 stringCp(std::string_view value)
    {
        std::string key(value);
        const auto  it = strings_.find(key);
        if (it != strings_.end()) {
            return it->second;
        }
        const ca::u16 utf8Idx = utf8(value);
        putU8(cp_, 0x08);
        putU16(cp_, utf8Idx);
        const ca::u16 idx = next_++;
        strings_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 integerCp(ca::i32 value)
    {
        const auto it = integers_.find(value);
        if (it != integers_.end()) {
            return it->second;
        }
        putU8(cp_, 0x03);
        putU32(cp_, static_cast<ca::u32>(value));
        const ca::u16 idx = next_++;
        integers_.emplace(value, idx);
        return idx;
    }

    ca::u16 nat(std::string_view name, std::string_view desc)
    {
        std::string key = std::string(name) + "." + std::string(desc);
        const auto  it  = nats_.find(key);
        if (it != nats_.end()) {
            return it->second;
        }
        const ca::u16 nameIdx = utf8(name);
        const ca::u16 descIdx = utf8(desc);
        putU8(cp_, 0x0C);
        putU16(cp_, nameIdx);
        putU16(cp_, descIdx);
        const ca::u16 idx = next_++;
        nats_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 ref(ca::u8 tag, std::string_view owner, std::string_view name,
                std::string_view desc)
    {
        std::string key = std::string(1, static_cast<char>(tag)) + "." +
                          std::string(owner) + "." + std::string(name) + "." +
                          std::string(desc);
        const auto it = refs_.find(key);
        if (it != refs_.end()) {
            return it->second;
        }
        const ca::u16 ownerIdx = cls(owner);
        const ca::u16 natIdx   = nat(name, desc);
        putU8(cp_, tag);
        putU16(cp_, ownerIdx);
        putU16(cp_, natIdx);
        const ca::u16 idx = next_++;
        refs_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 methodHandleCp(const HandleSpec& handle)
    {
        std::string key = "H." + std::to_string(handle.kind) + "." +
                          std::string(handle.owner) + "." + std::string(handle.name) +
                          "." + std::string(handle.desc);
        const auto it = handles_.find(key);
        if (it != handles_.end()) {
            return it->second;
        }
        const ca::u8 tag = handle.kind <= 4 ? 0x09 : (handle.kind == 9 ? 0x0B : 0x0A);
        const ca::u16 refIdx = ref(tag, handle.owner, handle.name, handle.desc);
        putU8(cp_, 0x0F);
        putU8(cp_, handle.kind);
        putU16(cp_, refIdx);
        const ca::u16 idx = next_++;
        handles_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 methodTypeCp(std::string_view desc)
    {
        std::string key(desc);
        const auto  it = methodTypes_.find(key);
        if (it != methodTypes_.end()) {
            return it->second;
        }
        const ca::u16 descIdx = utf8(desc);
        putU8(cp_, 0x10);
        putU16(cp_, descIdx);
        const ca::u16 idx = next_++;
        methodTypes_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 indyCp(ca::u16 bsm, std::string_view name, std::string_view desc)
    {
        std::string key = "I." + std::to_string(bsm) + "." + std::string(name) + "." +
                          std::string(desc);
        const auto it = indys_.find(key);
        if (it != indys_.end()) {
            return it->second;
        }
        const ca::u16 natIdx = nat(name, desc);
        putU8(cp_, 0x12);
        putU16(cp_, bsm);
        putU16(cp_, natIdx);
        const ca::u16 idx = next_++;
        indys_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 bsmArgCp(const BsmArgSpec& arg)
    {
        switch (arg.kind) {
        case BsmArgSpec::Kind::Int: return integerCp(arg.num);
        case BsmArgSpec::Kind::String: return stringCp(arg.text);
        case BsmArgSpec::Kind::Class: return cls(arg.text);
        case BsmArgSpec::Kind::MethodType: return methodTypeCp(arg.text);
        case BsmArgSpec::Kind::MethodHandle: return methodHandleCp(arg.handle);
        }
        return 0;
    }

    void declareBsm(const BsmSpec& bsm)
    {
        methodHandleCp(bsm.handle);
        for (const BsmArgSpec& arg : bsm.args) {
            bsmArgCp(arg);
        }
    }

    void declareMethod(const MethodSpec& method)
    {
        utf8(method.name);
        utf8(method.desc);
        for (const InsnSpec& insn : method.insns) {
            if (insn.kind == InsnSpec::Kind::Indy) {
                indyCp(insn.bsm, insn.name, insn.desc);
            }
            else if (insn.kind == InsnSpec::Kind::InvokeVirtual) {
                ref(0x0A, insn.owner, insn.name, insn.desc);
            }
        }
    }

    Bytes codeOf(const MethodSpec& method)
    {
        Bytes code;
        for (const InsnSpec& insn : method.insns) {
            switch (insn.kind) {
            case InsnSpec::Kind::Simple: putU8(code, insn.opcode); break;
            case InsnSpec::Kind::Indy:
                putU8(code, static_cast<ca::u8>(Op::INVOKEDYNAMIC));
                putU16(code, indyCp(insn.bsm, insn.name, insn.desc));
                putU16(code, 0);   // 两个保留字节
                break;
            case InsnSpec::Kind::InvokeVirtual:
                putU8(code, static_cast<ca::u8>(Op::INVOKEVIRTUAL));
                putU16(code, ref(0x0A, insn.owner, insn.name, insn.desc));
                break;
            }
        }
        return code;
    }

    void writeMethod(Bytes& out, const MethodSpec& method)
    {
        const Bytes code = codeOf(method);
        putU16(out, method.access);
        putU16(out, utf8(method.name));
        putU16(out, utf8(method.desc));
        putU16(out, 1);   // 一个 Code 属性
        putU16(out, codeName_);
        putU32(out, static_cast<ca::u32>(12 + code.size()));
        putU16(out, 8);   // max_stack
        putU16(out, 8);   // max_locals
        putU32(out, static_cast<ca::u32>(code.size()));
        out.insert(out.end(), code.begin(), code.end());
        putU16(out, 0);   // exception table
        putU16(out, 0);   // Code 嵌套属性
    }

    void writeClassAttrs(Bytes& out)
    {
        if (bsms_.empty()) {
            putU16(out, 0);
            return;
        }
        Bytes payload;
        putU16(payload, static_cast<ca::u16>(bsms_.size()));
        for (const BsmSpec& bsm : bsms_) {
            putU16(payload, methodHandleCp(bsm.handle));
            putU16(payload, static_cast<ca::u16>(bsm.args.size()));
            for (const BsmArgSpec& arg : bsm.args) {
                putU16(payload, bsmArgCp(arg));
            }
        }
        putU16(out, 1);
        putU16(out, bsmName_);
        putU32(out, static_cast<ca::u32>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
    }

    ca::u16 next_    = 1;
    ca::u16 thisIdx_ = 0;
    ca::u16 superIdx_ = 0;
    ca::u16 codeName_ = 0;
    ca::u16 bsmName_  = 0;
    Bytes   cp_;
    std::vector<std::pair<std::string_view, std::string_view>> fields_;
    std::vector<MethodSpec> methods_;
    std::vector<BsmSpec>    bsms_;

    std::map<std::string, ca::u16> utf8s_;
    std::map<std::string, ca::u16> classes_;
    std::map<std::string, ca::u16> strings_;
    std::map<ca::i32, ca::u16>     integers_;
    std::map<std::string, ca::u16> nats_;
    std::map<std::string, ca::u16> refs_;
    std::map<std::string, ca::u16> handles_;
    std::map<std::string, ca::u16> methodTypes_;
    std::map<std::string, ca::u16> indys_;
};

Bytes lambdaHostBytes()
{
    ClassBuilder builder("sample/LambdaHost");

    const ca::u16 bsmLambda = builder.addBootstrap(
        HandleSpec{6, "java/lang/invoke/LambdaMetafactory", "metafactory",
                   kMetafactoryDesc},
        {methodTypeArg("(Ljava/lang/Object;)Ljava/lang/Object;"),
         handleArg(6, "sample/LambdaHost", "lambda$run$0",
                   "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;"),
         methodTypeArg("(Ljava/lang/Object;)Ljava/lang/Object;")});

    const ca::u16 bsmStaticRef = builder.addBootstrap(
        HandleSpec{6, "java/lang/invoke/LambdaMetafactory", "metafactory",
                   kMetafactoryDesc},
        {methodTypeArg("(Ljava/lang/Object;)Ljava/lang/Object;"),
         handleArg(6, "sample/Target", "impl", "(Ljava/lang/Object;)Ljava/lang/Object;"),
         methodTypeArg("(Ljava/lang/Object;)Ljava/lang/Object;")});

    const ca::u16 bsmVirtualRef = builder.addBootstrap(
        HandleSpec{6, "java/lang/invoke/LambdaMetafactory", "metafactory",
                   kMetafactoryDesc},
        {methodTypeArg("(Ljava/lang/Object;)Ljava/lang/Object;"),
         handleArg(5, "sample/Target", "virt", "(Ljava/lang/Object;)Ljava/lang/Object;"),
         methodTypeArg("(Ljava/lang/Object;)Ljava/lang/Object;")});

    const ca::u16 bsmAlt = builder.addBootstrap(
        HandleSpec{6, "java/lang/invoke/LambdaMetafactory", "altMetafactory",
                   kAltMetafactoryDesc},
        {methodTypeArg("(Ljava/lang/Object;)Ljava/lang/Object;"),
         handleArg(6, "sample/Target", "impl", "(Ljava/lang/Object;)Ljava/lang/Object;"),
         methodTypeArg("(Ljava/lang/Object;)Ljava/lang/Object;"),
         intArg(5),   // FLAG_SERIALIZABLE | FLAG_BRIDGES
         intArg(1),   // bridgeCount
         methodTypeArg("(Ljava/lang/String;)Ljava/lang/Object;")});

    const ca::u16 bsmUnknown = builder.addBootstrap(
        HandleSpec{6, "com/example/Mystery", "bootstrap", kUnknownBsmDesc}, {});

    const ca::u16 bsmConcat = builder.addBootstrap(
        HandleSpec{6, "java/lang/invoke/StringConcatFactory", "makeConcatWithConstants",
                   kUnknownBsmDesc},
        {stringArg("\u0001!")});

    const ca::u16 bsmRecord = builder.addBootstrap(
        HandleSpec{6, "java/lang/runtime/ObjectMethods", "bootstrap", kObjectMethodsDesc},
        {classArg("sample/Point"), stringArg("x;y"),
         handleArg(1, "sample/Point", "x", "I"),
         handleArg(1, "sample/Point", "y", "I")});

    builder.addMethod({"run",
                       "(Lsample/Ctx;)V",
                       0x0009,
                       {indy("apply", "(Lsample/Ctx;)Ljava/util/function/Function;",
                             bsmLambda),
                        simple(Op::POP), simple(Op::RETURN)}});
    builder.addMethod({"lambda$run$0",
                       "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;",
                       0x000A,   // private static
                       {simple(Op::ACONST_NULL), simple(Op::ARETURN)}});
    builder.addMethod({"ref",
                       "()Ljava/util/function/Function;",
                       0x0009,
                       {indy("apply", "()Ljava/util/function/Function;", bsmStaticRef),
                        simple(Op::ARETURN)}});
    builder.addMethod({"vref",
                       "(Lsample/Target;)Ljava/util/function/Function;",
                       0x0009,
                       {indy("apply", "(Lsample/Target;)Ljava/util/function/Function;",
                             bsmVirtualRef),
                        simple(Op::POP), simple(Op::RETURN)}});
    builder.addMethod({"altRef",
                       "()Ljava/util/function/Function;",
                       0x0009,
                       {indy("apply", "()Ljava/util/function/Function;", bsmAlt),
                        simple(Op::ARETURN)}});
    builder.addMethod({"mystery", "()V", 0x0009,
                       {indy("weird", "()V", bsmUnknown), simple(Op::RETURN)}});
    builder.addMethod({"concat",
                       "(Ljava/lang/String;)Ljava/lang/String;",
                       0x0009,
                       {indy("makeConcatWithConstants",
                             "(Ljava/lang/String;)Ljava/lang/String;", bsmConcat),
                        simple(Op::ARETURN)}});
    builder.addMethod({"recordToString",
                       "(Lsample/Point;)Ljava/lang/String;",
                       0x0009,
                       {indy("toString", "(Lsample/Point;)Ljava/lang/String;", bsmRecord),
                        simple(Op::ARETURN)}});
    builder.addMethod({"vhRead",
                       "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;)I",
                       0x0009,
                       {invokeVirtual("java/lang/invoke/VarHandle", "get",
                                      "(Ljava/lang/Object;)I"),
                        simple(Op::IRETURN)}});
    builder.addMethod({"vhWrite",
                       "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;I)V",
                       0x0009,
                       {invokeVirtual("java/lang/invoke/VarHandle", "set",
                                      "(Ljava/lang/Object;I)V"),
                        simple(Op::RETURN)}});
    builder.addMethod({"vhUpdate",
                       "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;II)Z",
                       0x0009,
                       {invokeVirtual("java/lang/invoke/VarHandle", "compareAndSet",
                                      "(Ljava/lang/Object;II)Z"),
                        simple(Op::IRETURN)}});
    builder.addMethod({"vhMeta",
                       "(Ljava/lang/invoke/VarHandle;"
                       "Ljava/lang/invoke/VarHandle$AccessMode;)"
                       "Ljava/lang/invoke/MethodType;",
                       0x0009,
                       {invokeVirtual("java/lang/invoke/VarHandle", "accessModeType",
                                      "(Ljava/lang/invoke/VarHandle$AccessMode;)"
                                      "Ljava/lang/invoke/MethodType;"),
                        simple(Op::ARETURN)}});
    return builder.assemble("sample/LambdaHost");
}

Bytes targetBytes()
{
    ClassBuilder builder("sample/Target");
    builder.addMethod({"impl",
                       "(Ljava/lang/Object;)Ljava/lang/Object;",
                       0x0009,
                       {simple(Op::ACONST_NULL), simple(Op::ARETURN)}});
    builder.addMethod({"virt",
                       "(Ljava/lang/Object;)Ljava/lang/Object;",
                       0x0001,   // public（实例方法）
                       {simple(Op::ACONST_NULL), simple(Op::ARETURN)}});
    return builder.assemble("sample/Target");
}

jbc::core::ByteView byteView(const Bytes& bytes)
{
    return jbc::core::ByteView{bytes.data(), bytes.size()};
}

bool hasMember(const std::vector<MemberKey>& keys, const mjbase::KeyPool& pool,
               std::string_view owner, std::string_view name, std::string_view desc)
{
    for (const MemberKey key : keys) {
        if (pool.str(key.cls) == owner && pool.str(key.name) == name &&
            pool.str(key.desc) == desc) {
            return true;
        }
    }
    return false;
}

IndyIndex fixtureIndex()
{
    const Bytes host   = lambdaHostBytes();
    const Bytes target = targetBytes();

    IndyIndexBuilder builder;
    EXPECT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    EXPECT_TRUE(builder.addClassBytes(byteView(target)).is_ok());
    auto built = builder.build();
    EXPECT_TRUE(built.is_ok());
    if (!built.is_ok()) {
        return IndyIndex{};
    }
    return std::move(built).unwrap();
}

}   // namespace

// ---------------------------------------------------------------------------
// BSM 分族与展开产物
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, IndyLambdaMetafactoryExposesTargetAndSignature)
{
    const IndyIndex index_ = fixtureIndex();
    const std::vector<const IndyResolution*> found =
        index_.resolutionsOf("sample/LambdaHost", "run", "(Lsample/Ctx;)V");
    ASSERT_EQ(found.size(), 1u);
    const IndyResolution& res = *found[0];

    EXPECT_EQ(res.family, BootstrapFamily::LambdaMetafactory);
    EXPECT_TRUE(res.known());
    EXPECT_TRUE(res.resolved());
    EXPECT_EQ(res.name, "apply");
    EXPECT_EQ(res.desc, "(Lsample/Ctx;)Ljava/util/function/Function;");
    EXPECT_EQ(res.functionalInterface, "java/util/function/Function");
    EXPECT_EQ(res.samMethodType, "(Ljava/lang/Object;)Ljava/lang/Object;");
    EXPECT_EQ(res.instantiatedMethodType, "(Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_EQ(res.capturedTypes.size(), 1u);
    EXPECT_EQ(res.capturedTypes[0], "Lsample/Ctx;");

    // BSM 自身句柄。
    EXPECT_EQ(res.bootstrap.kind, 6u);
    EXPECT_EQ(res.bootstrap.owner, "java/lang/invoke/LambdaMetafactory");
    EXPECT_EQ(res.bootstrap.name, "metafactory");

    ASSERT_EQ(res.targets.size(), 1u);
    const auto& target = res.targets[0];
    EXPECT_EQ(target.kind, 6u);
    EXPECT_EQ(target.dispatch(), HandleDispatch::Static);
    EXPECT_EQ(target.owner, "sample/LambdaHost");
    EXPECT_EQ(target.name, "lambda$run$0");
    EXPECT_EQ(target.desc, "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;");
}

TEST(MjAsmClasspathTest, IndyAltMetafactoryExtractsBridgesAndKeepsImplTarget)
{
    const IndyIndex index_ = fixtureIndex();
    const std::vector<const IndyResolution*> found =
        index_.resolutionsOf("sample/LambdaHost", "altRef",
                             "()Ljava/util/function/Function;");
    ASSERT_EQ(found.size(), 1u);
    const IndyResolution& res = *found[0];

    EXPECT_EQ(res.family, BootstrapFamily::LambdaMetafactory);
    ASSERT_EQ(res.targets.size(), 1u);
    EXPECT_EQ(res.targets[0].owner, "sample/Target");
    EXPECT_EQ(res.targets[0].name, "impl");
    ASSERT_EQ(res.bridgeMethodTypes.size(), 1u);
    EXPECT_EQ(res.bridgeMethodTypes[0], "(Ljava/lang/String;)Ljava/lang/Object;");
}

TEST(MjAsmClasspathTest, IndyKnownFamiliesWithoutCallTargetsStayKnown)
{
    const IndyIndex index_ = fixtureIndex();
    const auto concat = index_.resolutionsOf("sample/LambdaHost", "concat",
                                             "(Ljava/lang/String;)Ljava/lang/String;");
    ASSERT_EQ(concat.size(), 1u);
    EXPECT_EQ(concat[0]->family, BootstrapFamily::StringConcatFactory);
    EXPECT_TRUE(concat[0]->known());
    EXPECT_TRUE(concat[0]->targets.empty());
    ASSERT_EQ(concat[0]->capturedTypes.size(), 1u);
    EXPECT_EQ(concat[0]->capturedTypes[0], "Ljava/lang/String;");

    const auto record = index_.resolutionsOf(
        "sample/LambdaHost", "recordToString", "(Lsample/Point;)Ljava/lang/String;");
    ASSERT_EQ(record.size(), 1u);
    EXPECT_EQ(record[0]->family, BootstrapFamily::ObjectMethods);
    ASSERT_EQ(record[0]->targets.size(), 2u);
    EXPECT_EQ(record[0]->targets[0].dispatch(), HandleDispatch::Field);
    EXPECT_EQ(record[0]->targets[0].name, "x");
    EXPECT_EQ(record[0]->targets[1].name, "y");
}

TEST(MjAsmClasspathTest, IndyUnknownBootstrapFailsClosed)
{
    const IndyIndex index_ = fixtureIndex();
    const auto found =
        index_.resolutionsOf("sample/LambdaHost", "mystery", "()V");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0]->family, BootstrapFamily::Unknown);
    EXPECT_FALSE(found[0]->known());
    EXPECT_TRUE(found[0]->targets.empty());
    EXPECT_TRUE(index_.hasUnknownFamilies());
}

// ---------------------------------------------------------------------------
// VarHandle 访问模式
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, VarHandleAccessModesClassifyReadWrite)
{
    const IndyIndex index_ = fixtureIndex();
    const auto read =
        index_.varHandleAccessesOf("sample/LambdaHost", "vhRead",
                                   "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;)I");
    ASSERT_EQ(read.size(), 1u);
    EXPECT_EQ(read[0]->mode, "get");
    EXPECT_TRUE(read[0]->read);
    EXPECT_FALSE(read[0]->write);

    const auto write = index_.varHandleAccessesOf(
        "sample/LambdaHost", "vhWrite",
        "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;I)V");
    ASSERT_EQ(write.size(), 1u);
    EXPECT_EQ(write[0]->mode, "set");
    EXPECT_FALSE(write[0]->read);
    EXPECT_TRUE(write[0]->write);

    const auto update = index_.varHandleAccessesOf(
        "sample/LambdaHost", "vhUpdate",
        "(Ljava/lang/invoke/VarHandle;Ljava/lang/Object;II)Z");
    ASSERT_EQ(update.size(), 1u);
    EXPECT_EQ(update[0]->mode, "compareAndSet");
    EXPECT_TRUE(update[0]->read);
    EXPECT_TRUE(update[0]->write);

    // accessModeType 是普通方法，不是访问模式。
    EXPECT_TRUE(index_
                    .varHandleAccessesOf("sample/LambdaHost", "vhMeta",
                                         "(Ljava/lang/invoke/VarHandle;"
                                         "Ljava/lang/invoke/VarHandle$AccessMode;)"
                                         "Ljava/lang/invoke/MethodType;")
                    .empty());
}

// ---------------------------------------------------------------------------
// CallGraph 织入
// ---------------------------------------------------------------------------

TEST(MjAsmClasspathTest, CallGraphExpandsLambdaIndyToRealEdge)
{
    const Bytes host   = lambdaHostBytes();
    const Bytes target = targetBytes();
    CallGraphBuilder builder;
    ASSERT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    ASSERT_TRUE(builder.addClassBytes(byteView(target)).is_ok());
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    const MemberKey run =
        mjbase::memberKeyOf(graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"),
                            "run", "(Lsample/Ctx;)V");
    const MemberKey targetImpl =
        mjbase::memberKeyOf(graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/Target"),
                            "impl", "(Ljava/lang/Object;)Ljava/lang/Object;");
    const MemberKey leaf = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "lambda$run$0",
        "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;");

    const std::vector<CallSite> sites = graph.outgoingSitesOf(run);
    ASSERT_EQ(sites.size(), 2u);

    // 原始 indy 保守边在前，展开边在后，共用同一条 INVOKEDYNAMIC 指令。
    EXPECT_EQ(sites[0].kind, InvokeKind::Dynamic);
    EXPECT_FALSE(sites[0].indyExpanded);
    EXPECT_EQ(sites[0].indyFamily, BootstrapFamily::LambdaMetafactory);
    EXPECT_EQ(sites[1].callee, leaf);
    EXPECT_EQ(sites[1].kind, InvokeKind::Static);
    EXPECT_TRUE(sites[1].indyExpanded);
    EXPECT_EQ(sites[1].indyFamily, BootstrapFamily::LambdaMetafactory);
    EXPECT_EQ(sites[1].insn, sites[0].insn);

    EXPECT_TRUE(graph.isDeclared(leaf));
    EXPECT_TRUE(hasMember(graph.calleesOf(run), graph.pool(), "sample/LambdaHost",
                          "lambda$run$0",
                          "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;"));

    const std::vector<MemberKey> closure = graph.reachableClosure({run});
    EXPECT_TRUE(hasMember(closure, graph.pool(), "sample/LambdaHost", "lambda$run$0",
                          "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;"));

    // ref 与 altRef 的 BSM impl 都是 Target.impl，两处各展开一条边。
    EXPECT_EQ(graph.callSiteCountOf(targetImpl), 2u);
}

TEST(MjAsmClasspathTest, CallGraphFiltersIndyTargetsIndependently)
{
    const Bytes host = lambdaHostBytes();
    CallGraphBuilder builder(CallGraphOptions{/*includeExternalTargets=*/false});
    ASSERT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    const MemberKey run =
        mjbase::memberKeyOf(graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"),
                            "run", "(Lsample/Ctx;)V");
    const MemberKey leaf = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "lambda$run$0",
        "(Lsample/Ctx;Ljava/lang/Object;)Ljava/lang/Object;");

    // 保守伪 callee 未声明被丢；展开目标是本类声明方法，保留。
    const std::vector<CallSite> sites = graph.outgoingSitesOf(run);
    ASSERT_EQ(sites.size(), 1u);
    EXPECT_TRUE(sites[0].indyExpanded);
    EXPECT_EQ(sites[0].callee, leaf);

    // ref 的展开目标是外部类 Target.impl，与保守边一起丢弃。
    const MemberKey ref = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "ref",
        "()Ljava/util/function/Function;");
    EXPECT_TRUE(graph.outgoingSitesOf(ref).empty());
}

TEST(MjAsmClasspathTest, CallGraphMarksVirtualHandleAsVirtualDispatch)
{
    const Bytes host   = lambdaHostBytes();
    const Bytes target = targetBytes();
    CallGraphBuilder builder;
    ASSERT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    ASSERT_TRUE(builder.addClassBytes(byteView(target)).is_ok());
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    const MemberKey vref = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "vref",
        "(Lsample/Target;)Ljava/util/function/Function;");

    bool sawExpanded = false;
    for (const CallSite& site : graph.outgoingSitesOf(vref)) {
        if (!site.indyExpanded) {
            continue;
        }
        sawExpanded = true;
        EXPECT_EQ(site.kind, InvokeKind::Virtual);
        EXPECT_TRUE(site.isVirtualDispatch());
        EXPECT_EQ(graph.pool().str(site.callee.name), "virt");
    }
    EXPECT_TRUE(sawExpanded);
}

TEST(MjAsmClasspathTest, CallGraphKeepsUnknownBsmConservative)
{
    const Bytes host   = lambdaHostBytes();
    const Bytes target = targetBytes();
    CallGraphBuilder builder;
    ASSERT_TRUE(builder.addClassBytes(byteView(host)).is_ok());
    ASSERT_TRUE(builder.addClassBytes(byteView(target)).is_ok());
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    CallGraph graph = std::move(built).unwrap();

    const MemberKey mystery = mjbase::memberKeyOf(
        graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), "mystery", "()V");

    const std::vector<CallSite> sites = graph.outgoingSitesOf(mystery);
    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].kind, InvokeKind::Dynamic);
    EXPECT_FALSE(sites[0].indyExpanded);
    EXPECT_EQ(sites[0].indyFamily, BootstrapFamily::Unknown);

    // record getter 是字段句柄，不产调用边；字符串拼接族无目标。
    for (const char* name : {"recordToString", "concat"}) {
        const MemberKey key = mjbase::memberKeyOf(
            graph.pool(), mjbase::classKeyOf(graph.pool(), "sample/LambdaHost"), name,
            name == std::string_view("concat")
                ? "(Ljava/lang/String;)Ljava/lang/String;"
                : "(Lsample/Point;)Ljava/lang/String;");
        const std::vector<CallSite> methodSites = graph.outgoingSitesOf(key);
        ASSERT_EQ(methodSites.size(), 1u);
        EXPECT_EQ(methodSites[0].kind, InvokeKind::Dynamic);
        EXPECT_FALSE(methodSites[0].indyExpanded);
    }
}
