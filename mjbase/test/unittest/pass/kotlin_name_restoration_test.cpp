// KotlinNameRestorer 单测：Kotlin metadata 真名映射生成面（消费收集件模型 +
// 描述符唯一匹配产家族映射）。覆盖：var 属性三槽恢复（field/getter/setter）、
// 扩展接收者首参函数、容器真名段 reverseMap + stdlib 替换双形态描述符、is 前
// 缀访问器命名、setter mutability 门（val 属性同形用户方法不误恢复）、合成名
// 消解（access$ 转发不抢唯一候选槽）、同槽冲突消解（属性访问器 > 函数）、
// JvmName 注解类名兜底（无 metadata 与注解优先两态、根包无前缀）、已正名零
// 条目、多命中放弃、无注解空映射、hasSetter 模型面、容器字节恒等、幂等二跑、
// 注册挂名。
//
// 夹具纪律：类骨架一律 jasm 文本装配（JvmJasmService）；kotlin/Metadata 与
// kotlin/jvm/JvmName 注解 jasm 方言无法表达（无注解 overlay），经 CF 级
// typed AnnotationsAttr 写面挂载（纪律例外②申报——模拟上游 kotlinc 工序）。
// d1 载荷编码器（BitEncoding + protobuf 线格式）为 kotlin_metadata_collection_
// test.cpp 同口径复制件（申报：未抽公共 helper，避免回改已落库收集件测试）；
// 编码器只产注解元素值数据面内容，不经任何 classfile 结构级字节拼装。
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/kotlin_metadata_collection.hpp"
#include "mjbase/pass/kotlin_name_restoration.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

using jbc::core::AnnotationsAttr;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;

using mjbase::mapping::IntermediateMapping;
using mjbase::pass::KotlinNameRestorationAnalysis;
using mjbase::pass::KotlinNameRestorer;
using mjbase::pass::KtClass;
using mjbase::pass::KotlinMetadataError;

/// 最小构造器（jasm 实例类形状：aload_0 转发 super 构造；std::string 以
/// 供夹具 body 拼接）。
const std::string kCtor = R"JASM(  .method public <init> ()V {
    code: {
      aload_0
      invokespecial java/lang/Object.<init> ()V
      return
    }
  }
)JASM";

/// UTF-8 → UTF-16（jasm 服务入参口径）。
ca::str::Utf16String utf16Of(const std::string& s)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(s.data()), s.size()));
}

/// jasm 文本汇编为 class 字节（汇编失败逐条报诊断并致命，夹具错误不静默）。
Bytes assembleClass(const std::string& internalName, const std::string& body)
{
    mjbase::jasm::JvmJasmService service;
    const std::string source = R"JASM(.super java/lang/Object
.class public super )JASM" + internalName + R"JASM( {
)JASM" + body + R"JASM(}
)JASM";
    const auto sourceName = utf16Of(internalName + ".jasm");
    const auto sourceText = utf16Of(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            std::string where;
            if (d.location.has_value()) {
                where = " [line " + std::to_string(d.location->line) + " col "
                        + std::to_string(d.location->column) + "]";
            }
            ADD_FAILURE() << "jasm" << where << ": "
                          << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败，源码如下：\n" << source;
        return {};
    }
    return result.value->bytes;
}

// ── d1 载荷数据面（kotlinc 同口径编码器；kotlin_metadata_collection_test
//    同口径复制件，只产注解元素值内容）──

