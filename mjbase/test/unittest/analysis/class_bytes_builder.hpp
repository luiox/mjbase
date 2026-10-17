/// @file        class_bytes_builder.hpp
/// @brief       analysis 测试共用的手拼 classfile 汇编器。
///
/// 与 asm/classpath 测试的 ClassBytesBuilder 同法（CP 惰性驻留、两遍装配保证
/// cpCount 与索引稳定），扩展出 analysis 夹具需要的面：字段表、异常表、
/// LDC/String/Class/Integer/Long 载荷、字段/类型/跳转/变址指令与标签。
/// 每个字节受控，不依赖本地语料；指令字面量一律走 jbc::core::Op。
#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

namespace mjbase::analysis::test_support {

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

class ClassBytesBuilder;

/// 异常表条目：start/end/handler 为标签 id（bindLabel 落 pc 后回填）；
/// catchType 为类内部名，空串 = any（catch_type 0）。
struct ExcSpec
{
    std::size_t      start   = 0;
    std::size_t      end     = 0;
    std::size_t      handler = 0;
    std::string_view catchType;
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
};

struct FieldSpec
{
    ca::u16          access = 0x0002;   // private
    std::string_view name;
    std::string_view desc;
};

/// 单方法代码发射器：标签先占号后绑定，branch 2 字节相对偏移装配末回填。
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

    // 裸字节出口（畸形序列/非常规指令用）。
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

    /// LDC String 载荷（夹具 CP 规模小，恒 1 字节索引）。
    void ldcString(std::string_view s);
    /// LDC 类常量（"class:X" 事实与反射接收者测试的载荷）。
    void ldcClass(std::string_view internalName);
    void ldcInteger(ca::i32 v);
    /// LDC2_W Long 载荷（cat2）。
    void ldcLong(ca::i64 v);

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

    /// 分支/跳转（2 字节相对偏移，装配末回填）。
    void branch(jbc::core::Op o, std::size_t label)
    {
        op(o);
        patches_.push_back({code_->size(), label});
        u8(0);
        u8(0);
    }

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
            const ca::i16 off   = static_cast<ca::i16>(labels_[p.label] - static_cast<ca::u32>(p.at - 1));
            (*code_)[p.at]      = static_cast<ca::u8>(static_cast<ca::u16>(off) >> 8);
            (*code_)[p.at + 1]  = static_cast<ca::u8>(static_cast<ca::u16>(off) & 0xFF);
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
        std::size_t at;
        std::size_t label;
    };
    static constexpr ca::u32 kUnbound = 0xFFFFFFFFu;

    ClassBytesBuilder*       owner_ = nullptr;
    Bytes*                   code_  = nullptr;
    std::vector<ca::u32>     labels_;
    std::vector<Patch>       patches_;
    bool                     finished_ = false;
};

/// 手工拼最小 classfile（V1_8 / public super / super=java/lang/Object）。
class ClassBytesBuilder
{
public:
    explicit ClassBytesBuilder(std::string_view thisClass)
    {
        thisIdx_  = cls(thisClass);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
    }

    // CP 驻留（CodeEmit 与字段/方法装配共用；两遍幂等）。
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

    /// Long/Double 占两个 slot。
    ca::u16 longCp(ca::i64 v)
    {
        const auto it = longs_.find(v);
        if (it != longs_.end()) {
            return it->second;
        }
        putU8(cp_, 0x05);
        putU32(cp_, static_cast<ca::u32>(static_cast<ca::u64>(v) >> 32));
        putU32(cp_, static_cast<ca::u32>(static_cast<ca::u64>(v) & 0xFFFFFFFFu));
        const ca::u16 idx = next_;
        next_             = static_cast<ca::u16>(next_ + 2);
        longs_.emplace(v, idx);
        return idx;
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
                Bytes           scratch;
                CodeEmit        e(this, &scratch);
                m.emit(e);
            }
        }

        // 方法名/描述符与异常捕获类型在此预驻留：它们只在装配段引用，
        // 若惰性驻留会落在 cpCount/cp 字节写完之后（CP 永远缺条目）。
        for (const MethodSpec& meth : methods) {
            utf8(meth.name);
            utf8(meth.desc);
            for (const ExcSpec& x : meth.exceptions) {
                if (!x.catchType.empty()) cls(x.catchType);
            }
        }

        Bytes out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);   // V1_8
        putU16(out, next_);  // cpCount（slot 0 无效；long 已双计）
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, 0x0021);   // public super
        putU16(out, thisIdx_);
        putU16(out, superIdx_);
        putU16(out, 0);   // interfaces
        putU16(out, static_cast<ca::u16>(fields_.size()));

        for (const FieldSpec& f : fields_) {
            putU16(out, f.access);
            putU16(out, utf8(f.name));
            putU16(out, utf8(f.desc));
            putU16(out, 0);   // 无字段属性
        }

        putU16(out, static_cast<ca::u16>(methods.size()));
        for (const MethodSpec& m : methods) {
            putU16(out, m.access);
            putU16(out, utf8(m.name));
            putU16(out, utf8(m.desc));
            if (!m.emit) {
                putU16(out, 0);   // 无 Code：abstract/native
                continue;
            }
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
                putU16(attr, e.labelPc(x.start));
                putU16(attr, e.labelPc(x.end));
                putU16(attr, e.labelPc(x.handler));
                putU16(attr, x.catchType.empty() ? 0 : cls(x.catchType));
            }
            putU16(attr, 0);   // Code 无嵌套属性
            putU16(out, 1);    // method attr count
            putU16(out, codeName_);
            putU32(out, static_cast<ca::u32>(attr.size()));
            out.insert(out.end(), attr.begin(), attr.end());
        }
        putU16(out, 0);   // class attrs
        return out;
    }

private:
    /// emplace 结果（pair<iterator,bool>）回填 idx 并推进计数。
    template <typename EmplaceResult>
    ca::u16 intern(EmplaceResult&& inserted)
    {
        const ca::u16 idx  = next_;
        inserted.first->second = idx;
        next_                  = static_cast<ca::u16>(next_ + 1);
        return idx;
    }

    Bytes                       cp_;
    ca::u16                     next_ = 1;   // slot 0 无效
    std::map<std::string, ca::u16> utf8s_;
    std::map<std::string, ca::u16> classes_;
    std::map<std::string, ca::u16> strings_;
    std::map<ca::i32, ca::u16>     integers_;
    std::map<ca::i64, ca::u16>     longs_;
    std::map<std::string, ca::u16> nats_;
    std::map<std::string, ca::u16> refs_;
    std::vector<FieldSpec>      fields_;
    ca::u16                     thisIdx_  = 0;
    ca::u16                     superIdx_ = 0;
    ca::u16                     codeName_ = 0;
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
inline void CodeEmit::ldcString(std::string_view s)
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
inline void CodeEmit::ldcLong(ca::i64 v)
{
    op(jbc::core::Op::LDC2_W);
    u16(owner_->longCp(v));
}

/// 单类装配便利入口。
inline Bytes assembleClass(std::string_view thisClass, const std::vector<FieldSpec>& fields,
                           const std::vector<MethodSpec>& methods)
{
    ClassBytesBuilder b(thisClass);
    for (const FieldSpec& f : fields) {
        b.addField(f);
    }
    return b.assemble(methods);
}

}   // namespace mjbase::analysis::test_support
