/// meta 元数据清理家族（类级骨架组 13 件）单测：移植自
/// mbase meta 段用例，并按新契约补齐
/// 载体特有面——三层属性槽摘除/槽内过滤的落盘位置、access 标志位双面同步、
/// Code 嵌套属性表的有界破例面（家族头注）、条件 PA。
///
/// 夹具纪律：类骨架（类头/字段/方法/code 体）一律 jasm 文本装配（major 49
/// 帧less 口径，与退役的字节级 FixtureBuilder 夹具同实；本家族所需 access
/// 位全部在装配器修饰词表内）。纪律例外②申报：类/成员属性（注解表、
/// TypeAnnotations、Signature、SourceFile/SourceDebugExtension、Record、
/// MethodParameters、ConstantValue、AnnotationDefault、未知杂槽与 legacy
/// 载荷形）jasm 无指令面——从装配合法骨架出发，经 readClass→CP 物化→
/// AttrSlot 挂 Raw 新槽→writeClass 产出夹具字节（与会话解码路径同源；
/// Code 嵌套槽同面，经 decodeCode 挂体后向 CodeAttr::attrs 追加）。属性
/// 载荷为二进制正形（JVMS 4.7.16-4.7.30），putU16 拼装属属性载荷构建而非
/// 类构造。唯一 access 杂位：类级 0x0040 位词表无修饰词
///（IllegalAccessRemover 用例），附着面单点 OR（extraClassAccess）。
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/bytes.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/pass/meta_info_remover.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

using jbc::core::AnnotationsAttr;
using jbc::core::AttrKind;
using jbc::core::ByteSink;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::MemberInfo;
using jbc::core::RawAttr;
using jbc::core::RecordAttr;
using jbc::core::TypeAnnotationsAttr;

// ── 属性载荷构造（例外②附着面；CP 索引在附着时物化）──

/// 属性规格：槽名 + 载荷构建器（构建器在附着时对目标 ClassFile 的 CP
/// 物化索引——CP 索引由 addUtf8/addInteger 去重确定，与会话侧一致）。
struct AttrSpec
{
    std::string                      name;
    std::function<Bytes(ClassFile&)> build;
};

/// 字面载荷槽（SourceDebugExtension/未知杂槽/legacy 布局/畸形载荷——字节
/// 即权威形）。
AttrSpec rawAttr(std::string name, Bytes raw)
{
    return AttrSpec{std::move(name), [raw](ClassFile&) { return raw; }};
}

/// CP Utf8 去重物化（findUtf8 命中既有池返回原索引，miss 才追加——同一
/// desc 多次物化不会产生重复条目；行载荷逐字节断言与 typed 面原样重编码
/// 要求同 desc 同索引）。
ca::u32 internUtf8(ClassFile& cf, std::string_view s)
{
    const ca::u32 found = cf.cp.findUtf8(s);
    return found != 0 ? found : cf.cp.addUtf8(s);
}

/// SourceFile：u2 源文件名 CP。
AttrSpec sourceFileAttr(std::string name)
{
    return AttrSpec{"SourceFile", [name](ClassFile& cf) {
        Bytes p;
        putU16(p, internUtf8(cf, name));
        return p;
    }};
}

/// Signature：u2 签名串 CP。
AttrSpec signatureAttr(std::string sig)
{
    return AttrSpec{"Signature", [sig](ClassFile& cf) {
        Bytes p;
        putU16(p, internUtf8(cf, sig));
        return p;
    }};
}

/// 注解表（Runtime{Visible,Invisible}Annotations）：u2 条数 + {u2 类型描述符
/// CP, u2 键值对数=0}*。
AttrSpec annoAttr(std::string slotName, std::vector<std::string> descs)
{
    return AttrSpec{std::move(slotName), [descs](ClassFile& cf) {
        Bytes p;
        putU16(p, static_cast<ca::u16>(descs.size()));
        for (const std::string& d : descs) {
            putU16(p, internUtf8(cf, d));
            putU16(p, 0);
        }
        return p;
    }};
}

/// type annotation 表（Runtime{Visible,Invisible}TypeAnnotations）：u2 条数 +
/// {u1 target_type, 空 target_info, u1 路径长=0, u2 描述符 CP, u2 键值对=0}*。
/// 空 target_info 仅对 0x13/0x14/0x15 形态成立——本夹具只用这三类（含挂在
/// Code 嵌套槽的合成形态：libjbc 按条目自身 target_type 解码，不验挂载点）。
AttrSpec typeAnnoAttr(std::string slotName, ca::u8 targetType,
                      std::vector<std::string> descs)
{
    return AttrSpec{std::move(slotName), [targetType, descs](ClassFile& cf) {
        Bytes p;
        putU16(p, static_cast<ca::u16>(descs.size()));
        for (const std::string& d : descs) {
            putU8(p, targetType);
            putU8(p, 0);
            putU16(p, internUtf8(cf, d));
            putU16(p, 0);
        }
        return p;
    }};
}

/// 参数注解 legacy 形（u2 参数数=1 + u2 注解数 + 注解表）：仅整槽摘除面
/// 用（载荷形状与摘除判定无关）。
AttrSpec paramAnnoTableAttr(std::string slotName, std::vector<std::string> descs)
{
    return AttrSpec{std::move(slotName), [descs](ClassFile& cf) {
        Bytes p;
        putU16(p, 1);
        putU16(p, static_cast<ca::u16>(descs.size()));
        for (const std::string& d : descs) {
            putU16(p, internUtf8(cf, d));
            putU16(p, 0);
        }
        return p;
    }};
}

/// 注解条目构建器（行载荷的条目单元，构建时对 CP 物化）。
using EntryBuilder = std::function<Bytes(ClassFile&)>;

/// 平条目：type + 0 键值对。
EntryBuilder simpleAnnoEntry(std::string desc)
{
    return [desc](ClassFile& cf) {
        Bytes e;
        putU16(e, internUtf8(cf, desc));
        putU16(e, 0);
        return e;
    };
}

/// 带嵌套载荷的注解条目：type + 1 键值对（'[ 数组 → '@ 嵌套注解）——参数
/// 注解条目测量器的递归面（条目随顶层描述符整体摘除）。
EntryBuilder nestedAnnoEntry(std::string desc, std::string nestedDesc)
{
    return [desc, nestedDesc](ClassFile& cf) {
        Bytes e;
        putU16(e, internUtf8(cf, desc));
        putU16(e, 1);                    // 键值对数
        putU16(e, internUtf8(cf, "value"));
        putU8(e, '[');                   // 数组
        putU16(e, 1);                    // 数组元素数
        putU8(e, '@');                   // 嵌套注解
        putU16(e, internUtf8(cf, nestedDesc));
        putU16(e, 0);                    // 嵌套注解键值对数
        return e;
    };
}

/// 参数注解行载荷（JVMS 4.7.20 正形：u1 参数数 + {u2 注解数 + 条目}*）。
AttrSpec paramAnnoRowsAttr(std::string slotName,
                           std::vector<std::vector<EntryBuilder>> rows)
{
    return AttrSpec{std::move(slotName), [rows](ClassFile& cf) {
        Bytes p;
        putU8(p, static_cast<ca::u8>(rows.size()));
        for (const std::vector<EntryBuilder>& row : rows) {
            putU16(p, static_cast<ca::u16>(row.size()));
            for (const EntryBuilder& e : row) appendBytes(p, e(cf));
        }
        return p;
    }};
}

