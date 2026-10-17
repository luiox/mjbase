/// @file        structural_pass_util.hpp
/// @brief       结构级 pass/原语单测的共享夹具：手工 classfile 字节构造。
///
/// 升格自 test/unittest/pass/（原 pass 域三试点专用），供 pass 与 spi 两域的
/// 结构级用例共用（成员 sorter / 结构编辑原语 / pass 名字注册面的构造-运行
/// 路径测试）。字节模式照 spi 测试与 libs/libjbc/demo/control_flow.cpp；在
/// spi_test_util 的最小构造器之上扩展：支持字段、Fieldref/Methodref/NameAndType
/// CP 条目与 Code 异常表（访问修饰符收紧 / 异常表剪枝用例的载体）。仅用单槽
/// CP 条目（无 long/double 双槽坑）。
#pragma once

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::test {

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

inline void putRaw(Bytes& b, std::string_view s)
{
    for (const char c : s) b.push_back(static_cast<ca::u8>(c));
}

/// @brief 把 src 追加到 out（临时实参安全版——两次取 begin/end 必须同一对象）。
inline void appendBytes(Bytes& out, Bytes src)
{
    out.insert(out.end(), src.begin(), src.end());
}

/// Code 异常表条目（catchCp = 0 表示 any）。
struct ExcSpec
{
    ca::u16 startPc   = 0;
    ca::u16 endPc     = 0;
    ca::u16 handlerPc = 0;
    ca::u16 catchCp   = 0;
};

/// 属性槽规格：名字 + 原始载荷字节（名字在 assemble 时物化为 Utf8 CP；载荷
/// 内的 CP 索引由用例经 utf8()/intConst() 等入口先行取得）。
struct FixtureAttr
{
    std::string_view name;
    Bytes            payload;
};

/// 方法规格：code 为裸指令字节。
struct FixtureMethod
{
    std::string_view   name;
    std::string_view   desc;
    Bytes              code;
    ca::u16            access = 0x0009;   // ACC_PUBLIC | ACC_STATIC
    std::vector<ExcSpec> exc;
    ca::u16            maxStack  = 1;
    ca::u16            maxLocals = 1;
    /// 方法级附加属性槽（排在 Code 之后；meta 家族单测用）。
    std::vector<FixtureAttr> attrs;
    /// Code 属性嵌套属性槽（LNT/LVT/嵌套注解族等）。
    std::vector<FixtureAttr> codeAttrs;
};

/// 字段规格。
struct FixtureField
{
    std::string_view name;
    std::string_view desc;
    ca::u16          access = 0x0009;
    /// 字段级附加属性槽（ConstantValue / Signature / 注解族等）。
    std::vector<FixtureAttr> attrs;
};

/// @brief 手工拼最小 classfile（V1_8 / public super / super=java/lang/Object /
///        无接口无类属性），支持字段与带异常表的方法。
///
/// 组装分两遍：先把全部 CP 条目物化（cpCount 才算得对），再写字节。
class FixtureBuilder
{
public:
    explicit FixtureBuilder(std::string_view thisClass)
    {
        thisIdx_  = cls(thisClass);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
    }

    /// @brief 覆写父类（虚覆盖 / 层次夹具用；须在 assemble 前调用——CP 只增）。
    FixtureBuilder& setSuper(std::string_view super)
    {
        superIdx_ = cls(super);
        return *this;
    }

    void addField(const FixtureField& f) { fields_.push_back(f); }

    void addMethod(const FixtureMethod& m) { methods_.push_back(m); }

    /// 类级附加属性槽（SourceFile / 注解族 / 未知属性等；assemble 前追加）。
    void addClassAttr(const FixtureAttr& a) { classAttrs_.push_back(a); }

    /// 覆写类访问标志（非法 access 位 / enum / synthetic 夹具用）。
    FixtureBuilder& setClassAccess(ca::u16 access)
    {
        classAccess_ = access;
        return *this;
    }

    Bytes assemble()
    {
        // 第一遍：成员 name/desc 先物化——cpCount 与 CP 字节必须等全部条目
        // 齐备后再落（否则索引越界，libjbc 直接拒读）。属性槽名同样前置物化。
        struct NameDesc
        {
            ca::u16 name;
            ca::u16 desc;
        };
        std::vector<NameDesc> fieldSlots;
        fieldSlots.reserve(fields_.size());
        for (const FixtureField& f : fields_) {
            fieldSlots.push_back({utf8(f.name), utf8(f.desc)});
            for (const FixtureAttr& a : f.attrs) utf8(a.name);
        }
        std::vector<NameDesc> methodSlots;
        methodSlots.reserve(methods_.size());
        for (const FixtureMethod& m : methods_) {
            methodSlots.push_back({utf8(m.name), utf8(m.desc)});
            for (const FixtureAttr& a : m.attrs) utf8(a.name);
            for (const FixtureAttr& a : m.codeAttrs) utf8(a.name);
        }
        for (const FixtureAttr& a : classAttrs_) utf8(a.name);

        Bytes out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);   // V1_8
        putU16(out, next_);  // cpCount（slot 0 无效）
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, classAccess_);   // 缺省 public super
        putU16(out, thisIdx_);
        putU16(out, superIdx_);
        putU16(out, 0);   // interfaces
        putU16(out, static_cast<ca::u16>(fields_.size()));
        for (ca::usize i = 0; i < fields_.size(); ++i) {
            const FixtureField& f = fields_[i];
            putU16(out, f.access);
            putU16(out, fieldSlots[i].name);
            putU16(out, fieldSlots[i].desc);
            putU16(out, static_cast<ca::u16>(f.attrs.size()));
            for (const FixtureAttr& a : f.attrs) {
                putU16(out, utf8(a.name));
                putU32(out, static_cast<ca::u32>(a.payload.size()));
                out.insert(out.end(), a.payload.begin(), a.payload.end());
            }
        }
        putU16(out, static_cast<ca::u16>(methods_.size()));
        for (ca::usize i = 0; i < methods_.size(); ++i) {
            const FixtureMethod& m = methods_[i];
            putU16(out, m.access);
            putU16(out, methodSlots[i].name);
            putU16(out, methodSlots[i].desc);
            Bytes attr;
            putU16(attr, m.maxStack);
            putU16(attr, m.maxLocals);
            putU32(attr, static_cast<ca::u32>(m.code.size()));
            attr.insert(attr.end(), m.code.begin(), m.code.end());
            putU16(attr, static_cast<ca::u16>(m.exc.size()));
            for (const ExcSpec& e : m.exc) {
                putU16(attr, e.startPc);
                putU16(attr, e.endPc);
                putU16(attr, e.handlerPc);
                putU16(attr, e.catchCp);
            }
            putU16(attr, static_cast<ca::u16>(m.codeAttrs.size()));
            for (const FixtureAttr& a : m.codeAttrs) {
                putU16(attr, utf8(a.name));
                putU32(attr, static_cast<ca::u32>(a.payload.size()));
                attr.insert(attr.end(), a.payload.begin(), a.payload.end());
            }
            putU16(out, static_cast<ca::u16>(1 + m.attrs.size()));   // Code + 附加槽
            putU16(out, codeName_);
            putU32(out, static_cast<ca::u32>(attr.size()));
            out.insert(out.end(), attr.begin(), attr.end());
            for (const FixtureAttr& a : m.attrs) {
                putU16(out, utf8(a.name));
                putU32(out, static_cast<ca::u32>(a.payload.size()));
                out.insert(out.end(), a.payload.begin(), a.payload.end());
            }
        }
        putU16(out, static_cast<ca::u16>(classAttrs_.size()));
        for (const FixtureAttr& a : classAttrs_) {
            putU16(out, utf8(a.name));
            putU32(out, static_cast<ca::u32>(a.payload.size()));
            out.insert(out.end(), a.payload.begin(), a.payload.end());
        }
        return out;
    }

    // ---- CP 物化入口（用例内拼引用指令的操作数用）----

    ca::u16 utf8(std::string_view s)
    {
        std::string key(s);
        const auto  it = utf8s_.find(key);
        if (it != utf8s_.end()) return it->second;
        putU8(cp_, 0x01);
        putU16(cp_, static_cast<ca::u16>(s.size()));
        putRaw(cp_, s);
        const ca::u16 idx = next_++;
        utf8s_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 cls(std::string_view n)
    {
        std::string key(n);
        const auto  it = classes_.find(key);
        if (it != classes_.end()) return it->second;
        const ca::u16 u = utf8(n);
        putU8(cp_, 0x07);
        putU16(cp_, u);
        const ca::u16 idx = next_++;
        classes_.emplace(std::move(key), idx);
        return idx;
    }

    ca::u16 nameType(std::string_view n, std::string_view d)
    {
        std::string key(std::string(n).append("|").append(d));
        const auto  it = nameTypes_.find(key);
        if (it != nameTypes_.end()) return it->second;
        const ca::u16 ni = utf8(n);
        const ca::u16 di = utf8(d);
        putU8(cp_, 0x0c);
        putU16(cp_, ni);
        putU16(cp_, di);
        const ca::u16 idx = next_++;
        nameTypes_.emplace(std::move(key), idx);
        return idx;
    }

    /// Fieldref(owner.name:desc) 的 CP 索引（GETSTATIC/PUTSTATIC 操作数）。
    ca::u16 fieldRef(std::string_view owner, std::string_view n, std::string_view d)
    {
        std::string key(std::string(owner).append("|").append(n).append("|").append(d));
        const auto  it = refs_.find(key);
        if (it != refs_.end()) return it->second;
        const ca::u16 ci = cls(owner);
        const ca::u16 nt = nameType(n, d);
        putU8(cp_, 0x09);
        putU16(cp_, ci);
        putU16(cp_, nt);
        const ca::u16 idx = next_++;
        refs_.emplace(std::move(key), idx);
        return idx;
    }

    /// Methodref(owner.name(desc)) 的 CP 索引（INVOKESTATIC 操作数）。
    ca::u16 methodRef(std::string_view owner, std::string_view n, std::string_view d)
    {
        std::string key(std::string(owner).append("|").append(n).append("|").append(d));
        const auto  it = refs_.find(key);
        if (it != refs_.end()) return it->second;
        const ca::u16 ci = cls(owner);
        const ca::u16 nt = nameType(n, d);
        putU8(cp_, 0x0a);
        putU16(cp_, ci);
        putU16(cp_, nt);
        const ca::u16 idx = next_++;
        refs_.emplace(std::move(key), idx);
        return idx;
    }

    /// Integer 常量的 CP 索引（LDC 操作数；单槽条目）。
    ca::u16 intConst(ca::i32 v)
    {
        std::string key = "int:" + std::to_string(v);
        const auto  it = ints_.find(key);
        if (it != ints_.end()) return it->second;
        putU8(cp_, 0x03);
        putU32(cp_, static_cast<ca::u32>(v));
        const ca::u16 idx = next_++;
        ints_.emplace(std::move(key), idx);
        return idx;
    }

    /// long 常量的 CP 索引（LDC2_W 操作数；tag5 双槽条目，占两个下标）。
    ca::u16 longConst(ca::i64 v)
    {
        std::string key = "long:" + std::to_string(v);
        const auto  it = longs_.find(key);
        if (it != longs_.end()) return it->second;
        putU8(cp_, 0x05);
        putU32(cp_, static_cast<ca::u32>(static_cast<ca::u64>(v) >> 32));
        putU32(cp_, static_cast<ca::u32>(v));
        const ca::u16 idx = next_;
        next_ += 2;   // 双槽：下一 CP 条目跳过 placeholder 下标
        longs_.emplace(std::move(key), idx);
        return idx;
    }

    /// String 常量的 CP 索引（LDC 操作数；tag8 单槽条目，挂 Utf8 载荷）。
    ca::u16 strConst(std::string_view v)
    {
        std::string key(v);
        const auto  it = strs_.find(key);
        if (it != strs_.end()) return it->second;
        const ca::u16 u = utf8(v);
        putU8(cp_, 0x08);
        putU16(cp_, u);
        const ca::u16 idx = next_++;
        strs_.emplace(std::move(key), idx);
        return idx;
    }

private:
    Bytes   cp_;
    ca::u16 next_ = 1;   // slot 0 无效
    std::unordered_map<std::string, ca::u16> utf8s_;
    std::unordered_map<std::string, ca::u16> classes_;
    std::unordered_map<std::string, ca::u16> nameTypes_;
    std::unordered_map<std::string, ca::u16> refs_;
    std::unordered_map<std::string, ca::u16> ints_;
    std::unordered_map<std::string, ca::u16> longs_;
    std::unordered_map<std::string, ca::u16> strs_;
    ca::u16 thisIdx_  = 0;
    ca::u16 superIdx_ = 0;
    ca::u16 codeName_ = 0;
    ca::u16 classAccess_ = 0x0021;
    std::vector<FixtureAttr>   classAttrs_;
    std::vector<FixtureField>  fields_;
    std::vector<FixtureMethod> methods_;
};

/// 常用指令片段：GETSTATIC cp; POP（占栈 1 出栈 1）。
inline Bytes getStaticPop(ca::u16 cp)
{
    return {0xb2, static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp), 0x57};
}

/// 常用指令片段：INVOKESTATIC cp（无参无返回，栈效应 0）。
inline Bytes invokeStatic(ca::u16 cp)
{
    return {0xb8, static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp)};
}

/// 常用指令片段：NOP; RETURN。
inline Bytes nopReturn()
{
    return {0x00, 0xb1};
}

/// 常用指令片段：RETURN。
inline Bytes bareReturn()
{
    return {0xb1};
}

/// 把 bytes 作为 ClassResource 加入容器。
inline void addClassResource(mjbase::model::ResourceContainer& container, const char* uri,
                             Bytes bytes)
{
    container.addResource(uri,
                          std::make_unique<mjbase::model::ClassResource>(uri, std::move(bytes)));
}

}   // namespace mjbase::test
