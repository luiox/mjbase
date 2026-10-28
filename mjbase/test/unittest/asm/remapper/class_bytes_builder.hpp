/// @file        class_bytes_builder.hpp
/// @brief       remapper 测试共用的手拼 classfile 汇编器。
///
/// 与 reference 测试的 class_bytes_builder.hpp 同法（CP 惰性驻留、两遍装配
/// 保证 cpCount 与索引稳定），扩展 remapper 夹具需要的面：方法级 Exceptions
/// 属性、Code 嵌套 LineNumberTable/LocalVariableTable/LocalVariableTypeTable/
/// StackMapTable、CONSTANT_Dynamic（condy）与 'C'（Class）形 BSM 实参、
/// module-info 形态（ACC_MODULE / 无父类）。每个字节受控，不依赖本地语料。
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

namespace mjbase::asm_remapper::test_support {

using Bytes = std::vector<ca::u8>;

inline void putU8(Bytes& b, ca::u8 v) { b.push_back(v); }

inline void putU16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

inline void putU32(Bytes& b, ca::u32 v)
{
    b.push_back(static_cast<ca::u8>(v >> 24));
    b.push_back(static_cast<ca::u8>(v >> 16));
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

inline void putUtf8Raw(Bytes& b, std::string_view s)
{
    for (const char c : s) {
        b.push_back(static_cast<ca::u8>(c));
    }
}

/// 异常表条目：start/end/handler 为标签 id（bindLabel 落 pc 后回填）；
/// catchType 为类内部名，空串 = any（catch_type 0）。
struct ExcSpec
{
    std::size_t      start   = 0;
    std::size_t      end     = 0;
    std::size_t      handler = 0;
    std::string_view catchType;
};

/// 行号表行：line + 标签 id。
struct LineSpec
{
    ca::u32     line  = 0;
    std::size_t label = 0;
};

/// 局部变量行：sig 空 = LocalVariableTable 行；sig 非空 = LVTT 行（desc=签名）。
struct LvtSpec
{
    std::string_view name;
    std::string_view desc;
    std::string_view signature;   ///< 空 = LVT；非空 = LVTT
    std::size_t      start = 0;
    std::size_t      end   = 0;
    ca::u16          slot  = 0;
};

/// StackMapTable Object 校验类型：tag=7（Object）+ 类内部名/数组描述符。
struct VTypeSpec
{
    std::string_view className;   ///< 空视图 = Top（tag 0）
};

/// 全帧（tag 255）规格：offset 0、locals/stack 序列。
struct FullFrameSpec
{
    std::vector<VTypeSpec> locals;
    std::vector<VTypeSpec> stack;
};

/// 注解元素值（'c'：返回描述符形态的 Utf8 载荷，JVMS 4.7.16.1；'['：数组
/// 载荷 items；'@'：嵌套注解载荷 nested——注解数组/嵌套注解边角用）。
struct ElementValue
{
    char                      tag    = 'c';
    std::string_view          value  = {};   ///< 'c': 返回描述符文本（如 "Lcom/Old;"）
    std::vector<ElementValue> items;         ///< '[': 逐项元素值
    const struct AnnotationSpec* nested = nullptr;   ///< '@': 嵌套注解（借用）
};

struct AnnotationPair
{
    std::string_view name;
    ElementValue     value;
};

/// 注解规格：type 为描述符形态（"Lcom/example/Anno;"）。
struct AnnotationSpec
{
    std::string_view            type;
    std::vector<AnnotationPair> pairs;
};

/// type annotation 规格（JVMS 4.7.20）：target_type + target_info 载荷 +
/// type_path + 注解体。载荷支持：0x13/0x14/0x15（空）、0x00/0x01（u1）、
/// 0x11/0x12（u1+u1）、0x10/0x17/0x42/0x43..0x46（u2）、0x47/0x48..0x4B
/// （u2+u1）；0x40/0x41（localvar）夹具不支持。
struct TypeAnnotationSpec
{
    ca::u8                                 targetType = 0x13;   ///< 空载荷 = 字段类型位
    ca::u8                                 index8     = 0;   ///< u1 载荷（type_parameter/类型实参下标等）
    ca::u8                                 boundIndex = 0;   ///< 0x11/0x12 的第二 u1（bound_index）
    ca::u16                                index16    = 0;   ///< u2 载荷（supertype/throws/catch/offset）
    std::vector<std::pair<ca::u8, ca::u8>> typePath;   ///< (kind, type_argument_index)
    AnnotationSpec anno;
};

/// Record 组件规格。
struct RecordComponentSpec
{
    std::string_view                        name;
    std::string_view                        desc;
    std::string_view                        signature;   ///< 空 = 无 Signature 属性
    std::vector<AnnotationSpec>             annotations; ///< 组件 RuntimeVisibleAnnotations
    std::vector<TypeAnnotationSpec>         typeAnnotations;   ///< 组件 RuntimeVisibleTypeAnnotations
};

/// Bootstrap 方法实参：'M' = MethodType(desc)；'H' = MethodHandle；'C' = Class。
struct BsmArg
{
    char             tag        = 'M';
    std::string_view desc;              ///< 'M' 载荷
    ca::u16          handleKind = 6;    ///< 'H' 载荷（H_INVOKESTATIC）
    std::string_view owner;
    std::string_view name;
    std::string_view handleDesc;
};

/// 方法装配规格：emit 为空 = 无 Code 属性（abstract/native）。
struct MethodSpec
{
    ca::u16                              access = 0x0009;   // public static
    std::string_view                     name;
    std::string_view                     desc;
    std::string_view                     signature;   ///< 空 = 无 Signature 属性
    ca::u16                              maxStack  = 1;
    ca::u16                              maxLocals = 1;
    std::function<void(class CodeEmit&)> emit;
    std::vector<ExcSpec>                 excTable;      ///< Code 异常表
    std::vector<std::string_view>        exceptions;    ///< Exceptions 属性
    std::vector<LineSpec>                lineNumbers;   ///< LNT
    std::vector<LvtSpec>                 localVariables;
    std::vector<FullFrameSpec>           stackMapFrames;
    std::vector<AnnotationSpec>          annotations;   ///< RuntimeVisibleAnnotations
    std::vector<TypeAnnotationSpec>      typeAnnotations;   ///< RuntimeVisibleTypeAnnotations
    std::vector<TypeAnnotationSpec>      codeTypeAnnotations;   ///< Code 嵌套 RVTA
    std::vector<std::vector<AnnotationSpec>> parameterAnnotations;  ///< RVPA 行表
    ElementValue                         annotationDefault;   ///< AnnotationDefault 载荷
    bool                                 hasAnnotationDefault = false;
};

struct FieldSpec
{
    ca::u16                            access = 0x0002;   // private
    std::string_view                   name;
    std::string_view                   desc;
    std::string_view                   signature;   ///< 空 = 无 Signature 属性
    std::vector<AnnotationSpec>        annotations; ///< RuntimeVisibleAnnotations
    std::vector<TypeAnnotationSpec>    typeAnnotations;   ///< RuntimeVisibleTypeAnnotations
};

/// 单方法代码发射器：标签先占号后绑定，branch 2 字节相对偏移装配末回填。
class ClassBytesBuilder;   ///< 前置声明：CodeEmit 以裸指针借用 builder（定义在后）

class CodeEmit
{
public:
    CodeEmit(ClassBytesBuilder* owner, Bytes* code)
        : owner_(owner)
        , code_(code)
    {}

    // CP 句柄（两遍幂等）。
    ca::u16 utf8(std::string_view s) const;
    ca::u16 cls(std::string_view n) const;
    ca::u16 fieldRef(std::string_view o, std::string_view n, std::string_view d) const;
    ca::u16 methodRef(std::string_view o, std::string_view n, std::string_view d) const;
    ca::u16 ifaceMethodRef(std::string_view o, std::string_view n, std::string_view d) const;

    // 裸字节出口。
    void u8(ca::u8 v) { putU8(*code_, v); }
    void u16(ca::u16 v) { putU16(*code_, v); }

    void op(jbc::core::Op o) { putU8(*code_, static_cast<ca::u8>(o)); }

    /// 变址指令（u8 索引；夹具不含 wide 形态）。
    void varInsn(jbc::core::Op o, ca::u16 idx)
    {
        op(o);
        u8(static_cast<ca::u8>(idx));
    }

    /// 类型指令：NEW/ANEWARRAY/CHECKCAST/INSTANCEOF（2 字节 Class 索引）。
    void typeInsn(jbc::core::Op o, std::string_view internalName)
    {
        op(o);
        u16(cls(internalName));
    }

    /// 字段指令：GETSTATIC/PUTSTATIC/GETFIELD/PUTFIELD。
    void fieldInsn(jbc::core::Op o, std::string_view owner, std::string_view name,
                   std::string_view desc)
    {
        op(o);
        u16(fieldRef(owner, name, desc));
    }

    /// invoke virtual/special/static（methodref）。
    void invoke(jbc::core::Op o, std::string_view owner, std::string_view name,
                std::string_view desc)
    {
        op(o);
        u16(methodRef(owner, name, desc));
    }

    /// invokeinterface（iface methodref + count + 0）。
    void invokeInterface(std::string_view owner, std::string_view name, std::string_view desc,
                         ca::u8 count)
    {
        op(jbc::core::Op::INVOKEINTERFACE);
        u16(ifaceMethodRef(owner, name, desc));
        u8(count);
        u8(0);
    }

    /// multianewarray（数组描述符 Class 索引 + 维度）。
    void multiANewArray(std::string_view arrayDesc, ca::u8 dims)
    {
        op(jbc::core::Op::MULTIANEWARRAY);
        u16(cls(arrayDesc));
        u8(dims);
    }

    /// LDC String 载荷（夹具 CP 规模小，恒 1 字节索引）。
    void ldcstring(std::string_view s);
    /// LDC 类常量（内部名或数组描述符）。
    void ldcClass(std::string_view internalName);
    void ldcInteger(ca::i32 v);
    /// LDC MethodHandle 载荷。
    void ldcMethodHandle(ca::u16 kind, std::string_view owner, std::string_view name,
                         std::string_view desc);
    /// LDC MethodType 载荷。
    void ldcMethodType(std::string_view desc);
    /// LDC CONSTANT_Dynamic 载荷（tag 17：bsmIndex + NameAndType(name, desc)）。
    void ldcDynamic(ca::u16 bsmIndex, std::string_view name, std::string_view desc);

    /// int 常量：-1..5 走 iconst_n，其余走 LDC。
    void i32const(ca::i32 v)
    {
        if (v >= -1 && v <= 5) {
            op(v < 0 ? jbc::core::Op::ICONST_M1
                     : static_cast<jbc::core::Op>(static_cast<ca::u8>(jbc::core::Op::ICONST_0) +
                                                  static_cast<ca::u8>(v)));
            return;
        }
        ldcInteger(v);
    }

    /// 分支/跳转（2 字节相对偏移，装配末回填；JSR/GOTO/IFEQ 同形）。
    void branch(jbc::core::Op o, std::size_t label)
    {
        op(o);
        patches_.push_back({code_->size(), label});
        u8(0);
        u8(0);
    }

    /// INVOKEDYNAMIC（indy CP + 两字节 0；bsmIndex 为 addBootstrapMethod 序号）。
    void invokedynamic(ca::u16 bsmIndex, std::string_view name, std::string_view desc);

    std::size_t newLabel()
    {
        labels_.push_back(kUnbound);
        return labels_.size() - 1;
    }

    void bindLabel(std::size_t label)
    {
        if (label >= labels_.size()) {
            labels_.resize(label + 1, kUnbound);
        }
        labels_[label] = static_cast<ca::u32>(code_->size());
    }

    /// 回填全部 branch 偏移（相对分支指令自身 pc）；异常表经 labelPc 读取。
    void finish()
    {
        for (const auto& p : patches_) {
            ASSERT_LT(p.label, labels_.size());
            ASSERT_NE(labels_[p.label], kUnbound);
            const ca::i16 off  = static_cast<ca::i16>(labels_[p.label] - static_cast<ca::u32>(p.at - 1));
            (*code_)[p.at]     = static_cast<ca::u8>(static_cast<ca::u16>(off) >> 8);
            (*code_)[p.at + 1] = static_cast<ca::u8>(static_cast<ca::u16>(off) & 0xFF);
        }
        finished_ = true;
    }

    /// 标签落点 pc（finish 后读取；异常表/调试表装配用）。
    ca::u16 labelPc(std::size_t label) const
    {
        EXPECT_TRUE(finished_);
        EXPECT_LT(label, labels_.size());
        EXPECT_NE(labels_[label], kUnbound);
        return static_cast<ca::u16>(labels_[label]);
    }

private:
    struct Patch
    {
        std::size_t at;
        std::size_t label;
    };
    static constexpr ca::u32 kUnbound = 0xFFFFFFFFu;

    ClassBytesBuilder*   owner_ = nullptr;
    Bytes*               code_  = nullptr;
    std::vector<ca::u32> labels_;
    std::vector<Patch>   patches_;
    bool                 finished_ = false;
};

/// 手工拼最小 classfile（V1_8 / 默认 public super / super=java/lang/Object）。
class ClassBytesBuilder
{
public:
    explicit ClassBytesBuilder(std::string_view thisClass)
    {
        thisIdx_  = cls(thisClass);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
        sigName_  = utf8("Signature");
        annoName_ = utf8("RuntimeVisibleAnnotations");
    }

    // CP 驻留（CodeEmit 与字段/方法/属性装配共用；两遍幂等）。
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
        return intern(utf8s_.emplace(std::move(key), 0));
    }

    ca::u16 cls(std::string_view n)
    {
        std::string key(n);
        const auto  it = classes_.find(key);
        if (it != classes_.end()) {
            return it->second;
        }
        const ca::u16 u = utf8(n);
        putU8(cp_, 0x07);
        putU16(cp_, u);
        return intern(classes_.emplace(std::move(key), 0));
    }

    ca::u16 stringCp(std::string_view s)
    {
        std::string key(s);
        const auto  it = strings_.find(key);
        if (it != strings_.end()) {
            return it->second;
        }
        const ca::u16 u = utf8(s);
        putU8(cp_, 0x08);
        putU16(cp_, u);
        return intern(strings_.emplace(std::move(key), 0));
    }

    ca::u16 integerCp(ca::i32 v)
    {
        const auto it = integers_.find(v);
        if (it != integers_.end()) {
            return it->second;
        }
        putU8(cp_, 0x03);
        putU32(cp_, static_cast<ca::u32>(v));
        return intern(integers_.emplace(v, 0));
    }

    ca::u16 nat(std::string_view name, std::string_view desc)
    {
        std::string key = std::string(name) + "." + std::string(desc);
        const auto  it = nats_.find(key);
        if (it != nats_.end()) {
            return it->second;
        }
        const ca::u16 un = utf8(name);
        const ca::u16 ud = utf8(desc);
        putU8(cp_, 0x0C);
        putU16(cp_, un);
        putU16(cp_, ud);
        return intern(nats_.emplace(std::move(key), 0));
    }

    /// tag：0x09 Fieldref / 0x0A Methodref / 0x0B InterfaceMethodref。
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
        const ca::u16 uc = cls(owner);
        const ca::u16 un = nat(name, desc);
        putU8(cp_, tag);
        putU16(cp_, uc);
        putU16(cp_, un);
        return intern(refs_.emplace(std::move(key), 0));
    }

