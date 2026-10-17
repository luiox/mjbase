/// @file        member_remap_plan_test.cpp
/// @brief       asm/remapper 模块测试：计划/类改写器/资源改写器/映射门面。
///
/// 旧栈对应物：mbase/unittest/asm/remapper/member_remap_plan_test.cpp（21 用例
/// 全量移植，语义为 oracle）；夹具从 ASM ClassNode 手拼改为
/// class_bytes_builder.hpp 的受控字节装配 + libjbc 会话往返，断言面从内存树
/// 改为 writeClass → readClass 的产物字节（更贴近旧栈 ClassWriter 输出的
/// 可观察行为）。另补 MappingRemapper（utf8_remapper 去 visitor 重设计）用例。
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/analysis/alias_analysis.hpp"
#include "mjbase/asm/remapper/mapping_remapper.hpp"
#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/jar/manifest.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "class_bytes_builder.hpp"

namespace {

namespace test = mjbase::asm_remapper::test_support;
using test::ClassBytesBuilder;

using jbc::core::ClassFile;
using jbc::core::MemberInfo;
using Bytes = std::vector<ca::u8>;

using mjbase::asm_remapper::FieldRemapRule;
using mjbase::asm_remapper::MappingRemapper;
using mjbase::asm_remapper::MemberRemapPlan;
using mjbase::asm_remapper::MethodRemapRule;
using mjbase::asm_remapper::RemapPolicy;
using mjbase::asm_remapper::RemapError;

// ---------------------------------------------------------------------------
// 样板助手：会话开字节 / 写回 / 重解析 / CP 读取 / 指令定位。
// ---------------------------------------------------------------------------

/// @brief 字节 → 会话；失败即本用例失败（返回空指针，用例自行 ASSERT）。
std::shared_ptr<jbc::ir::ClassSession> openSession(const Bytes& bytes)
{
    auto r = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (r.is_err()) {
        ADD_FAILURE() << "makeSessionFromBytes failed";
        return nullptr;
    }
    return std::move(r).unwrap();
}

/// @brief 字节 → 独立 ClassFile（测试断言只读解析产物）。
std::unique_ptr<ClassFile> parseClass(const Bytes& bytes)
{
    auto r = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (r.is_err()) {
        ADD_FAILURE() << "readClass failed";
        return nullptr;
    }
    return std::move(r).unwrap().file;
}

/// @brief 会话 backing 写回字节（失败即用例失败，返回空字节）。
Bytes writeClassOf(jbc::ir::ClassSession& session)
{
    auto r = jbc::core::writeClass(session.backing());
    if (r.is_err()) {
        ADD_FAILURE() << "writeClass failed";
        return {};
    }
    return std::move(r).unwrap();
}

/// @brief 应用计划并把产物字节重解析成只读 ClassFile；任一步失败返回空。
std::unique_ptr<ClassFile> applyAndReparse(jbc::ir::ClassSession&                     session,
                                           const mjbase::asm_remapper::MemberRemapPlan& plan,
                                           const RemapPolicy&                           policy)
{
    auto applied = mjbase::asm_remapper::applyMemberRemapPlan(session, plan, policy);
    if (applied.is_err()) {
        ADD_FAILURE() << "applyMemberRemapPlan err";
        return nullptr;
    }
    Bytes out = writeClassOf(session);
    if (out.empty()) return nullptr;
    return parseClass(out);
}

std::string utf8TextOf(const ClassFile& cf, ca::u32 index)
{
    return std::string(cf.cp.utf8Of(index));
}

/// @brief 字面文本 → 字节（资源面夹具用）。
Bytes toBytes(const char* text)
{
    return Bytes(text, text + std::strlen(text));
}

/// @brief CP Class 条目 → 内部名；非 Class/越界返回空串。
std::string classTextOf(const ClassFile& cf, ca::u32 index)
{
    if (!cf.cp.valid(index)) return {};
    const auto& entry = cf.cp.at(index);
    if (entry.tag != jbc::core::CpTag::Class) return {};
    return utf8TextOf(cf, entry.a);
}

/// @brief 成员引用三段（owner/name/desc）；非引用条目 ok=false。
struct MemberRefText
{
    std::string owner;
    std::string name;
    std::string desc;
    bool        ok = false;
};

MemberRefText memberRefOf(const ClassFile& cf, ca::u32 index)
{
    MemberRefText text;
    if (!cf.cp.valid(index)) return text;
    const auto& entry = cf.cp.at(index);
    if (entry.tag != jbc::core::CpTag::Fieldref && entry.tag != jbc::core::CpTag::Methodref &&
        entry.tag != jbc::core::CpTag::InterfaceMethodref) {
        return text;
    }
    if (!cf.cp.valid(entry.b)) return text;
    const auto& nat = cf.cp.at(entry.b);
    text.owner      = classTextOf(cf, entry.a);
    text.name       = utf8TextOf(cf, nat.a);
    text.desc       = utf8TextOf(cf, nat.b);
    text.ok         = true;
    return text;
}

MemberInfo* methodOf(ClassFile& cf, std::string_view name)
{
    for (MemberInfo& m : cf.methods) {
        if (cf.cp.utf8Of(m.nameCp) == name) return &m;
    }
    return nullptr;
}

MemberInfo* fieldOf(ClassFile& cf, std::string_view name)
{
    for (MemberInfo& m : cf.fields) {
        if (cf.cp.utf8Of(m.nameCp) == name) return &m;
    }
    return nullptr;
}

/// @brief 属性槽按名字定位（比较名字字节；fresh 解析的 cf 直接可变）。
const jbc::core::AttrSlot* slotOf(const ClassFile&                        cf,
                                  const std::vector<jbc::core::AttrSlot>& attrs,
                                  std::string_view                        name)
{
    for (const jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// @brief 解码注解属性槽（RuntimeVisible/InvisibleAnnotations）。
const jbc::core::AnnotationsAttr* annotationsOf(ClassFile& cf, const jbc::core::AttrSlot& slot)
{
    if (!slot.decoded()) {
        if (cf.decodeAttr(const_cast<jbc::core::AttrSlot&>(slot)).is_err()) return nullptr;
    }
    return ca::core::tag_cast::cast<const jbc::core::AnnotationsAttr>(slot.peek());
}

/// @brief 注解列表首个注解的类型描述符；无注解返回空。
std::string annotationTypeText(ClassFile& cf, const jbc::core::AttrSlot& slot)
{
    const jbc::core::AnnotationsAttr* body = annotationsOf(cf, slot);
    if (body == nullptr || body->annos.empty()) return {};
    return utf8TextOf(cf, body->annos[0].typeCp);
}

/// @brief 注解首元素的 'c' 载荷文本；无注解/无元素返回空。
std::string annotationElementText(ClassFile& cf, const jbc::core::AttrSlot& slot)
{
    const jbc::core::AnnotationsAttr* body = annotationsOf(cf, slot);
    if (body == nullptr || body->annos.empty() || body->annos[0].values.empty()) return {};
    return utf8TextOf(cf, body->annos[0].values[0].second.cp);
}

/// @brief 解码属性槽并按 tag_cast 取 typed SignatureAttr；不匹配返回空。
const jbc::core::SignatureAttr* signatureAttrOf(ClassFile&         cf,
                                                const jbc::core::AttrSlot& slot)
{
    if (!slot.decoded()) {
        if (cf.decodeAttr(const_cast<jbc::core::AttrSlot&>(slot)).is_err()) return nullptr;
    }
    return ca::core::tag_cast::cast<const jbc::core::SignatureAttr>(slot.peek());
}

/// @brief Signature 属性文本（typed SignatureAttr）；无该属性返回空。
std::string signatureOf(ClassFile& cf, std::vector<jbc::core::AttrSlot>& attrs)
{
    const jbc::core::AttrSlot* slot = slotOf(cf, attrs, "Signature");
    if (slot == nullptr) return {};
    const jbc::core::SignatureAttr* attr = signatureAttrOf(cf, *slot);
    if (attr == nullptr) return {};
    return utf8TextOf(cf, attr->signatureCp);
}

/// @brief Exceptions 属性的异常类内部名列表；无该属性返回空。
///
/// typed ExceptionsAttr 体经公开解码入口读出（B3 分派接线前后同形，不依赖
/// decodeAttr 的分派形态）。
std::vector<std::string> exceptionsOf(ClassFile& cf, MemberInfo& member)
{
    std::vector<std::string>          names;
    const jbc::core::AttrSlot* slot = slotOf(cf, member.attrs, "Exceptions");
    if (slot == nullptr) return names;
    auto d = decodeExceptionsBody(cf, *slot);
    if (d.is_err()) return names;
    const auto body = std::move(d).unwrap();
    const auto& exc = static_cast<const jbc::core::ExceptionsAttr&>(*body);
    names.reserve(exc.exceptionCps.size());
    for (const ca::u32 classCp : exc.exceptionCps) {
        names.push_back(classTextOf(cf, classCp));
    }
    return names;
}

/// @brief 解码 Code 属性（含嵌套属性一并解码）；无 Code 返回空。
const jbc::core::CodeAttr* codeOf(ClassFile& cf, MemberInfo& member)
{
    jbc::core::AttrSlot* slot =
        const_cast<jbc::core::AttrSlot*>(slotOf(cf, member.attrs, "Code"));
    if (slot == nullptr) return nullptr;
    if (!slot->decoded()) {
        if (cf.decodeAttr(*slot).is_err()) return nullptr;
    }
    const auto* code = ca::core::tag_cast::cast<const jbc::core::CodeAttr>(slot->peek());
    if (code == nullptr) return nullptr;
    for (jbc::core::AttrSlot& nested : const_cast<jbc::core::CodeAttr*>(code)->attrs) {
        if (!nested.decoded()) {
            (void)cf.decodeAttr(nested);
        }
    }
    return code;
}

/// @brief 方法体内 LVT/LVTT 行（二者合并列出；行含属性名归属）。
struct LvtRowText
{
    std::string attrName;
    std::string name;
    std::string desc;
    ca::u32     slot = 0;
};

std::vector<LvtRowText> localVariablesOf(ClassFile& cf, const jbc::core::CodeAttr& code)
{
    std::vector<LvtRowText> rows;
    for (const jbc::core::AttrSlot& slot : code.attrs) {
        const std::string_view attrName = cf.cp.utf8Of(slot.nameCp);
        if (attrName != "LocalVariableTable" && attrName != "LocalVariableTypeTable") continue;
        const auto* body = ca::core::tag_cast::cast<const jbc::core::LocalVariableTableAttr>(slot.peek());
        if (body == nullptr) continue;
        for (const jbc::core::LocalVariableTableAttr::Row& row : body->rows) {
            rows.push_back(LvtRowText{std::string(attrName), utf8TextOf(cf, row.nameCp),
                                      utf8TextOf(cf, row.descCp), row.slot});
        }
    }
    return rows;
}

/// @brief 提升指定方法体（按名定位），返回 CodeUnit；无方法/失败返回空。
const jbc::ir::CodeUnit* promoteCodeOf(jbc::ir::ClassSession& session, std::string_view name)
{
    auto& unit = session.unit();
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (unit.symbols().str(unit.methods[i].name) != name) continue;
        auto r = jbc::ir::promoteMethod(session, i);
        if (r.is_err()) {
            ADD_FAILURE() << "promoteMethod failed for " << name;
            return nullptr;
        }
        return &*std::move(r).unwrap();
    }
    ADD_FAILURE() << "method not found: " << name;
    return nullptr;
}

const jbc::ir::Insn* insnAt(jbc::ir::ClassSession& session, std::string_view method, ca::usize pos)
{
    const jbc::ir::CodeUnit* code = promoteCodeOf(session, method);
    if (code == nullptr) return nullptr;
    const std::vector<jbc::ir::InsnId>& order = code->dom.order();
    if (pos >= order.size()) {
        ADD_FAILURE() << "insn index out of range: " << pos;
        return nullptr;
    }
    return code->dom.peek(order[pos]);
}

/// @brief 方法体内 LDC 字符串常量全量收集（反射面断言用「改名后不再出现」）。
std::vector<std::string> ldcStringsOf(jbc::ir::ClassSession& session, std::string_view method)
{
    std::vector<std::string>      strings;
    const jbc::ir::CodeUnit* code = promoteCodeOf(session, method);
    if (code == nullptr) return strings;
    for (const jbc::ir::InsnId id : code->dom.order()) {
        const jbc::ir::Insn* insn = code->dom.peek(id);
        if (insn == nullptr || insn->op != jbc::core::Op::LDC) continue;
        if (!session.backing().cp.valid(insn->cp)) continue;
        const auto& entry = session.backing().cp.at(insn->cp);
        if (entry.tag == jbc::core::CpTag::String) {
            strings.push_back(utf8TextOf(session.backing(), entry.a));
        }
    }
    return strings;
}

bool contains(const std::vector<std::string>& values, std::string_view value)
{
    for (const std::string& v : values) {
        if (v == value) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 共享夹具装配器（与旧栈 fixture 一一对应）。
// ---------------------------------------------------------------------------

/// @brief 旧栈 buildRemapClass：OldOwner 族 + 反射字符串 + 注解/签名三面。
///        字段 oldField、方法 oldMethod/use；use 体覆盖 invoke/getfield/
///        NEW/ANEWARRAY/MULTIANEWARRAY/ldc 字符串。
Bytes buildRemapClass()
{
    ClassBytesBuilder b("OldOwner");
    b.setSuper("OldIface");
    b.setInterfaces({"OldMarker"});
    b.setClassSignature("Ljava/lang/Object;LOldGeneric;");
    test::AnnotationSpec anno;
    anno.type = "LOldAnno;";
    b.addClassAnnotation(anno);

    test::FieldSpec field;
    field.access    = 0x0001;   // public
    field.name      = "oldField";
    field.desc      = "LOldType;";
    field.signature = "LOldGeneric;";
    b.addField(field);

    test::MethodSpec oldMethod;
    oldMethod.access    = 0x0009;   // public static
    oldMethod.name      = "oldMethod";
    oldMethod.desc      = "(LOldType;)LOldType;";
    oldMethod.signature = "LOldGeneric;";
    oldMethod.emit      = [](test::CodeEmit& e) {
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.op(jbc::core::Op::ARETURN);
    };

    test::MethodSpec use;
    use.access    = 0x0009;
    use.name      = "use";
    use.desc      = "(LOldType;)LOldType;";
    use.signature = "()LOldMethodGeneric;";
    use.maxStack  = 3;
    use.maxLocals = 1;
    use.emit      = [](test::CodeEmit& e) {
        e.ldcstring("pkg.OldReflect");
        e.invoke(jbc::core::Op::INVOKESTATIC, "java/lang/Class", "forName",
                 "(Ljava/lang/String;)Ljava/lang/Class;");
        e.op(jbc::core::Op::POP);
        e.ldcstring("pkg.OldNoise");
        e.op(jbc::core::Op::POP);
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.invoke(jbc::core::Op::INVOKESTATIC, "OldOwner", "oldMethod",
                 "(LOldType;)LOldType;");
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.fieldInsn(jbc::core::Op::GETFIELD, "OldOwner", "oldField", "LOldType;");
        e.op(jbc::core::Op::POP);
        e.typeInsn(jbc::core::Op::NEW, "OldType");
        e.op(jbc::core::Op::POP);
        e.i32const(1);
        e.typeInsn(jbc::core::Op::ANEWARRAY, "OldType");
        e.op(jbc::core::Op::POP);
        e.i32const(1);
        e.i32const(1);
        e.multiANewArray("[[LOldType;", 2);
        e.op(jbc::core::Op::POP);
        e.op(jbc::core::Op::ARETURN);
    };
    return b.assemble({oldMethod, use});
}

/// @brief T4 夹具：类常量/方法句柄/invokedynamic/condy/SMT 全帧。
Bytes buildTypeHandleClass()
{
    ClassBytesBuilder b("pkg/Caller");
    const ca::u16 bsmMeta  = b.addBootstrapMethod(
        "java/lang/invoke/LambdaMetafactory", "metafactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
        "MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/"
        "invoke/MethodType;)Ljava/lang/invoke/CallSite;",
        {test::BsmArg{'M', "()V"}, test::BsmArg{'H', "", 6, "pkg/OldOwner", "oldMethod", "()V"}});
    const ca::u16 bsmRaw = b.addBootstrapMethod(
        "pkg/Bootstrap", "bootstrap",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
        "MethodType;)Ljava/lang/invoke/CallSite;",
        {test::BsmArg{'M', "()V"}});
    const ca::u16 bsmConst = b.addBootstrapMethod(
        "pkg/Bootstrap", "bootstrap",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
        "MethodType;)Ljava/lang/Object;",
        {test::BsmArg{'C', "", 6, "pkg/OldType", "", ""}});

    test::MethodSpec use;
    use.access    = 0x0009;
    use.name      = "use";
    use.desc      = "()V";
    use.maxStack  = 4;
    use.maxLocals = 0;
    use.emit      = [&](test::CodeEmit& e) {
        e.ldcClass("pkg/OldType");
        e.ldcMethodHandle(6, "pkg/OldOwner", "oldMethod", "()V");
        e.invokedynamic(bsmMeta, "run", "()Lpkg/OldIface;");
        e.invokedynamic(bsmRaw, "run", "()Lpkg/OldIface;");
        e.ldcDynamic(bsmConst, "constant", "Lpkg/OldType;");
        e.op(jbc::core::Op::RETURN);
    };
    use.stackMapFrames.push_back(
        test::FullFrameSpec{{{test::VTypeSpec{"pkg/OldType"}}, {test::VTypeSpec{"[Lpkg/OldType;"}}}, {}});
    return b.assemble({use});
}

/// @brief T5 夹具：模块属性 + Exceptions + try/catch + LVT/LVTT。
Bytes buildModuleAndDebugClass()
{
    ClassBytesBuilder b("pkg/OldMain");
    b.setModule("app.module", "pkg/OldMain", {"pkg/OldService"},
                {{"pkg/OldService", {"pkg/OldProvider"}}});

    test::MethodSpec debug;
    debug.access     = 0x0009;
    debug.name       = "debug";
    debug.desc       = "()V";
    debug.exceptions = {"pkg/OldException"};
    debug.maxStack   = 1;
    debug.maxLocals  = 1;
    const std::size_t l0 = 0;
    const std::size_t l1 = 1;
    const std::size_t l2 = 2;
    debug.emit           = [l0, l1, l2](test::CodeEmit& e) {
        e.bindLabel(l0);
        e.i32const(0);
        e.varInsn(jbc::core::Op::ISTORE, 0);
        e.bindLabel(l1);
        e.op(jbc::core::Op::RETURN);
        e.bindLabel(l2);
        e.op(jbc::core::Op::RETURN);
    };
    debug.excTable.push_back(test::ExcSpec{l0, l1, l2, "pkg/OldException"});
    debug.lineNumbers.push_back(test::LineSpec{123, l0});
    debug.localVariables.push_back(
        test::LvtSpec{"value", "Lpkg/OldType;", "Ljava/util/List<Lpkg/OldType;>;", l0, l1, 0});
    return b.assemble({debug});
}

/// @brief T6 夹具：EnclosingMethod/Nest/Permitted/InnerClasses/Record 全家桶。
Bytes buildStructuralClass()
{
    ClassBytesBuilder b("OldOwner");
    b.setSuper("OldIface");
    b.setEnclosingMethod("OldOuter", "oldOuter", "(LOldType;)LOldReturn;");
    b.setNestHost("OldNestHost");
    b.addNestMember("OldNestMember");
    b.addPermittedSubclass("OldPermitted");
    b.addInnerClass("OldOuter$OldInner", "OldOuter", "OldInner", 0x0001);

    test::RecordComponentSpec component;
    component.name       = "oldComponent";
    component.desc       = "LOldType;";
    component.signature  = "LOldGeneric;";
    test::AnnotationSpec compAnno;
    compAnno.type   = "LOldAnno;";
    compAnno.pairs  = {};
    component.annotations = {compAnno};
    b.addRecordComponent(component);

    test::MethodSpec nothing;
    nothing.access   = 0x0009;
    nothing.name     = "noop";
    nothing.desc     = "()V";
    nothing.emit     = [](test::CodeEmit& e) { e.op(jbc::core::Op::RETURN); };
    return b.assemble({nothing});
}

/// @brief T7 夹具：owner+name 无描述符字段规则的目标类。
Bytes buildDescriptorlessClass()
{
    ClassBytesBuilder b("OldOwner");
    test::FieldSpec field;
    field.access = 0x0001;
    field.name   = "oldField";
    field.desc   = "LOldType;";
    b.addField(field);

    test::MethodSpec use;
    use.access    = 0x0009;
    use.name      = "use";
    use.desc      = "()V";
    use.maxStack  = 1;
    use.maxLocals = 0;
    use.emit      = [](test::CodeEmit& e) {
        e.i32const(0);
        e.fieldInsn(jbc::core::Op::GETFIELD, "OldOwner", "oldField", "LOldType;");
        e.op(jbc::core::Op::POP);
        e.op(jbc::core::Op::RETURN);
    };
    return b.assemble({use});
}

/// @brief T9/T10 夹具：常量反射成员查找（getDeclaredMethod/getMethod/
///        getDeclaredField + forName）。
Bytes buildReflectionMemberClass()
{
    ClassBytesBuilder b("pkg/OldOwner");

    test::FieldSpec field;
    field.access = 0x0009;
    field.name   = "oldField";
    field.desc   = "I";
    b.addField(field);

    test::MethodSpec oldMethod;
    oldMethod.access = 0x0009;
    oldMethod.name   = "oldMethod";
    oldMethod.desc   = "()V";
    oldMethod.emit   = [](test::CodeEmit& e) { e.op(jbc::core::Op::RETURN); };

    test::MethodSpec oldStringMethod;
    oldStringMethod.access = 0x0009;
    oldStringMethod.name   = "oldStringMethod";
    oldStringMethod.desc   = "(Ljava/lang/String;)V";
    oldStringMethod.emit   = [](test::CodeEmit& e) { e.op(jbc::core::Op::RETURN); };

    test::MethodSpec oldForNameStringMethod;
    oldForNameStringMethod.access = 0x0009;
    oldForNameStringMethod.name   = "oldForNameStringMethod";
    oldForNameStringMethod.desc   = "(Ljava/lang/String;)V";
    oldForNameStringMethod.emit   = [](test::CodeEmit& e) { e.op(jbc::core::Op::RETURN); };

    test::MethodSpec use;
    use.access    = 0x0009;
    use.name      = "use";
    use.desc      = "()V";
    use.maxStack  = 6;
    use.maxLocals = 0;
    use.emit      = [](test::CodeEmit& e) {
        e.ldcstring("noise");
        e.op(jbc::core::Op::POP);
        e.ldcClass("pkg/OldOwner");
        e.ldcstring("oldMethod");
        e.op(jbc::core::Op::ACONST_NULL);
        e.invoke(jbc::core::Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
        e.op(jbc::core::Op::POP);
        e.ldcClass("pkg/OldOwner");
        e.ldcstring("oldStringMethod");
        e.i32const(1);
        e.typeInsn(jbc::core::Op::ANEWARRAY, "java/lang/Class");
        e.op(jbc::core::Op::DUP);
        e.i32const(0);
        e.ldcClass("java/lang/String");
        e.op(jbc::core::Op::AASTORE);
        e.invoke(jbc::core::Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
        e.op(jbc::core::Op::POP);
        e.ldcstring("pkg.OldOwner");
        e.invoke(jbc::core::Op::INVOKESTATIC, "java/lang/Class", "forName",
                 "(Ljava/lang/String;)Ljava/lang/Class;");
        e.op(jbc::core::Op::POP);
        e.ldcstring("oldForNameStringMethod");
        e.i32const(1);
        e.typeInsn(jbc::core::Op::ANEWARRAY, "java/lang/Class");
        e.op(jbc::core::Op::DUP);
        e.i32const(0);
        e.ldcClass("java/lang/String");
        e.op(jbc::core::Op::AASTORE);
        e.invoke(jbc::core::Op::INVOKEVIRTUAL, "java/lang/Class", "getMethod",
                 "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
        e.op(jbc::core::Op::POP);
        e.ldcstring("pkg.OldOwner");
        e.invoke(jbc::core::Op::INVOKESTATIC, "java/lang/Class", "forName",
                 "(Ljava/lang/String;)Ljava/lang/Class;");
        e.op(jbc::core::Op::POP);
        e.ldcstring("oldField");
        e.invoke(jbc::core::Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredField",
                 "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
        e.op(jbc::core::Op::POP);
        e.op(jbc::core::Op::RETURN);
    };
    return b.assemble({oldMethod, oldStringMethod, oldForNameStringMethod, use});
}

/// @brief T11 夹具：类注解携带 'c'（class literal）元素。
Bytes buildAnnotationClass()
{
    ClassBytesBuilder b("pkg/Holder");
    test::AnnotationSpec anno;
    anno.type = "Lpkg/Anno;";
    anno.pairs.push_back(test::AnnotationPair{"value", test::ElementValue{'c', "Lpkg/OldClass;"}});
    b.addClassAnnotation(anno);

    test::MethodSpec use;
    use.access = 0x0009;
    use.name   = "use";
    use.desc   = "()V";
    use.emit   = [](test::CodeEmit& e) { e.op(jbc::core::Op::RETURN); };
    return b.assemble({use});
}

/// @brief T13 夹具：反射接收者别名形态全家桶（identity/静态槽/容器/字段/
///        消费队列），各 use 方法各覆盖一条别名来源。
Bytes buildAliasReflectionClass()
{
    ClassBytesBuilder b("pkg/Caller");

    test::FieldSpec classSlot;
    classSlot.access = 0x0009;
    classSlot.name   = "classSlot";
    classSlot.desc   = "Ljava/lang/Class;";
    b.addField(classSlot);

    test::FieldSpec instanceClass;
    instanceClass.access = 0x0001;
    instanceClass.name   = "instanceClass";
    instanceClass.desc   = "Ljava/lang/Class;";
    b.addField(instanceClass);

    test::MethodSpec clinit;
    clinit.access = 0x0008;   // static
    clinit.name   = "<clinit>";
    clinit.desc   = "()V";
    clinit.emit   = [](test::CodeEmit& e) {
        e.ldcClass("pkg/Target");
        e.fieldInsn(jbc::core::Op::PUTSTATIC, "pkg/Caller", "classSlot", "Ljava/lang/Class;");
        e.op(jbc::core::Op::RETURN);
    };

    test::MethodSpec identityClass;
    identityClass.access = 0x0009;
    identityClass.name   = "identityClass";
    identityClass.desc   = "(Ljava/lang/Class;)Ljava/lang/Class;";
    identityClass.emit   = [](test::CodeEmit& e) {
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.op(jbc::core::Op::ARETURN);
    };

    test::MethodSpec classFromField;
    classFromField.access = 0x0009;
    classFromField.name   = "classFromField";
    classFromField.desc   = "()Ljava/lang/Class;";
    classFromField.emit   = [](test::CodeEmit& e) {
        e.fieldInsn(jbc::core::Op::GETSTATIC, "pkg/Caller", "classSlot", "Ljava/lang/Class;");
        e.op(jbc::core::Op::ARETURN);
    };

    test::MethodSpec classFromMap;
    classFromMap.access   = 0x0009;
    classFromMap.name     = "classFromMap";
    classFromMap.desc     = "(Ljava/util/Map;)Ljava/lang/Class;";
    classFromMap.maxStack = 4;
    classFromMap.emit     = [](test::CodeEmit& e) {
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.ldcstring("target");
        e.ldcClass("pkg/Target");
        e.invokeInterface("java/util/Map", "put",
                          "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", 3);
        e.op(jbc::core::Op::POP);
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.ldcstring("target");
        e.invokeInterface("java/util/Map", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", 2);
        e.typeInsn(jbc::core::Op::CHECKCAST, "java/lang/Class");
        e.op(jbc::core::Op::ARETURN);
    };

    test::MethodSpec setInstance;
    setInstance.access = 0x0009;
    setInstance.name   = "setInstance";
    setInstance.desc   = "(Lpkg/Caller;)V";
    setInstance.emit   = [](test::CodeEmit& e) {
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.ldcClass("pkg/Target");
        e.fieldInsn(jbc::core::Op::PUTFIELD, "pkg/Caller", "instanceClass", "Ljava/lang/Class;");
        e.op(jbc::core::Op::RETURN);
    };

    auto lookupTail = [](test::CodeEmit& e) {
        e.i32const(0);
        e.typeInsn(jbc::core::Op::ANEWARRAY, "java/lang/Class");
        e.invoke(jbc::core::Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                 "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
        e.op(jbc::core::Op::POP);
        e.op(jbc::core::Op::RETURN);
    };

    test::MethodSpec use;
    use.access    = 0x0009;
    use.name      = "use";
    use.desc      = "()V";
    use.maxStack  = 3;
    use.maxLocals = 0;
    use.emit      = [lookupTail](test::CodeEmit& e) {
        e.ldcClass("pkg/Target");
        e.invoke(jbc::core::Op::INVOKESTATIC, "pkg/Caller", "identityClass",
                 "(Ljava/lang/Class;)Ljava/lang/Class;");
        e.ldcstring("oldMethod");
        lookupTail(e);
    };

    test::MethodSpec fieldUse;
    fieldUse.access    = 0x0009;
    fieldUse.name      = "fieldUse";
    fieldUse.desc      = "()V";
    fieldUse.maxStack  = 3;
    fieldUse.maxLocals = 0;
    fieldUse.emit      = [lookupTail](test::CodeEmit& e) {
        e.invoke(jbc::core::Op::INVOKESTATIC, "pkg/Caller", "classFromField",
                 "()Ljava/lang/Class;");
        e.ldcstring("oldMethod");
        lookupTail(e);
    };

    test::MethodSpec containerUse;
    containerUse.access    = 0x0009;
    containerUse.name      = "containerUse";
    containerUse.desc      = "()V";
    containerUse.maxStack  = 4;
    containerUse.maxLocals = 0;
    containerUse.emit      = [lookupTail](test::CodeEmit& e) {
        e.typeInsn(jbc::core::Op::NEW, "java/util/HashMap");
        e.op(jbc::core::Op::DUP);
        e.invoke(jbc::core::Op::INVOKESPECIAL, "java/util/HashMap", "<init>", "()V");
        e.invoke(jbc::core::Op::INVOKESTATIC, "pkg/Caller", "classFromMap",
                 "(Ljava/util/Map;)Ljava/lang/Class;");
        e.ldcstring("oldMethod");
        lookupTail(e);
    };

    test::MethodSpec instanceUse;
    instanceUse.access    = 0x0009;
    instanceUse.name      = "instanceUse";
    instanceUse.desc      = "(Lpkg/Caller;)V";
    instanceUse.maxStack  = 3;
    instanceUse.maxLocals = 1;
    instanceUse.emit      = [lookupTail](test::CodeEmit& e) {
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.invoke(jbc::core::Op::INVOKESTATIC, "pkg/Caller", "setInstance", "(Lpkg/Caller;)V");
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.fieldInsn(jbc::core::Op::GETFIELD, "pkg/Caller", "instanceClass", "Ljava/lang/Class;");
        e.ldcstring("oldMethod");
        lookupTail(e);
    };

    test::MethodSpec consumedQueueUse;
    consumedQueueUse.access    = 0x0009;
    consumedQueueUse.name      = "consumedQueueUse";
    consumedQueueUse.desc      = "()V";
    consumedQueueUse.maxStack  = 4;
    consumedQueueUse.maxLocals = 1;
    consumedQueueUse.emit      = [lookupTail](test::CodeEmit& e) {
        e.typeInsn(jbc::core::Op::NEW, "java/util/ArrayDeque");
        e.op(jbc::core::Op::DUP);
        e.invoke(jbc::core::Op::INVOKESPECIAL, "java/util/ArrayDeque", "<init>", "()V");
        e.varInsn(jbc::core::Op::ASTORE, 0);
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.ldcClass("pkg/Target");
        e.invoke(jbc::core::Op::INVOKEVIRTUAL, "java/util/ArrayDeque", "offer",
                 "(Ljava/lang/Object;)Z");
        e.op(jbc::core::Op::POP);
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.invoke(jbc::core::Op::INVOKEVIRTUAL, "java/util/ArrayDeque", "poll",
                 "()Ljava/lang/Object;");
        e.op(jbc::core::Op::POP);
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.invoke(jbc::core::Op::INVOKEVIRTUAL, "java/util/ArrayDeque", "poll",
                 "()Ljava/lang/Object;");
        e.typeInsn(jbc::core::Op::CHECKCAST, "java/lang/Class");
        e.ldcstring("oldMethod");
        lookupTail(e);
    };

    return b.assemble({clinit, identityClass, classFromField, classFromMap, setInstance, use,
                       fieldUse, containerUse, instanceUse, consumedQueueUse});
}

/// @brief T21 夹具：行号表 + 无签名 LVT 的调试元数据。
Bytes buildDebugClass()
{
    ClassBytesBuilder b("pkg/Debug");

    test::MethodSpec debug;
    debug.access    = 0x0009;
    debug.name      = "debug";
    debug.desc      = "(I)V";
    debug.maxStack  = 1;
    debug.maxLocals = 1;
    const std::size_t l0 = 0;
    const std::size_t l1 = 1;
    debug.emit           = [l0, l1](test::CodeEmit& e) {
        e.bindLabel(l0);
        e.varInsn(jbc::core::Op::ILOAD, 0);
        e.bindLabel(l1);
        e.op(jbc::core::Op::RETURN);
    };
    debug.lineNumbers.push_back(test::LineSpec{123, l0});
    debug.localVariables.push_back(test::LvtSpec{"arg", "I", "", l0, l1, 0});
    return b.assemble({debug});
}

/// @brief 简单单方法类（run()V；容器面夹具用）。
Bytes buildSimpleClass(std::string_view name)
{
    ClassBytesBuilder b(name);
    test::MethodSpec  run;
    run.access = 0x0009;
    run.name   = "run";
    run.desc   = "()V";
    run.emit   = [](test::CodeEmit& e) { e.op(jbc::core::Op::RETURN); };
    return b.assemble({run});
}

/// @brief 向容器添加一份类字节资源（URI 即资源路径）。
void addClassBytes(mjbase::model::ResourceContainer& container, std::string_view uri, Bytes bytes)
{
    container.addResource(uri,
                          std::make_unique<mjbase::model::ClassResource>(uri, std::move(bytes)));
}

mjbase::model::ClassResource* classResourceOf(mjbase::model::IResource* resource)
{
    return ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource);
}

mjbase::model::UnknownResource* unknownResourceOf(mjbase::model::IResource* resource)
{
    return ca::core::tag_cast::cast<mjbase::model::UnknownResource>(resource);
}

mjbase::model::ManifestResource* manifestResourceOf(mjbase::model::IResource* resource)
{
    return ca::core::tag_cast::cast<mjbase::model::ManifestResource>(resource);
}

std::string manifestText(const mjbase::jar::Manifest& manifest, const char* key)
{
    return std::string(static_cast<std::string_view>(manifest.getMainAttribute(
        ca::str::Utf8StringRef::from_cstr(key))));
}

// ---------------------------------------------------------------------------
// 用例（旧栈 21 例全量移植 + MappingRemapper 2 例）。
// ---------------------------------------------------------------------------

// 旧例 1：rewritesClassDescriptorsAndCallSites
TEST(MjAsmRemapperTest, rewritesClassDescriptorsAndCallSites)
{
    MemberRemapPlan plan;
    plan.mapClass("OldOwner", "NewOwner");
    plan.mapClass("OldType", "NewType");
    plan.mapClass("OldIface", "NewIface");
    plan.mapMethod("OldOwner", "oldMethod", "(LOldType;)LOldType;", "NewOwner", "newMethod",
                   "(LNewType;)LNewType;");
    plan.mapField("OldOwner", "oldField", "LOldType;", "NewOwner", "newField", "LNewType;");

    const Bytes bytes = buildRemapClass();
    auto        session = openSession(bytes);
    ASSERT_TRUE(session);
    auto cf = applyAndReparse(*session, plan, RemapPolicy::bytecodeOnly());
    ASSERT_TRUE(cf);

    EXPECT_EQ("NewOwner", classTextOf(*cf, cf->thisClass));
    EXPECT_EQ("NewIface", classTextOf(*cf, cf->superClass));
    ASSERT_EQ(1U, cf->interfaces.size());
    EXPECT_EQ("OldMarker", classTextOf(*cf, cf->interfaces[0]));

    const MemberInfo* field = fieldOf(*cf, "newField");
    ASSERT_TRUE(field != nullptr);
    EXPECT_EQ("LNewType;", utf8TextOf(*cf, field->descCp));

    const MemberInfo* oldMethod = methodOf(*cf, "newMethod");
    ASSERT_TRUE(oldMethod != nullptr);
    EXPECT_EQ("(LNewType;)LNewType;", utf8TextOf(*cf, oldMethod->descCp));
    const MemberInfo* use = methodOf(*cf, "use");
    ASSERT_TRUE(use != nullptr);
    EXPECT_EQ("(LNewType;)LNewType;", utf8TextOf(*cf, use->descCp));

    auto out = writeClassOf(*session);
    ASSERT_FALSE(out.empty());
    auto back = openSession(out);
    ASSERT_TRUE(back);

    const jbc::ir::Insn* call = insnAt(*back, "use", 6);
    ASSERT_TRUE(call != nullptr);
    const MemberRefText target = memberRefOf(back->backing(), call->cp);
    ASSERT_TRUE(target.ok);
    EXPECT_EQ("NewOwner", target.owner);
    EXPECT_EQ("newMethod", target.name);
    EXPECT_EQ("(LNewType;)LNewType;", target.desc);

    const jbc::ir::Insn* getfield = insnAt(*back, "use", 8);
    ASSERT_TRUE(getfield != nullptr);
    const MemberRefText fieldRef = memberRefOf(back->backing(), getfield->cp);
    ASSERT_TRUE(fieldRef.ok);
    EXPECT_EQ("NewOwner", fieldRef.owner);
    EXPECT_EQ("newField", fieldRef.name);
    EXPECT_EQ("LNewType;", fieldRef.desc);

    const jbc::ir::Insn* newInsn = insnAt(*back, "use", 10);
    ASSERT_TRUE(newInsn != nullptr);
    EXPECT_EQ("NewType", classTextOf(back->backing(), newInsn->cp));
    const jbc::ir::Insn* arrayInsn = insnAt(*back, "use", 13);
    ASSERT_TRUE(arrayInsn != nullptr);
    EXPECT_EQ("NewType", classTextOf(back->backing(), arrayInsn->cp));
    const jbc::ir::Insn* multiInsn = insnAt(*back, "use", 17);
    ASSERT_TRUE(multiInsn != nullptr);
    EXPECT_EQ("[[LNewType;", classTextOf(back->backing(), multiInsn->cp));
}

// 旧例 2：mapsDescriptorsWithoutApplyingClassTree
TEST(MjAsmRemapperTest, mapsDescriptorsWithoutApplyingClassTree)
{
    MemberRemapPlan empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ("LOldOwner;", std::string(empty.mapFieldDescriptor("LOldOwner;")));
    EXPECT_EQ("(LOldOwner;)LOldOwner;",
              std::string(empty.mapMethodDescriptor("(LOldOwner;)LOldOwner;")));

    MemberRemapPlan plan;
    plan.mapClass("OldOwner", "NewOwner");
    EXPECT_FALSE(plan.empty());
    EXPECT_EQ("NewOwner", std::string(plan.mapInternalName("OldOwner")));
    EXPECT_EQ("Other", std::string(plan.mapInternalName("Other")));
    EXPECT_EQ("LNewOwner;", std::string(plan.mapFieldDescriptor("LOldOwner;")));
    EXPECT_EQ("[LNewOwner;", std::string(plan.mapFieldDescriptor("[LOldOwner;")));
    EXPECT_EQ("I", std::string(plan.mapFieldDescriptor("I")));
    EXPECT_EQ("(LNewOwner;)LNewOwner;",
              std::string(plan.mapMethodDescriptor("(LOldOwner;)LOldOwner;")));
    EXPECT_EQ("(I)V", std::string(plan.mapMethodDescriptor("(I)V")));
    EXPECT_EQ("Ljava/lang/Object;LNewOwner;",
              std::string(plan.mapSignature("Ljava/lang/Object;LOldOwner;")));

    // 池驻留视图稳定性：同一输入重复查询得到同一文本。
    EXPECT_EQ(std::string(plan.mapFieldDescriptor("LOldOwner;")),
              std::string(plan.mapFieldDescriptor("LOldOwner;")));
}

// 旧例 3：descriptorCacheFollowsClassMappingUpdates
TEST(MjAsmRemapperTest, descriptorCacheFollowsClassMappingUpdates)
{
    MemberRemapPlan plan;
    plan.mapClass("OldOwner", "NewOwner");
    EXPECT_EQ("LNewOwner;", std::string(plan.mapFieldDescriptor("LOldOwner;")));
    EXPECT_EQ("(LNewOwner;)V", std::string(plan.mapMethodDescriptor("(LOldOwner;)V")));
    EXPECT_EQ("LNewOwner;", std::string(plan.mapSignature("LNewOwner;")));

    // 同名旧类二次映射：描述符缓存必须整体失效，不得吐旧结果。
    plan.mapClass("OldOwner", "FinalOwner");
    EXPECT_EQ("LFinalOwner;", std::string(plan.mapFieldDescriptor("LOldOwner;")));
    EXPECT_EQ("(LFinalOwner;)V", std::string(plan.mapMethodDescriptor("(LOldOwner;)V")));
    EXPECT_EQ("LFinalOwner;", std::string(plan.mapSignature("LOldOwner;")));
}

// 旧例 4：remapsTypeHandleInvokeDynamicAndFrameConstants
TEST(MjAsmRemapperTest, remapsTypeHandleInvokeDynamicAndFrameConstants)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldType", "pkg/NewType");
    plan.mapClass("pkg/OldOwner", "pkg/NewOwner");
    plan.mapClass("pkg/OldIface", "pkg/NewIface");
    plan.mapMethod("pkg/OldOwner", "oldMethod", "()V", "pkg/NewOwner", "renamedMethod", "()V");
    // 接口方法规则按真实方法描述符（= BSM 首参 SAM MethodType）登记。
    plan.mapMethod("pkg/OldIface", "run", "()V", "pkg/NewIface", "renamedRun", "()V");

    const Bytes bytes = buildTypeHandleClass();
    auto        session = openSession(bytes);
    ASSERT_TRUE(session);
    auto cf = applyAndReparse(*session, plan, RemapPolicy::bytecodeOnly());
    ASSERT_TRUE(cf);

    auto out = writeClassOf(*session);
    ASSERT_FALSE(out.empty());
    auto back = openSession(out);
    ASSERT_TRUE(back);

    ClassFile& bcf = back->backing();
    const jbc::ir::Insn* ldcClass = insnAt(*back, "use", 0);
    ASSERT_TRUE(ldcClass != nullptr);
    EXPECT_EQ("pkg/NewType", classTextOf(bcf, ldcClass->cp));

    const jbc::ir::Insn* ldcHandle = insnAt(*back, "use", 1);
    ASSERT_TRUE(ldcHandle != nullptr);
    ASSERT_TRUE(bcf.cp.valid(ldcHandle->cp));
    ASSERT_EQ(jbc::core::CpTag::MethodHandle, bcf.cp.at(ldcHandle->cp).tag);
    const MemberRefText handleRef = memberRefOf(bcf, bcf.cp.at(ldcHandle->cp).b);
    ASSERT_TRUE(handleRef.ok);
    EXPECT_EQ("pkg/NewOwner", handleRef.owner);
    EXPECT_EQ("renamedMethod", handleRef.name);
    EXPECT_EQ("()V", handleRef.desc);

    // indy0：LambdaMetafactory 形态 —— SAM 名与描述符随接口映射，BSM 句柄实参
    // 随方法规则改名。
    const jbc::ir::Insn* indy0 = insnAt(*back, "use", 2);
    ASSERT_TRUE(indy0 != nullptr);
    ASSERT_EQ(jbc::core::CpTag::InvokeDynamic, bcf.cp.at(indy0->cp).tag);
    {
        const auto& entry = bcf.cp.at(indy0->cp);
        const auto& nat   = bcf.cp.at(entry.b);
        EXPECT_EQ("renamedRun", utf8TextOf(bcf, nat.a));
        EXPECT_EQ("()Lpkg/NewIface;", utf8TextOf(bcf, nat.b));
    }
    const jbc::core::AttrSlot* bsmSlot = slotOf(bcf, bcf.attrs, "BootstrapMethods");
    ASSERT_TRUE(bsmSlot != nullptr);
    if (!bsmSlot->decoded()) {
        ASSERT_FALSE(bcf.decodeAttr(const_cast<jbc::core::AttrSlot&>(*bsmSlot)).is_err());
    }
    const auto* bsm = ca::core::tag_cast::cast<const jbc::core::BootstrapMethodsAttr>(bsmSlot->peek());
    ASSERT_TRUE(bsm != nullptr);
    ASSERT_EQ(3U, bsm->entries.size());
    ASSERT_EQ(2U, bsm->entries[0].args.size());
    ASSERT_EQ(jbc::core::CpTag::MethodType, bcf.cp.at(bsm->entries[0].args[0]).tag);
    EXPECT_EQ("()V", utf8TextOf(bcf, bcf.cp.at(bsm->entries[0].args[0]).a));
    ASSERT_EQ(jbc::core::CpTag::MethodHandle, bcf.cp.at(bsm->entries[0].args[1]).tag);
    {
        const MemberRefText samRef = memberRefOf(bcf, bcf.cp.at(bsm->entries[0].args[1]).b);
        ASSERT_TRUE(samRef.ok);
        EXPECT_EQ("pkg/NewOwner", samRef.owner);
        EXPECT_EQ("renamedMethod", samRef.name);
    }

    // indy1：非 LambdaMetafactory 形态 —— 名字保留、描述符仍重映射。
    const jbc::ir::Insn* indy1 = insnAt(*back, "use", 3);
    ASSERT_TRUE(indy1 != nullptr);
    {
        const auto& entry = bcf.cp.at(indy1->cp);
        const auto& nat   = bcf.cp.at(entry.b);
        EXPECT_EQ("run", utf8TextOf(bcf, nat.a));
        EXPECT_EQ("()Lpkg/NewIface;", utf8TextOf(bcf, nat.b));
    }

    // condy：描述符重映射 + BSM Class 实参重映射（entry.b 重指向新 NAT）。
    const jbc::ir::Insn* ldcCondy = insnAt(*back, "use", 4);
    ASSERT_TRUE(ldcCondy != nullptr);
    ASSERT_EQ(jbc::core::CpTag::Dynamic, bcf.cp.at(ldcCondy->cp).tag);
    {
        const auto& entry = bcf.cp.at(ldcCondy->cp);
        const auto& nat   = bcf.cp.at(entry.b);
        EXPECT_EQ("constant", utf8TextOf(bcf, nat.a));
        EXPECT_EQ("Lpkg/NewType;", utf8TextOf(bcf, nat.b));
        const auto& constBsm = bsm->entries[2];
        ASSERT_EQ(1U, constBsm.args.size());
        EXPECT_EQ("pkg/NewType", classTextOf(bcf, constBsm.args[0]));
        // condy 的 BSM 方法句柄本身（pkg/Bootstrap.bootstrap）不在映射内，保持。
        ASSERT_EQ(jbc::core::CpTag::MethodHandle, bcf.cp.at(constBsm.methodRef).tag);
        const MemberRefText bootRef = memberRefOf(bcf, bcf.cp.at(constBsm.methodRef).b);
        ASSERT_TRUE(bootRef.ok);
        EXPECT_EQ("pkg/Bootstrap", bootRef.owner);
        EXPECT_EQ("bootstrap", bootRef.name);
    }

    // SMT 全帧 Object 校验类型重映射。
    MemberInfo* use = methodOf(bcf, "use");
    ASSERT_TRUE(use != nullptr);
    const jbc::core::CodeAttr* code = codeOf(bcf, *use);
    ASSERT_TRUE(code != nullptr);
    const jbc::core::AttrSlot* smtSlot = slotOf(bcf, code->attrs, "StackMapTable");
    ASSERT_TRUE(smtSlot != nullptr);
    const auto* smt = ca::core::tag_cast::cast<const jbc::core::StackMapTableAttr>(smtSlot->peek());
    ASSERT_TRUE(smt != nullptr);
    ASSERT_EQ(1U, smt->frames.size());
    ASSERT_EQ(2U, smt->frames[0].locals.size());
    ASSERT_EQ(7, smt->frames[0].locals[0].tag);
    EXPECT_EQ("pkg/NewType", classTextOf(bcf, smt->frames[0].locals[0].cp));
    ASSERT_EQ(7, smt->frames[0].locals[1].tag);
    EXPECT_EQ("[Lpkg/NewType;", classTextOf(bcf, smt->frames[0].locals[1].cp));
}

// 旧例 5：remapsMethodDebugExceptionAndModuleClassMetadata
TEST(MjAsmRemapperTest, remapsMethodDebugExceptionAndModuleClassMetadata)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldMain", "pkg/NewMain");
    plan.mapClass("pkg/OldException", "pkg/NewException");
    plan.mapClass("pkg/OldType", "pkg/NewType");
    plan.mapClass("pkg/OldService", "pkg/NewService");
    plan.mapClass("pkg/OldProvider", "pkg/NewProvider");

    const Bytes bytes = buildModuleAndDebugClass();
    auto        session = openSession(bytes);
    ASSERT_TRUE(session);
    auto cf = applyAndReparse(*session, plan, RemapPolicy::all());
    ASSERT_TRUE(cf);

    const MemberInfo* debug = methodOf(*cf, "debug");
    ASSERT_TRUE(debug != nullptr);
    const std::vector<std::string> exceptions = exceptionsOf(*cf, const_cast<MemberInfo&>(*debug));
    ASSERT_EQ(1U, exceptions.size());
    EXPECT_EQ("pkg/NewException", exceptions[0]);

    const jbc::core::CodeAttr* code = codeOf(*cf, const_cast<MemberInfo&>(*debug));
    ASSERT_TRUE(code != nullptr);
    ASSERT_EQ(1U, code->excTable.size());
    EXPECT_NE(0U, code->excTable[0].catchType);
    EXPECT_EQ("pkg/NewException", classTextOf(*cf, code->excTable[0].catchType));

    // LVT 行描述符重映射；LVTT 行（desc=签名）按签名重映射。行序随装配，
    // 按属性名各自定位断言。
    const std::vector<LvtRowText> rows = localVariablesOf(*cf, *code);
    ASSERT_EQ(2U, rows.size());
    bool sawLvt  = false;
    bool sawLvtt = false;
    for (const LvtRowText& row : rows) {
        if (row.attrName == "LocalVariableTable") {
            sawLvt = true;
            EXPECT_EQ("value", row.name);
            EXPECT_EQ("Lpkg/NewType;", row.desc);
        }
        else if (row.attrName == "LocalVariableTypeTable") {
            sawLvtt = true;
            EXPECT_EQ("value", row.name);
            EXPECT_EQ("Ljava/util/List<Lpkg/NewType;>;", row.desc);
        }
    }
    EXPECT_TRUE(sawLvt);
    EXPECT_TRUE(sawLvtt);

    // Module 族：uses/provides 重映射；ModuleMainClass 重映射。typed 体经
    // 公开解码入口直读（分派翻 typed 后输出槽不再是 Raw 形）。
    const jbc::core::AttrSlot* moduleSlot = slotOf(*cf, cf->attrs, "Module");
    ASSERT_TRUE(moduleSlot != nullptr);
    {
        auto d = jbc::core::decodeModuleBody(*cf, *moduleSlot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        const auto& mod = static_cast<const jbc::core::ModuleAttr&>(*body);
        // 夹具 requires/exports/opens 全空：uses 1 条、provides 1 服务 1 实现。
        ASSERT_EQ(1U, mod.usesCps.size());
        EXPECT_EQ("pkg/NewService", classTextOf(*cf, mod.usesCps[0]));
        ASSERT_EQ(1U, mod.provides.size());
        EXPECT_EQ("pkg/NewService", classTextOf(*cf, mod.provides[0].serviceCp));
        ASSERT_EQ(1U, mod.provides[0].withCps.size());
        EXPECT_EQ("pkg/NewProvider", classTextOf(*cf, mod.provides[0].withCps[0]));
    }

    const jbc::core::AttrSlot* mainSlot = slotOf(*cf, cf->attrs, "ModuleMainClass");
    ASSERT_TRUE(mainSlot != nullptr);
    {
        auto d = jbc::core::decodeModuleMainClassBody(*cf, *mainSlot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        const auto& mc = static_cast<const jbc::core::ModuleMainClassAttr&>(*body);
        EXPECT_EQ("pkg/NewMain", classTextOf(*cf, mc.mainClassCp));
    }
}

// 旧例 6：remapsStructuralClassMetadata
TEST(MjAsmRemapperTest, remapsStructuralClassMetadata)
{
    MemberRemapPlan plan;
    plan.mapClass("OldOwner", "NewOwner");
    plan.mapClass("OldType", "NewType");
    plan.mapClass("OldIface", "NewIface");
    plan.mapClass("OldOuter", "NewOuter");
    plan.mapClass("OldOuter$OldInner", "NewOuter$NewInner");
    plan.mapClass("OldInner", "NewInner");
    plan.mapClass("OldReturn", "NewReturn");
    plan.mapClass("OldNestHost", "NewNestHost");
    plan.mapClass("OldNestMember", "NewNestMember");
    plan.mapClass("OldPermitted", "NewPermitted");
    plan.mapClass("OldGeneric", "NewGeneric");
    plan.mapClass("OldAnno", "NewAnno");
    plan.mapMethod("OldOuter", "oldOuter", "(LOldType;)LOldReturn;", "NewOuter", "newOuter",
                   "(LNewType;)LNewReturn;");
    plan.mapField("OldOwner", "oldComponent", "LOldType;", "NewOwner", "newComponent",
                  "LNewType;");

    const Bytes bytes = buildStructuralClass();

    // 面一：默认策略（仅 bytecode）—— 结构面改名，Record 组件签名/注解不动。
    {
        auto session = openSession(bytes);
        ASSERT_TRUE(session);
        auto cf = applyAndReparse(*session, plan, RemapPolicy::bytecodeOnly());
        ASSERT_TRUE(cf);

        const jbc::core::AttrSlot* enclosingSlot = slotOf(*cf, cf->attrs, "EnclosingMethod");
        ASSERT_TRUE(enclosingSlot != nullptr);
        // typed 体公开入口读槽（B3 分派接线前后同形）：两槽语义逐字段断言。
        {
            auto d = decodeEnclosingMethodBody(*cf, *enclosingSlot);
            ASSERT_TRUE(d.is_ok());
            const auto body = std::move(d).unwrap();
            const auto& em  = static_cast<const jbc::core::EnclosingMethodAttr&>(*body);
            EXPECT_EQ("NewOuter", classTextOf(*cf, em.classCp));
            const auto& nat = cf->cp.at(em.methodNatCp);
            EXPECT_EQ("newOuter", utf8TextOf(*cf, nat.a));
            EXPECT_EQ("(LNewType;)LNewReturn;", utf8TextOf(*cf, nat.b));
        }

        // Nest 族三件 typed 体公开入口读槽（分派翻 typed 后 Raw 形不可达）。
        const jbc::core::AttrSlot* nestHostSlot = slotOf(*cf, cf->attrs, "NestHost");
        ASSERT_TRUE(nestHostSlot != nullptr);
        {
            auto d = jbc::core::decodeNestHostBody(*cf, *nestHostSlot);
            ASSERT_TRUE(d.is_ok());
            const auto body = std::move(d).unwrap();
            const auto& nh  = static_cast<const jbc::core::NestHostAttr&>(*body);
            EXPECT_EQ("NewNestHost", classTextOf(*cf, nh.hostClassCp));
        }

        const jbc::core::AttrSlot* nestMembersSlot = slotOf(*cf, cf->attrs, "NestMembers");
        ASSERT_TRUE(nestMembersSlot != nullptr);
        {
            auto d = jbc::core::decodeNestMembersBody(*cf, *nestMembersSlot);
            ASSERT_TRUE(d.is_ok());
            const auto body = std::move(d).unwrap();
            const auto& nm  = static_cast<const jbc::core::NestMembersAttr&>(*body);
            ASSERT_GE(nm.memberCps.size(), 1U);
            EXPECT_EQ("NewNestMember", classTextOf(*cf, nm.memberCps[0]));
        }

        const jbc::core::AttrSlot* permittedSlot = slotOf(*cf, cf->attrs, "PermittedSubclasses");
        ASSERT_TRUE(permittedSlot != nullptr);
        {
            auto d = jbc::core::decodePermittedSubclassesBody(*cf, *permittedSlot);
            ASSERT_TRUE(d.is_ok());
            const auto body = std::move(d).unwrap();
            const auto& ps  = static_cast<const jbc::core::PermittedSubclassesAttr&>(*body);
            ASSERT_GE(ps.permittedCps.size(), 1U);
            EXPECT_EQ("NewPermitted", classTextOf(*cf, ps.permittedCps[0]));
        }

        const jbc::core::AttrSlot* innerSlot = slotOf(*cf, cf->attrs, "InnerClasses");
        ASSERT_TRUE(innerSlot != nullptr);
        // typed 体公开入口读槽（同上）：行三槽语义逐字段断言。
        {
            auto d = decodeInnerClassesBody(*cf, *innerSlot);
            ASSERT_TRUE(d.is_ok());
            const auto body = std::move(d).unwrap();
            const auto& ic  = static_cast<const jbc::core::InnerClassesAttr&>(*body);
            ASSERT_GE(ic.rows.size(), 1U);
            EXPECT_EQ("NewOuter$NewInner", classTextOf(*cf, ic.rows[0].innerClassInfoCp));
            EXPECT_EQ("NewOuter", classTextOf(*cf, ic.rows[0].outerClassInfoCp));
            EXPECT_EQ("NewInner", utf8TextOf(*cf, ic.rows[0].innerNameCp));
        }

        // Record 组件 typed 体公开入口读槽（分派翻 typed 后 Raw 形不可达）。
        const jbc::core::AttrSlot* recordSlot = slotOf(*cf, cf->attrs, "Record");
        ASSERT_TRUE(recordSlot != nullptr);
        {
            auto d = jbc::core::decodeRecordBody(*cf, *recordSlot);
            ASSERT_TRUE(d.is_ok());
            const auto body = std::move(d).unwrap();
            const auto& rec = static_cast<const jbc::core::RecordAttr&>(*body);
            ASSERT_EQ(1U, rec.components.size());
            const auto& comp = rec.components[0];
            EXPECT_EQ("newComponent", utf8TextOf(*cf, comp.nameCp));
            EXPECT_EQ("LNewType;", utf8TextOf(*cf, comp.descriptorCp));
            // 组件 Signature/注解属性未开面：原样保留。
            ASSERT_EQ(2U, comp.attrs.size());
            const jbc::core::AttrSlot* sigSlot = slotOf(*cf, comp.attrs, "Signature");
            ASSERT_TRUE(sigSlot != nullptr);
            const jbc::core::SignatureAttr* sig = signatureAttrOf(*cf, *sigSlot);
            ASSERT_TRUE(sig != nullptr);
            EXPECT_EQ("LOldGeneric;", utf8TextOf(*cf, sig->signatureCp));
            const jbc::core::AttrSlot* annoSlot =
                slotOf(*cf, comp.attrs, "RuntimeVisibleAnnotations");
            ASSERT_TRUE(annoSlot != nullptr);
            EXPECT_EQ("LOldAnno;", annotationTypeText(*cf, *annoSlot));
        }
    }

    // 面二：全开策略 —— Record 组件签名/注解一并重映射。
    {
        auto session = openSession(bytes);
        ASSERT_TRUE(session);
        auto cf = applyAndReparse(*session, plan, RemapPolicy::all());
        ASSERT_TRUE(cf);
        const jbc::core::AttrSlot* recordSlot = slotOf(*cf, cf->attrs, "Record");
        ASSERT_TRUE(recordSlot != nullptr);
        {
            auto d = jbc::core::decodeRecordBody(*cf, *recordSlot);
            ASSERT_TRUE(d.is_ok());
            const auto body = std::move(d).unwrap();
            const auto& rec = static_cast<const jbc::core::RecordAttr&>(*body);
            ASSERT_EQ(1U, rec.components.size());
            const auto& comp = rec.components[0];
            EXPECT_EQ("newComponent", utf8TextOf(*cf, comp.nameCp));
            EXPECT_EQ("LNewType;", utf8TextOf(*cf, comp.descriptorCp));
            const jbc::core::AttrSlot* sigSlot = slotOf(*cf, comp.attrs, "Signature");
            ASSERT_TRUE(sigSlot != nullptr);
            const jbc::core::SignatureAttr* sig = signatureAttrOf(*cf, *sigSlot);
            ASSERT_TRUE(sig != nullptr);
            EXPECT_EQ("LNewGeneric;", utf8TextOf(*cf, sig->signatureCp));
            const jbc::core::AttrSlot* annoSlot =
                slotOf(*cf, comp.attrs, "RuntimeVisibleAnnotations");
            ASSERT_TRUE(annoSlot != nullptr);
            EXPECT_EQ("LNewAnno;", annotationTypeText(*cf, *annoSlot));
        }
    }
}

// 旧例 7：descriptorlessFieldRuleMatchesByOwnerAndName
TEST(MjAsmRemapperTest, descriptorlessFieldRuleMatchesByOwnerAndName)
{
    MemberRemapPlan plan;
    plan.mapClass("OldOwner", "NewOwner");
    plan.mapClass("OldType", "NewType");
    plan.mapField("OldOwner", "oldField", "", "NewOwner", "newField", "");

    const FieldRemapRule* rule = plan.findField("OldOwner", "oldField", "LOldType;");
    ASSERT_TRUE(rule != nullptr);
    EXPECT_EQ("", std::string(plan.pool().str(rule->oldKey.desc)));

    const Bytes bytes = buildDescriptorlessClass();
    auto        session = openSession(bytes);
    ASSERT_TRUE(session);
    auto cf = applyAndReparse(*session, plan, RemapPolicy::bytecodeOnly());
    ASSERT_TRUE(cf);

    const MemberInfo* field = fieldOf(*cf, "newField");
    ASSERT_TRUE(field != nullptr);
    EXPECT_EQ("LOldType;", utf8TextOf(*cf, field->descCp));

    auto out  = writeClassOf(*session);
    auto back = openSession(out);
    ASSERT_TRUE(back);
    const jbc::ir::Insn* getfield = insnAt(*back, "use", 1);
    ASSERT_TRUE(getfield != nullptr);
    const MemberRefText ref = memberRefOf(back->backing(), getfield->cp);
    ASSERT_TRUE(ref.ok);
    EXPECT_EQ("NewOwner", ref.owner);
    EXPECT_EQ("newField", ref.name);
    EXPECT_EQ("LOldType;", ref.desc);
}

// 旧例 8：policyControlsReflectionStringsAnnotationsAndSignatures
TEST(MjAsmRemapperTest, policyControlsReflectionStringsAnnotationsAndSignatures)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldReflect", "pkg/NewReflect");
    plan.mapClass("pkg/OldNoise", "pkg/NewNoise");
    plan.mapClass("OldGeneric", "NewGeneric");
    plan.mapClass("OldMethodGeneric", "NewMethodGeneric");
    plan.mapClass("OldAnno", "NewAnno");

    const Bytes bytes = buildRemapClass();

    // 默认策略：签名/注解/反射字符串全部不动，改写器如实报 false。
    {
        auto session = openSession(bytes);
        ASSERT_TRUE(session);
        auto applied = mjbase::asm_remapper::applyMemberRemapPlan(*session, plan);
        ASSERT_FALSE(applied.is_err());
        EXPECT_FALSE(applied.unwrap());
        auto cf = parseClass(writeClassOf(*session));
        ASSERT_TRUE(cf);
        EXPECT_EQ("Ljava/lang/Object;LOldGeneric;", signatureOf(*cf, cf->attrs));
        const jbc::core::AttrSlot* annoSlot = slotOf(*cf, cf->attrs, "RuntimeVisibleAnnotations");
        ASSERT_TRUE(annoSlot != nullptr);
        ASSERT_TRUE(annotationsOf(*cf, *annoSlot) != nullptr);
        EXPECT_EQ("LOldAnno;", annotationTypeText(*cf, *annoSlot));
        auto back = openSession(writeClassOf(*session));
        ASSERT_TRUE(back);
        const jbc::ir::Insn* ldc = insnAt(*back, "use", 0);
        ASSERT_TRUE(ldc != nullptr);
        const auto& entry = back->backing().cp.at(ldc->cp);
        EXPECT_EQ("pkg.OldReflect", utf8TextOf(back->backing(), entry.a));
    }

    // 全开：签名/注解/反射字符串各就各位；非类名字符串（pkg.OldNoise）保持。
    {
        auto session = openSession(bytes);
        ASSERT_TRUE(session);
        auto cf = applyAndReparse(*session, plan, RemapPolicy::all());
        ASSERT_TRUE(cf);
        EXPECT_EQ("Ljava/lang/Object;LNewGeneric;", signatureOf(*cf, cf->attrs));
        const jbc::core::AttrSlot* annoSlot = slotOf(*cf, cf->attrs, "RuntimeVisibleAnnotations");
        ASSERT_TRUE(annoSlot != nullptr);
        EXPECT_EQ("LNewAnno;", annotationTypeText(*cf, *annoSlot));

        const MemberInfo* field = fieldOf(*cf, "oldField");
        ASSERT_TRUE(field != nullptr);
        EXPECT_EQ("LNewGeneric;", signatureOf(*cf, const_cast<MemberInfo&>(*field).attrs));

        const MemberInfo* use = methodOf(*cf, "use");
        ASSERT_TRUE(use != nullptr);
        EXPECT_EQ("()LNewMethodGeneric;", signatureOf(*cf, const_cast<MemberInfo&>(*use).attrs));

        auto out  = writeClassOf(*session);
        auto back = openSession(out);
        ASSERT_TRUE(back);
        const std::vector<std::string> strings = ldcStringsOf(*back, "use");
        EXPECT_TRUE(contains(strings, "pkg.NewReflect"));
        EXPECT_TRUE(contains(strings, "pkg.OldNoise"));
        EXPECT_FALSE(contains(strings, "pkg.OldReflect"));
    }
}

// 旧例 9：reflectionPolicyRewritesConstantMemberLookupNames
TEST(MjAsmRemapperTest, reflectionPolicyRewritesConstantMemberLookupNames)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldOwner", "pkg/NewOwner");
    plan.mapField("pkg/OldOwner", "oldField", "I", "pkg/NewOwner", "renamedField", "I");
    plan.mapMethod("pkg/OldOwner", "oldMethod", "()V", "pkg/NewOwner", "renamedMethod", "()V");
    plan.mapMethod("pkg/OldOwner", "oldStringMethod", "(Ljava/lang/String;)V", "pkg/NewOwner",
                   "renamedStringMethod", "(Ljava/lang/String;)V");
    plan.mapMethod("pkg/OldOwner", "oldForNameStringMethod", "(Ljava/lang/String;)V",
                   "pkg/NewOwner", "renamedForNameStringMethod", "(Ljava/lang/String;)V");

    const Bytes bytes = buildReflectionMemberClass();

    // 默认策略：反射字符串不动（类面 pkg/OldOwner→pkg/NewOwner 照常生效）。
    {
        auto session = openSession(bytes);
        ASSERT_TRUE(session);
        auto applied = mjbase::asm_remapper::applyMemberRemapPlan(*session, plan);
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        const std::vector<std::string> strings = ldcStringsOf(*session, "use");
        EXPECT_TRUE(contains(strings, "oldMethod"));
        EXPECT_TRUE(contains(strings, "oldField"));
    }

    // 反射字符串开启：三条 getDeclaredMethod/getMethod 与 getDeclaredField 的
    // 名称常量各自改名；forName 的类名常量随之改名。
    {
        auto session = openSession(bytes);
        ASSERT_TRUE(session);
        auto applied = mjbase::asm_remapper::applyMemberRemapPlan(
            *session, plan, RemapPolicy::bytecodeAndReflectionStrings());
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        const std::vector<std::string> strings = ldcStringsOf(*session, "use");
        EXPECT_TRUE(contains(strings, "renamedMethod"));
        EXPECT_TRUE(contains(strings, "renamedStringMethod"));
        EXPECT_TRUE(contains(strings, "renamedForNameStringMethod"));
        EXPECT_TRUE(contains(strings, "renamedField"));
        EXPECT_TRUE(contains(strings, "pkg.NewOwner"));
        EXPECT_FALSE(contains(strings, "oldMethod"));
        EXPECT_FALSE(contains(strings, "oldStringMethod"));
        EXPECT_FALSE(contains(strings, "oldForNameStringMethod"));
        EXPECT_FALSE(contains(strings, "oldField"));
        EXPECT_FALSE(contains(strings, "pkg.OldOwner"));
    }
}

// 旧例 10：reflectionMemberLookupKeepsAmbiguousOverloadName
TEST(MjAsmRemapperTest, reflectionMemberLookupKeepsAmbiguousOverloadName)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldOwner", "pkg/NewOwner");
    plan.mapMethod("pkg/OldOwner", "oldMethod", "()V", "pkg/NewOwner", "renamedMethod", "()V");
    plan.mapMethod("pkg/OldOwner", "oldMethod", "(Ljava/lang/String;)V", "pkg/NewOwner",
                   "renamedA", "(Ljava/lang/String;)V");

    const Bytes bytes = buildReflectionMemberClass();
    auto        session = openSession(bytes);
    ASSERT_TRUE(session);
    auto applied = mjbase::asm_remapper::applyMemberRemapPlan(
        *session, plan, RemapPolicy::bytecodeAndReflectionStrings());
    ASSERT_FALSE(applied.is_err());
    EXPECT_TRUE(applied.unwrap());

    // 名称查询歧义：反射名称常量保持原名；声明面精确匹配照常改名。
    const std::vector<std::string> strings = ldcStringsOf(*session, "use");
    EXPECT_TRUE(contains(strings, "oldMethod"));
    EXPECT_FALSE(contains(strings, "renamedMethod"));
    EXPECT_FALSE(contains(strings, "renamedA"));

    auto out = writeClassOf(*session);
    ASSERT_FALSE(out.empty());
    auto cf = parseClass(out);
    ASSERT_TRUE(cf);
    EXPECT_TRUE(methodOf(*cf, "renamedMethod") != nullptr);
    EXPECT_TRUE(methodOf(*cf, "oldMethod") == nullptr);
}

// 旧例 11：policyControlsAnnotationClassLiteralElements
TEST(MjAsmRemapperTest, policyControlsAnnotationClassLiteralElements)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/Holder", "pkg/NewHolder");
    plan.mapClass("pkg/Anno", "pkg/NewAnno");
    plan.mapClass("pkg/OldClass", "pkg/NewClass");

    const Bytes bytes = buildAnnotationClass();

    {
        auto session = openSession(bytes);
        ASSERT_TRUE(session);
        auto applied = mjbase::asm_remapper::applyMemberRemapPlan(*session, plan);
        ASSERT_FALSE(applied.is_err());
        // 注解面未开启：注解保持，但类头（pkg/Holder→pkg/NewHolder）已改名。
        EXPECT_TRUE(applied.unwrap());
        auto cf = parseClass(writeClassOf(*session));
        ASSERT_TRUE(cf);
        const jbc::core::AttrSlot* annoSlot = slotOf(*cf, cf->attrs, "RuntimeVisibleAnnotations");
        ASSERT_TRUE(annoSlot != nullptr);
        ASSERT_TRUE(annotationsOf(*cf, *annoSlot) != nullptr);
        EXPECT_EQ("Lpkg/Anno;", annotationTypeText(*cf, *annoSlot));
        EXPECT_EQ("Lpkg/OldClass;", annotationElementText(*cf, *annoSlot));
    }

    {
        auto session = openSession(bytes);
        ASSERT_TRUE(session);
        auto cf = applyAndReparse(*session, plan, RemapPolicy::all());
        ASSERT_TRUE(cf);
        const jbc::core::AttrSlot* annoSlot = slotOf(*cf, cf->attrs, "RuntimeVisibleAnnotations");
        ASSERT_TRUE(annoSlot != nullptr);
        EXPECT_EQ("Lpkg/NewAnno;", annotationTypeText(*cf, *annoSlot));
        EXPECT_EQ("Lpkg/NewClass;", annotationElementText(*cf, *annoSlot));
    }
}

// 旧例 12：containerRemapKeepsResourceNamesByDefault
TEST(MjAsmRemapperTest, containerRemapKeepsResourceNamesByDefault)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldName", "pkg/NewName");

    mjbase::model::ResourceContainer container;
    addClassBytes(container, "pkg/OldName.class", buildSimpleClass("pkg/OldName"));
    auto applied = mjbase::asm_remapper::applyResourceRemapPlan(container, plan);
    ASSERT_FALSE(applied.is_err());
    EXPECT_TRUE(applied.unwrap());

    EXPECT_TRUE(container.getResource("pkg/OldName.class") != nullptr);
    EXPECT_TRUE(container.getResource("pkg/NewName.class") == nullptr);

    auto* resource = classResourceOf(container.getResource("pkg/OldName.class"));
    ASSERT_TRUE(resource != nullptr);
    auto cf = parseClass(resource->get());
    ASSERT_TRUE(cf);
    EXPECT_EQ("pkg/NewName", classTextOf(*cf, cf->thisClass));
}

// 旧例 13：remapsReflectionMemberAfterAliasWrapper
TEST(MjAsmRemapperTest, remapsReflectionMemberAfterAliasWrapper)
{
    MemberRemapPlan plan;
    plan.mapMethod("pkg/Target", "oldMethod", "()V", "pkg/Target", "newMethod", "()V");

    mjbase::model::ResourceContainer container;
    addClassBytes(container, "pkg/Caller.class", buildAliasReflectionClass());
    auto applied = mjbase::asm_remapper::applyResourceRemapPlan(
        container, plan, RemapPolicy::bytecodeAndReflectionStrings());
    ASSERT_FALSE(applied.is_err());
    EXPECT_TRUE(applied.unwrap());

    auto* resource = classResourceOf(container.getResource("pkg/Caller.class"));
    ASSERT_TRUE(resource != nullptr);
    auto session = openSession(resource->get());
    ASSERT_TRUE(session);
    for (const std::string_view method :
         {"use", "fieldUse", "containerUse", "instanceUse"}) {
        const std::vector<std::string> strings = ldcStringsOf(*session, method);
        EXPECT_TRUE(contains(strings, "newMethod")) << method;
        EXPECT_FALSE(contains(strings, "oldMethod")) << method;
    }
    // 偏离点：consumedQueueUse 的 poll→checkcast 队列消费形态，mjbase
    // AliasAnalysis 未建模（fail-open 保留原名）；旧栈别名引擎可证该接收者。
    // 等价能力见迁移报告「共享设施请求」。
    {
        const std::vector<std::string> strings = ldcStringsOf(*session, "consumedQueueUse");
        EXPECT_TRUE(contains(strings, "oldMethod"));
        EXPECT_FALSE(contains(strings, "newMethod"));
    }
}

// 旧例 14：containerRemapMovesClassResourceNamesWhenPolicyAllows
TEST(MjAsmRemapperTest, containerRemapMovesClassResourceNamesWhenPolicyAllows)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldName", "pkg/NewName");

    mjbase::model::ResourceContainer container;
    addClassBytes(container, "pkg/OldName.class", buildSimpleClass("pkg/OldName"));
    const Bytes textBytes{'O', 'l', 'd', 'N', 'a', 'm', 'e', '\n'};
    container.addResource("data.txt",
                          std::make_unique<mjbase::model::UnknownResource>("data.txt", textBytes));

    auto applied = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, RemapPolicy::all());
    ASSERT_FALSE(applied.is_err());
    EXPECT_TRUE(applied.unwrap());

    EXPECT_TRUE(container.getResource("pkg/OldName.class") == nullptr);
    auto* moved = classResourceOf(container.getResource("pkg/NewName.class"));
    ASSERT_TRUE(moved != nullptr);
    auto cf = parseClass(moved->get());
    ASSERT_TRUE(cf);
    EXPECT_EQ("pkg/NewName", classTextOf(*cf, cf->thisClass));

    auto* text = unknownResourceOf(container.getResource("data.txt"));
    ASSERT_TRUE(text != nullptr);
    EXPECT_EQ(textBytes, text->get());
}

// 旧例 15：containerRemapRewritesServiceMetadataWhenPolicyAllows
TEST(MjAsmRemapperTest, containerRemapRewritesServiceMetadataWhenPolicyAllows)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldName", "pkg/NewName");
    plan.mapClass("pkg/OldProvider", "pkg/NewProvider");

    const Bytes serviceBytes = toBytes("pkg/OldProvider\npkg/Other\n");
    auto buildContainer = [&serviceBytes] {
        mjbase::model::ResourceContainer container;
        container.addResource("META-INF/services/pkg/OldName",
                              std::make_unique<mjbase::model::UnknownResource>(
                                  "META-INF/services/pkg/OldName", serviceBytes));
        return container;
    };

    // 默认策略：服务文件完全不动。
    {
        auto container = buildContainer();
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan);
        ASSERT_FALSE(applied.is_err());
        EXPECT_FALSE(applied.unwrap());
        auto* service = unknownResourceOf(container.getResource("META-INF/services/pkg/OldName"));
        ASSERT_TRUE(service != nullptr);
        EXPECT_EQ(serviceBytes, service->get());
        EXPECT_TRUE(container.getResource("META-INF/services/pkg/NewName") == nullptr);
    }