/// 平描述符行的正形参数注解（条目全部 0 键值对）。
AttrSpec paramAnnoFlatRowsAttr(std::string slotName,
                               std::vector<std::vector<std::string>> rows)
{
    std::vector<std::vector<EntryBuilder>> builders;
    builders.reserve(rows.size());
    for (std::vector<std::string>& row : rows) {
        std::vector<EntryBuilder> rowBuilders;
        rowBuilders.reserve(row.size());
        for (std::string& d : row) rowBuilders.push_back(simpleAnnoEntry(std::move(d)));
        builders.push_back(std::move(rowBuilders));
    }
    return paramAnnoRowsAttr(std::move(slotName), std::move(builders));
}

/// Record 组件规格：名/描述符 + 嵌套属性规格。
struct RecordCompSpec
{
    std::string           name;
    std::string           desc;
    std::vector<AttrSpec> attrs;
};

/// Record 属性（JVMS 4.7.30）：u2 组件数 + {u2 名 CP, u2 描述符 CP,
/// u2 属性数 + 嵌套属性槽}*。
AttrSpec recordAttr(std::vector<RecordCompSpec> comps)
{
    return AttrSpec{"Record", [comps](ClassFile& cf) {
        Bytes p;
        putU16(p, static_cast<ca::u16>(comps.size()));
        for (const RecordCompSpec& c : comps) {
            putU16(p, internUtf8(cf, c.name));
            putU16(p, internUtf8(cf, c.desc));
            putU16(p, static_cast<ca::u16>(c.attrs.size()));
            for (const AttrSpec& a : c.attrs) {
                putU16(p, internUtf8(cf, a.name));
                const Bytes payload = a.build(cf);
                putU32(p, static_cast<ca::u32>(payload.size()));
                appendBytes(p, payload);
            }
        }
        return p;
    }};
}

/// MethodParameters：u1 参数数（JVMS 4.7.24 parameters_count）+
/// {u2 名字 CP, u2 access}*。
AttrSpec methodParametersAttr(std::vector<std::pair<std::string, ca::u16>> params)
{
    return AttrSpec{"MethodParameters", [params](ClassFile& cf) {
        Bytes p;
        putU8(p, static_cast<ca::u8>(params.size()));
        for (const auto& [n, acc] : params) {
            putU16(p, internUtf8(cf, n));
            putU16(p, acc);
        }
        return p;
    }};
}

/// AnnotationDefault：element_value（'B' 常量形态：tag + u2 Integer CP）。
AttrSpec annotationDefaultAttr(ca::i32 v)
{
    return AttrSpec{"AnnotationDefault", [v](ClassFile& cf) {
        Bytes p;
        putU8(p, 'B');
        putU16(p, cf.cp.addInteger(v));
        return p;
    }};
}

/// ConstantValue：u2 Integer CP。
AttrSpec constValueAttr(ca::i32 v)
{
    return AttrSpec{"ConstantValue", [v](ClassFile& cf) {
        Bytes p;
        putU16(p, cf.cp.addInteger(v));
        return p;
    }};
}

/// legacy MethodParameters 载荷（旧栈 writer 字节形：u1 count + {u2 name,
/// u2 acc}*，与 JVMS 4.7.24 同形）：名字 CP 指针以池内真实 Utf8 承载
///（原 0x0021 不透明字面值的同语义落点；断言只钉 count 位与 access 位）。
AttrSpec legacyParamLayoutAttr()
{
    return AttrSpec{"MethodParameters", [](ClassFile& cf) {
        const ca::u16 nameCp = static_cast<ca::u16>(internUtf8(cf, "p0"));
        return Bytes{0x01,
                     static_cast<ca::u8>(nameCp >> 8),
                     static_cast<ca::u8>(nameCp),
                     0x80, 0x11};
    }};
}

// ── 类骨架装配 + 附着面 ──

/// 方法 code 体便捷文本。
constexpr std::string_view kReturn    = "return\n";
constexpr std::string_view kNopReturn = "nop\nreturn\n";

/// access 位 → jasm 修饰词序（与装配器词表同源：0x20 类=super/方法=
/// synchronized，0x40/0x80 方法=bridge/varargs、字段=volatile/transient；
/// 类 0x40 无词表修饰词——extraClassAccess 单点 OR 承载）。
std::string accessModifiers(ca::u16 acc, int kind)   // 0=class 1=method 2=field
{
    std::string out;
    const auto add = [&](std::string_view w, ca::u16 bit) {
        if ((acc & bit) != 0) {
            out += w;
            out += ' ';
        }
    };
    add("public", 0x0001);
    add("private", 0x0002);
    add("protected", 0x0004);
    add("static", 0x0008);
    add("final", 0x0010);
    if (kind == 0) add("super", 0x0020);
    if (kind == 1) add("synchronized", 0x0020);
    if (kind == 1) add("bridge", 0x0040);
    if (kind == 2) add("volatile", 0x0040);
    if (kind == 1) add("varargs", 0x0080);
    if (kind == 2) add("transient", 0x0080);
    add("native", 0x0100);
    add("interface", 0x0200);
    add("abstract", 0x0400);
    add("strict", 0x0800);
    add("synthetic", 0x1000);
    add("annotation", 0x2000);
    add("enum", 0x4000);
    return out;
}

struct FieldSpec
{
    std::string_view      name;
    std::string_view      desc;
    ca::u16               access = 0x0009;
    std::vector<AttrSpec> attrs;
};

struct MethodSpec
{
    std::string_view      name;
    std::string_view      desc;
    std::string_view      body;   // code 体文本（不含外壳）
    ca::u16               access  = 0x0009;
    std::vector<AttrSpec> attrs;  // 方法级（Code 外）
    std::vector<AttrSpec> codeAttrs;   // Code 嵌套
};

/// 例外②附着：向属性表挂 Raw 新槽（载荷构建时对 CP 物化）。
void attachAttrs(ClassFile& cf, std::vector<AttrSlot>& attrs,
                 const std::vector<AttrSpec>& specs)
{
    for (const AttrSpec& a : specs) {
        attrs.push_back(AttrSlot::makeNew(internUtf8(cf, a.name),
                                          std::make_unique<RawAttr>(a.build(cf))));
    }
}

/// 例外②附着（Code 嵌套）：decodeCode 挂体后向 CodeAttr::attrs 追加
///（decode→改→write 的既有路径，既有嵌套槽原样保留）。
void attachCodeAttrs(ClassFile& cf, MemberInfo& m, const std::vector<AttrSpec>& specs)
{
    for (AttrSlot& slot : m.attrs) {
        if (cf.utf8(slot.nameCp) != "Code") continue;
        const auto d = cf.decodeCode(m);
        EXPECT_TRUE(d.is_ok());
        if (d.is_err()) return;
        CodeAttr* code = d.unwrap();
        for (const AttrSpec& a : specs) {
            code->attrs.push_back(AttrSlot::makeNew(internUtf8(cf, a.name),
                                                    std::make_unique<RawAttr>(a.build(cf))));
        }
        return;
    }
    ADD_FAILURE() << "attachCodeAttrs: 方法无 Code 槽";
}