    ca::u16 fieldRef(std::string_view o, std::string_view n, std::string_view d)
    {
        return ref(0x09, o, n, d);
    }
    ca::u16 methodRef(std::string_view o, std::string_view n, std::string_view d)
    {
        return ref(0x0A, o, n, d);
    }
    ca::u16 ifaceMethodRef(std::string_view o, std::string_view n, std::string_view d)
    {
        return ref(0x0B, o, n, d);
    }

    /// MethodHandle 常量（tag 0x0F；kind 1-4 fieldref，5-9 methodref）。
    ca::u16 methodHandleCp(ca::u16 kind, std::string_view owner, std::string_view name,
                           std::string_view desc)
    {
        std::string key = "H." + std::to_string(kind) + "." + std::string(owner) + "." +
                          std::string(name) + "." + std::string(desc);
        const auto it = handles_.find(key);
        if (it != handles_.end()) {
            return it->second;
        }
        const ca::u16 r = kind <= 4 ? fieldRef(owner, name, desc)
                                    : methodRef(owner, name, desc);
        putU8(cp_, 0x0F);
        putU8(cp_, static_cast<ca::u8>(kind));   // JVMS 4.4.8：reference_kind 是 u1
        putU16(cp_, r);
        return intern(handles_.emplace(std::move(key), 0));
    }