    // resources 面：URI 与 provider 行各自改名；未映射行保持。
    {
        auto container = buildContainer();
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, RemapPolicy::all());
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        EXPECT_TRUE(container.getResource("META-INF/services/pkg/OldName") == nullptr);
        auto* service =
            unknownResourceOf(container.getResource("META-INF/services/pkg/NewName"));
        ASSERT_TRUE(service != nullptr);
        EXPECT_EQ(toBytes("pkg/NewProvider\npkg/Other\n"), service->get());
    }
}

// 旧例 16：containerRemapRewritesEmbeddedMappingTextWhenPolicyAllows
TEST(MjAsmRemapperTest, containerRemapRewritesEmbeddedMappingTextWhenPolicyAllows)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldName", "pkg/NewName");
    plan.mapClass("pkg/OldType", "pkg/NewType");
    plan.mapField("pkg/OldName", "oldField", "", "pkg/NewName", "renamedField", "");

    const Bytes mappingBytes = toBytes("pkg/OldName Lpkg/OldType;\npkg/OldName.oldField\n");
    mjbase::model::ResourceContainer container;
    container.addResource("mappings/old.tsrg",
                          std::make_unique<mjbase::model::UnknownResource>("mappings/old.tsrg",
                                                                           mappingBytes));

    auto applied = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, RemapPolicy::all());
    ASSERT_FALSE(applied.is_err());
    EXPECT_TRUE(applied.unwrap());
    auto* mapping = unknownResourceOf(container.getResource("mappings/old.tsrg"));
    ASSERT_TRUE(mapping != nullptr);
    // 映射文本语义：类 token 整体映射、描述符 token 重映射、成员 token 只随
    // owner 前缀映射（成员名是目标名，不做成员改名）。
    EXPECT_EQ(toBytes("pkg/NewName Lpkg/NewType;\npkg/NewName.oldField\n"), mapping->get());
}