/// 家族夹具：类骨架 jasm 装配（major 49 帧less）+ 例外②属性附着。
/// classAccess 只传词表可表达位；extraClassAccess 为附着面单点 OR（类
/// 0x0040 杂位）。
Bytes familyFixture(ca::u16 classAccess, ca::u16 extraClassAccess,
                    std::vector<AttrSpec> classAttrs, std::vector<FieldSpec> fields,
                    std::vector<MethodSpec> methods)
{
    std::string src = R"JASM(.super java/lang/Object
.class )JASM" + accessModifiers(classAccess, 0) + "T {\n";
    for (const FieldSpec& f : fields) {
        src += "  .field " + accessModifiers(f.access, 2) + std::string(f.name) + " "
               + std::string(f.desc) + "\n";
    }
    for (const MethodSpec& m : methods) {
        src += "  .method " + accessModifiers(m.access, 1) + std::string(m.name) + " "
               + std::string(m.desc) + R"JASM( {
    code: {
)JASM" + std::string(m.body) + R"JASM(    }
  }
)JASM";
    }
    src += "}\n";
    const Bytes assembled = assembleJasm(src);
    if (assembled.empty()) return {};

    auto read = jbc::core::readClass(jbc::core::ByteView(assembled));
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return {};
    jbc::core::ReadResult rr = std::move(read).unwrap();
    ClassFile&        cf     = *rr.file;
    cf.access |= extraClassAccess;

    attachAttrs(cf, cf.attrs, classAttrs);
    EXPECT_EQ(cf.fields.size(), fields.size());
    if (cf.fields.size() == fields.size()) {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            EXPECT_EQ(cf.utf8(cf.fields[i].nameCp), fields[i].name);
            attachAttrs(cf, cf.fields[i].attrs, fields[i].attrs);
        }
    }
    EXPECT_EQ(cf.methods.size(), methods.size());
    if (cf.methods.size() == methods.size()) {
        for (std::size_t i = 0; i < methods.size(); ++i) {
            EXPECT_EQ(cf.utf8(cf.methods[i].nameCp), methods[i].name);
            attachAttrs(cf, cf.methods[i].attrs, methods[i].attrs);
            if (!methods[i].codeAttrs.empty()) {
                attachCodeAttrs(cf, cf.methods[i], methods[i].codeAttrs);
            }
        }
    }

    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    return written.is_ok() ? std::move(written).unwrap() : Bytes{};
}

// ── 断言辅助 ──

MemberInfo* findMember(ClassFile& cf, std::vector<MemberInfo>& ms, std::string_view name)
{
    for (MemberInfo& m : ms) {
        if (cf.utf8(m.nameCp) == name) return &m;
    }
    return nullptr;
}

std::set<std::string> attrNames(const ClassFile& cf, const std::vector<AttrSlot>& attrs)
{
    std::set<std::string> out;
    for (const AttrSlot& s : attrs) out.emplace(std::string(cf.utf8(s.nameCp)));
    return out;
}

/// 注解槽内条目的类型描述符序列（保持原序）。
std::vector<std::string> annoDescs(ClassFile& cf, AttrSlot& slot)
{
    std::vector<std::string> out;
    auto                     decoded = cf.decodeAttr(slot);
    if (decoded.is_err()) return out;
    if (std::move(decoded).unwrap()->kind() != AttrKind::Annotations) return out;
    const auto* body = ca::core::tag_cast::cast<const AnnotationsAttr*>(slot.peek());
    for (const AnnotationsAttr::Annotation& a : body->annos) {
        out.emplace_back(cf.utf8(a.typeCp));
    }
    return out;
}

/// type annotation 槽内条目的类型描述符序列（保持原序）。
std::vector<std::string> typeAnnoDescs(ClassFile& cf, AttrSlot& slot)
{
    std::vector<std::string> out;
    auto                     decoded = cf.decodeAttr(slot);
    if (decoded.is_err()) return out;
    if (std::move(decoded).unwrap()->kind() != AttrKind::TypeAnnotations) return out;
    const auto* body = ca::core::tag_cast::cast<const TypeAnnotationsAttr*>(slot.peek());
    for (const TypeAnnotationsAttr::Annotation& a : body->annotations) {
        out.emplace_back(cf.utf8(a.typeIndexCp));
    }
    return out;
}

/// 类/成员上首个指定名注解槽的条目描述符（无槽为空）。
std::vector<std::string> firstAnnoDescs(ClassFile& cf, std::vector<AttrSlot>& attrs,
                                        const char* slotName)
{
    for (AttrSlot& slot : attrs) {
        if (cf.utf8(slot.nameCp) == slotName) return annoDescs(cf, slot);
    }
    return {};
}

CodeAttr* decodeCodeAttr(ClassFile& cf, MemberInfo& m)
{
    for (AttrSlot& slot : m.attrs) {
        if (cf.utf8(slot.nameCp) != "Code") continue;
        auto decoded = cf.decodeCode(m);
        if (decoded.is_err()) return nullptr;
        return decoded.unwrap();
    }
    return nullptr;
}

/// 类级 Record 槽的 typed 体（经 decodeAttr 挂体；无槽/解码失败/类型不符
/// 为空）。
const RecordAttr* decodedRecordAttr(ClassFile& cf)
{
    for (AttrSlot& slot : cf.attrs) {
        if (cf.utf8(slot.nameCp) != "Record") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return nullptr;
        if (std::move(d).unwrap()->kind() != AttrKind::Record) return nullptr;
        return ca::core::tag_cast::cast<const RecordAttr*>(slot.peek());
    }
    return nullptr;
}

/// 属性表指定名槽的载荷字节（typed 体经 encodeAttr 重编码去槽头；Raw 直取；
/// 解码失败的 legacy 形态——body 为空且未被接管——走源 span）。
/// 无槽/序列化失败为 nullopt。
std::optional<std::vector<ca::u8>> slotPayloadBytes(ClassFile& cf, std::vector<AttrSlot>& attrs,
                                                    std::string_view name)
{
    for (AttrSlot& slot : attrs) {
        if (cf.utf8(slot.nameCp) != name) continue;
        if (auto d = cf.decodeAttr(slot); d.is_err() && !slot.decoded()) {
            const auto view = cf.source().spanOrEmpty(slot.data);
            return std::vector<ca::u8>(view.data(), view.data() + view.size());
        }
        if (slot.decoded() && slot.peek()->kind() == AttrKind::Raw) {
            return ca::core::tag_cast::cast<const RawAttr*>(slot.peek())->bytes();
        }
        ByteSink out;
        if (encodeAttr(cf, slot, out, cf.source()).is_err()) return std::nullopt;
        std::vector<ca::u8> full = std::move(out.bytes());
        if (full.size() < 6) return std::nullopt;
        const ca::u32 len = (static_cast<ca::u32>(full[2]) << 24)
                            | (static_cast<ca::u32>(full[3]) << 16)
                            | (static_cast<ca::u32>(full[4]) << 8)
                            | static_cast<ca::u32>(full[5]);
        if (full.size() != 6 + static_cast<std::size_t>(len)) return std::nullopt;
        return std::vector<ca::u8>(full.begin() + 6, full.end());
    }
    return std::nullopt;
}

/// CP 内指定 Utf8 的索引（夹具载荷先行物化，必在；会话侧 CP 与夹具侧
/// 同源——pass 不动 CP）。
ca::u16 utf8IndexOf(ClassFile& cf, std::string_view s)
{
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (cf.cp.isUtf8(i) && cf.utf8(i) == s) return static_cast<ca::u16>(i);
    }
    ADD_FAILURE() << "Utf8 缺席: " << s;
    return 0;
}

