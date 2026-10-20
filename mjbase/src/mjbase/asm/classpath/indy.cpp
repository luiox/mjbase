/// @file        indy.cpp
/// @brief       indy / MethodHandle / VarHandle 载体解析实现。
#include "indy.hpp"

#include <string>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"

#include "libca/core/tag_cast.hpp"

namespace mjbase::asm_classpath {

namespace {

using jbc::core::ConstantPool;
using jbc::core::CpTag;
using jbc::ir::ClassSession;

constexpr std::string_view kLambdaOwner        = "java/lang/invoke/LambdaMetafactory";
constexpr std::string_view kStringConcatOwner  = "java/lang/invoke/StringConcatFactory";
constexpr std::string_view kObjectMethodsOwner = "java/lang/runtime/ObjectMethods";
constexpr std::string_view kConstantOwner      = "java/lang/invoke/ConstantBootstraps";
constexpr std::string_view kSwitchOwner        = "java/lang/runtime/SwitchBootstraps";
constexpr std::string_view kVarHandleOwner     = "java/lang/invoke/VarHandle";

/// altMetafactory flags（java.lang.invoke.LambdaMetafactory）。
constexpr ca::i32 kFlagMarkers  = 1 << 1;
constexpr ca::i32 kFlagBridges  = 1 << 2;

/// 类常量内部的字段类型起点；数组/对象/基本类型之外返回 false。
bool skipFieldType(std::string_view desc, ca::usize& i)
{
    while (i < desc.size() && desc[i] == '[') {
        ++i;
    }
    if (i >= desc.size()) {
        return false;
    }
    switch (desc[i]) {
    case 'B':
    case 'C':
    case 'D':
    case 'F':
    case 'I':
    case 'J':
    case 'S':
    case 'Z': ++i; return true;
    case 'L': {
        const ca::usize semi = desc.find(';', i);
        if (semi == std::string_view::npos) {
            return false;
        }
        i = semi + 1;
        return true;
    }
    default: return false;
    }
}

/// 方法描述符 → 参数类型切片 + 返回类型切片；畸形返回 false。
bool parseMethodDesc(std::string_view desc, std::vector<std::string_view>& params,
                     std::string_view& ret)
{
    params.clear();
    ret = {};
    if (desc.size() < 3 || desc.front() != '(') {
        return false;
    }
    ca::usize i = 1;
    while (i < desc.size() && desc[i] != ')') {
        const ca::usize start = i;
        if (!skipFieldType(desc, i)) {
            return false;
        }
        params.push_back(desc.substr(start, i - start));
    }
    if (i >= desc.size() || desc[i] != ')') {
        return false;
    }
    ++i;
    const ca::usize start = i;
    if (!skipFieldType(desc, i) || i != desc.size()) {
        return false;
    }
    ret = desc.substr(start);
    return true;
}

/// "Lx/Y;" → "x/Y"；非类类型返回空。
std::string_view classTypeName(std::string_view fieldType)
{
    if (fieldType.size() >= 3 && fieldType.front() == 'L' && fieldType.back() == ';') {
        return fieldType.substr(1, fieldType.size() - 2);
    }
    return {};
}

/// 读 CP MethodHandle 条目；结构不符返回 false。
bool readHandle(const ConstantPool& cp, ca::u32 index, HandleTarget& out)
{
    out = HandleTarget{};
    if (!cp.valid(index)) {
        return false;
    }
    const auto& handle = cp.at(index);
    if (handle.tag != CpTag::MethodHandle || !cp.valid(handle.b)) {
        return false;
    }
    const auto& ref = cp.at(handle.b);
    if (ref.tag != CpTag::Fieldref && ref.tag != CpTag::Methodref &&
        ref.tag != CpTag::InterfaceMethodref) {
        return false;
    }
    const auto& owner = cp.at(ref.a);
    const auto& nat   = cp.at(ref.b);
    if (owner.tag != CpTag::Class || nat.tag != CpTag::NameAndType) {
        return false;
    }
    out.kind        = static_cast<ca::u16>(handle.a);
    out.owner       = cp.utf8Of(owner.a);
    out.name        = cp.utf8Of(nat.a);
    out.desc        = cp.utf8Of(nat.b);
    out.isInterface = ref.tag == CpTag::InterfaceMethodref;
    return !out.owner.empty() && !out.name.empty() && !out.desc.empty() &&
           out.kind >= 1 && out.kind <= 9;
}

/// CP MethodType 条目的描述符；非该形态返回空。
std::string_view methodTypeOf(const ConstantPool& cp, ca::u32 index)
{
    if (!cp.valid(index)) {
        return {};
    }
    const auto& entry = cp.at(index);
    return entry.tag == CpTag::MethodType ? cp.utf8Of(entry.a) : std::string_view{};
}

bool intValueOf(const ConstantPool& cp, ca::u32 index, ca::i32& out)
{
    if (!cp.valid(index)) {
        return false;
    }
    const auto& entry = cp.at(index);
    if (entry.tag != CpTag::Integer) {
        return false;
    }
    out = entry.ival;
    return true;
}

const jbc::core::BootstrapMethodsAttr* bootstrapMethodsOf(ClassSession& session)
{
    jbc::core::ClassFile& cf = session.backing();
    jbc::core::AttrSlot*  slot = cf.findClassAttr("BootstrapMethods");
    if (slot == nullptr) {
        return nullptr;
    }
    auto decoded = cf.decodeAttr(*slot);
    if (decoded.is_err()) {
        return nullptr;
    }
    return ca::core::tag_cast::dyn_cast<jbc::core::BootstrapMethodsAttr>(decoded.unwrap());
}

void collectHandleArgs(const ConstantPool& cp, const std::vector<ca::u32>& args,
                       std::vector<HandleTarget>& out)
{
    for (const ca::u32 arg : args) {
        HandleTarget target;
        if (readHandle(cp, arg, target)) {
            out.push_back(target);
        }
    }
}

/// ConstantBootstraps.invoke 的静态参数首项是 MethodHandle。
void collectFirstHandleArg(const ConstantPool& cp, const std::vector<ca::u32>& args,
                           std::vector<HandleTarget>& out)
{
    for (const ca::u32 arg : args) {
        HandleTarget target;
        if (readHandle(cp, arg, target)) {
            out.push_back(target);
            return;
        }
    }
}

/// altMetafactory 的尾部 flags：[markers...][bridges...]。
void collectAltMetadata(const ConstantPool& cp, const std::vector<ca::u32>& args,
                        IndyResolution& out)
{
    ca::usize i = 3;   // args[0..2] = samMethodType / implMethod / instantiatedMethodType
    if (i >= args.size()) {
        return;
    }
    ca::i32 flags = 0;
    if (!intValueOf(cp, args[i++], flags)) {
        return;
    }
    if ((flags & kFlagMarkers) != 0) {
        ca::i32 count = 0;
        if (i >= args.size() || !intValueOf(cp, args[i++], count) || count < 0) {
            return;
        }
        if (static_cast<ca::usize>(count) > args.size() - i) {
            return;
        }
        i += static_cast<ca::usize>(count);   // 标记接口是 Class 常量，不是调用目标
    }
    if ((flags & kFlagBridges) != 0) {
        ca::i32 count = 0;
        if (i >= args.size() || !intValueOf(cp, args[i++], count) || count < 0) {
            return;
        }
        for (ca::i32 n = 0; n < count && i < args.size(); ++n, ++i) {
            const std::string_view bridge = methodTypeOf(cp, args[i]);
            if (!bridge.empty()) {
                out.bridgeMethodTypes.push_back(bridge);
            }
        }
    }
}

void expandLambda(const ConstantPool& cp, const jbc::core::BootstrapMethodsAttr::Entry& entry,
                  std::string_view bsmName, IndyResolution& out)
{
    std::string_view returnType;
    if (parseMethodDesc(out.desc, out.capturedTypes, returnType)) {
        out.functionalInterface = classTypeName(returnType);
    }
    else {
        out.capturedTypes.clear();
    }

    const std::vector<ca::u32>& args = entry.args;
    if (args.size() >= 1) {
        out.samMethodType = methodTypeOf(cp, args[0]);
    }
    if (args.size() >= 2) {
        HandleTarget impl;
        if (readHandle(cp, args[1], impl)) {
            out.targets.push_back(impl);
        }
    }
    if (args.size() >= 3) {
        out.instantiatedMethodType = methodTypeOf(cp, args[2]);
    }
    if (bsmName == "altMetafactory") {
        collectAltMetadata(cp, args, out);
    }
}

bool isKnownConstantBootstrap(std::string_view name) noexcept
{
    return name == "nullConstant" || name == "primitiveClass" || name == "enumConstant" ||
           name == "getStaticFinal" || name == "invoke" || name == "fieldVarHandle" ||
           name == "staticFieldVarHandle" || name == "arrayVarHandle" ||
           name == "explicitCast";
}

BootstrapFamily classifyBootstrap(std::string_view owner, std::string_view name) noexcept
{
    if (owner == kLambdaOwner) {
        return (name == "metafactory" || name == "altMetafactory")
                   ? BootstrapFamily::LambdaMetafactory
                   : BootstrapFamily::Unknown;
    }
    if (owner == kStringConcatOwner) {
        return (name == "makeConcat" || name == "makeConcatWithConstants")
                   ? BootstrapFamily::StringConcatFactory
                   : BootstrapFamily::Unknown;
    }
    if (owner == kObjectMethodsOwner && name == "bootstrap") {
        return BootstrapFamily::ObjectMethods;
    }
    if (owner == kSwitchOwner && (name == "typeSwitch" || name == "enumSwitch")) {
        return BootstrapFamily::SwitchBootstraps;
    }
    if (owner == kConstantOwner && isKnownConstantBootstrap(name)) {
        return BootstrapFamily::ConstantBootstraps;
    }
    return BootstrapFamily::Unknown;
}

struct VarHandleMode
{
    std::string_view name;
    bool             read;
    bool             write;
};

/// java.lang.invoke.VarHandle 的签名多态访问模式（JVMS 2.9.3 / JDK 17）。
constexpr VarHandleMode kVarHandleModes[] = {
    {"get", true, false},
    {"getVolatile", true, false},
    {"getOpaque", true, false},
    {"getAcquire", true, false},
    {"set", false, true},
    {"setVolatile", false, true},
    {"setOpaque", false, true},
    {"setRelease", false, true},
    {"compareAndSet", true, true},
    {"compareAndExchange", true, true},
    {"compareAndExchangeAcquire", true, true},
    {"compareAndExchangeRelease", true, true},
    {"weakCompareAndSetPlain", true, true},
    {"weakCompareAndSet", true, true},
    {"weakCompareAndSetAcquire", true, true},
    {"weakCompareAndSetRelease", true, true},
    {"getAndSet", true, true},
    {"getAndSetAcquire", true, true},
    {"getAndSetRelease", true, true},
    {"getAndAdd", true, true},
    {"getAndAddAcquire", true, true},
    {"getAndAddRelease", true, true},
    {"getAndBitwiseOr", true, true},
    {"getAndBitwiseOrAcquire", true, true},
    {"getAndBitwiseOrRelease", true, true},
    {"getAndBitwiseAnd", true, true},
    {"getAndBitwiseAndAcquire", true, true},
    {"getAndBitwiseAndRelease", true, true},
    {"getAndBitwiseXor", true, true},
    {"getAndBitwiseXorAcquire", true, true},
    {"getAndBitwiseXorRelease", true, true},
};

const VarHandleMode* findVarHandleMode(std::string_view name) noexcept
{
    for (const VarHandleMode& mode : kVarHandleModes) {
        if (mode.name == name) {
            return &mode;
        }
    }
    return nullptr;
}

}   // namespace

const char* bootstrapFamilyName(BootstrapFamily family) noexcept
{
    switch (family) {
    case BootstrapFamily::Unknown: return "Unknown";
    case BootstrapFamily::LambdaMetafactory: return "LambdaMetafactory";
    case BootstrapFamily::StringConcatFactory: return "StringConcatFactory";
    case BootstrapFamily::ObjectMethods: return "ObjectMethods";
    case BootstrapFamily::ConstantBootstraps: return "ConstantBootstraps";
    case BootstrapFamily::SwitchBootstraps: return "SwitchBootstraps";
    }
    return "Unknown";
}

HandleDispatch dispatchOfHandle(ca::u16 referenceKind) noexcept
{
    switch (referenceKind) {
    case 1:
    case 2:
    case 3:
    case 4: return HandleDispatch::Field;
    case 5: return HandleDispatch::Virtual;
    case 6: return HandleDispatch::Static;
    case 7: return HandleDispatch::Special;
    case 8: return HandleDispatch::Constructor;
    case 9: return HandleDispatch::Interface;
    default: return HandleDispatch::Unknown;
    }
}

bool resolveIndyInsn(ClassSession& session, const jbc::ir::Insn& insn, IndyResolution& out)
{
    out = IndyResolution{};
    if (insn.op != jbc::core::Op::INVOKEDYNAMIC) {
        return false;
    }

    const ConstantPool& cp = session.backing().cp;
    if (!cp.valid(insn.cp)) {
        return true;
    }
    const auto& indy = cp.at(insn.cp);
    if (indy.tag != CpTag::InvokeDynamic) {
        return true;
    }

    const auto& nat = cp.at(indy.b);
    if (nat.tag == CpTag::NameAndType) {
        out.name = cp.utf8Of(nat.a);
        out.desc = cp.utf8Of(nat.b);
    }
    std::string_view returnType;
    if (!out.desc.empty() && !parseMethodDesc(out.desc, out.capturedTypes, returnType)) {
        out.capturedTypes.clear();
    }

    const jbc::core::BootstrapMethodsAttr* table = bootstrapMethodsOf(session);
    if (table == nullptr || indy.a >= table->entries.size()) {
        return true;
    }
    const auto& entry = table->entries[indy.a];
    if (!readHandle(cp, entry.methodRef, out.bootstrap)) {
        return true;
    }

    out.family = classifyBootstrap(out.bootstrap.owner, out.bootstrap.name);
    switch (out.family) {
    case BootstrapFamily::LambdaMetafactory:
        expandLambda(cp, entry, out.bootstrap.name, out);
        break;
    case BootstrapFamily::ObjectMethods:
        collectHandleArgs(cp, entry.args, out.targets);
        break;
    case BootstrapFamily::ConstantBootstraps:
        if (out.bootstrap.name == "invoke") {
            collectFirstHandleArg(cp, entry.args, out.targets);
        }
        break;
    default: break;
    }
    return true;
}

bool classifyVarHandleAccess(const ClassSession& session, const jbc::ir::Insn& insn,
                             VarHandleAccess& out)
{
    out = VarHandleAccess{};
    if (insn.op != jbc::core::Op::INVOKEVIRTUAL) {
        return false;
    }
    ClassSession::MemberSem sem;
    if (!session.memberOf(insn, sem) || sem.owner != kVarHandleOwner) {
        return false;
    }
    const VarHandleMode* mode = findVarHandleMode(sem.name);
    if (mode == nullptr) {
        return false;
    }
    out.mode  = sem.name;
    out.read  = mode->read;
    out.write = mode->write;
    return true;
}

// ---------------------------------------------------------------------------
// IndyIndex
// ---------------------------------------------------------------------------

std::string_view IndyIndex::keep(std::string_view s)
{
    return pool_.str(pool_.intern(s));
}

HandleTarget IndyIndex::keep(const HandleTarget& target)
{
    HandleTarget out = target;
    out.owner        = keep(target.owner);
    out.name         = keep(target.name);
    out.desc         = keep(target.desc);
    return out;
}

void IndyIndex::append(IndyResolution resolution)
{
    resolution.site.sourceClass  = keep(resolution.site.sourceClass);
    resolution.site.sourceMethod = keep(resolution.site.sourceMethod);
    resolution.site.sourceDesc   = keep(resolution.site.sourceDesc);
    resolution.name              = keep(resolution.name);
    resolution.desc              = keep(resolution.desc);
    resolution.functionalInterface    = keep(resolution.functionalInterface);
    resolution.samMethodType          = keep(resolution.samMethodType);
    resolution.instantiatedMethodType = keep(resolution.instantiatedMethodType);
    resolution.bootstrap              = keep(resolution.bootstrap);
    for (std::string_view& captured : resolution.capturedTypes) {
        captured = keep(captured);
    }
    for (std::string_view& bridge : resolution.bridgeMethodTypes) {
        bridge = keep(bridge);
    }
    for (HandleTarget& target : resolution.targets) {
        target = keep(target);
    }

    if (!resolution.known()) {
        unknownFamilies_ = true;
    }

    const ca::u32 index = static_cast<ca::u32>(resolutions_.size());
    resolutions_.push_back(std::move(resolution));
    const IndySite& site = resolutions_.back().site;
    const MemberKey method =
        memberKeyOf(pool_, classKeyOf(pool_, site.sourceClass), site.sourceMethod,
                    site.sourceDesc);
    resolutionsByMethod_[method].push_back(index);
}

void IndyIndex::append(VarHandleAccess access)
{
    access.site.sourceClass  = keep(access.site.sourceClass);
    access.site.sourceMethod = keep(access.site.sourceMethod);
    access.site.sourceDesc   = keep(access.site.sourceDesc);
    access.mode              = keep(access.mode);

    const ca::u32 index = static_cast<ca::u32>(varHandleAccesses_.size());
    varHandleAccesses_.push_back(std::move(access));
    const IndySite& site = varHandleAccesses_.back().site;
    const MemberKey method =
        memberKeyOf(pool_, classKeyOf(pool_, site.sourceClass), site.sourceMethod,
                    site.sourceDesc);
    varHandleAccessesByMethod_[method].push_back(index);
}

std::vector<const IndyResolution*> IndyIndex::resolutionsOf(MemberKey method) const
{
    std::vector<const IndyResolution*> out;
    const auto it = resolutionsByMethod_.find(method);
    if (it == resolutionsByMethod_.end()) {
        return out;
    }
    out.reserve(it->second.size());
    for (const ca::u32 index : it->second) {
        out.push_back(&resolutions_[index]);
    }
    return out;
}

std::vector<const IndyResolution*> IndyIndex::resolutionsOf(std::string_view owner,
                                                            std::string_view name,
                                                            std::string_view desc) const
{
    return resolutionsOf(memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc));
}