    /// MethodType 常量（tag 0x10）。
    ca::u16 methodTypeCp(std::string_view desc)
    {
        std::string key(desc);
        const auto  it = methodTypes_.find(key);
        if (it != methodTypes_.end()) {
            return it->second;
        }
        const ca::u16 u = utf8(desc);
        putU8(cp_, 0x10);
        putU16(cp_, u);
        return intern(methodTypes_.emplace(std::move(key), 0));
    }

    /// CONSTANT_Dynamic 常量（tag 0x11；bsmIndex = addBootstrapMethod 序号）。
    ca::u16 dynamicCp(ca::u16 bsmIndex, std::string_view name, std::string_view desc)
    {
        std::string key = "D." + std::to_string(bsmIndex) + "." + std::string(name) + "." +
                          std::string(desc);
        const auto it = dynamics_.find(key);
        if (it != dynamics_.end()) {
            return it->second;
        }
        const ca::u16 n = nat(name, desc);
        putU8(cp_, 0x11);
        putU16(cp_, bsmIndex);
        putU16(cp_, n);
        return intern(dynamics_.emplace(std::move(key), 0));
    }

    /// InvokeDynamic 常量（tag 0x12；bsmIndex = addBootstrapMethod 序号）。
    ca::u16 indyCp(ca::u16 bsmIndex, std::string_view name, std::string_view desc)
    {
        std::string key = "I." + std::to_string(bsmIndex) + "." + std::string(name) + "." +
                          std::string(desc);
        const auto it = indys_.find(key);
        if (it != indys_.end()) {
            return it->second;
        }
        const ca::u16 n = nat(name, desc);
        putU8(cp_, 0x12);
        putU16(cp_, bsmIndex);
        putU16(cp_, n);
        return intern(indys_.emplace(std::move(key), 0));
    }

