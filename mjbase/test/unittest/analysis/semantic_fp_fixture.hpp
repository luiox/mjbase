/// @file        semantic_fp_fixture.hpp
/// @brief       semantic_fp 测试共用的手拼 classfile 汇编器。
///
/// 与 analysis/reference 测试的 class_bytes_builder.hpp 同法（CP 惰性驻留、
/// 两遍装配保证 cpCount 与索引稳定），扩展指纹夹具需要的面：ldc 全载荷
/// （Float/Long/Double/MethodHandle/MethodType/condy）、wide 前缀变址与
/// iinc、tableswitch/lookupswitch（4 字节对齐 padding）、goto_w、
/// BootstrapMethods 全 tag 实参（含 JVMS 禁的嵌套动态常量，合成畸形用）、
/// 方法级原始属性（LineNumberTable/LVT/注解等排除面字节）、以及
/// CP 前缀垃圾注入（全部语义 CP 槽位平移，逼全量重索引）。每个字节受控，
/// 不依赖本地语料；指令字面量一律走 jbc::core::Op。
#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

namespace mjbase::analysis::fp_test_support {

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

inline void putI32(Bytes& b, ca::i32 v) { putU32(b, static_cast<ca::u32>(v)); }

inline void putUtf8Raw(Bytes& b, std::string_view s)
{
    for (const char c : s) {
        b.push_back(static_cast<ca::u8>(c));
    }
}

/// 异常表条目：start/end/handler 为标签 id（bindLabel 落 pc 后回填）；
/// catchType 为类内部名，空串 = any（catch_type 0）。rawPc=true 时三个字段
/// 直接当原始 pc 写出（端点映射语义测试需要非指令边界 pc）。
struct ExcSpec
{
    std::size_t      start   = 0;
    std::size_t      end     = 0;
    std::size_t      handler = 0;
    std::string_view catchType;
    bool             rawPc   = false;
};

/// 原始属性（排除面测试用：LineNumberTable/LVT/RuntimeVisibleAnnotations/
/// Exceptions/SourceFile 等按需手拼载荷字节）。
struct RawAttrSpec
{
    std::string_view name;
    Bytes            payload;
};

/// Bootstrap 方法实参：'M' MethodType / 'H' MethodHandle / 'S' String /
/// 'C' Class / 'I' Integer / 'F' Float(bits) / 'L' Long / 'D' Double(bits)；
/// 'Z' Dynamic 与 'Y' InvokeDynamic 是 JVMS 禁的嵌套形态，仅供
/// 「BSM 解一层不递归」的畸形合成用。
struct BsmArg
{
    char             tag        = 'M';
    std::string_view desc;              ///< 'M' 载荷
    ca::u16          handleKind = 6;    ///< 'H' 载荷（REF_invokeStatic）
    std::string_view owner;             ///< 'H' 载荷
    std::string_view name;              ///< 'H'/'Z'/'Y' 载荷
    std::string_view handleDesc;        ///< 'H' 载荷
    std::string_view s;                 ///< 'S' 载荷
    std::string_view className;         ///< 'C' 载荷
    ca::u16          bsmIndex  = 0;     ///< 'Z'/'Y' 载荷
    ca::i32          ival      = 0;     ///< 'I' 载荷
    ca::u32          fbits     = 0;     ///< 'F' 载荷
    ca::i64          lval      = 0;     ///< 'L' 载荷
    ca::u64          dbits     = 0;     ///< 'D' 载荷