/// varint 写（LEB128）。
void pbVarint(std::string& out, ca::u64 v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

/// 字段键（field 号 << 3 | 线型）。
void pbKey(std::string& out, ca::u32 field, ca::u8 wire)
{
    pbVarint(out, (static_cast<ca::u64>(field) << 3) | wire);
}

/// varint 字段（wire 0）。
std::string pbInt(ca::u32 field, ca::u64 v)
{
    std::string out;
    pbKey(out, field, 0);
    pbVarint(out, v);
    return out;
}

/// 长度定界字段（wire 2）。
std::string pbLen(ca::u32 field, const std::string& payload)
{
    std::string out;
    pbKey(out, field, 2);
    pbVarint(out, payload.size());
    out += payload;
    return out;
}

/// Type 消息（class_name 串表索引 + 显式 nullable 布尔）。
std::string pbType(ca::u32 nameIdx, bool nullable)
{
    return pbInt(6, nameIdx) + pbInt(3, nullable ? 1 : 0);
}

/// d1 帧：varint32 串表长度前缀 + StringTableTypes + 主消息。
std::string frameD1(const std::string& table, const std::string& main)
{
    std::string out;
    pbVarint(out, table.size());
    out += table;
    out += main;
    return out;
}

/// 平铺串表（单 record range=n 覆盖全部 d2 位）。
std::string plainTable(ca::u32 count) { return pbLen(1, pbInt(1, count)); }

/// 8 位 → 7 位组包（kotlin BitEncoding.encode8to7 同口径）。
std::vector<ca::u8> encode8to7(const std::vector<ca::u8>& data)
{
    const ca::usize     resultLength = (data.size() * 8 + 6) / 7;
    std::vector<ca::u8> result(resultLength, 0);
    ca::usize           byteIndex = 0;
    ca::usize           bit       = 0;
    for (ca::usize i = 0; i + 1 < resultLength; ++i) {
        if (bit == 0) {
            result[i] = static_cast<ca::u8>(data[byteIndex] & 0x7F);
            bit       = 7;
            continue;
        }
        const ca::u8 firstPart = static_cast<ca::u8>(data[byteIndex] >> bit);
        const ca::usize newBit = (bit + 7) & 7;
        ++byteIndex;
        const ca::u8 secondPart =
            static_cast<ca::u8>((data[byteIndex] & ((1 << newBit) - 1)) << (8 - bit));
        result[i] = static_cast<ca::u8>(firstPart + secondPart);
        bit       = newBit;
    }
    if (resultLength > 0) {
        result[resultLength - 1] = static_cast<ca::u8>(data[byteIndex] >> bit);
    }
    return result;
}

/// d1 串码元序列（UTF8 模式 U+0000 标记；7to8 模式 U+FFFF 标记 + 组包 +
/// 模 128 加 1）。
std::vector<ca::u16> encodeData1Units(const std::vector<ca::u8>& payload, bool use7to8)
{
    std::vector<ca::u16> units;
    if (use7to8) {
        std::vector<ca::u8> packed = encode8to7(payload);
        for (ca::u8& b : packed) b = static_cast<ca::u8>((b + 1) & 0x7F);
        units.push_back(0xFFFF);
        for (const ca::u8 b : packed) units.push_back(b);
        return units;
    }
    units.push_back(0x0000);
    for (const ca::u8 b : payload) units.push_back(b);
    return units;
}

/// 码元 → CP modified-UTF8 字节串（0x00 → C0 80；余 1/2/3 字节标准形）。
std::string modifiedUtf8Of(const std::vector<ca::u16>& units)
{
    std::string out;
    for (const ca::u16 u : units) {
        if (u == 0x0000) {
            out += "\xC0\x80";
        }
        else if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        }
        else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

// ── 注解写面（CF 级 typed AnnotationsAttr；jasm 无注解 overlay，例外②）──

/// @brief Metadata 注解构造规格（kotlin_metadata_collection_test 同款）。
struct MetadataSpec
{
    ca::u32                  k       = 1;
    std::vector<ca::i32>     mv      = {1, 9, 0};
    std::string              payload;   ///< d1 载荷（proto 线格式帧）
    std::vector<std::string> d2;
    bool                     use7to8 = false;
    std::optional<ca::i32>   xi;
};

/// 串数组元素值（'[' + 's'*）。
AnnotationsAttr::ElemValue stringArrayElem(ClassFile& cf, const std::vector<std::string>& texts)
{
    AnnotationsAttr::ElemValue arr;
    arr.tag = '[';
    for (const std::string& t : texts) {
        AnnotationsAttr::ElemValue item;
        item.tag = 's';
        item.cp  = cf.cp.addString(cf.cp.addUtf8(t));
        arr.items.push_back(std::move(item));
    }
    return arr;
}

/// int 元素值（'I'）。
AnnotationsAttr::ElemValue intElem(ClassFile& cf, ca::i32 v)
{
    AnnotationsAttr::ElemValue item;
    item.tag = 'I';
    item.cp  = cf.cp.addInteger(v);
    return item;
}

/// 按 spec 构造 Metadata 注解条目（挂载会话内取 CP 索引）。
AnnotationsAttr::Annotation metadataEntry(ClassFile& cf, const MetadataSpec& spec)
{
    AnnotationsAttr::Annotation entry;
    entry.typeCp = cf.cp.addUtf8("Lkotlin/Metadata;");
    entry.values.emplace_back(cf.cp.addUtf8("k"), intElem(cf, static_cast<ca::i32>(spec.k)));
    {
        AnnotationsAttr::ElemValue mv;
        mv.tag = '[';
        for (const ca::i32 v : spec.mv) mv.items.push_back(intElem(cf, v));
        entry.values.emplace_back(cf.cp.addUtf8("mv"), std::move(mv));
    }
    const std::vector<ca::u16> units =
        encodeData1Units(std::vector<ca::u8>(spec.payload.begin(), spec.payload.end()),
                         spec.use7to8);
    entry.values.emplace_back(cf.cp.addUtf8("d1"), stringArrayElem(cf, {modifiedUtf8Of(units)}));
    entry.values.emplace_back(cf.cp.addUtf8("d2"), stringArrayElem(cf, spec.d2));
    if (spec.xi.has_value()) {
        entry.values.emplace_back(cf.cp.addUtf8("xi"), intElem(cf, *spec.xi));
    }
    return entry;
}

/// 按 name 构造 kotlin/jvm/JvmName 注解条目（文件门面回退面）。
AnnotationsAttr::Annotation jvmNameEntry(ClassFile& cf, const std::string& name)
{
    AnnotationsAttr::ElemValue value;
    value.tag = 's';
    value.cp  = cf.cp.addString(cf.cp.addUtf8(name));
    AnnotationsAttr::Annotation entry;
    entry.typeCp = cf.cp.addUtf8("Lkotlin/jvm/JvmName;");
    entry.values.emplace_back(cf.cp.addUtf8("name"), std::move(value));
    return entry;
}

/// 挂类级 RuntimeVisibleAnnotations（一槽多条目）：Metadata 与 JvmName 可
/// 单独或共存（同一属性内两个注解条目——kotlinc 同槽工序）。
Bytes withClassAnnotations(const Bytes&                             assembled,
                           const std::optional<MetadataSpec>& meta,
                           const std::optional<std::string>&  jvmName)
{
    auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(assembled));
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return {};
    auto       session = std::move(made).unwrap();
    ClassFile& cf      = session->backing();

    auto anno = std::make_unique<AnnotationsAttr>();
    if (meta.has_value()) anno->annos.push_back(metadataEntry(cf, *meta));
    if (jvmName.has_value()) anno->annos.push_back(jvmNameEntry(cf, *jvmName));
    cf.attrs.push_back(
        AttrSlot::makeNew(cf.cp.addUtf8("RuntimeVisibleAnnotations"), std::move(anno)));
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok())
        << "writeClass failed: "
        << (written.is_err() ? jbc::core::to_string(written.unwrap_err()) : "ok");
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// 仅挂 Metadata 的便捷面。
Bytes withKotlinMetadata(const Bytes& assembled, const MetadataSpec& spec)
{
    return withClassAnnotations(assembled, spec, std::nullopt);
}

/// 仅挂 JvmName 的便捷面。
Bytes withJvmName(const Bytes& assembled, const std::string& name)
{
    return withClassAnnotations(assembled, std::nullopt, name);
}

/// 单类解析（hasSetter 模型面用）。
ca::core::Result<KtClass, KotlinMetadataError> parseOne(const Bytes&              bytes,
                                                        ca::str::Utf8StringArena& arena)
{
    auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return ca::core::Err(KotlinMetadataError::BadAnnotationShape);
    auto session = std::move(made).unwrap();
    return mjbase::pass::parseKotlinMetadata(session->backing(), arena);
}

// ── 夹具（kotlin 名与混淆名的对照矩阵）──

/// var 属性全槽类（app/a；真名 app.Svc）：混淆 field b/getter a/setter c +
/// 函数 m。setter_parameter 在场（var 门开）。
Bytes svcFixture()
{
    const std::string stt = plainTable(6);
    // d2: 0 app.Svc / 1 value / 2 java/lang/String / 3 compute /
    //     4 kotlin/Int / 5 x
    const std::string prop = pbInt(2, 1) + pbLen(3, pbType(2, false)) + pbLen(6, "");
    const std::string fn   = pbInt(2, 3) + pbLen(3, pbType(4, false))
                         + pbLen(6, pbInt(2, 5) + pbLen(3, pbType(4, false)));
    const std::string cls = pbInt(3, 0) + pbLen(10, prop) + pbLen(9, fn);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Svc", "value", "java/lang/String", "compute", "kotlin/Int", "x"};
    const std::string body = kCtor + R"JASM(  .field private b Ljava/lang/String;
  .method public a ()Ljava/lang/String; {
    code: {
      aconst_null
      areturn
    }
  }
  .method public c (Ljava/lang/String;)V {
    code: {
      return
    }
  }
  .method public m (I)I {
    code: {
      iconst_0
      ireturn
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/a", body), spec);
}

/// val 属性 + 同形诱饵类（app/v；真名 app.Vault）：无 setter_parameter
///（val 门关），用户方法 r(String)V 与假想 setter 同形——不得误恢复。
Bytes valDecoyFixture()
{
    const std::string stt = plainTable(3);
    // d2: 0 app.Vault / 1 token / 2 java/lang/String
    const std::string prop = pbInt(2, 1) + pbLen(3, pbType(2, false));
    const std::string cls = pbInt(3, 0) + pbLen(10, prop);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Vault", "token", "java/lang/String"};
    const std::string body = kCtor + R"JASM(  .field private t Ljava/lang/String;
  .method public g ()Ljava/lang/String; {
    code: {
      aconst_null
      areturn
    }
  }
  .method public r (Ljava/lang/String;)V {
    code: {
      return
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/v", body), spec);
}

/// 容器真名段反向重写对（app/c/app/d）：Board.link(Cell): Unit 的参数类型
/// 命中收集 Cell 的 kotlin 真名 → reverseMap 到混淆二进制名，Unit 经 stdlib
/// 换 V；混淆方法 q(Lapp/c;)V 唯一命中。
Bytes cellFixture()
{
    const std::string stt = plainTable(1);
    // d2: 0 app.Cell
    const std::string cls = pbInt(3, 0);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Cell"};
    return withKotlinMetadata(assembleClass("app/c", kCtor), spec);
}

Bytes boardFixture()
{
    const std::string stt = plainTable(4);
    // d2: 0 app.Board / 1 link / 2 app.Cell / 3 kotlin/Unit
    const std::string fn  = pbInt(2, 1) + pbLen(3, pbType(3, false))
                        + pbLen(6, pbLen(3, pbType(2, false)));
    const std::string cls = pbInt(3, 0) + pbLen(9, fn);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Board", "link", "app.Cell", "kotlin/Unit"};
    const std::string body = kCtor + R"JASM(  .method public q (Lapp/c;)V {
    code: {
      return
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/d", body), spec);
}

/// is 前缀属性类（app/i；真名 app.Probe）：isActive Boolean var——getter
/// 保 is 前缀（isActive），setter 去 is（setActive）。
Bytes isPrefixFixture()
{
    const std::string stt = plainTable(3);
    // d2: 0 app.Probe / 1 isActive / 2 kotlin/Boolean
    const std::string prop = pbInt(2, 1) + pbLen(3, pbType(2, false)) + pbLen(6, "");
    const std::string cls = pbInt(3, 0) + pbLen(10, prop);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Probe", "isActive", "kotlin/Boolean"};
    const std::string body = kCtor + R"JASM(  .field private f Z
  .method public h ()Z {
    code: {
      iconst_0
      ireturn
    }
  }
  .method public s (Z)V {
    code: {
      return
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/i", body), spec);
}

/// 合成名消解类（app/f；真名 app.Flag）：access$g 转发与混淆 getter g 同
/// 描述符 ()Z——含 $ 成员被剔除候选集，g 仍唯一命中（无门时双候选互斥）。
Bytes forwarderFixture()
{
    const std::string stt = plainTable(3);
    // d2: 0 app.Flag / 1 flag / 2 kotlin/Boolean
    const std::string prop = pbInt(2, 1) + pbLen(3, pbType(2, false));
    const std::string cls = pbInt(3, 0) + pbLen(10, prop);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Flag", "flag", "kotlin/Boolean"};
    const std::string body = kCtor + R"JASM(  .field private f Z
  .method public g ()Z {
    code: {
      iconst_0
      ireturn
    }
  }
  .method public static access$g ()Z {
    code: {
      iconst_0
      ireturn
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/f", body), spec);
}

/// 同槽冲突类（app/x；真名 app.Box）：属性 data 的 getter 与函数 read 同
/// kt 描述符 ()Lkotlin/Int; → JVM ()I，混淆方法 z 单候选——属性访问器
/// 首写者胜（getData），函数侧同名幂等跳过。
Bytes conflictFixture()
{
    const std::string stt = plainTable(4);
    // d2: 0 app.Box / 1 data / 2 kotlin/Int / 3 read
    const std::string prop = pbInt(2, 1) + pbLen(3, pbType(2, false));
    const std::string fn   = pbInt(2, 3) + pbLen(3, pbType(2, false));
    const std::string cls = pbInt(3, 0) + pbLen(10, prop) + pbLen(9, fn);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Box", "data", "kotlin/Int", "read"};
    const std::string body = kCtor + R"JASM(  .method public z ()I {
    code: {
      iconst_0
      ireturn
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/x", body), spec);
}

/// JvmName 门面回退类（app/u）：无 Metadata，仅 JvmName 注解 name=Util。
Bytes jvmNameOnlyFixture() { return withJvmName(assembleClass("app/u", kCtor), "Util"); }

/// 根包 JvmName 类（bare）：回退名无包前缀（Recaf 产 "/Name" 的边界修正）。
Bytes jvmNameRootFixture() { return withJvmName(assembleClass("bare", kCtor), "Bare"); }

/// 注解优先类（app/w）：Metadata（真名 app.Widget + 混淆属性面）与 JvmName
///（RealWidget）并存——类名槽注解胜，成员槽照常走 metadata。
Bytes jvmNameWinsFixture()
{
    const std::string stt = plainTable(3);
    // d2: 0 app.Widget / 1 label / 2 java/lang/String
    const std::string prop = pbInt(2, 1) + pbLen(3, pbType(2, false));
    const std::string cls = pbInt(3, 0) + pbLen(10, prop);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Widget", "label", "java/lang/String"};
    const std::string body = kCtor + R"JASM(  .field private b Ljava/lang/String;
  .method public a ()Ljava/lang/String; {
    code: {
      aconst_null
      areturn
    }
  }
)JASM";
    return withClassAnnotations(assembleClass("app/w", body), spec, std::string{"RealWidget"});
}

/// 已正名门面（app/n，k=2）：模型名 = 定义类二进制名（类名槽零条目），成员
/// 全部已在 kotlinc 规范名——整映射空。
Bytes canonicalFacadeFixture()
{
    const std::string stt = plainTable(4);
    // d2: 0 value / 1 java/lang/String / 2 run / 3 kotlin/Unit
    const std::string prop = pbInt(2, 0) + pbLen(3, pbType(1, false)) + pbLen(6, "");
    const std::string fn   = pbInt(2, 2) + pbLen(3, pbType(3, false));
    const std::string pkg  = pbLen(4, prop) + pbLen(3, fn);
    MetadataSpec spec;
    spec.k       = 2;   ///< 文件门面（Package 载荷进 k=2 解析器）
    spec.payload = frameD1(stt, pkg);
    spec.d2      = {"value", "java/lang/String", "run", "kotlin/Unit"};
    const std::string body = kCtor + R"JASM(  .field private value Ljava/lang/String;
  .method public getValue ()Ljava/lang/String; {
    code: {
      aconst_null
      areturn
    }
  }
  .method public setValue (Ljava/lang/String;)V {
    code: {
      return
    }
  }
  .method public run ()V {
    code: {
      return
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/n", body), spec);
}

/// 多命中类（app/m2；真名 app.Multi）：双 String 字段与双 ()String getter
/// 同候选——唯一匹配口径放弃（属性面零条目）。
Bytes multiCandidateFixture()
{
    const std::string stt = plainTable(3);
    // d2: 0 app.Multi / 1 value / 2 java/lang/String
    const std::string prop = pbInt(2, 1) + pbLen(3, pbType(2, false));
    const std::string cls = pbInt(3, 0) + pbLen(10, prop);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Multi", "value", "java/lang/String"};
    const std::string body = kCtor + R"JASM(  .field private p1 Ljava/lang/String;
  .field private p2 Ljava/lang/String;
  .method public g1 ()Ljava/lang/String; {
    code: {
      aconst_null
      areturn
    }
  }
  .method public g2 ()Ljava/lang/String; {
    code: {
      aconst_null
      areturn
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/m2", body), spec);
}

/// 扩展函数门面（app/e，k=2）：plusInt 接收者 kotlin/Int 作 JVM 首参
///（收集件描述符面）→ (IJ)Z，混淆方法 p 唯一命中。
Bytes extFacadeFixture()
{
    const std::string stt = plainTable(5);
    // d2: 0 plusInt / 1 kotlin/Int / 2 x / 3 kotlin/Long / 4 kotlin/Boolean
    const std::string fn  = pbInt(2, 0)                          ///< name=plusInt
                        + pbLen(5, pbInt(6, 1))                  ///< receiver kotlin/Int
                        + pbLen(3, pbType(4, false))             ///< return kotlin/Boolean
                        + pbLen(6, pbInt(2, 2) + pbLen(3, pbType(3, false)));
    const std::string pkg = pbLen(3, fn);
    MetadataSpec spec;
    spec.k       = 2;
    spec.payload = frameD1(stt, pkg);
    spec.d2      = {"plusInt", "kotlin/Int", "x", "kotlin/Long", "kotlin/Boolean"};
    const std::string body = kCtor + R"JASM(  .method public p (IJ)Z {
    code: {
      iconst_0
      ireturn
    }
  }
)JASM";
    return withKotlinMetadata(assembleClass("app/e", body), spec);
}

// ── 运行与断言便捷面 ──

/// 跑 pass 并返回容器 AM 上的映射（只读面：字节零改动由调用方断言）。
const IntermediateMapping& runPass(mjbase::model::ResourceContainer&   container,
                                   mjbase::spi::GlobalAnalysisManager& gam)
{
    KotlinNameRestorer                    pass;
    const mjbase::spi::PreservedAnalyses pa = pass.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved()) << "还原件只产映射，恒 all()";
    return gam.getResult<KotlinNameRestorationAnalysis>(container);
}

const mjbase::model::ClassResource* classResource(mjbase::model::ResourceContainer& container,
                                                  const std::string&                uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

std::string mappedClass(const IntermediateMapping& m, std::string_view name)
{
    const auto mapped = m.getMappedClassName(ca::str::Utf8StringRef::from_string_view(name));
    return std::string{static_cast<std::string_view>(mapped)};
}

std::string mappedField(const IntermediateMapping& m, std::string_view owner,
                        std::string_view name, std::string_view desc)
{
    const auto mapped =
        m.getMappedFieldName(ca::str::Utf8StringRef::from_string_view(owner),
                             ca::str::Utf8StringRef::from_string_view(name),
                             ca::str::Utf8StringRef::from_string_view(desc));
    return std::string{static_cast<std::string_view>(mapped)};
}

std::string mappedMethod(const IntermediateMapping& m, std::string_view owner,
                         std::string_view name, std::string_view desc)
{
    const auto mapped =
        m.getMappedMethodName(ca::str::Utf8StringRef::from_string_view(owner),
                              ca::str::Utf8StringRef::from_string_view(name),
                              ca::str::Utf8StringRef::from_string_view(desc));
    return std::string{static_cast<std::string_view>(mapped)};
}

/// 类内映射条目数（槽位去重断言用）。
ca::usize entriesOfClass(const IntermediateMapping& m, std::string_view owner, bool method)
{
    const auto ownerRef = ca::str::Utf8StringRef::from_string_view(owner);
    if (method) {
        const auto* list = m.getClassMethodMappings(ownerRef);
        return list == nullptr ? ca::usize{0} : list->size();
    }
    const auto* list = m.getClassFieldMappings(ownerRef);
    return list == nullptr ? ca::usize{0} : list->size();
}

}   // namespace

// ── var 属性三槽 + 函数全恢复 ──
TEST(MjPassKotlinNameRestorationTest, restoresVarPropertyMemberNames)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/a.class", svcFixture());
    const Bytes inBytes = classResource(container, "app/a.class")->get();

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mappedClass(mapping, "app/a"), "app$Svc") << "fqName 真名映射";
    EXPECT_EQ(mappedField(mapping, "app/a", "b", "Ljava/lang/String;"), "value")
        << "backing field 真名";
    EXPECT_EQ(mappedMethod(mapping, "app/a", "a", "()Ljava/lang/String;"), "getValue")
        << "getter 真名";
    EXPECT_EQ(mappedMethod(mapping, "app/a", "c", "(Ljava/lang/String;)V"), "setValue")
        << "setter 真名（var 门开）";
    EXPECT_EQ(mappedMethod(mapping, "app/a", "m", "(I)I"), "compute") << "函数真名";
    EXPECT_EQ(entriesOfClass(mapping, "app/a", false), ca::usize{1});
    EXPECT_EQ(entriesOfClass(mapping, "app/a", true), ca::usize{3})
        << "槽位去重：getter/setter/函数各一条";
    EXPECT_EQ(mapping.getClasses().size(), ca::usize{1});
    // 字节零改动（只读件）。
    EXPECT_EQ(classResource(container, "app/a.class")->get(), inBytes);
}

// ── 扩展接收者：收集件已展开为 JVM 首参 ──
TEST(MjPassKotlinNameRestorationTest, restoresExtensionFunctionWithReceiverFirstParam)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/e.class", extFacadeFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_TRUE(mappedClass(mapping, "app/e").empty()) << "门面模型名 = 二进制名，类名槽零条目";
    EXPECT_EQ(mappedMethod(mapping, "app/e", "p", "(IJ)Z"), "plusInt")
        << "接收者首参描述符 (IJ)Z 唯一命中";
    EXPECT_EQ(entriesOfClass(mapping, "app/e", true), ca::usize{1});
}

// ── 容器真名段 reverseMap + stdlib 替换双形态 ──
TEST(MjPassKotlinNameRestorationTest, reverseMapsCollectedNamesInDescriptors)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/c.class", cellFixture());
    addClassResource(container, "app/d.class", boardFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mappedClass(mapping, "app/c"), "app$Cell");
    EXPECT_EQ(mappedClass(mapping, "app/d"), "app$Board");
    EXPECT_EQ(mappedMethod(mapping, "app/d", "q", "(Lapp/c;)V"), "link")
        << "kotlin 真名段 Lapp$Cell; 反向重写为 Lapp/c; 后 stdlib Unit→V 命中";
    EXPECT_EQ(entriesOfClass(mapping, "app/d", true), ca::usize{1});
}

// ── is 前缀访问器命名 ──
TEST(MjPassKotlinNameRestorationTest, isPrefixPropertyAccessors)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/i.class", isPrefixFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mappedField(mapping, "app/i", "f", "Z"), "isActive");
    EXPECT_EQ(mappedMethod(mapping, "app/i", "h", "()Z"), "isActive")
        << "getter 保 is 前缀（kotlinc 约定）";
    EXPECT_EQ(mappedMethod(mapping, "app/i", "s", "(Z)V"), "setActive")
        << "setter 去 is 前缀（isActive → setActive）";
}

// ── setter mutability 门：val 属性同形用户方法不误恢复 ──
TEST(MjPassKotlinNameRestorationTest, setterGatedByMetadataMutability)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/v.class", valDecoyFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mappedClass(mapping, "app/v"), "app$Vault");
    EXPECT_EQ(mappedField(mapping, "app/v", "t", "Ljava/lang/String;"), "token");
    EXPECT_EQ(mappedMethod(mapping, "app/v", "g", "()Ljava/lang/String;"), "getToken");
    EXPECT_TRUE(mappedMethod(mapping, "app/v", "r", "(Ljava/lang/String;)V").empty())
        << "val 属性无 setter_parameter 门：同形用户方法 r 不恢复";
    EXPECT_EQ(entriesOfClass(mapping, "app/v", true), ca::usize{1});
}

// ── 合成名消解：access$ 转发不进候选集，也不收映射 ──
TEST(MjPassKotlinNameRestorationTest, skipsSyntheticNamedMembers)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/f.class", forwarderFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mappedField(mapping, "app/f", "f", "Z"), "flag");
    EXPECT_EQ(mappedMethod(mapping, "app/f", "g", "()Z"), "getFlag")
        << "access$ 剔除后 getter 唯一命中（无门时 ()Z 双候选互斥全弃）";
    EXPECT_TRUE(mappedMethod(mapping, "app/f", "access$g", "()Z").empty())
        << "合成形态成员不收映射";
    EXPECT_EQ(entriesOfClass(mapping, "app/f", true), ca::usize{1});
}

// ── 同槽冲突消解：属性访问器 > 函数 ──
TEST(MjPassKotlinNameRestorationTest, propertyAccessorWinsFunctionConflict)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/x.class", conflictFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mappedMethod(mapping, "app/x", "z", "()I"), "getData") << "属性 getter 先写者胜";
    EXPECT_EQ(entriesOfClass(mapping, "app/x", true), ca::usize{1})
        << "函数 read 同槽 (desc, old) 幂等跳过，不产第二条";
}

// ── JvmName 注解类名兜底 ──
TEST(MjPassKotlinNameRestorationTest, jvmNameFallbackWithoutMetadata)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/u.class", jvmNameOnlyFixture());
    addClassResource(container, "bare.class", jvmNameRootFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mappedClass(mapping, "app/u"), "app/Util") << "包前缀取当前二进制名包段";
    EXPECT_EQ(mappedClass(mapping, "bare"), "Bare") << "根包回退名无前导 '/'";
    EXPECT_FALSE(mapping.isEmpty());
    EXPECT_EQ(entriesOfClass(mapping, "app/u", false), ca::usize{0});
    EXPECT_EQ(entriesOfClass(mapping, "app/u", true), ca::usize{0}) << "无模型：成员槽零产出";
}

// ── 注解优先：JvmName 与 metadata 并存时类名槽注解胜 ──
TEST(MjPassKotlinNameRestorationTest, jvmNameAnnotationBeatsMetadataName)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/w.class", jvmNameWinsFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mapping.getClasses().size(), ca::usize{1}) << "类名槽单条目";
    EXPECT_EQ(mappedClass(mapping, "app/w"), "app/RealWidget")
        << "注解声明的名字优先于 metadata fqName";
    EXPECT_EQ(mappedField(mapping, "app/w", "b", "Ljava/lang/String;"), "label")
        << "成员槽照常走 metadata";
    EXPECT_EQ(mappedMethod(mapping, "app/w", "a", "()Ljava/lang/String;"), "getLabel");
}

// ── 已正名：整映射空（映射面最小化）──
TEST(MjPassKotlinNameRestorationTest, canonicalNamesProduceEmptyMapping)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/n.class", canonicalFacadeFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_TRUE(mapping.isEmpty()) << "类名槽（门面名 = 二进制名）与成员槽（已正名）全零条目";
}