    // ── 类头塑形 ──

    void setAccess(ca::u16 access) { access_ = access; }
    /// 无父类条目（module-info 形态；super_index = 0）。
    void setNoSuper() { hasSuper_ = false; }
    /// 自定义父类（默认 java/lang/Object）。
    void setSuper(std::string_view name) { superIdx_ = cls(name); }
    void setInterfaces(std::vector<std::string_view> ifaces) { interfaces_ = std::move(ifaces); }

    // ── 类级属性 ──

    void setClassSignature(std::string_view sig) { classSignature_ = sig; }
    void addClassAnnotation(const AnnotationSpec& anno) { classAnnotations_.push_back(anno); }
    void addClassTypeAnnotation(const TypeAnnotationSpec& ta)
    {
        classTypeAnnotations_.push_back(ta);
    }
    void setEnclosingMethod(std::string_view owner, std::string_view name,
                            std::string_view desc)
    {
        enclosing_ = std::make_tuple(owner, name, desc);
    }
    void setNestHost(std::string_view host) { nestHost_ = host; }
    void addNestMember(std::string_view member) { nestMembers_.push_back(member); }
    void addPermittedSubclass(std::string_view sub) { permittedSubclasses_.push_back(sub); }
    void addInnerClass(std::string_view inner, std::string_view outer,
                       std::string_view innerName, ca::u16 access)
    {
        innerClasses_.emplace_back(inner, outer, innerName, access);
    }
    void addRecordComponent(const RecordComponentSpec& component)
    {
        recordComponents_.push_back(component);
    }
    void setModule(std::string_view moduleName, std::string_view mainClass,
                   std::vector<std::string_view> uses,
                   std::vector<std::pair<std::string_view, std::vector<std::string_view>>>
                       provides)
    {
        module_         = moduleName;
        moduleMain_     = mainClass;
        moduleUses_     = std::move(uses);
        moduleProvides_ = std::move(provides);
    }

    /// 登记一个 Bootstrap 方法，返回其 bsm 序号（invokedynamic/condy 引用）。
    ca::u16 addBootstrapMethod(std::string_view owner, std::string_view name,
                               std::string_view desc, std::vector<BsmArg> args)
    {
        bootstrapMethods_.push_back(BsmSpec{owner, name, desc, std::move(args)});
        return static_cast<ca::u16>(bootstrapMethods_.size() - 1);
    }

    void addField(const FieldSpec& f)
    {
        utf8(f.name);
        utf8(f.desc);
        fields_.push_back(f);
    }