/// 组会话（按字节开组）。
std::shared_ptr<jbc::ir::ClassSession> makeSession(const Bytes& bytes)
{
    auto result = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(result.is_ok());
    return result.is_ok() ? std::move(result).unwrap() : nullptr;
}

}   // namespace

// ── MetaInfoRemover ──

TEST(MjPassMetaInfoRemoverTest, StripsListedMetaAcrossThreeLevels)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            sourceFileAttr("T.java"),
            rawAttr("SourceDebugExtension", Bytes{'a', 'b', 'c'}),
            annoAttr("RuntimeVisibleAnnotations", {"LFoo/Keep;"}),
            typeAnnoAttr("RuntimeVisibleTypeAnnotations", 0x13, {"LFoo/Keep;"}),
        },
        {
            {"fin", "I", 0x0019, {annoAttr("RuntimeVisibleAnnotations", {"LFoo/Keep;"})}},
        },
        {
            {"m", "()V", kReturn, 0x0009,
             {annoAttr("RuntimeVisibleAnnotations", {"LFoo/Keep;"}),
              paramAnnoTableAttr("RuntimeVisibleParameterAnnotations", {"LFoo/Keep;"}),
              paramAnnoTableAttr("RuntimeInvisibleParameterAnnotations", {"LFoo/Keep;"}),
              methodParametersAttr({{"p0", 0x0010}}),
              annotationDefaultAttr(7)},
             {}},
            {"debug", "()V", kNopReturn, 0x0009, {},
             {rawAttr("LineNumberTable", Bytes{0x00, 0x01, 0x00, 0x00, 0x00, 0x2A}),
              rawAttr("LocalVariableTable", Bytes{0x00, 0x00}),
              typeAnnoAttr("RuntimeVisibleTypeAnnotations", 0x13, {"LFoo/Keep;"})}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::MetaInfoRemover     pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());

    ClassFile& cf = session->backing();
    EXPECT_TRUE(attrNames(cf, cf.attrs).empty());
    ASSERT_EQ(cf.fields.size(), 1u);
    EXPECT_TRUE(attrNames(cf, cf.fields[0].attrs).empty());
    ASSERT_EQ(cf.methods.size(), 2u);
    for (MemberInfo& m : cf.methods) {
        const std::set<std::string> names = attrNames(cf, m.attrs);
        EXPECT_EQ(names.count("Code"), 1u) << "Code 必须存活";
        EXPECT_EQ(names.size(), 1u) << "方法级 meta 槽应全数摘除";
    }
    // Code 嵌套：debug 方法的 LNT/LVT/嵌套 TA 整表消失，Code 本体存活
    //（nop;return，装配器真值 maxStack=0）。
    CodeAttr* debugCode = decodeCodeAttr(cf, cf.methods[1]);
    ASSERT_NE(debugCode, nullptr);
    EXPECT_TRUE(debugCode->attrs.empty());
    EXPECT_EQ(debugCode->code.size(), 2u);   // NOP; RETURN 原样
    EXPECT_EQ(debugCode->maxStack, 0u);
}

TEST(MjPassMetaInfoRemoverTest, KeepsSignatureConstantValueAndUnknownAttrs)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            signatureAttr("Ljava/lang/List<Ljava/lang/String;>;"),
            rawAttr("NotAStandardAttr", Bytes{0x01, 0x02}),
        },
        {
            {"cv", "I", 0x0009, {constValueAttr(42), signatureAttr("I")}},
        },
        {
            {"m", "(I)V", kReturn, 0x0009, {signatureAttr("(I)V)")}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::MetaInfoRemover     pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_FALSE(pa.changed()) << "本夹具无任何可摘面——条件 PA 应 all()";

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"Signature", "NotAStandardAttr"}));
    EXPECT_EQ(attrNames(cf, cf.fields[0].attrs), (std::set<std::string>{"ConstantValue", "Signature"}));
    EXPECT_EQ(attrNames(cf, cf.methods[0].attrs), (std::set<std::string>{"Code", "Signature"}));
}

TEST(MjPassMetaInfoRemoverTest, CleanClassIsByteIdentical)
{
    const Bytes in = familyFixture(0x0021, 0, {}, {}, {{"m", "()V", kReturn, 0x0009, {}, {}}});
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
    auto beforeResult = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(beforeResult.is_ok());
    const Bytes before = std::move(beforeResult).unwrap();

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::MetaInfoRemover     pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.areAllPreserved());

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(written.unwrap(), before);
}

// record 组件嵌套注解清空（typed RecordAttr 组件 AttrSlot 表，父 Record 槽
// 连带置脏）。

TEST(MjPassMetaInfoRemoverTest, ClearsRecordComponentAnnotationSlots)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            recordAttr({
                {"x", "I",
                 {annoAttr("RuntimeVisibleAnnotations", {"LFoo/Keep;"}),
                  typeAnnoAttr("RuntimeVisibleTypeAnnotations", 0x13, {"LFoo/Keep;"}),
                  signatureAttr("I")}},
                {"y", "Ljava/lang/String;",
                 {signatureAttr("Ljava/lang/String;")}},
            }),
        },
        {},
        {
            {"m", "()V", kReturn, 0x0009, {}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::MetaInfoRemover     pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs).count("Record"), 1u) << "Record 槽本身不动";
    const RecordAttr* rec = decodedRecordAttr(cf);
    ASSERT_NE(rec, nullptr);
    ASSERT_EQ(rec->components.size(), 2u);
    EXPECT_EQ(cf.utf8(rec->components[0].nameCp), "x");
    // 组件 x：注解与 TypeAnnotation 两槽清空，Signature 幸存。
    ASSERT_EQ(rec->components[0].attrs.size(), 1u);
    EXPECT_EQ(cf.utf8(rec->components[0].attrs[0].nameCp), "Signature");
    // 组件 y 无注解面：原样。
    ASSERT_EQ(rec->components[1].attrs.size(), 1u);
    EXPECT_EQ(cf.utf8(rec->components[1].attrs[0].nameCp), "Signature");
    // 父 Record 槽置脏后的重编码写回面可落盘。
    auto written = jbc::core::writeClass(cf);
    ASSERT_TRUE(written.is_ok());
}