// ── 多命中放弃（唯一匹配口径）──
TEST(MjPassKotlinNameRestorationTest, ambiguousDescriptorMatchProducesNothing)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "app/m2.class", multiCandidateFixture());

    const auto& mapping = runPass(container, gam);

    EXPECT_EQ(mappedClass(mapping, "app/m2"), "app$Multi");
    EXPECT_EQ(entriesOfClass(mapping, "app/m2", false), ca::usize{0}) << "双 String 字段放弃";
    EXPECT_EQ(entriesOfClass(mapping, "app/m2", true), ca::usize{0}) << "双 getter 放弃";
}

// ── 无注解类零产出 ──
TEST(MjPassKotlinNameRestorationTest, plainClassProducesEmptyMapping)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    addClassResource(container, "Plain.class", assembleClass("Plain", kCtor));

    const auto& mapping = runPass(container, gam);

    EXPECT_TRUE(mapping.isEmpty());
}

// ── hasSetter 模型面（收集件扩展位直证）──
TEST(MjPassKotlinNameRestorationTest, modelSurfaceExposesSetterPresence)
{
    ca::str::Utf8StringArena arena;
    {
        auto parsed = parseOne(svcFixture(), arena);
        ASSERT_TRUE(parsed.is_ok());
        KtClass model = std::move(parsed).unwrap();
        ASSERT_EQ(model.properties.size(), ca::usize{1});
        EXPECT_TRUE(model.properties[0].hasSetter) << "setter_parameter 在场 = var 属性";
    }
    {
        auto parsed = parseOne(valDecoyFixture(), arena);
        ASSERT_TRUE(parsed.is_ok());
        KtClass model = std::move(parsed).unwrap();
        ASSERT_EQ(model.properties.size(), ca::usize{1});
        EXPECT_FALSE(model.properties[0].hasSetter) << "setter_parameter 缺席 = val 属性";
    }
}