    Bytes assemble(const std::vector<MethodSpec>& methods)
    {
        // 两遍装配：第一遍只为了 CP 驻留（emit 回调幂等），第二遍出真字节。
        for (const MethodSpec& m : methods) {
            if (m.emit) {
                Bytes    scratch;
                CodeEmit e(this, &scratch);
                m.emit(e);
            }
        }

        // 方法名/描述符与异常捕获类型、接口在此预驻留：它们只在装配段引用，
        // 若惰性驻留会落在 cpCount/cp 字节写完之后（CP 永远缺条目）。
        for (const MethodSpec& meth : methods) {
            utf8(meth.name);
            utf8(meth.desc);
            for (const ExcSpec& x : meth.excTable) {
                if (!x.catchType.empty()) cls(x.catchType);
            }
            for (const std::string_view ex : meth.exceptions) {
                cls(ex);
            }
            for (const LvtSpec& row : meth.localVariables) {
                utf8(row.name);
                utf8(row.desc);
            }
        }
        for (const std::string_view iface : interfaces_) {
            cls(iface);
        }

        // 属性字节整体先建（其中引用的 CP 由此全部驻留），再写文件。
        std::vector<AttrBytes> classAttrs = buildClassAttrs();
        std::vector<std::vector<AttrBytes>> fieldAttrs;
        fieldAttrs.reserve(fields_.size());
        for (const FieldSpec& f : fields_) {
            fieldAttrs.push_back(
                buildAnnotatableAttrs(f.signature, f.annotations, f.typeAnnotations));
        }
        std::vector<std::vector<AttrBytes>> methodAttrs;
        methodAttrs.reserve(methods.size());
        for (const MethodSpec& m : methods) {
            std::vector<AttrBytes> attrs =
                buildAnnotatableAttrs(m.signature, m.annotations, m.typeAnnotations);
            if (!m.parameterAnnotations.empty()) {
                attrs.push_back(AttrBytes{utf8("RuntimeVisibleParameterAnnotations"),
                                          parameterAnnotationsPayload(m.parameterAnnotations)});
            }
            if (m.hasAnnotationDefault) {
                Bytes payload;
                writeElemValue(payload, m.annotationDefault);
                attrs.push_back(AttrBytes{utf8("AnnotationDefault"), std::move(payload)});
            }
            if (!m.exceptions.empty()) {
                Bytes payload;
                putU16(payload, static_cast<ca::u16>(m.exceptions.size()));
                for (const std::string_view ex : m.exceptions) {
                    putU16(payload, cls(ex));
                }
                attrs.push_back(AttrBytes{utf8("Exceptions"), std::move(payload)});
            }
            if (m.emit) {
                Bytes    code;
                CodeEmit e(this, &code);
                m.emit(e);
                e.finish();

                Bytes attr;
                putU16(attr, m.maxStack);
                putU16(attr, m.maxLocals);
                putU32(attr, static_cast<ca::u32>(code.size()));
                attr.insert(attr.end(), code.begin(), code.end());
                putU16(attr, static_cast<ca::u16>(m.excTable.size()));
                for (const ExcSpec& x : m.excTable) {
                    putU16(attr, e.labelPc(x.start));
                    putU16(attr, e.labelPc(x.end));
                    putU16(attr, e.labelPc(x.handler));
                    putU16(attr, x.catchType.empty() ? 0 : cls(x.catchType));
                }
                std::vector<AttrBytes> nested = buildCodeAttrs(m, e);
                putU16(attr, static_cast<ca::u16>(nested.size()));
                for (const AttrBytes& nestedAttr : nested) {
                    putU16(attr, nestedAttr.nameCp);
                    putU32(attr, static_cast<ca::u32>(nestedAttr.payload.size()));
                    attr.insert(attr.end(), nestedAttr.payload.begin(),
                                nestedAttr.payload.end());
                }
                attrs.push_back(AttrBytes{codeName_, std::move(attr)});
            }
            methodAttrs.push_back(std::move(attrs));
        }

        Bytes out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);   // V1_8
        putU16(out, next_);  // cpCount（slot 0 无效）
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, access_);
        putU16(out, thisIdx_);
        putU16(out, hasSuper_ ? superIdx_ : 0);
        putU16(out, static_cast<ca::u16>(interfaces_.size()));
        for (const std::string_view iface : interfaces_) {
            putU16(out, cls(iface));
        }
        putU16(out, static_cast<ca::u16>(fields_.size()));
        for (ca::usize i = 0; i < fields_.size(); ++i) {
            const FieldSpec& f = fields_[i];
            putU16(out, f.access);
            putU16(out, utf8(f.name));
            putU16(out, utf8(f.desc));
            writeAttrs(out, fieldAttrs[i]);
        }
        putU16(out, static_cast<ca::u16>(methods.size()));
        for (ca::usize i = 0; i < methods.size(); ++i) {
            const MethodSpec& m = methods[i];
            putU16(out, m.access);
            putU16(out, utf8(m.name));
            putU16(out, utf8(m.desc));
            writeAttrs(out, methodAttrs[i]);
        }
        writeAttrs(out, classAttrs);
        return out;
    }

