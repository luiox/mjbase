/// @file        hierarchy_test_util.hpp
/// @brief       hierarchy 单测共用工具：KeyPool 工厂、手工摘要构造、
///        手工 classfile 字节构造（骨架，无方法体属性）。
///
/// 字节模式照 libs/libjbc/demo/control_flow.cpp：utf8/class 描述符条目
/// + 成员骨架；仅用单槽 CP 条目（Utf8/Class），无 long/double 双槽坑。
#pragma once

#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"

namespace mjbase::hierarchy::test {

/// @brief 新池：烧掉驻留 id 0（keys.hpp 约定 0 保留给「未设置」）。
inline KeyPool makePool()
{
    KeyPool pool;
    (void)pool.intern("");
    return pool;
}

/// @brief 手工类摘要（无成员）；super 留空 = 无父类（Object 形态）。
inline ClassSummary makeSummary(KeyPool& pool, std::string_view self,
                                std::string_view super, ca::u32 access = 0)
{
    ClassSummary s;
    s.self   = classKeyOf(pool, self);
    s.super  = super.empty() ? ClassKey{} : classKeyOf(pool, super);
    s.access = access;
    return s;
}

/// @brief 手工成员摘要。
inline MemberSummary makeMember(KeyPool& pool, std::string_view name,
                                std::string_view desc, ca::u32 access)
{
    MemberSummary m;
    m.name     = pool.intern(name);
    m.desc     = pool.intern(desc);
    m.access   = access;
    m.isStatic = (access & kAccStatic) != 0;
    return m;
}

/// @brief 手工 classfile 字节构造器（骨架级：字段/方法不带属性）。
class ClassBytesBuilder
{
public:
    struct MemberSpec
    {
        std::string_view name;
        std::string_view desc;
        ca::u32 access;
    };

    ClassBytesBuilder& setThis(std::string_view n)
    {
        thisName_ = n;
        return *this;
    }
    /// 留空 = 无父类（java/lang/Object 形态，super_class = 0）。
    ClassBytesBuilder& setSuper(std::string_view n)
    {
        superName_ = n;
        return *this;
    }
    ClassBytesBuilder& setAccess(ca::u32 a)
    {
        access_ = a;
        return *this;
    }
    ClassBytesBuilder& addInterface(std::string_view n)
    {
        ifaces_.push_back(n);
        return *this;
    }
    ClassBytesBuilder& addMethod(const MemberSpec& m)
    {
        methods_.push_back(m);
        return *this;
    }
    ClassBytesBuilder& addField(const MemberSpec& f)
    {
        fields_.push_back(f);
        return *this;
    }

    std::vector<ca::u8> build()
    {
        // 先物化全部 CP 引用（按需去重追加），再拼 classfile 主体。
        const ca::u16 thisCp = classRef(thisName_);
        const ca::u16 superCp = superName_.empty() ? 0 : classRef(superName_);
        std::vector<ca::u16> ifaceCps;
        for (const std::string_view n : ifaces_) ifaceCps.push_back(classRef(n));

        std::vector<std::array<ca::u16, 3>> fieldInfos;   // {access, name, desc}
        for (const MemberSpec& f : fields_) {
            fieldInfos.push_back({static_cast<ca::u16>(f.access), utf8(f.name),
                                  utf8(f.desc)});
        }
        std::vector<std::array<ca::u16, 3>> methodInfos;
        for (const MemberSpec& m : methods_) {
            methodInfos.push_back({static_cast<ca::u16>(m.access), utf8(m.name),
                                   utf8(m.desc)});
        }

        std::vector<ca::u8> out;
        putU4(out, 0xCAFEBABEu);
        putU2(out, 0);                       // minor
        putU2(out, 52);                      // major: V1_8
        putU2(out, nextIndex_);              // cp_count（0 槽不计条目）
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU2(out, static_cast<ca::u16>(access_));
        putU2(out, thisCp);
        putU2(out, superCp);
        putU2(out, static_cast<ca::u16>(ifaceCps.size()));
        for (const ca::u16 cp : ifaceCps) putU2(out, cp);
        putU2(out, static_cast<ca::u16>(fieldInfos.size()));
        for (const auto& f : fieldInfos) {
            putU2(out, f[0]);
            putU2(out, f[1]);
            putU2(out, f[2]);
            putU2(out, 0);   // attributes_count
        }
        putU2(out, static_cast<ca::u16>(methodInfos.size()));
        for (const auto& m : methodInfos) {
            putU2(out, m[0]);
            putU2(out, m[1]);
            putU2(out, m[2]);
            putU2(out, 0);   // attributes_count（无 Code，骨架形态）
        }
        putU2(out, 0);   // class attributes_count
        return out;
    }

private:
    ca::u16 utf8(std::string_view s)
    {
        const auto it = utf8Index_.find(s);
        if (it != utf8Index_.end()) return it->second;
        const ca::u16 idx = nextIndex_++;
        putU1(cp_, 0x01);
        putU2(cp_, static_cast<ca::u16>(s.size()));
        cp_.insert(cp_.end(), s.begin(), s.end());
        utf8Index_.emplace(s, idx);
        return idx;
    }

    ca::u16 classRef(std::string_view s)
    {
        const auto it = classIndex_.find(s);
        if (it != classIndex_.end()) return it->second;
        const ca::u16 nameCp = utf8(s);
        const ca::u16 idx    = nextIndex_++;
        putU1(cp_, 0x07);
        putU2(cp_, nameCp);
        classIndex_.emplace(s, idx);
        return idx;
    }

    static void putU1(std::vector<ca::u8>& out, ca::u8 v) { out.push_back(v); }

    static void putU2(std::vector<ca::u8>& out, ca::u16 v)
    {
        out.push_back(static_cast<ca::u8>(v >> 8));
        out.push_back(static_cast<ca::u8>(v & 0xFF));
    }

    static void putU4(std::vector<ca::u8>& out, ca::u32 v)
    {
        putU2(out, static_cast<ca::u16>(v >> 16));
        putU2(out, static_cast<ca::u16>(v & 0xFFFF));
    }

    std::string_view thisName_;
    std::string_view superName_;
    ca::u32 access_ = 0;
    std::vector<std::string_view> ifaces_;
    std::vector<MemberSpec> fields_;
    std::vector<MemberSpec> methods_;

    std::vector<ca::u8> cp_;
    ca::u16 nextIndex_ = 1;
    std::unordered_map<std::string_view, ca::u16> utf8Index_;
    std::unordered_map<std::string_view, ca::u16> classIndex_;
};

}   // namespace mjbase::hierarchy::test