// 旧例 17：containerRemapRewritesBusinessTextOnlyWhenExplicitlyAuthorized
TEST(MjAsmRemapperTest, containerRemapRewritesBusinessTextOnlyWhenExplicitlyAuthorized)
{
    MemberRemapPlan plan;
    plan.mapClass("OldName", "NewName");
    plan.mapClass("OldType", "NewType");

    const Bytes textBytes = toBytes("OldName and OldType\n");
    auto buildContainer = [&textBytes] {
        mjbase::model::ResourceContainer container;
        container.addResource("business.txt",
                              std::make_unique<mjbase::model::UnknownResource>("business.txt",
                                                                               textBytes));
        return container;
    };

    {
        auto container = buildContainer();
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, RemapPolicy::all());
        ASSERT_FALSE(applied.is_err());
        EXPECT_FALSE(applied.unwrap());
        auto* text = unknownResourceOf(container.getResource("business.txt"));
        ASSERT_TRUE(text != nullptr);
        EXPECT_EQ(textBytes, text->get());
    }

    {
        RemapPolicy  policy    = RemapPolicy::all();
        policy.authorizedTextResourceUris = {"business.txt"};
        auto      container = buildContainer();
        auto      applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, policy);
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        auto* text = unknownResourceOf(container.getResource("business.txt"));
        ASSERT_TRUE(text != nullptr);
        EXPECT_EQ(toBytes("NewName and NewType\n"), text->get());
    }
}