std::vector<const VarHandleAccess*> IndyIndex::varHandleAccessesOf(MemberKey method) const
{
    std::vector<const VarHandleAccess*> out;
    const auto it = varHandleAccessesByMethod_.find(method);
    if (it == varHandleAccessesByMethod_.end()) {
        return out;
    }
    out.reserve(it->second.size());
    for (const ca::u32 index : it->second) {
        out.push_back(&varHandleAccesses_[index]);
    }
    return out;
}

std::vector<const VarHandleAccess*> IndyIndex::varHandleAccessesOf(
    std::string_view owner, std::string_view name, std::string_view desc) const
{
    return varHandleAccessesOf(memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc));
}

// ---------------------------------------------------------------------------
// IndyIndexBuilder
// ---------------------------------------------------------------------------

ca::core::Result<void, jbc::ir::SessionError> IndyIndexBuilder::addClassBytes(
    jbc::core::ByteView bytes)
{
    auto session = jbc::ir::makeSessionFromBytes(bytes);
    if (session.is_err()) {
        return ca::core::Err(session.unwrap_err());
    }
    sessions_.push_back(std::move(session).unwrap());
    return ca::core::Ok();
}

void IndyIndexBuilder::addSession(std::shared_ptr<jbc::ir::ClassSession> session)
{
    if (session == nullptr) {
        return;
    }
    for (const auto& existing : sessions_) {
        if (existing.get() == session.get()) {
            return;
        }
    }
    sessions_.push_back(std::move(session));
}