TEST(MjPassMetaInfoRemoverTest, RecordWithoutComponentAnnotationsUntouched)
{
    // 组件只有 Signature（清空面零命中）→ 零改动，copy-through 字节恒等
    //（decode 不置脏，不触发 Record 重编码）。
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            recordAttr({
                {"x", "I", {signatureAttr("I")}},
                {"y", "Ljava/lang/String;", {}},
            }),
        },
        {},
        {
            {"m", "()V", kReturn, 0x0009, {}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
    auto beforeResult = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(beforeResult.is_ok());
    const Bytes before = std::move(beforeResult).unwrap();

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::MetaInfoRemover     pass;
    EXPECT_FALSE(pass.run(*session, cam).changed()) << "组件无注解面——条件 PA 应 all()";

    auto after = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(std::move(after).unwrap(), before);
}

TEST(MjPassMetaInfoRemoverTest, RecordComponentAnnotationsOutsideFaceUntouched)
{
    // opt-in 面外：清空面只归 MetaInfoRemover——KotlinMetadataRemover 对
    // record 组件嵌套注解零触碰。
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            recordAttr({
                {"x", "I", {annoAttr("RuntimeVisibleAnnotations", {"LFoo/Keep;"})}},
            }),
        },
        {},
        {
            {"m", "()V", kReturn, 0x0009, {}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
    auto beforeResult = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(beforeResult.is_ok());
    const Bytes before = std::move(beforeResult).unwrap();

    ca::str::Utf8StringArena            arena;
    mjbase::spi::ClassAnalysisManager   cam(arena);
    mjbase::pass::KotlinMetadataRemover pass;
    EXPECT_FALSE(pass.run(*session, cam).changed());

    auto after = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(std::move(after).unwrap(), before);
}

// ── FinalRemover ──

TEST(MjPassFinalRemoverTest, ClearsFieldFinalTwoSided)
{
    const Bytes in = familyFixture(0x0021, 0, {},
                                   {
                                       {"fin", "I", 0x0019, {}},    // public static final
                                       {"plain", "I", 0x0009, {}},
                                   },
                                   {
                                       {"m", "()V", kReturn, 0x0009, {}, {}},
                                   });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::FinalRemover        pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());

    ClassFile&            cf   = session->backing();
    jbc::ir::ClassUnit&   unit = session->unit();
    EXPECT_EQ(cf.fields[0].access, 0x0009);
    EXPECT_EQ(unit.fields[0].access & 0x0010u, 0u) << "unit 侧 FINAL 须同步清除";
    EXPECT_EQ(cf.fields[1].access, 0x0009);
    EXPECT_EQ(unit.fields[1].access & 0x0010u, 0u);
    EXPECT_EQ(cf.methods[0].access, 0x0009) << "方法不受面";

    EXPECT_TRUE(pass.run(*session, cam).areAllPreserved()) << "二跑幂等";
}

// ── SyntheticBridgeRemover ──

TEST(MjPassSyntheticBridgeRemoverTest, ClearsSyntheticAndBridgeBits)
{
    const Bytes in = familyFixture(
        0x1021, 0, {},   // public super synthetic
        {
            {"syn", "I", 0x1019, {}},   // public static final synthetic
            {"plain", "I", 0x0009, {}},
        },
        {
            {"br", "()V", kReturn, 0x1041, {}, {}},   // public bridge synthetic
            {"plain", "()V", kReturn, 0x0009, {}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::SyntheticBridgeRemover pass;
    const auto                           pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());

    ClassFile&          cf   = session->backing();
    jbc::ir::ClassUnit& unit = session->unit();
    EXPECT_EQ(cf.access, 0x0021);
    EXPECT_EQ(unit.access & 0x1000u, 0u);
    EXPECT_EQ(cf.methods[0].access, 0x0001);
    EXPECT_EQ((cf.methods[0].access & (0x1000 | 0x0040)), 0);
    EXPECT_EQ(cf.methods[1].access, 0x0009);
    EXPECT_EQ(cf.fields[0].access, 0x0019);   // 0x1019 去 synthetic
    EXPECT_EQ(cf.fields[1].access, 0x0009);

    EXPECT_TRUE(pass.run(*session, cam).areAllPreserved()) << "二跑幂等";
}

// ── KotlinMetadataRemover ──

TEST(MjPassKotlinMetadataRemoverTest, ClassVisibleOnlyAndJetbrainsAtMembers)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            annoAttr("RuntimeVisibleAnnotations", {"Lkotlin/Metadata;", "Lcom/app/Keep;"}),
            annoAttr("RuntimeInvisibleAnnotations", {"Lkotlin/Metadata;"}),
        },
        {
            {"f", "I", 0x0009,
             {annoAttr("RuntimeVisibleAnnotations",
                       {"Lorg/jetbrains/annotations/JvmName;"})}},
        },
        {
            {"m1", "()V", kReturn, 0x0009,
             {annoAttr("RuntimeVisibleAnnotations",
                       {"Lorg/jetbrains/annotations/NotNull;"})},
             {}},
            {"m2", "()V", kReturn, 0x0009,
             {annoAttr("RuntimeInvisibleAnnotations",
                       {"Lorg/jetbrains/annotations/Nullable;"})},
             {}},
            {"m3", "()V", kReturn, 0x0009,
             {annoAttr("RuntimeVisibleAnnotations", {"Lkotlin/Metadata;"})},
             {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::KotlinMetadataRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    // 类级 visible 槽：kotlin 条目删除，幸存者保持。
    const std::vector<std::string> classVisible =
        firstAnnoDescs(cf, cf.attrs, "RuntimeVisibleAnnotations");
    ASSERT_EQ(classVisible.size(), 1u);
    EXPECT_EQ(classVisible[0], "Lcom/app/Keep;");
    // 类级 invisible 不在面内：kotlin 条目原样。
    const std::vector<std::string> classInvisible =
        firstAnnoDescs(cf, cf.attrs, "RuntimeInvisibleAnnotations");
    ASSERT_EQ(classInvisible.size(), 1u);
    EXPECT_EQ(classInvisible[0], "Lkotlin/Metadata;");
    // m1/m2：jetbrains 两槽全清；m3：方法级 kotlin 不在面内。
    MemberInfo* m1 = findMember(cf, cf.methods, "m1");
    MemberInfo* m2 = findMember(cf, cf.methods, "m2");
    MemberInfo* m3 = findMember(cf, cf.methods, "m3");
    ASSERT_NE(m1, nullptr);
    ASSERT_NE(m2, nullptr);
    ASSERT_NE(m3, nullptr);
    EXPECT_TRUE(attrNames(cf, m1->attrs).count("RuntimeVisibleAnnotations") == 0);
    EXPECT_TRUE(attrNames(cf, m2->attrs).count("RuntimeInvisibleAnnotations") == 0);
    const std::vector<std::string> m3Visible =
        firstAnnoDescs(cf, m3->attrs, "RuntimeVisibleAnnotations");
    ASSERT_EQ(m3Visible.size(), 1u);
    EXPECT_EQ(m3Visible[0], "Lkotlin/Metadata;");
    // 字段 jetbrains 清除。
    MemberInfo* f = findMember(cf, cf.fields, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(attrNames(cf, f->attrs).count("RuntimeVisibleAnnotations") == 0);
}

// ── SignatureRemover / SourceInfoRemover ──

TEST(MjPassSignatureRemoverTest, StripsAllThreeLevels)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            signatureAttr("Ljava/lang/List<Ljava/lang/String;>;"),
            sourceFileAttr("T.java"),
        },
        {
            {"f", "I", 0x0009, {signatureAttr("I")}},
        },
        {
            {"m", "(I)V", kReturn, 0x0009, {signatureAttr("(I)V)")}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::SignatureRemover    pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"SourceFile"}));
    EXPECT_TRUE(attrNames(cf, cf.fields[0].attrs).empty());
    EXPECT_EQ(attrNames(cf, cf.methods[0].attrs), (std::set<std::string>{"Code"}));
}

TEST(MjPassSourceInfoRemoverTest, StripsAndConditionalPa)
{
    const Bytes in = familyFixture(0x0021, 0,
                                   {
                                       sourceFileAttr("T.java"),
                                       rawAttr("SourceDebugExtension", Bytes{'s', 'm', 'a', 'p'}),
                                   },
                                   {},
                                   {
                                       {"m", "()V", kReturn, 0x0009, {}, {}},
                                   });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::SourceInfoRemover   pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());
    EXPECT_TRUE(attrNames(session->backing(), session->backing().attrs).empty());

    // 干净类：零改动 → all()。
    const Bytes cleanIn =
        familyFixture(0x0021, 0, {}, {}, {{"m", "()V", kReturn, 0x0009, {}, {}}});
    auto cleanSession = makeSession(cleanIn);
    ASSERT_NE(cleanSession, nullptr);
    EXPECT_TRUE(pass.run(*cleanSession, cam).areAllPreserved());
}

// ── IllegalAnnotationRemover ──

TEST(MjPassIllegalAnnotationRemoverTest, FiltersMalformedEntriesKeepsValid)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            // 混槽：合法幸存、非法（码点 ≤ 3）删除。
            annoAttr("RuntimeVisibleAnnotations", {"Lgood/Anno;", "-"}),
        },
        {
            // 纯非法槽：整槽消失（不以 L 开头）。
            {"f", "I", 0x0009, {annoAttr("RuntimeVisibleAnnotations", {"Xbad/Anno;"})}},
        },
        {
            // 方法 invisible 槽同面（不以 ; 结尾）。
            {"m", "()V", kReturn, 0x0009,
             {annoAttr("RuntimeInvisibleAnnotations", {"Lbad/Anno"})},
             {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena             arena;
    mjbase::spi::ClassAnalysisManager    cam(arena);
    mjbase::pass::IllegalAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    const std::vector<std::string> classVisible =
        firstAnnoDescs(cf, cf.attrs, "RuntimeVisibleAnnotations");
    ASSERT_EQ(classVisible.size(), 1u);
    EXPECT_EQ(classVisible[0], "Lgood/Anno;");
    MemberInfo* f = findMember(cf, cf.fields, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(attrNames(cf, f->attrs).count("RuntimeVisibleAnnotations") == 0);
    MemberInfo* m = findMember(cf, cf.methods, "m");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(attrNames(cf, m->attrs).count("RuntimeInvisibleAnnotations") == 0);
}

// ── IllegalSignatureRemover ──

TEST(MjPassIllegalSignatureRemoverTest, RemovesParserRejectedKeepsValid)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            signatureAttr("<T:Ljava/lang/Object;>Ljava/lang/Object;"),
        },
        {
            {"bad", "I", 0x0009, {signatureAttr("-")}},
            {"good", "Ljava/util/List;", 0x0009,
             {signatureAttr("Ljava/util/List<Ljava/lang/String;>;")}},
        },
        {
            {"bad", "(I)V", kReturn, 0x0009, {signatureAttr("-")}, {}},
            {"good", "()V", kReturn, 0x0009, {signatureAttr("()V")}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena             arena;
    mjbase::spi::ClassAnalysisManager    cam(arena);
    mjbase::pass::IllegalSignatureRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"Signature"}))
        << "合法类签名保留";
    MemberInfo* bad  = findMember(cf, cf.fields, "bad");
    MemberInfo* good = findMember(cf, cf.fields, "good");
    ASSERT_NE(bad, nullptr);
    ASSERT_NE(good, nullptr);
    EXPECT_TRUE(attrNames(cf, bad->attrs).empty());
    EXPECT_EQ(attrNames(cf, good->attrs), (std::set<std::string>{"Signature"}));
    MemberInfo* mBad  = findMember(cf, cf.methods, "bad");
    MemberInfo* mGood = findMember(cf, cf.methods, "good");
    ASSERT_NE(mBad, nullptr);
    ASSERT_NE(mGood, nullptr);
    EXPECT_EQ(attrNames(cf, mBad->attrs), (std::set<std::string>{"Code"}));
    EXPECT_EQ(attrNames(cf, mGood->attrs), (std::set<std::string>{"Code", "Signature"}));
}