// 旧例 18：containerRemapRewritesManifestClassAttributesWhenPolicyAllows
TEST(MjAsmRemapperTest, containerRemapRewritesManifestClassAttributesWhenPolicyAllows)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldMain", "pkg/NewMain");
    plan.mapClass("pkg/OldAgent", "pkg/NewAgent");
    plan.mapClass("pkg/OldPremain", "pkg/NewPremain");
    plan.mapClass("pkg/OldFx", "pkg/NewFx");
    plan.mapClass("pkg/OldOther", "pkg/NewOther");

    const Bytes manifestBytes = toBytes(
        "Manifest-Version: 1.0\r\nMain-Class: pkg.OldMain\r\nLauncher-Agent-Class: "
        "pkg.OldAgent\r\nPremain-Class: pkg.OldPremain\r\nAgent-Class: pkg.OldAgent\r\n"
        "JavaFX-Application-Class: pkg.OldFx\r\nX-Other: pkg.OldOther\r\n\r\n");
    auto buildContainer = [&manifestBytes] {
        mjbase::model::ResourceContainer container;
        container.addResource(
            mjbase::jar::JarUtil::kManifestFileName,
            std::make_unique<mjbase::model::ManifestResource>(
                mjbase::model::ManifestResource::fromBytes(manifestBytes)));
        return container;
    };

    {
        auto container = buildContainer();
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan);
        ASSERT_FALSE(applied.is_err());
        EXPECT_FALSE(applied.unwrap());
        auto* manifestResource = manifestResourceOf(container.getResource(
            mjbase::jar::JarUtil::kManifestFileName));
        ASSERT_TRUE(manifestResource != nullptr);
        EXPECT_EQ("pkg.OldMain", manifestText(manifestResource->get(), "Main-Class"));
    }

    {
        auto container = buildContainer();
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, RemapPolicy::all());
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        auto* manifestResource = manifestResourceOf(container.getResource(
            mjbase::jar::JarUtil::kManifestFileName));
        ASSERT_TRUE(manifestResource != nullptr);
        const mjbase::jar::Manifest& manifest = manifestResource->get();
        EXPECT_EQ("pkg.NewMain", manifestText(manifest, "Main-Class"));
        EXPECT_EQ("pkg.NewAgent", manifestText(manifest, "Launcher-Agent-Class"));
        EXPECT_EQ("pkg.NewPremain", manifestText(manifest, "Premain-Class"));
        EXPECT_EQ("pkg.NewAgent", manifestText(manifest, "Agent-Class"));
        EXPECT_EQ("pkg.NewFx", manifestText(manifest, "JavaFX-Application-Class"));
        // 非 class 形主属性不做类名口径改写。
        EXPECT_EQ("pkg.OldOther", manifestText(manifest, "X-Other"));
    }
}