private:
    struct AttrBytes
    {
        ca::u16 nameCp = 0;
        Bytes   payload;
    };
    struct BsmSpec
    {
        std::string_view    owner;
        std::string_view    name;
        std::string_view    desc;
        std::vector<BsmArg> args;
    };

    /// emplace 结果（pair<iterator,bool>）回填 idx 并推进计数。
    template <typename EmplaceResult>
    ca::u16 intern(EmplaceResult&& inserted)
    {
        const ca::u16 idx      = next_;
        inserted.first->second = idx;
        next_                  = static_cast<ca::u16>(next_ + 1);
        return idx;
    }

    /// Code 嵌套属性：LNT / LVT / LVTT / StackMapTable。
    std::vector<AttrBytes> buildCodeAttrs(const MethodSpec& m, const CodeEmit& e)
    {
        std::vector<AttrBytes> attrs;
        if (!m.lineNumbers.empty()) {            Bytes payload;
            putU16(payload, static_cast<ca::u16>(m.lineNumbers.size()));
            for (const LineSpec& row : m.lineNumbers) {
                putU16(payload, e.labelPc(row.label));
                putU16(payload, row.line);
            }
            attrs.push_back(AttrBytes{utf8("LineNumberTable"), std::move(payload)});
        }
        if (!m.localVariables.empty()) {
            const bool hasSignatureRows = std::any_of(m.localVariables.begin(),
                                                      m.localVariables.end(),
                                                      [](const LvtSpec& row) {
                                                          return !row.signature.empty();
                                                      });
            if (hasSignatureRows) {
                Bytes payload;
                const auto sigRows = [&] {
                    std::vector<const LvtSpec*> rows;
                    for (const LvtSpec& row : m.localVariables) {
                        if (!row.signature.empty()) rows.push_back(&row);
                    }
                    return rows;
                }();
                putU16(payload, static_cast<ca::u16>(sigRows.size()));
                for (const LvtSpec* row : sigRows) {
                    putU16(payload, e.labelPc(row->start));
                    putU16(payload, static_cast<ca::u16>(e.labelPc(row->end) -
                                                         e.labelPc(row->start)));
                    putU16(payload, utf8(row->name));
                    putU16(payload, utf8(row->signature));
                    putU16(payload, row->slot);
                }
                attrs.push_back(AttrBytes{utf8("LocalVariableTypeTable"), std::move(payload)});
            }
            Bytes payload;
            putU16(payload, static_cast<ca::u16>(m.localVariables.size()));
            for (const LvtSpec& row : m.localVariables) {
                putU16(payload, e.labelPc(row.start));
                putU16(payload,
                       static_cast<ca::u16>(e.labelPc(row.end) - e.labelPc(row.start)));
                putU16(payload, utf8(row.name));
                putU16(payload, utf8(row.desc));
                putU16(payload, row.slot);
            }
            attrs.push_back(AttrBytes{utf8("LocalVariableTable"), std::move(payload)});
        }
        if (!m.stackMapFrames.empty()) {
            Bytes payload;
            putU16(payload, static_cast<ca::u16>(m.stackMapFrames.size()));
            for (const FullFrameSpec& frame : m.stackMapFrames) {
                putU8(payload, 255);   // full_frame
                putU16(payload, 0);    // offset_delta
                putU16(payload, static_cast<ca::u16>(frame.locals.size()));
                for (const VTypeSpec& v : frame.locals) writeVType(payload, v);
                putU16(payload, static_cast<ca::u16>(frame.stack.size()));
                for (const VTypeSpec& v : frame.stack) writeVType(payload, v);
            }
            attrs.push_back(AttrBytes{utf8("StackMapTable"), std::move(payload)});
        }
        if (!m.codeTypeAnnotations.empty()) {
            attrs.push_back(AttrBytes{utf8("RuntimeVisibleTypeAnnotations"),
                                      typeAnnotationListPayload(m.codeTypeAnnotations)});
        }
        return attrs;
    }

    void writeVType(Bytes& payload, const VTypeSpec& v)
    {
        if (v.className.empty()) {
            putU8(payload, 0);   // Top
            return;
        }
        putU8(payload, 7);   // Object
        putU16(payload, cls(v.className));
    }

    /// 注解元素值载荷（'c'/'['/'@'；其余 tag 夹具不支持）。
    void writeElemValue(Bytes& payload, const ElementValue& v)
    {
        putU8(payload, static_cast<ca::u8>(v.tag));
        switch (v.tag) {
        case 'c': putU16(payload, utf8(v.value)); break;
        case '[':
            putU16(payload, static_cast<ca::u16>(v.items.size()));
            for (const ElementValue& item : v.items) {
                writeElemValue(payload, item);
            }
            break;
        case '@':
            if (v.nested == nullptr) {
                ADD_FAILURE() << "'@' element value requires nested annotation";
                break;
            }
            putU16(payload, utf8(v.nested->type));
            putU16(payload, static_cast<ca::u16>(v.nested->pairs.size()));
            for (const AnnotationPair& pair : v.nested->pairs) {
                putU16(payload, utf8(pair.name));
                writeElemValue(payload, pair.value);
            }
            break;
        default: ADD_FAILURE() << "unsupported element value tag in fixture"; break;
        }
    }

    /// 注解列表载荷：u2 count + 注解体（type + pairs + element_value）。
    Bytes annotationListPayload(const std::vector<AnnotationSpec>& annos)
    {
        Bytes payload;
        putU16(payload, static_cast<ca::u16>(annos.size()));
        for (const AnnotationSpec& anno : annos) {
            putU16(payload, utf8(anno.type));
            putU16(payload, static_cast<ca::u16>(anno.pairs.size()));
            for (const AnnotationPair& pair : anno.pairs) {
                putU16(payload, utf8(pair.name));
                writeElemValue(payload, pair.value);
            }
        }
        return payload;
    }

    /// 参数注解载荷（JVMS 4.7.18/4.7.19）：u1 参数数 + { u2 注解数 + 注解体 }*。
    Bytes parameterAnnotationsPayload(
        const std::vector<std::vector<AnnotationSpec>>& rows)
    {
        Bytes payload;
        putU8(payload, static_cast<ca::u8>(rows.size()));
        for (const std::vector<AnnotationSpec>& row : rows) {
            putU16(payload, static_cast<ca::u16>(row.size()));
            for (const AnnotationSpec& anno : row) {
                putU16(payload, utf8(anno.type));
                putU16(payload, static_cast<ca::u16>(anno.pairs.size()));
                for (const AnnotationPair& pair : anno.pairs) {
                    putU16(payload, utf8(pair.name));
                    writeElemValue(payload, pair.value);
                }
            }
        }
        return payload;
    }

    /// type annotation 条目载荷：target_type + target_info + type_path + 注解体。
    Bytes typeAnnotationEntryPayload(const TypeAnnotationSpec& ta)
    {
        Bytes payload;
        putU8(payload, ta.targetType);
        switch (ta.targetType) {
        case 0x00:
        case 0x01: putU8(payload, ta.index8); break;
        case 0x11:
        case 0x12:
            putU8(payload, ta.index8);
            putU8(payload, ta.boundIndex);
            break;
        case 0x10:
        case 0x17:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46: putU16(payload, ta.index16); break;
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
            putU16(payload, ta.index16);
            putU8(payload, ta.index8);
            break;
        case 0x13:
        case 0x14:
        case 0x15: break;
        default:
            ADD_FAILURE() << "unsupported type_annotation target in fixture";
            break;
        }
        putU8(payload, static_cast<ca::u8>(ta.typePath.size()));
        for (const auto& [kind, argIndex] : ta.typePath) {
            putU8(payload, kind);
            putU8(payload, argIndex);
        }
        putU16(payload, utf8(ta.anno.type));
        putU16(payload, static_cast<ca::u16>(ta.anno.pairs.size()));
        for (const AnnotationPair& pair : ta.anno.pairs) {
            putU16(payload, utf8(pair.name));
            writeElemValue(payload, pair.value);
        }
        return payload;
    }

    /// type annotation 列表载荷（JVMS 4.7.20）：u2 count + 条目*。
    Bytes typeAnnotationListPayload(const std::vector<TypeAnnotationSpec>& tas)
    {
        Bytes payload;
        putU16(payload, static_cast<ca::u16>(tas.size()));
        for (const TypeAnnotationSpec& ta : tas) {
            const Bytes entry = typeAnnotationEntryPayload(ta);
            payload.insert(payload.end(), entry.begin(), entry.end());
        }
        return payload;
    }

    /// Signature/RuntimeVisibleAnnotations/RuntimeVisibleTypeAnnotations 属性族
    ///（字段/方法/Record 组件共用）。
    std::vector<AttrBytes> buildAnnotatableAttrs(
        std::string_view                           signature,
        const std::vector<AnnotationSpec>&         annotations,
        const std::vector<TypeAnnotationSpec>&     typeAnnotations)
    {
        std::vector<AttrBytes> attrs;
        if (!signature.empty()) {
            Bytes payload;
            putU16(payload, utf8(signature));
            attrs.push_back(AttrBytes{sigName_, std::move(payload)});
        }
        if (!annotations.empty()) {
            attrs.push_back(AttrBytes{annoName_, annotationListPayload(annotations)});
        }
        if (!typeAnnotations.empty()) {
            attrs.push_back(AttrBytes{utf8("RuntimeVisibleTypeAnnotations"),
                                      typeAnnotationListPayload(typeAnnotations)});
        }
        return attrs;
    }

    std::vector<AttrBytes> buildClassAttrs()
    {
        std::vector<AttrBytes> attrs;
        if (!classSignature_.empty()) {
            Bytes payload;
            putU16(payload, utf8(classSignature_));
            attrs.push_back(AttrBytes{sigName_, std::move(payload)});
        }
        if (!classAnnotations_.empty()) {
            attrs.push_back(AttrBytes{annoName_, annotationListPayload(classAnnotations_)});
        }
        if (!classTypeAnnotations_.empty()) {
            attrs.push_back(AttrBytes{utf8("RuntimeVisibleTypeAnnotations"),
                                      typeAnnotationListPayload(classTypeAnnotations_)});
        }
        if (enclosing_ != std::nullopt) {
            Bytes payload;
            putU16(payload, cls(std::get<0>(*enclosing_)));
            putU16(payload, nat(std::get<1>(*enclosing_), std::get<2>(*enclosing_)));
            attrs.push_back(AttrBytes{utf8("EnclosingMethod"), std::move(payload)});
        }
        if (!nestHost_.empty()) {
            Bytes payload;
            putU16(payload, cls(nestHost_));
            attrs.push_back(AttrBytes{utf8("NestHost"), std::move(payload)});
        }
        if (!nestMembers_.empty()) {
            Bytes payload;
            putU16(payload, static_cast<ca::u16>(nestMembers_.size()));
            for (const std::string_view member : nestMembers_) {
                putU16(payload, cls(member));
            }
            attrs.push_back(AttrBytes{utf8("NestMembers"), std::move(payload)});
        }
        if (!permittedSubclasses_.empty()) {
            Bytes payload;
            putU16(payload, static_cast<ca::u16>(permittedSubclasses_.size()));
            for (const std::string_view sub : permittedSubclasses_) {
                putU16(payload, cls(sub));
            }
            attrs.push_back(AttrBytes{utf8("PermittedSubclasses"), std::move(payload)});
        }
        if (!innerClasses_.empty()) {
            Bytes payload;
            putU16(payload, static_cast<ca::u16>(innerClasses_.size()));
            for (const auto& [inner, outer, innerName, access] : innerClasses_) {
                putU16(payload, cls(inner));
                putU16(payload, cls(outer));
                putU16(payload, innerName.empty() ? 0 : utf8(innerName));
                putU16(payload, access);
            }
            attrs.push_back(AttrBytes{utf8("InnerClasses"), std::move(payload)});
        }
        if (!recordComponents_.empty()) {
            Bytes payload;
            putU16(payload, static_cast<ca::u16>(recordComponents_.size()));
            for (const RecordComponentSpec& rc : recordComponents_) {
                putU16(payload, utf8(rc.name));
                putU16(payload, utf8(rc.desc));
                std::vector<AttrBytes> compAttrs = buildAnnotatableAttrs(
                    rc.signature, rc.annotations, rc.typeAnnotations);
                putU16(payload, static_cast<ca::u16>(compAttrs.size()));
                for (const AttrBytes& attr : compAttrs) {
                    putU16(payload, attr.nameCp);
                    putU32(payload, static_cast<ca::u32>(attr.payload.size()));
                    payload.insert(payload.end(), attr.payload.begin(), attr.payload.end());
                }
            }
            attrs.push_back(AttrBytes{utf8("Record"), std::move(payload)});
        }
        if (!module_.empty()) {
            // Module（JVMS 4.7.25）：name/flags/version + requires/exports/opens
            // 空段 + uses + provides。
            Bytes payload;
            putU16(payload, utf8(module_));
            putU16(payload, 0);   // module_flags
            putU16(payload, 0);   // module_version
            putU16(payload, 0);   // requires_count
            putU16(payload, 0);   // exports_count
            putU16(payload, 0);   // opens_count
            putU16(payload, static_cast<ca::u16>(moduleUses_.size()));
            for (const std::string_view use : moduleUses_) {
                putU16(payload, cls(use));
            }
            putU16(payload, static_cast<ca::u16>(moduleProvides_.size()));
            for (const auto& [service, impls] : moduleProvides_) {
                putU16(payload, cls(service));
                putU16(payload, static_cast<ca::u16>(impls.size()));
                for (const std::string_view impl : impls) {
                    putU16(payload, cls(impl));
                }
            }
            attrs.push_back(AttrBytes{utf8("Module"), std::move(payload)});
            if (!moduleMain_.empty()) {
                Bytes mainPayload;
                putU16(mainPayload, cls(moduleMain_));
                attrs.push_back(AttrBytes{utf8("ModuleMainClass"), std::move(mainPayload)});
            }
        }
        if (!bootstrapMethods_.empty()) {
            Bytes payload;
            putU16(payload, static_cast<ca::u16>(bootstrapMethods_.size()));
            for (const BsmSpec& bsm : bootstrapMethods_) {
                // JVMS 4.7.23：bootstrap_method_ref 是 CONSTANT_MethodHandle
                //（BSM 恒为 REF_invokeStatic = 6）。
                putU16(payload, methodHandleCp(6, bsm.owner, bsm.name, bsm.desc));
                putU16(payload, static_cast<ca::u16>(bsm.args.size()));
                for (const BsmArg& arg : bsm.args) {
                    if (arg.tag == 'M') {
                        putU16(payload, methodTypeCp(arg.desc));
                    }
                    else if (arg.tag == 'H') {
                        putU16(payload,
                               methodHandleCp(arg.handleKind, arg.owner, arg.name,
                                              arg.handleDesc));
                    }
                    else if (arg.tag == 'C') {
                        putU16(payload, cls(arg.owner));
                    }
                    else {
                        ADD_FAILURE() << "unsupported bsm arg tag in fixture";
                    }
                }
            }
            attrs.push_back(AttrBytes{utf8("BootstrapMethods"), std::move(payload)});
        }
        return attrs;
    }

    static void writeAttrs(Bytes& out, const std::vector<AttrBytes>& attrs)
    {
        putU16(out, static_cast<ca::u16>(attrs.size()));
        for (const AttrBytes& attr : attrs) {
            putU16(out, attr.nameCp);
            putU32(out, static_cast<ca::u32>(attr.payload.size()));
            out.insert(out.end(), attr.payload.begin(), attr.payload.end());
        }
    }

    Bytes                                 cp_;
    ca::u16                               next_ = 1;   // slot 0 无效
    std::map<std::string, ca::u16>        utf8s_;
    std::map<std::string, ca::u16>        classes_;
    std::map<std::string, ca::u16>        strings_;
    std::map<ca::i32, ca::u16>            integers_;
    std::map<std::string, ca::u16>        nats_;
    std::map<std::string, ca::u16>        refs_;
    std::map<std::string, ca::u16>        handles_;
    std::map<std::string, ca::u16>        methodTypes_;
    std::map<std::string, ca::u16>        dynamics_;
    std::map<std::string, ca::u16>        indys_;
    std::vector<FieldSpec>                fields_;
    ca::u16                               thisIdx_  = 0;
    ca::u16                               superIdx_ = 0;
    ca::u16                               codeName_ = 0;
    ca::u16                               sigName_  = 0;
    ca::u16                               annoName_ = 0;
    ca::u16                               access_   = 0x0021;   // public super
    bool                                  hasSuper_ = true;
    std::vector<std::string_view>         interfaces_;
    std::string_view                      classSignature_;
    std::vector<AnnotationSpec>           classAnnotations_;
    std::vector<TypeAnnotationSpec>       classTypeAnnotations_;
    std::optional<std::tuple<std::string_view, std::string_view, std::string_view>> enclosing_;
    std::string_view                      nestHost_;
    std::vector<std::string_view>         nestMembers_;
    std::vector<std::string_view>         permittedSubclasses_;
    std::vector<std::tuple<std::string_view, std::string_view, std::string_view, ca::u16>>
        innerClasses_;
    std::vector<RecordComponentSpec>      recordComponents_;
    std::string_view                      module_;
    std::string_view                      moduleMain_;
    std::vector<std::string_view>         moduleUses_;
    std::vector<std::pair<std::string_view, std::vector<std::string_view>>> moduleProvides_;
    std::vector<BsmSpec>                  bootstrapMethods_;
};