ca::core::Result<IndyIndex, IndyBuildError> IndyIndexBuilder::build()
{
    IndyIndex index;
    for (const auto& sessionPtr : sessions_) {
        ClassSession& session = *sessionPtr;
        const jbc::ir::ClassUnit& unit = session.unit();
        const jbc::ir::SymbolPool& syms = unit.symbols();
        const std::string_view className = syms.str(unit.thisClass);

        for (const jbc::ir::CodeVisit& visit : jbc::ir::collectCodeUnits(session)) {
            const jbc::ir::MethodUnit& method = unit.methods[visit.methodIdx];
            const std::string_view methodName = syms.str(method.name);
            const std::string_view methodDesc = syms.str(method.desc);

            if (visit.kind == jbc::ir::CodeVisit::Kind::Failed) {
                return ca::core::Err(
                    IndyBuildError{visit.error, className, methodName, methodDesc});
            }
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) {
                continue;
            }

            const jbc::ir::CodeDom& dom = visit.unit->dom;
            ca::i32 position = 0;
            for (const jbc::ir::InsnId id : dom.order()) {
                const jbc::ir::Insn* insn = dom.peek(id);
                if (insn != nullptr) {
                    IndyResolution resolution;
                    if (resolveIndyInsn(session, *insn, resolution)) {
                        resolution.site =
                            IndySite{className, methodName, methodDesc, position};
                        index.append(std::move(resolution));
                    }
                    else {
                        VarHandleAccess access;
                        if (classifyVarHandleAccess(session, *insn, access)) {
                            access.site =
                                IndySite{className, methodName, methodDesc, position};
                            index.append(std::move(access));
                        }
                    }
                }
                ++position;
            }
        }
    }
    return ca::core::Ok(std::move(index));
}

}   // namespace mjbase::asm_classpath