// 旧例 19：resourceSubSwitchesGateManifestAndServiceLoader
TEST(MjAsmRemapperTest, resourceSubSwitchesGateManifestAndServiceLoader)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/OldName", "pkg/NewName");
    plan.mapClass("pkg/OldProvider", "pkg/NewProvider");

    const Bytes serviceBytes = toBytes("pkg/OldProvider\n");
    auto buildContainer = [&serviceBytes] {
        mjbase::model::ResourceContainer container;
        container.addResource("META-INF/services/pkg/OldName",
                              std::make_unique<mjbase::model::UnknownResource>(
                                  "META-INF/services/pkg/OldName", serviceBytes));
        const Bytes manifestBytes = toBytes("Manifest-Version: 1.0\r\nMain-Class: pkg.OldName\r\n\r\n");
        container.addResource(
            mjbase::jar::JarUtil::kManifestFileName,
            std::make_unique<mjbase::model::ManifestResource>(
                mjbase::model::ManifestResource::fromBytes(manifestBytes)));
        return container;
    };

    // resources=true 但两个细分门全关：主属性与服务文件都保持。
    {
        RemapPolicy  policy = RemapPolicy::all();
        policy.manifest      = false;
        policy.serviceLoader = false;
        auto container      = buildContainer();
        auto applied        = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, policy);
        ASSERT_FALSE(applied.is_err());
        EXPECT_FALSE(applied.unwrap());
        auto* manifestResource = manifestResourceOf(container.getResource(
            mjbase::jar::JarUtil::kManifestFileName));
        ASSERT_TRUE(manifestResource != nullptr);
        EXPECT_EQ("pkg.OldName", manifestText(manifestResource->get(), "Main-Class"));
        auto* service =
            unknownResourceOf(container.getResource("META-INF/services/pkg/OldName"));
        ASSERT_TRUE(service != nullptr);
        EXPECT_EQ(serviceBytes, service->get());
    }

    // 只开 manifest：主属性改名，服务文件保持。
    {
        RemapPolicy  policy = RemapPolicy::all();
        policy.serviceLoader = false;
        auto container      = buildContainer();
        auto applied        = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, policy);
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        auto* manifestResource = manifestResourceOf(container.getResource(
            mjbase::jar::JarUtil::kManifestFileName));
        ASSERT_TRUE(manifestResource != nullptr);
        EXPECT_EQ("pkg.NewName", manifestText(manifestResource->get(), "Main-Class"));
        auto* service =
            unknownResourceOf(container.getResource("META-INF/services/pkg/OldName"));
        ASSERT_TRUE(service != nullptr);
        EXPECT_EQ(serviceBytes, service->get());
    }

    // 只开 serviceLoader：服务文件改名，主属性保持。
    {
        RemapPolicy  policy = RemapPolicy::all();
        policy.manifest      = false;
        auto container      = buildContainer();
        auto applied        = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, policy);
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        auto* manifestResource = manifestResourceOf(container.getResource(
            mjbase::jar::JarUtil::kManifestFileName));
        ASSERT_TRUE(manifestResource != nullptr);
        EXPECT_EQ("pkg.OldName", manifestText(manifestResource->get(), "Main-Class"));
        EXPECT_TRUE(container.getResource("META-INF/services/pkg/OldName") == nullptr);
        auto* service =
            unknownResourceOf(container.getResource("META-INF/services/pkg/NewName"));
        ASSERT_TRUE(service != nullptr);
        EXPECT_EQ(toBytes("pkg/NewProvider\n"), service->get());
    }
}