    static BsmArg methodType(std::string_view d)
    {
        BsmArg a;
        a.tag  = 'M';
        a.desc = d;
        return a;
    }
    static BsmArg handle(ca::u16 kind, std::string_view o, std::string_view n,
                         std::string_view d)
    {
        BsmArg a;
        a.tag        = 'H';
        a.handleKind = kind;
        a.owner      = o;
        a.name       = n;
        a.handleDesc = d;
        return a;
    }
    static BsmArg str(std::string_view v)
    {
        BsmArg a;
        a.tag = 'S';
        a.s   = v;
        return a;
    }
    static BsmArg cls(std::string_view v)
    {
        BsmArg a;
        a.tag       = 'C';
        a.className = v;
        return a;
    }
    static BsmArg integer(ca::i32 v)
    {
        BsmArg a;
        a.tag  = 'I';
        a.ival = v;
        return a;
    }
    static BsmArg floatBits(ca::u32 bits)
    {
        BsmArg a;
        a.tag   = 'F';
        a.fbits = bits;
        return a;
    }
    static BsmArg longVal(ca::i64 v)
    {
        BsmArg a;
        a.tag  = 'L';
        a.lval = v;
        return a;
    }
    static BsmArg doubleBits(ca::u64 bits)
    {
        BsmArg a;
        a.tag   = 'D';
        a.dbits = bits;
        return a;
    }
    static BsmArg condy(ca::u16 bsm, std::string_view n, std::string_view d)
    {
        BsmArg a;
        a.tag      = 'Z';
        a.bsmIndex = bsm;
        a.name     = n;
        a.desc     = d;
        return a;
    }
    static BsmArg indy(ca::u16 bsm, std::string_view n, std::string_view d)
    {
        BsmArg a;
        a.tag      = 'Y';
        a.bsmIndex = bsm;
        a.name     = n;
        a.desc     = d;
        return a;
    }
};

/// 方法装配规格：emit 为空 = 无 Code 属性（abstract/native）。
struct MethodSpec
{
    ca::u16                              access    = 0x0009;   // public static
    std::string_view                     name;
    std::string_view                     desc;
    ca::u16                              maxStack  = 1;
    ca::u16                              maxLocals = 1;
    std::function<void(class CodeEmit&)> emit;
    std::vector<ExcSpec>                 exceptions;
    std::vector<RawAttrSpec>             rawAttrs;   ///< Code 之外的方法级属性
    std::vector<RawAttrSpec>             codeAttrs;  ///< Code 嵌套属性（SMT/LNT 等）
};

/// 单方法代码发射器：标签先占号后绑定；branch 2 字节/goto_w 与 switch
/// 操作数 4 字节，装配末统一回填（相对各自指令 pc）。
class ClassBytesBuilder;   ///< 前置声明：CodeEmit 以裸指针借用 builder

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
    ca::u16 stringCp(std::string_view s) const;
    ca::u16 integerCp(ca::i32 v) const;
    ca::u16 floatCp(ca::u32 bits) const;
    ca::u16 longCp(ca::i64 v) const;
    ca::u16 doubleCp(ca::u64 bits) const;
    ca::u16 methodHandleCp(ca::u16 kind, std::string_view owner, std::string_view name,
                           std::string_view desc) const;
    ca::u16 methodTypeCp(std::string_view desc) const;
    ca::u16 indyCp(ca::u16 bsmIndex, std::string_view name, std::string_view desc) const;
    ca::u16 dynamicCp(ca::u16 bsmIndex, std::string_view name, std::string_view desc) const;

    // 裸字节出口。
    void u8(ca::u8 v) { putU8(*code_, v); }
    void u16(ca::u16 v) { putU16(*code_, v); }
    void u32(ca::u32 v) { putU32(*code_, v); }

    void op(jbc::core::Op o) { putU8(*code_, static_cast<ca::u8>(o)); }

    /// 变址指令（u8 索引形态）。
    void varInsn(jbc::core::Op o, ca::u16 idx)
    {
        op(o);
        u8(static_cast<ca::u8>(idx));
    }

    /// wide 前缀变址（WIDE + op + u16 槽位）。
    void varInsnW(jbc::core::Op o, ca::u16 idx)
    {
        op(jbc::core::Op::WIDE);
        op(o);
        u16(idx);
    }

    /// iinc（u8 槽位 + i8 增量）。
    void iincInsn(ca::u8 slot, ca::i8 delta)
    {
        op(jbc::core::Op::IINC);
        u8(slot);
        u8(static_cast<ca::u8>(delta));
    }

    /// wide iinc（WIDE + IINC + u16 槽位 + i16 增量）。
    void iincInsnW(ca::u16 slot, ca::i16 delta)
    {
        op(jbc::core::Op::WIDE);
        op(jbc::core::Op::IINC);
        u16(slot);
        u16(static_cast<ca::u16>(delta));
    }