// ── IllegalTypeAnnotationRemover ──

TEST(MjPassIllegalTypeAnnotationRemoverTest, CoversMemberAndCodeNestedSlots)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            // 混槽：合法幸存、非法（描述符 "-"）删除；target 0x13 = 字段类型。
            typeAnnoAttr("RuntimeVisibleTypeAnnotations", 0x13, {"Lgood/Anno;", "-"}),
        },
        {},
        {
            {"m", "()V", kNopReturn, 0x0009, {},
             {typeAnnoAttr("RuntimeVisibleTypeAnnotations", 0x13, {"-"})}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena                  arena;
    mjbase::spi::ClassAnalysisManager         cam(arena);
    mjbase::pass::IllegalTypeAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    const std::vector<std::string> classTa = [&] {
        for (AttrSlot& slot : cf.attrs) {
            if (cf.utf8(slot.nameCp) == "RuntimeVisibleTypeAnnotations") {
                return typeAnnoDescs(cf, slot);
            }
        }
        return std::vector<std::string>{};
    }();
    ASSERT_EQ(classTa.size(), 1u);
    EXPECT_EQ(classTa[0], "Lgood/Anno;");

    MemberInfo* m = findMember(cf, cf.methods, "m");
    ASSERT_NE(m, nullptr);
    CodeAttr* code = decodeCodeAttr(cf, *m);
    ASSERT_NE(code, nullptr);
    bool nestedTaSurvived = false;
    for (AttrSlot& slot : code->attrs) {
        if (cf.utf8(slot.nameCp) == "RuntimeVisibleTypeAnnotations") nestedTaSurvived = true;
    }
    EXPECT_FALSE(nestedTaSurvived) << "Code 嵌套槽内删空后连槽摘除";
}

// ── IllegalSourceInfoRemover ──

TEST(MjPassIllegalSourceInfoRemoverTest, LegalKeptIllegalStripped)
{
    const Bytes in = familyFixture(0x0021, 0,
                                   {
                                       sourceFileAttr("1-evil.txt"),
                                       rawAttr("SourceDebugExtension",
                                               Bytes{'G', 'o', 'o', 'd', '.', 'j', 'a', 'v', 'a'}),
                                   },
                                   {},
                                   {
                                       {"m", "()V", kReturn, 0x0009, {}, {}},
                                   });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena               arena;
    mjbase::spi::ClassAnalysisManager      cam(arena);
    mjbase::pass::IllegalSourceInfoRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"SourceDebugExtension"}))
        << "合法 SourceDebug 保留，非法 SourceFile 摘除";

    // 双合法：零改动 → all()。
    const Bytes legalIn = familyFixture(0x0021, 0,
                                        {
                                            sourceFileAttr("Legal1.java"),
                                        },
                                        {},
                                        {
                                            {"m", "()V", kReturn, 0x0009, {}, {}},
                                        });
    auto legalSession = makeSession(legalIn);
    ASSERT_NE(legalSession, nullptr);
    EXPECT_TRUE(pass.run(*legalSession, cam).areAllPreserved());
}

// ── UnknownAttributeCleaner ──