// 旧例 20：resourceRemapperKeepsAllPolicySurfacesIndependent
TEST(MjAsmRemapperTest, resourceRemapperKeepsAllPolicySurfacesIndependent)
{
    MemberRemapPlan plan;
    plan.mapClass("OldOwner", "NewOwner");
    plan.mapClass("OldType", "NewType");
    plan.mapClass("OldIface", "NewIface");
    plan.mapClass("pkg/OldReflect", "pkg/NewReflect");
    plan.mapClass("pkg/OldNoise", "pkg/NewNoise");
    plan.mapClass("OldGeneric", "NewGeneric");
    plan.mapClass("OldMethodGeneric", "NewMethodGeneric");
    plan.mapClass("OldAnno", "NewAnno");
    plan.mapMethod("OldOwner", "oldMethod", "(LOldType;)LOldType;", "NewOwner", "newMethod",
                   "(LNewType;)LNewType;");
    plan.mapField("OldOwner", "oldField", "LOldType;", "NewOwner", "newField", "LNewType;");

    const Bytes bytes = buildRemapClass();

    RemapPolicy p1 = RemapPolicy::bytecodeOnly();
    RemapPolicy p2 = p1;
    p2.reflectionStrings = true;
    RemapPolicy p3 = p2;
    p3.annotations = true;
    RemapPolicy p4 = p3;
    p4.signatures  = true;

    // p1：仅字节码面——类树改名；字符串/签名/注解保持。
    {
        mjbase::model::ResourceContainer container;
        addClassBytes(container, "OldOwner.class", bytes);
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, p1);
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        auto* resource = classResourceOf(container.getResource("OldOwner.class"));
        ASSERT_TRUE(resource != nullptr);
        auto session = openSession(resource->get());
        ASSERT_TRUE(session);
        EXPECT_TRUE(contains(ldcStringsOf(*session, "use"), "pkg.OldReflect"));
        auto cf = parseClass(resource->get());
        ASSERT_TRUE(cf);
        EXPECT_EQ("NewOwner", classTextOf(*cf, cf->thisClass));
        EXPECT_EQ("Ljava/lang/Object;LOldGeneric;", signatureOf(*cf, cf->attrs));
        const jbc::core::AttrSlot* annoSlot = slotOf(*cf, cf->attrs, "RuntimeVisibleAnnotations");
        ASSERT_TRUE(annoSlot != nullptr);
        EXPECT_EQ("LOldAnno;", annotationTypeText(*cf, *annoSlot));
    }
    // p2：+反射字符串。
    {
        mjbase::model::ResourceContainer container;
        addClassBytes(container, "OldOwner.class", bytes);
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, p2);
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        auto* resource = classResourceOf(container.getResource("OldOwner.class"));
        ASSERT_TRUE(resource != nullptr);
        auto session = openSession(resource->get());
        ASSERT_TRUE(session);
        EXPECT_TRUE(contains(ldcStringsOf(*session, "use"), "pkg.NewReflect"));
        auto cf = parseClass(resource->get());
        ASSERT_TRUE(cf);
        EXPECT_EQ("Ljava/lang/Object;LOldGeneric;", signatureOf(*cf, cf->attrs));
    }
    // p3：+注解（签名仍保持）。
    {
        mjbase::model::ResourceContainer container;
        addClassBytes(container, "OldOwner.class", bytes);
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, p3);
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        auto* resource = classResourceOf(container.getResource("OldOwner.class"));
        ASSERT_TRUE(resource != nullptr);
        auto cf = parseClass(resource->get());
        ASSERT_TRUE(cf);
        const jbc::core::AttrSlot* annoSlot = slotOf(*cf, cf->attrs, "RuntimeVisibleAnnotations");
        ASSERT_TRUE(annoSlot != nullptr);
        EXPECT_EQ("LNewAnno;", annotationTypeText(*cf, *annoSlot));
        EXPECT_EQ("Ljava/lang/Object;LOldGeneric;", signatureOf(*cf, cf->attrs));
    }
    // p4：+签名。
    {
        mjbase::model::ResourceContainer container;
        addClassBytes(container, "OldOwner.class", bytes);
        auto applied   = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, p4);
        ASSERT_FALSE(applied.is_err());
        EXPECT_TRUE(applied.unwrap());
        auto* resource = classResourceOf(container.getResource("OldOwner.class"));
        ASSERT_TRUE(resource != nullptr);
        auto cf = parseClass(resource->get());
        ASSERT_TRUE(cf);
        EXPECT_EQ("Ljava/lang/Object;LNewGeneric;", signatureOf(*cf, cf->attrs));
    }
}