    /// 类型指令：NEW/ANEWARRAY/CHECKCAST/INSTANCEOF。
    void typeInsn(jbc::core::Op o, std::string_view internalName)
    {
        op(o);
        u16(cls(internalName));
    }

    void fieldInsn(jbc::core::Op o, std::string_view owner, std::string_view name,
                   std::string_view desc)
    {
        op(o);
        u16(fieldRef(owner, name, desc));
    }

    void invoke(jbc::core::Op o, std::string_view owner, std::string_view name,
                std::string_view desc)
    {
        op(o);
        u16(methodRef(owner, name, desc));
    }

    void invokeInterface(std::string_view owner, std::string_view name, std::string_view desc,
                         ca::u8 count)
    {
        op(jbc::core::Op::INVOKEINTERFACE);
        u16(ifaceMethodRef(owner, name, desc));
        u8(count);
        u8(0);
    }

    // ---- ldc 家族：显式 CP 索引与载荷便利形 ----

    /// LDC（索引 ≤255 走 1 字节形态，否则 LDC_W）。
    void ldcCp(ca::u16 cp)
    {
        if (cp <= 0xFF) {
            op(jbc::core::Op::LDC);
            u8(static_cast<ca::u8>(cp));
        }
        else {
            op(jbc::core::Op::LDC_W);
            u16(cp);
        }
    }

    /// 恒 LDC_W 形态（与 LDC 同载荷同 token 的对拍用）。
    void ldcWCp(ca::u16 cp)
    {
        op(jbc::core::Op::LDC_W);
        u16(cp);
    }

    void ldc2Cp(ca::u16 cp)
    {
        op(jbc::core::Op::LDC2_W);
        u16(cp);
    }

    // 载荷便利形（定义在 ClassBytesBuilder 完整定义之后）。
    void ldcString(std::string_view s);
    void ldcClass(std::string_view n);
    void ldcInteger(ca::i32 v);
    void ldcFloatBits(ca::u32 bits);
    void ldcLong(ca::i64 v);
    void ldcDoubleBits(ca::u64 bits);
    void ldcMethodHandle(ca::u16 kind, std::string_view owner, std::string_view name,
                         std::string_view desc);
    void ldcMethodType(std::string_view desc);
    void ldcCondy(ca::u16 bsmIndex, std::string_view name, std::string_view desc);

    /// int 常量：-1..5 走 iconst_n，其余按值域走 bipush/sipush/LDC。
    void i32const(ca::i32 v)
    {
        if (v >= -1 && v <= 5) {
            op(v < 0 ? jbc::core::Op::ICONST_M1
                     : static_cast<jbc::core::Op>(static_cast<ca::u8>(jbc::core::Op::ICONST_0) +
                                                  static_cast<ca::u8>(v)));
            return;
        }
        if (v >= -128 && v <= 127) {
            op(jbc::core::Op::BIPUSH);
            u8(static_cast<ca::u8>(v));
            return;
        }
        if (v >= -32768 && v <= 32767) {
            op(jbc::core::Op::SIPUSH);
            u16(static_cast<ca::u16>(static_cast<ca::i16>(v)));
            return;
        }
        ldcInteger(v);
    }

    /// 分支（2 字节相对偏移，相对分支指令自身 pc）。
    void branch(jbc::core::Op o, std::size_t label)
    {
        const ca::u32 base = static_cast<ca::u32>(code_->size());
        op(o);
        patches_.push_back(Patch{static_cast<ca::u32>(code_->size()), 2, base, label});
        u8(0);
        u8(0);
    }

    /// 4 字节分支（GOTO_W/JSR_W）。
    void branchW(jbc::core::Op o, std::size_t label)
    {
        const ca::u32 base = static_cast<ca::u32>(code_->size());
        op(o);
        patches_.push_back(Patch{static_cast<ca::u32>(code_->size()), 4, base, label});
        u32(0);
    }