TEST(MjPassUnknownAttributeCleanerTest, RemovesUnknownKeepsStandardAndCode)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            sourceFileAttr("T.java"),
            rawAttr("ObfMark", Bytes{0xAA, 0xBB}),
        },
        {
            {"cv", "I", 0x0009, {constValueAttr(5), rawAttr("ObfMark", Bytes{0x01})}},
        },
        {
            {"m", "()V", kNopReturn, 0x0009,
             {rawAttr("ObfMark", Bytes{0x02})},
             {rawAttr("UnknownNested", Bytes{0x03})}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena            arena;
    mjbase::spi::ClassAnalysisManager   cam(arena);
    mjbase::pass::UnknownAttributeCleaner pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"SourceFile"}));
    EXPECT_EQ(attrNames(cf, cf.fields[0].attrs), (std::set<std::string>{"ConstantValue"}));
    EXPECT_EQ(attrNames(cf, cf.methods[0].attrs), (std::set<std::string>{"Code"}));
    // Code 嵌套不在面内：未知嵌套槽原样保留。
    CodeAttr* code = decodeCodeAttr(cf, cf.methods[0]);
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(attrNames(cf, code->attrs), (std::set<std::string>{"UnknownNested"}));

    EXPECT_TRUE(pass.run(*session, cam).areAllPreserved()) << "二跑幂等";
}

// ── IllegalAccessRemover ──