// CodeEmit 的 CP 转发（ClassBytesBuilder 完整定义之后）。
inline ca::u16 CodeEmit::utf8(std::string_view s) const { return owner_->utf8(s); }
inline ca::u16 CodeEmit::cls(std::string_view n) const { return owner_->cls(n); }
inline ca::u16 CodeEmit::fieldRef(std::string_view o, std::string_view n,
                                  std::string_view d) const
{
    return owner_->fieldRef(o, n, d);
}
inline ca::u16 CodeEmit::methodRef(std::string_view o, std::string_view n,
                                   std::string_view d) const
{
    return owner_->methodRef(o, n, d);
}
inline ca::u16 CodeEmit::ifaceMethodRef(std::string_view o, std::string_view n,
                                        std::string_view d) const
{
    return owner_->ifaceMethodRef(o, n, d);
}
inline void CodeEmit::ldcstring(std::string_view s)
{
    op(jbc::core::Op::LDC);
    u8(static_cast<ca::u8>(owner_->stringCp(s)));
}
inline void CodeEmit::ldcClass(std::string_view internalName)
{
    op(jbc::core::Op::LDC);
    u8(static_cast<ca::u8>(cls(internalName)));
}
inline void CodeEmit::ldcInteger(ca::i32 v)
{
    op(jbc::core::Op::LDC);
    u8(static_cast<ca::u8>(owner_->integerCp(v)));
}
inline void CodeEmit::ldcMethodHandle(ca::u16 kind, std::string_view owner,
                                      std::string_view name, std::string_view desc)
{
    op(jbc::core::Op::LDC);
    u8(static_cast<ca::u8>(owner_->methodHandleCp(kind, owner, name, desc)));
}
inline void CodeEmit::ldcMethodType(std::string_view desc)
{
    op(jbc::core::Op::LDC);
    u8(static_cast<ca::u8>(owner_->methodTypeCp(desc)));
}
inline void CodeEmit::ldcDynamic(ca::u16 bsmIndex, std::string_view name,
                                 std::string_view desc)
{
    op(jbc::core::Op::LDC);
    u8(static_cast<ca::u8>(owner_->dynamicCp(bsmIndex, name, desc)));
}
inline void CodeEmit::invokedynamic(ca::u16 bsmIndex, std::string_view name,
                                    std::string_view desc)
{
    op(jbc::core::Op::INVOKEDYNAMIC);
    u16(owner_->indyCp(bsmIndex, name, desc));
    u16(0);
}

}   // namespace mjbase::asm_remapper::test_support