    /// tableswitch：4 字节对齐 padding + default + low + high + 逐 case 偏移。
    void tableswitch(std::size_t defaultLabel, ca::i32 low, ca::i32 high,
                     const std::vector<std::size_t>& labels)
    {
        const ca::u32 base = static_cast<ca::u32>(code_->size());
        op(jbc::core::Op::TABLESWITCH);
        const ca::u32 operandStart = base + 1;
        for (ca::u32 pad = (4 - operandStart % 4) % 4; pad > 0; --pad) {
            u8(0);
        }
        patches_.push_back(Patch{static_cast<ca::u32>(code_->size()), 4, base, defaultLabel});
        u32(0);
        putI32(*code_, low);
        putI32(*code_, high);
        for (const std::size_t label : labels) {
            patches_.push_back(Patch{static_cast<ca::u32>(code_->size()), 4, base, label});
            u32(0);
        }
    }

    /// lookupswitch：padding + default + npairs + 逐 (key, offset) 对。
    void lookupswitch(std::size_t                            defaultLabel,
                      const std::vector<std::pair<ca::i32, std::size_t>>& pairs)
    {
        const ca::u32 base = static_cast<ca::u32>(code_->size());
        op(jbc::core::Op::LOOKUPSWITCH);
        const ca::u32 operandStart = base + 1;
        for (ca::u32 pad = (4 - operandStart % 4) % 4; pad > 0; --pad) {
            u8(0);
        }
        patches_.push_back(Patch{static_cast<ca::u32>(code_->size()), 4, base, defaultLabel});
        u32(0);
        putI32(*code_, static_cast<ca::i32>(pairs.size()));
        for (const auto& [key, label] : pairs) {
            putI32(*code_, key);
            patches_.push_back(Patch{static_cast<ca::u32>(code_->size()), 4, base, label});
            u32(0);
        }
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

    /// 回填全部 branch/switch 偏移（相对各自指令 pc）；异常表经 labelPc 读取。
    void finish()
    {
        for (const auto& p : patches_) {
            ASSERT_LT(p.label, labels_.size());
            ASSERT_NE(labels_[p.label], kUnbound);
            const ca::i64 off = static_cast<ca::i64>(labels_[p.label]) -
                                static_cast<ca::i64>(p.base);
            if (p.width == 2) {
                const auto  v   = static_cast<ca::i16>(off);
                (*code_)[p.at]     = static_cast<ca::u8>(static_cast<ca::u16>(v) >> 8);
                (*code_)[p.at + 1] = static_cast<ca::u8>(static_cast<ca::u16>(v) & 0xFF);
            }
            else {
                const auto v = static_cast<ca::u32>(off);
                (*code_)[p.at]     = static_cast<ca::u8>(v >> 24);
                (*code_)[p.at + 1] = static_cast<ca::u8>((v >> 16) & 0xFF);
                (*code_)[p.at + 2] = static_cast<ca::u8>((v >> 8) & 0xFF);
                (*code_)[p.at + 3] = static_cast<ca::u8>(v & 0xFF);
            }
        }
        finished_ = true;
    }

    /// 标签落点 pc（finish 后读取；异常表装配用）。
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
        ca::u32     at;     ///< 偏移操作数首字节在 code_ 中的位置
        ca::u8      width;  ///< 2 或 4
        ca::u32     base;   ///< 相对基准 = 指令自身 pc
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
///
/// 构造参数 cpPrefixJunk：装配时在 CP 头部插 n 个垃圾 Utf8 条目，全部语义
/// CP 槽位平移 n——同语义不同索引的「重排常量池、重写索引」夹具。
class ClassBytesBuilder
{
public:
    /// @param cpPrefixJunk CP 头部垃圾条目数：全部语义槽位平移该值（构造期
    /// 即定死——ctor 立刻驻留 this/super）。
    explicit ClassBytesBuilder(std::string_view thisClass, ca::u16 cpPrefixJunk = 0)
        : cpPrefixJunk_(cpPrefixJunk)
    {
        thisIdx_  = cls(thisClass);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
    }

    void setClassRawAttrs(std::vector<RawAttrSpec> attrs) { classRawAttrs_ = std::move(attrs); }

    // CP 驻留（CodeEmit 与属性装配共用；两遍幂等）。
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

    ca::u16 floatCp(ca::u32 bits)
    {
        const auto it = floats_.find(bits);
        if (it != floats_.end()) {
            return it->second;
        }
        putU8(cp_, 0x04);
        putU32(cp_, bits);
        return intern(floats_.emplace(bits, 0));
    }

    ca::u16 longCp(ca::i64 v)
    {
        const auto it = longs_.find(v);
        if (it != longs_.end()) {
            return it->second;
        }
        putU8(cp_, 0x05);
        putU32(cp_, static_cast<ca::u32>(v >> 32));
        putU32(cp_, static_cast<ca::u32>(v));
        return internWide(longs_.emplace(v, 0));
    }

    ca::u16 doubleCp(ca::u64 bits)
    {
        const auto it = doubles_.find(bits);
        if (it != doubles_.end()) {
            return it->second;
        }
        putU8(cp_, 0x06);
        putU32(cp_, static_cast<ca::u32>(bits >> 32));
        putU32(cp_, static_cast<ca::u32>(bits));
        return internWide(doubles_.emplace(bits, 0));
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
        // 先物化依赖条目再写字节（cls/nat 会向 cp_ 追加）。
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

    /// Dynamic 常量（tag 0x11，condy）。
    ca::u16 dynamicCp(ca::u16 bsmIndex, std::string_view name, std::string_view desc)
    {
        std::string key = "D." + std::to_string(bsmIndex) + "." + std::string(name) + "." +
                          std::string(desc);
        const auto it = dyns_.find(key);
        if (it != dyns_.end()) {
            return it->second;
        }
        const ca::u16 n = nat(name, desc);
        putU8(cp_, 0x11);
        putU16(cp_, bsmIndex);
        putU16(cp_, n);
        return intern(dyns_.emplace(std::move(key), 0));
    }

    /// 登记一个 Bootstrap 方法，返回其 bsm 序号。
    ca::u16 addBootstrapMethod(std::string_view owner, std::string_view name,
                               std::string_view desc, std::vector<BsmArg> args)
    {
        bootstrapMethods_.push_back(BsmSpec{owner, name, desc, std::move(args)});
        return static_cast<ca::u16>(bootstrapMethods_.size() - 1);
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

        // 方法名/描述符与异常捕获类型、原始属性名在此预驻留：它们只在装配段
        // 引用，惰性驻留会落在 cpCount/cp 字节写完之后（CP 永远缺条目）。
        for (const MethodSpec& meth : methods) {
            utf8(meth.name);
            utf8(meth.desc);
            for (const ExcSpec& x : meth.exceptions) {
                if (!x.catchType.empty()) {
                    cls(x.catchType);
                }
            }
            for (const RawAttrSpec& attr : meth.rawAttrs) {
                utf8(attr.name);
            }
            for (const RawAttrSpec& attr : meth.codeAttrs) {
                utf8(attr.name);
            }
        }
        for (const RawAttrSpec& attr : classRawAttrs_) {
            utf8(attr.name);
        }
        // BootstrapMethods 引用的 CP 全部在此预驻留（'Z'/'Y' 实参引用的
        // indy/condy CP 条目也按序号-名字-描述符驻留）；属性名自身同样必须在
        // cpCount 写出前入池。
        if (!bootstrapMethods_.empty()) {
            utf8("BootstrapMethods");
        }
        for (const BsmSpec& bsm : bootstrapMethods_) {
            methodHandleCp(6, bsm.owner, bsm.name, bsm.desc);
            for (const BsmArg& arg : bsm.args) {
                internBsmArg(arg);
            }
        }

        Bytes out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);   // V1_8
        putU16(out, static_cast<ca::u16>(next_ + cpPrefixJunk_));
        for (ca::u16 i = 0; i < cpPrefixJunk_; ++i) {
            // 垃圾前缀条目：内容无关紧要，占据低位槽位逼全部语义引用平移。
            const std::string junk = "fp.junk." + std::to_string(i);
            putU8(out, 0x01);
            putU16(out, static_cast<ca::u16>(junk.size()));
            putUtf8Raw(out, junk);
        }
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, 0x0021);   // public super
        putU16(out, thisIdx_);
        putU16(out, superIdx_);
        putU16(out, 0);   // 接口表空
        putU16(out, 0);   // 字段表空
        putU16(out, static_cast<ca::u16>(methods.size()));
        for (const MethodSpec& m : methods) {
            putU16(out, m.access);
            putU16(out, utf8(m.name));
            putU16(out, utf8(m.desc));
            std::vector<AttrBytes> attrs;
            for (const RawAttrSpec& raw : m.rawAttrs) {
                attrs.push_back(AttrBytes{utf8(raw.name), raw.payload});
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
                putU16(attr, static_cast<ca::u16>(m.exceptions.size()));
                for (const ExcSpec& x : m.exceptions) {
                    putU16(attr, x.rawPc ? static_cast<ca::u16>(x.start)
                                         : e.labelPc(x.start));
                    putU16(attr, x.rawPc ? static_cast<ca::u16>(x.end)
                                         : e.labelPc(x.end));
                    putU16(attr, x.rawPc ? static_cast<ca::u16>(x.handler)
                                         : e.labelPc(x.handler));
                    putU16(attr, x.catchType.empty() ? 0 : cls(x.catchType));
                }
                putU16(attr, static_cast<ca::u16>(m.codeAttrs.size()));
                for (const RawAttrSpec& nested : m.codeAttrs) {
                    putU16(attr, utf8(nested.name));
                    putU32(attr, static_cast<ca::u32>(nested.payload.size()));
                    attr.insert(attr.end(), nested.payload.begin(), nested.payload.end());
                }
                attrs.push_back(AttrBytes{codeName_, std::move(attr)});
            }
            writeAttrs(out, attrs);
        }
        std::vector<AttrBytes> classAttrs = classRawAttrBytes();
        if (!bootstrapMethods_.empty()) {
            Bytes bmPayload;
            putU16(bmPayload, static_cast<ca::u16>(bootstrapMethods_.size()));
            for (const BsmSpec& bsm : bootstrapMethods_) {
                putU16(bmPayload, methodHandleCp(6, bsm.owner, bsm.name, bsm.desc));
                putU16(bmPayload, static_cast<ca::u16>(bsm.args.size()));
                for (const BsmArg& arg : bsm.args) {
                    putU16(bmPayload, internBsmArg(arg));
                }
            }
            classAttrs.push_back(AttrBytes{utf8("BootstrapMethods"), std::move(bmPayload)});
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

    /// emplace 结果回填 idx 并推进计数；索引含垃圾前缀平移。
    template <typename EmplaceResult>
    ca::u16 intern(EmplaceResult&& inserted)
    {
        const ca::u16 idx      = static_cast<ca::u16>(next_ + cpPrefixJunk_);
        inserted.first->second = idx;
        next_                  = static_cast<ca::u16>(next_ + 1);
        return idx;
    }

    /// 追加宽条目（Long/Double）：占两槽，第二槽不可用。
    template <typename EmplaceResult>
    ca::u16 internWide(EmplaceResult&& inserted)
    {
        const ca::u16 idx = intern(std::move(inserted));
        next_             = static_cast<ca::u16>(next_ + 1);
        return idx;
    }

    std::vector<AttrBytes> classRawAttrBytes()
    {
        std::vector<AttrBytes> attrs;
        for (const RawAttrSpec& raw : classRawAttrs_) {
            attrs.push_back(AttrBytes{utf8(raw.name), raw.payload});
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

    ca::u16                        cpPrefixJunk_ = 0;
    Bytes                          cp_;
    ca::u16                        next_ = 1;   // slot 0 无效
    std::map<std::string, ca::u16> utf8s_;
    std::map<std::string, ca::u16> classes_;
    std::map<std::string, ca::u16> strings_;
    std::map<ca::i32, ca::u16>     integers_;
    std::map<ca::u32, ca::u16>     floats_;
    std::map<ca::i64, ca::u16>     longs_;
    std::map<ca::u64, ca::u16>     doubles_;
    std::map<std::string, ca::u16> nats_;
    std::map<std::string, ca::u16> refs_;
    std::map<std::string, ca::u16> handles_;
    std::map<std::string, ca::u16> methodTypes_;
    std::map<std::string, ca::u16> indys_;
    std::map<std::string, ca::u16> dyns_;
    std::vector<BsmSpec>           bootstrapMethods_;
    ca::u16                        thisIdx_  = 0;
    ca::u16                        superIdx_ = 0;
    ca::u16                        codeName_ = 0;
    std::vector<RawAttrSpec>       classRawAttrs_;

    friend class CodeEmit;

    /// BsmArg 的 CP 驻留（幂等；'Z'/'Y' 引用既有 bsm 序号对应的 indy/condy）。
    ca::u16 internBsmArg(const BsmArg& arg)
    {
        switch (arg.tag) {
        case 'M': return methodTypeCp(arg.desc);
        case 'H': return methodHandleCp(arg.handleKind, arg.owner, arg.name, arg.handleDesc);
        case 'S': return stringCp(arg.s);
        case 'C': return cls(arg.className);
        case 'I': return integerCp(arg.ival);
        case 'F': return floatCp(arg.fbits);
        case 'L': return longCp(arg.lval);
        case 'D': return doubleCp(arg.dbits);
        case 'Z': return dynamicCp(arg.bsmIndex, arg.name, arg.desc);
        case 'Y': return indyCp(arg.bsmIndex, arg.name, arg.desc);
        default:
            ADD_FAILURE() << "unsupported bsm arg tag in fixture";
            return 0;
        }
    }

};

// CodeEmit 的 CP 转发（ClassBytesBuilder 完整定义之后）。
inline ca::u16 CodeEmit::utf8(std::string_view s) const { return owner_->utf8(s); }
inline ca::u16 CodeEmit::cls(std::string_view n) const { return owner_->cls(n); }
inline ca::u16 CodeEmit::stringCp(std::string_view s) const { return owner_->stringCp(s); }
inline ca::u16 CodeEmit::integerCp(ca::i32 v) const { return owner_->integerCp(v); }
inline ca::u16 CodeEmit::floatCp(ca::u32 bits) const { return owner_->floatCp(bits); }
inline ca::u16 CodeEmit::longCp(ca::i64 v) const { return owner_->longCp(v); }
inline ca::u16 CodeEmit::doubleCp(ca::u64 bits) const { return owner_->doubleCp(bits); }
inline ca::u16 CodeEmit::methodHandleCp(ca::u16 kind, std::string_view owner,
                                        std::string_view name, std::string_view desc) const
{
    return owner_->methodHandleCp(kind, owner, name, desc);
}
inline ca::u16 CodeEmit::methodTypeCp(std::string_view desc) const
{
    return owner_->methodTypeCp(desc);
}
inline ca::u16 CodeEmit::indyCp(ca::u16 bsmIndex, std::string_view name,
                                std::string_view desc) const
{
    return owner_->indyCp(bsmIndex, name, desc);
}
inline ca::u16 CodeEmit::dynamicCp(ca::u16 bsmIndex, std::string_view name,
                                   std::string_view desc) const
{
    return owner_->dynamicCp(bsmIndex, name, desc);
}
inline void CodeEmit::ldcString(std::string_view s) { ldcCp(owner_->stringCp(s)); }
inline void CodeEmit::ldcClass(std::string_view n) { ldcCp(owner_->cls(n)); }
inline void CodeEmit::ldcInteger(ca::i32 v) { ldcCp(owner_->integerCp(v)); }
inline void CodeEmit::ldcFloatBits(ca::u32 bits) { ldcCp(owner_->floatCp(bits)); }
inline void CodeEmit::ldcLong(ca::i64 v) { ldc2Cp(owner_->longCp(v)); }
inline void CodeEmit::ldcDoubleBits(ca::u64 bits) { ldc2Cp(owner_->doubleCp(bits)); }
inline void CodeEmit::ldcMethodHandle(ca::u16 kind, std::string_view owner,
                                      std::string_view name, std::string_view desc)
{
    ldcCp(owner_->methodHandleCp(kind, owner, name, desc));
}
inline void CodeEmit::ldcMethodType(std::string_view desc)
{
    ldcCp(owner_->methodTypeCp(desc));
}
inline void CodeEmit::ldcCondy(ca::u16 bsmIndex, std::string_view name,
                               std::string_view desc)
{
    ldcCp(owner_->dynamicCp(bsmIndex, name, desc));
}
inline void CodeEmit::invokedynamic(ca::u16 bsmIndex, std::string_view name,
                                    std::string_view desc)
{
    op(jbc::core::Op::INVOKEDYNAMIC);
    u16(owner_->indyCp(bsmIndex, name, desc));
    u16(0);
}
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

}   // namespace mjbase::analysis::fp_test_support