// ── 幂等二跑：同 GAM 同址缓存；独立 GAM 逐条相等 ──
TEST(MjPassKotlinNameRestorationTest, idempotentSecondRun)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "app/a.class", svcFixture());
    addClassResource(container, "app/c.class", cellFixture());
    addClassResource(container, "app/d.class", boardFixture());

    const auto ownerRef = ca::str::Utf8StringRef::from_string_view("app/a");
    ca::str::Utf8StringArena           arena1;
    mjbase::spi::GlobalAnalysisManager gam1(arena1);
    const auto&                        first = runPass(container, gam1);
    const auto* firstMethods = first.getClassMethodMappings(ownerRef);
    ASSERT_NE(firstMethods, nullptr);
    const ca::usize firstCount = firstMethods->size();

    const auto& second = runPass(container, gam1);
    EXPECT_EQ(second.getClassMethodMappings(ownerRef), firstMethods) << "同 AM 二跑缓存同址";

    ca::str::Utf8StringArena           arena2;
    mjbase::spi::GlobalAnalysisManager gam2(arena2);
    const auto&                        third = runPass(container, gam2);
    ASSERT_EQ(third.getClasses().size(), first.getClasses().size());
    EXPECT_EQ(mappedClass(third, "app/a"), mappedClass(first, "app/a"));
    EXPECT_EQ(mappedMethod(third, "app/a", "a", "()Ljava/lang/String;"),
              mappedMethod(first, "app/a", "a", "()Ljava/lang/String;"));
    EXPECT_EQ(entriesOfClass(third, "app/a", true), firstCount);
}

// ── 注册挂名面：名字可解析且产出本 pass ──
TEST(MjPassKotlinNameRestorationTest, registrableByName)
{
    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass("KotlinNameRestorer");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