TEST(MjPassIllegalAccessRemoverTest, MasksPerKindTwoSidedAndParamPayload)
{
    const Bytes in = familyFixture(
        0x0029, 0x0040,   // public static super + 杂位 0x40（词表无修饰词，附着面 OR）
        {},
        {
            {"fe", "I", 0x4002, {}},    // private enum（类非 enum → 清）
            {"fn", "I", 0x0108, {}},    // static native → 清 native
            {"ft", "I", 0x0088, {}},    // static transient（合法，保留）
        },
        {
            {"mi", "()V", kReturn, 0x0201, {}, {}},   // public interface
            {"ms", "()V", kReturn, 0x0021, {}, {}},   // public synchronized(合法别名)
            {"mp", "()V", kReturn, 0x0009,
             {methodParametersAttr(
                 {{"p0", 0x0010}, {"p1", 0x0001}, {"p2", 0x8000}})},
             {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::IllegalAccessRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile&          cf   = session->backing();
    jbc::ir::ClassUnit& unit = session->unit();
    EXPECT_EQ(cf.access, 0x0021);
    EXPECT_EQ(unit.access & 0x0048u, 0u);

    MemberInfo* mi = findMember(cf, cf.methods, "mi");
    MemberInfo* ms = findMember(cf, cf.methods, "ms");
    ASSERT_NE(mi, nullptr);
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(mi->access, 0x0001);
    EXPECT_EQ(ms->access, 0x0021) << "方法 synchronized 是合法位，保留";

    MemberInfo* fe = findMember(cf, cf.fields, "fe");
    MemberInfo* fn = findMember(cf, cf.fields, "fn");
    MemberInfo* ft = findMember(cf, cf.fields, "ft");
    ASSERT_NE(fe, nullptr);
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(ft, nullptr);
    EXPECT_EQ(fe->access, 0x0002);
    EXPECT_EQ(fn->access, 0x0008);
    EXPECT_EQ(ft->access, 0x0088);

    // MethodParameters 载荷：PUBLIC 清、FINAL/MANDATED 幸存。
    MemberInfo* mp = findMember(cf, cf.methods, "mp");
    ASSERT_NE(mp, nullptr);
    AttrSlot* paramsSlot = nullptr;
    for (AttrSlot& slot : mp->attrs) {
        if (cf.utf8(slot.nameCp) == "MethodParameters") paramsSlot = &slot;
    }
    ASSERT_NE(paramsSlot, nullptr);
    const std::optional<std::vector<ca::u8>> payload = slotPayloadBytes(cf, mp->attrs, "MethodParameters");
    ASSERT_TRUE(payload.has_value());
    const std::vector<ca::u8>& bytes = *payload;
    ASSERT_EQ(bytes.size(), 1u + 3u * 4u);
    EXPECT_EQ((static_cast<ca::u16>(bytes[3]) << 8) | bytes[4], 0x0010)
        << "FINAL 位幸存";
    EXPECT_EQ((static_cast<ca::u16>(bytes[7]) << 8) | bytes[8], 0x0000)
        << "PUBLIC 位清除";
    EXPECT_EQ((static_cast<ca::u16>(bytes[11]) << 8) | bytes[12], 0x8000)
        << "MANDATED 位幸存";

    EXPECT_TRUE(pass.run(*session, cam).areAllPreserved()) << "二跑幂等";
}

TEST(MjPassIllegalAccessRemoverTest, LegacyU1CountParameterLayoutMasked)
{
    // 旧栈 writer 字节形（u1 count + {u2 name, u2 acc}*，与 JVMS 4.7.24 同
    // 形；名字 CP 指针以池内真实 Utf8 承载，不透明字面语义不变）：0.4.18
    // typed 面解码命中，行模型清位后重编码须逐字节恒同。
    const Bytes in = familyFixture(
        0x0021, 0, {},
        {},
        {
            {"mp", "()V", kReturn, 0x0009, {legacyParamLayoutAttr()}, {}},
            {"m", "()V", kReturn, 0x0009, {}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::IllegalAccessRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    MemberInfo* mp = findMember(cf, cf.methods, "mp");
    ASSERT_NE(mp, nullptr);
    AttrSlot* paramsSlot = nullptr;
    for (AttrSlot& slot : mp->attrs) {
        if (cf.utf8(slot.nameCp) == "MethodParameters") paramsSlot = &slot;
    }
    ASSERT_NE(paramsSlot, nullptr);
    const std::optional<std::vector<ca::u8>> payload = slotPayloadBytes(cf, mp->attrs, "MethodParameters");
    ASSERT_TRUE(payload.has_value());
    const std::vector<ca::u8>& bytes = *payload;
    ASSERT_EQ(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 0x01) << "u1 count 原样";
    EXPECT_EQ((static_cast<ca::u16>(bytes[3]) << 8) | bytes[4], 0x8010)
        << "PUBLIC 位清除，MANDATED 位幸存";
}

TEST(MjPassIllegalAccessRemoverTest, EnumClassKeepsFieldEnumBits)
{
    const Bytes in = familyFixture(
        0x4031, 0, {},   // public final super enum（词表全覆盖）
        {
            {"E1", "Lfoo/Color;", 0x4019, {}},   // public static final enum
        },
        {
            {"m", "()V", kReturn, 0x0009, {}, {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::IllegalAccessRemover pass;
    const auto                         pa = pass.run(*session, cam);

    EXPECT_FALSE(pa.changed()) << "enum 类的 enum 字段位全合法——零改动";
    EXPECT_EQ(session->backing().fields[0].access, 0x4019);
}

// ── SpecificAnnotationRemover ──

TEST(MjPassSpecificAnnotationRemoverTest, BlacklistAndMalformedFiltered)
{
    const Bytes in = familyFixture(
        0x0021, 0,
        {
            annoAttr("RuntimeVisibleAnnotations",
                     {"Ltech/origincloud/Mapping;", "Lcom/app/Keep;"}),
        },
        {
            {"f", "I", 0x0009,
             {annoAttr("RuntimeInvisibleAnnotations",
                       {"Ljvavx/annotation/NotNull;"})}},
        },
        {
            {"m", "()V", kReturn, 0x0009,
             {annoAttr("RuntimeInvisibleAnnotations",
                       {"Lorg/jetbrains/annotations/Nullable;", "-"})},
             {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena                arena;
    mjbase::spi::ClassAnalysisManager       cam(arena);
    mjbase::pass::SpecificAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    const std::vector<std::string> classVisible =
        firstAnnoDescs(cf, cf.attrs, "RuntimeVisibleAnnotations");
    ASSERT_EQ(classVisible.size(), 1u);
    EXPECT_EQ(classVisible[0], "Lcom/app/Keep;");
    MemberInfo* m = findMember(cf, cf.methods, "m");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(attrNames(cf, m->attrs).count("RuntimeInvisibleAnnotations") == 0);
    MemberInfo* f = findMember(cf, cf.fields, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(attrNames(cf, f->attrs).count("RuntimeInvisibleAnnotations") == 0);
}

// 参数注解两槽槽内过滤（Raw 面 JVMS 4.7.20 布局二进制级条目摘除）。

TEST(MjPassSpecificAnnotationRemoverTest, ParameterAnnotationEntriesFilteredSlotKept)
{
    const Bytes in = familyFixture(
        0x0021, 0, {}, {},
        {
            {"m", "(I)V", kReturn, 0x0009,
             {paramAnnoFlatRowsAttr("RuntimeVisibleParameterAnnotations",
                                    {{"Ltech/origincloud/Mapping;", "Lcom/app/Keep;"},
                                     {"Lcom/app/Keep;"}})},
             {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena                arena;
    mjbase::spi::ClassAnalysisManager       cam(arena);
    mjbase::pass::SpecificAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    // 部分命中：槽存活，行数保持（num_parameters 与方法描述符参数数对应）。
    const std::optional<std::vector<ca::u8>> raw = slotPayloadBytes(
        cf, cf.methods[0].attrs, "RuntimeVisibleParameterAnnotations");
    ASSERT_TRUE(raw.has_value());
    // 重建载荷逐字节断言：u1 参数数=2；行 0 摘黑名单留幸存；行 1 原样。
    const ca::u16 keep = utf8IndexOf(cf, "Lcom/app/Keep;");
    const Bytes   expected = {
        0x02,
        0x00, 0x01,
        static_cast<ca::u8>(keep >> 8), static_cast<ca::u8>(keep), 0x00, 0x00,
        0x00, 0x01,
        static_cast<ca::u8>(keep >> 8), static_cast<ca::u8>(keep), 0x00, 0x00,
    };
    EXPECT_EQ(*raw, expected);
}

TEST(MjPassSpecificAnnotationRemoverTest, ParameterAnnotationNestedEntryRemovedWhole)
{
    // 黑名单条目带 '[' 数组 + '@ 嵌套注解载荷——条目随顶层描述符整体摘除，
    // 嵌套形状测量不误切字节。
    const Bytes in = familyFixture(
        0x0021, 0, {}, {},
        {
            {"m", "(I)V", kReturn, 0x0009,
             {paramAnnoRowsAttr("RuntimeInvisibleParameterAnnotations",
                                {{nestedAnnoEntry("Ltech/origincloud/Mapping;", "Lfoo/Nested;"),
                                  simpleAnnoEntry("Lcom/app/Keep;")}})},
             {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena                arena;
    mjbase::spi::ClassAnalysisManager       cam(arena);
    mjbase::pass::SpecificAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    const std::optional<std::vector<ca::u8>> raw = slotPayloadBytes(
        cf, cf.methods[0].attrs, "RuntimeInvisibleParameterAnnotations");
    ASSERT_TRUE(raw.has_value());
    const ca::u16 keep     = utf8IndexOf(cf, "Lcom/app/Keep;");
    const Bytes   expected = {
        0x01, 0x00, 0x01,
        static_cast<ca::u8>(keep >> 8), static_cast<ca::u8>(keep), 0x00, 0x00,
    };
    EXPECT_EQ(*raw, expected);
}

TEST(MjPassSpecificAnnotationRemoverTest, ParameterAnnotationSlotDroppedWhenAllEmptied)
{
    const Bytes in = familyFixture(
        0x0021, 0, {}, {},
        {
            // 全条目命中（黑名单 + 空行）→ 槽摘除。
            {"m", "(I)V", kReturn, 0x0009,
             {paramAnnoFlatRowsAttr("RuntimeVisibleParameterAnnotations",
                                    {{"Ljvavx/annotation/NotNull;"}, {}})},
             {}},
            // 幸存槽照常保留。
            {"keep", "(I)V", kReturn, 0x0009,
             {paramAnnoFlatRowsAttr("RuntimeVisibleParameterAnnotations",
                                    {{"Lcom/app/Keep;"}})},
             {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    ca::str::Utf8StringArena                arena;
    mjbase::spi::ClassAnalysisManager       cam(arena);
    mjbase::pass::SpecificAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    MemberInfo* m = findMember(cf, cf.methods, "m");
    MemberInfo* keepM = findMember(cf, cf.methods, "keep");
    ASSERT_NE(m, nullptr);
    ASSERT_NE(keepM, nullptr);
    EXPECT_EQ(slotPayloadBytes(cf, m->attrs, "RuntimeVisibleParameterAnnotations"), std::nullopt)
        << "全空摘槽";
    EXPECT_NE(slotPayloadBytes(cf, keepM->attrs, "RuntimeVisibleParameterAnnotations"), std::nullopt)
        << "非目标注解不动";
}

TEST(MjPassSpecificAnnotationRemoverTest, NonTargetAndMalformedParameterAnnotationsUntouched)
{
    const Bytes in = familyFixture(
        0x0021, 0, {}, {},
        {
            // 非目标注解（谓词零命中）。
            {"m", "(I)V", kReturn, 0x0009,
             {paramAnnoFlatRowsAttr("RuntimeVisibleParameterAnnotations",
                                    {{"Lcom/app/Keep;"}})},
             {}},
            // 畸形载荷（条目截尾：typeCp 后缺键值对表）→ fail-closed，
            // 全类零改动字节恒等。
            {"bad", "(I)V", kReturn, 0x0009,
             {rawAttr("RuntimeInvisibleParameterAnnotations",
                      Bytes{0x01, 0x00, 0x01, 0x12, 0x34})},
             {}},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
    auto beforeResult = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(beforeResult.is_ok());
    const Bytes before = std::move(beforeResult).unwrap();

    ca::str::Utf8StringArena                arena;
    mjbase::spi::ClassAnalysisManager       cam(arena);
    mjbase::pass::SpecificAnnotationRemover pass;
    EXPECT_FALSE(pass.run(*session, cam).changed());

    auto after = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(std::move(after).unwrap(), before);
}

// ── 注册面：13 件全可按名构造且 scope=Class ──

TEST(MjPassMetaFamilyRegistryTest, CreatesAllThirteenByName)
{
    const char* names[] = {
        "MetaInfoRemover",           "FinalRemover",
        "SyntheticBridgeRemover",    "KotlinMetadataRemover",
        "SourceInfoRemover",         "SignatureRemover",
        "IllegalAnnotationRemover",  "IllegalSignatureRemover",
        "IllegalTypeAnnotationRemover", "IllegalSourceInfoRemover",
        "UnknownAttributeCleaner",   "IllegalAccessRemover",
        "SpecificAnnotationRemover",
    };
    mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    for (const char* name : names) {
        const auto entry = reg.find(name);
        ASSERT_TRUE(entry.is_ok()) << name;
        EXPECT_EQ(entry.unwrap().scope, mjbase::spi::PassScope::Class) << name;
        auto pass = reg.createClassPass(name);
        ASSERT_TRUE(pass.is_ok()) << name;
        EXPECT_NE(std::move(pass).unwrap(), nullptr) << name;
    }
}