// 旧例 21：containerRemapPreservesDebugMetadata
TEST(MjAsmRemapperTest, containerRemapPreservesDebugMetadata)
{
    MemberRemapPlan plan;
    plan.mapClass("pkg/Debug", "pkg/Release");
    plan.mapMethod("pkg/Debug", "debug", "(I)V", "pkg/Release", "shown", "(I)V");

    mjbase::model::ResourceContainer container;
    addClassBytes(container, "pkg/Debug.class", buildDebugClass());
    auto applied = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, RemapPolicy::all());
    ASSERT_FALSE(applied.is_err());
    EXPECT_TRUE(applied.unwrap());

    auto* resource = classResourceOf(container.getResource("pkg/Release.class"));
    ASSERT_TRUE(resource != nullptr);
    auto cf = parseClass(resource->get());
    ASSERT_TRUE(cf);
    EXPECT_EQ("pkg/Release", classTextOf(*cf, cf->thisClass));

    const MemberInfo* shown = methodOf(*cf, "shown");
    ASSERT_TRUE(shown != nullptr);
    const jbc::core::CodeAttr* code = codeOf(*cf, const_cast<MemberInfo&>(*shown));
    ASSERT_TRUE(code != nullptr);

    const jbc::core::AttrSlot* lntSlot = slotOf(*cf, code->attrs, "LineNumberTable");
    ASSERT_TRUE(lntSlot != nullptr);
    const auto* lnt = ca::core::tag_cast::cast<const jbc::core::LineNumberTableAttr>(lntSlot->peek());
    ASSERT_TRUE(lnt != nullptr);
    ASSERT_EQ(1U, lnt->rows.size());
    EXPECT_EQ(123U, lnt->rows[0].line);

    const std::vector<LvtRowText> rows = localVariablesOf(*cf, *code);
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ("arg", rows[0].name);
    EXPECT_EQ("I", rows[0].desc);
    EXPECT_EQ(0U, rows[0].slot);
}

// ---------------------------------------------------------------------------
// MappingRemapper（utf8_remapper 去 visitor 重设计）补充用例。
// ---------------------------------------------------------------------------

TEST(MjAsmRemapperTest, mappingRemapperMapsClassAndMemberTexts)
{
    using Ref = ca::str::Utf8StringRef;
    mjbase::mapping::IntermediateMapping mappings;
    mappings.addClass(Ref::from_cstr("OldName"), Ref::from_cstr("NewName"));
    mappings.addClass(Ref::from_cstr("OldType"), Ref::from_cstr("NewType"));
    mappings.addField(Ref::from_cstr("OldName"), Ref::from_cstr("LOldType;"),
                      Ref::from_cstr("oldField"), Ref::from_cstr("newField"));
    mappings.addMethod(Ref::from_cstr("OldName"), Ref::from_cstr("()V"),
                       Ref::from_cstr("oldRun"), Ref::from_cstr("newRun"));

    MappingRemapper remapper(mappings);
    EXPECT_TRUE(remapper.hasMappings());
    EXPECT_EQ("NewName", std::string(remapper.map("OldName")));
    EXPECT_EQ("NewType", std::string(remapper.map("OldType")));
    EXPECT_TRUE(remapper.map("Unknown").empty());
    EXPECT_EQ("newField", std::string(remapper.mapField("OldName", "oldField", "LOldType;")));
    EXPECT_TRUE(remapper.mapField("OldName", "missing", "LOldType;").empty());
    EXPECT_EQ("newRun", std::string(remapper.mapMethod("OldName", "oldRun", "()V")));
    EXPECT_TRUE(remapper.mapMethod("OldName", "oldRun", "(I)V").empty());
}

TEST(MjAsmRemapperTest, mappingRemapperRewritesDescriptorsAndSignatures)
{
    using Ref = ca::str::Utf8StringRef;
    mjbase::mapping::IntermediateMapping mappings;
    mappings.addClass(Ref::from_cstr("OldOwner"), Ref::from_cstr("NewOwner"));
    mappings.addClass(Ref::from_cstr("OldType"), Ref::from_cstr("NewType"));

    MappingRemapper remapper(mappings);
    EXPECT_EQ("LNewType;", std::string(remapper.mapFieldDescriptor("LOldType;")));
    EXPECT_EQ("[LNewType;", std::string(remapper.mapFieldDescriptor("[LOldType;")));
    EXPECT_EQ("(LNewType;)LNewType;",
              std::string(remapper.mapMethodDescriptor("(LOldType;)LOldType;")));
    EXPECT_EQ("(LNewOwner;)LNewOwner;",
              std::string(remapper.mapMethodDescriptor("(LOldOwner;)LOldOwner;")));
    // 非描述符文本原样返回（旧栈 Utf8Remapper 空命中语义）。
    EXPECT_EQ("I", std::string(remapper.mapFieldDescriptor("I")));
    EXPECT_EQ("Ljava/lang/Object;LNewType;",
              std::string(remapper.mapSignature("Ljava/lang/Object;LOldType;")));
    EXPECT_EQ("<T:Ljava/lang/Object;>(LNewType;)LNewOwner;",
              std::string(remapper.mapSignature("<T:Ljava/lang/Object;>(LOldType;)LOldOwner;")));
    EXPECT_TRUE(remapper.mapSignature("").empty());
}

}   // namespace
