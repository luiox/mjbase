// KotlinMetadataCollection（收集 + 描述符映射工具链）单测：收集面（k=1/2/3/5
// 四形态全模型、记录操作两路、无注解排除、畸形体逐码 fail-closed、幂等二跑
// 同址缓存、独立 AM 重算相等、与 KotlinMetadataRemover 独立共存、容器字节
// 恒等、opt-in 不进默认表）+ 描述符映射面（L 形、扩展接收者首参、泛型擦除
// + Array 特例、stdlib 替换表抽样、reverseMap 命中/未命中）+ javac 真类消
// 费闭环（JDK 缺席 GTEST_SKIP）。
//
// 夹具纪律：类骨架一律 jasm 文本装配（JvmJasmService）；kotlin/Metadata 注
// 解 jasm 方言无法表达（无注解 overlay），经 CF 级 typed AnnotationsAttr 写
// 面挂载（纪律例外②申报）。d1 载荷由测试侧 kotlinc 同口径编码器（BitEncoding
// encodeBytes + protobuf 线格式）在**注解元素值数据面**生成——这些字节是 CP
// 串内容，不是 classfile 结构字节，不经任何 classfile 结构级字节拼装。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/kotlin_metadata_collection.hpp"
#include "mjbase/pass/meta_info_remover.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "jdk_fixture.hpp"
#include "shared/structural_pass_util.hpp"
#include "test_helper.hpp"

using namespace mjbase::test;

namespace fs = std::filesystem;

namespace {

using jbc::core::AnnotationsAttr;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;

using mjbase::pass::KotlinMetadataCollection;
using mjbase::pass::KotlinMetadataCollectionAnalysis;
using mjbase::pass::KotlinMetadataCollector;
using mjbase::pass::KotlinMetadataError;
using mjbase::pass::KtClass;
using mjbase::pass::KtClassKind;
using mjbase::pass::KtFunction;
using mjbase::pass::KtNullability;
using mjbase::pass::KtType;
using mjbase::pass::KtVariable;

/// 最小类体（一个返回即止的静态方法）。
constexpr const char* kMinimalBody =
    "  .method public static m ()V {\n"
    "    code: {\n"
    "      return\n"
    "    }\n"
    "  }\n";

/// UTF-8 → UTF-16（jasm 服务入参口径）。
ca::str::Utf16String utf16Of(const std::string& s)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(s.data()), s.size()));
}

/// jasm 文本汇编为 class 字节（一下一个类；汇编失败即测试失败）。
Bytes assembleClass(const std::string& internalName, const std::string& superName,
                    const std::string& body)
{
    mjbase::jasm::JvmJasmService service;
    const std::string source = ".super " + superName +
                               "\n.class public super " + internalName + " {\n" +
                               body + "}\n";
    const auto sourceName = utf16Of(internalName + ".jasm");
    const auto sourceText = utf16Of(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败: " << internalName;
        return {};
    }
    return result.value->bytes;
}

// ── d1 载荷数据面（kotlinc 同口径编码器；只产注解元素值内容）──

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

/// packed varint 串字段（wire 2 载荷内连续 varint）。
std::string pbPacked(ca::u32 field, const std::vector<ca::u64>& values)
{
    std::string payload;
    for (const ca::u64 v : values) pbVarint(payload, v);
    return pbLen(field, payload);
}

/// Type 消息（class_name 串表索引 + 显式 nullable 布尔）。
std::string pbType(ca::u32 nameIdx, bool nullable)
{
    return pbInt(6, nameIdx) + pbInt(3, nullable ? 1 : 0);
}

/// d1 帧：varint32 串表长度前缀 + StringTableTypes + 主消息
/// （kotlin parseDelimitedWith 口径）。
std::string frameD1(const std::string& table, const std::string& main)
{
    std::string out;
    pbVarint(out, table.size());
    out += table;
    out += main;
    return out;
}

/// 平铺串表（单 record range=n 覆盖全部 d2 位——kotlinc 纯表形态）。
std::string plainTable(ca::u32 count)
{
    return pbLen(1, pbInt(1, count));
}

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

/// d1 串码元序列（kotlin BitEncoding.encodeBytes 的单串形态：载荷 < 64KiB
/// 恰一串。UTF8 模式 U+0000 标记；7to8 模式 U+FFFF 标记 + 组包 + 模 128 加 1）。
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

/// @brief Metadata 注解构造规格（有效形态用）。
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

/// 按 spec 挂 kotlin/Metadata 类级 RuntimeVisibleAnnotations（typed 写面）。
Bytes withKotlinMetadata(const Bytes& assembled, const MetadataSpec& spec)
{
    auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(assembled));
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return {};
    auto    session = std::move(made).unwrap();
    ClassFile& cf   = session->backing();

    auto anno = std::make_unique<AnnotationsAttr>();
    AnnotationsAttr::Annotation entry;
    entry.typeCp = cf.cp.addUtf8("Lkotlin/Metadata;");
    entry.values.emplace_back(cf.cp.addUtf8("k"), intElem(cf, static_cast<ca::i32>(spec.k)));
    {
        AnnotationsAttr::ElemValue mv;
        mv.tag = '[';
        for (const ca::i32 v : spec.mv) mv.items.push_back(intElem(cf, v));
        entry.values.emplace_back(cf.cp.addUtf8("mv"), std::move(mv));
    }
    const std::vector<ca::u16> units = encodeData1Units(
        std::vector<ca::u8>(spec.payload.begin(), spec.payload.end()), spec.use7to8);
    entry.values.emplace_back(cf.cp.addUtf8("d1"),
                              stringArrayElem(cf, {modifiedUtf8Of(units)}));
    entry.values.emplace_back(cf.cp.addUtf8("d2"), stringArrayElem(cf, spec.d2));
    if (spec.xi.has_value()) {
        entry.values.emplace_back(cf.cp.addUtf8("xi"), intElem(cf, *spec.xi));
    }
    anno->annos.push_back(std::move(entry));
    cf.attrs.push_back(AttrSlot::makeNew(cf.cp.addUtf8("RuntimeVisibleAnnotations"),
                                         std::move(anno)));
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok())
        << "writeClass failed: "
        << (written.is_err() ? jbc::core::to_string(written.unwrap_err()) : "ok");
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// 低阶挂载：注解元素表由 builder 在挂载会话内构造（畸形桶构造用；同 typed
/// 写面）。builder 收挂载会话的 ClassFile——元素里的 CP 索引须在本会话池内
/// 生成，跨会话借用索引会指到无关槽位。
Bytes withMetadataElements(
    const Bytes& assembled,
    const std::function<std::vector<std::pair<std::string, AnnotationsAttr::ElemValue>>(
        ClassFile&)>& builder)
{
    auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(assembled));
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return {};
    auto    session = std::move(made).unwrap();
    ClassFile& cf   = session->backing();
    auto        anno    = std::make_unique<AnnotationsAttr>();
    AnnotationsAttr::Annotation entry;
    entry.typeCp = cf.cp.addUtf8("Lkotlin/Metadata;");
    for (auto& [name, value] : builder(cf)) {
        entry.values.emplace_back(cf.cp.addUtf8(name), std::move(value));
    }
    anno->annos.push_back(std::move(entry));
    cf.attrs.push_back(
        AttrSlot::makeNew(cf.cp.addUtf8("RuntimeVisibleAnnotations"), std::move(anno)));
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// 单类解析：session 打开 + parseKotlinMetadata。
ca::core::Result<KtClass, KotlinMetadataError> parseOne(const Bytes&             bytes,
                                                        ca::str::Utf8StringArena& arena)
{
    auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return ca::core::Err(KotlinMetadataError::BadAnnotationShape);
    auto session = std::move(made).unwrap();
    return mjbase::pass::parseKotlinMetadata(session->backing(), arena);
}

/// 收集 pass + 同一 GAM；返回收集结果（pass 壳注册并触发，惰性缓存命中）。
const KotlinMetadataCollection& runCollect(mjbase::model::ResourceContainer&    container,
                                           mjbase::spi::GlobalAnalysisManager& gam)
{
    KotlinMetadataCollector collector;
    const auto              pa = collector.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved()) << "收集件只读，恒 all()";
    return gam.getResult<KotlinMetadataCollectionAnalysis>(container);
}

Bytes resourceBytes(mjbase::model::ResourceContainer& container, const std::string& uri)
{
    const auto* res = ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
    EXPECT_NE(res, nullptr);
    return res == nullptr ? Bytes{} : res->get();
}

/// javac 样本源（普通类，无 kotlin/Metadata）。
constexpr const char* kPlainJavaSrc = R"JAVA(
package hb.ktmeta;

public class Plain {
    public int add(int a, int b) {
        return a + b;
    }
}
)JAVA";

void writeSource(const fs::path& path, const char* src)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << "cannot write sample source: " << path.string();
    out << src;
}

Bytes readClassBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot read sample class: " << path.string();
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ── 四形态夹具 ──

/// k=1 全形态类（app/Host；真名 app.Host → app$Host）：伴生 + 双源父类型
///（id 表展开 + inline）+ 构造器参数 + 两函数（inline 返回 / 函数级
/// TypeTable id 展开）+ 属性（first_nullable 传播覆盖内联 false）。
Bytes hostClassFixture()
{
    const std::string stt = plainTable(8);
    // d2: 0 app.Host / 1 Companion / 2 java/lang/String / 3 java/util/List /
    //     4 size / 5 first / 6 getInt / 7 compute
    const std::string supTable   = pbLen(1, pbInt(6, 3));   ///< TypeTable{ type{class_name=3} }
    const std::string ctorParam  = pbInt(2, 5) + pbLen(3, pbType(2, false));
    const std::string ctor       = pbLen(8, pbLen(2, ctorParam));
    const std::string fnGetInt  = pbInt(2, 6) + pbLen(3, pbType(2, false));
    const std::string fnCompute = pbInt(2, 7) + pbInt(7, 0)
                                  + pbLen(6, pbInt(2, 5) + pbInt(5, 0))
                                  + pbLen(30, pbLen(1, pbInt(6, 3)));
    const std::string propTable = pbLen(1, pbType(2, false)) + pbInt(2, 0);   ///< first_nullable=0
    const std::string prop      = pbInt(2, 4) + pbInt(9, 0) + pbLen(30, propTable);
    const std::string cls       = pbInt(3, 0) + pbInt(4, 1)
                              + pbPacked(2, {0})              ///< supertype_id → 表展开
                              + pbLen(6, pbType(2, false))    ///< supertype inline NonNull
                              + ctor + pbLen(9, fnGetInt) + pbLen(9, fnCompute)
                              + pbLen(10, prop) + pbLen(30, supTable);
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"app.Host", "Companion", "java/lang/String", "java/util/List",
                    "size",     "first",     "getInt",           "compute"};
    spec.xi      = 42;
    return withKotlinMetadata(assembleClass("app/Host", "java/lang/Object", kMinimalBody), spec);
}

/// k=1 记录操作面（app/Inner）：真名经 op=1 串记录（$→.）resolve、父类型经
/// op=2 DESC 记录（去 L;/.）resolve；映射双方向 kotlin 真名 ≠ 二进制名。
Bytes recordOpsClassFixture()
{
    // records: r0 平铺 idx0；r1 op=1（app$Host$Inner → app.Host.Inner）；
    //          r2 op=2（Ljava/util/Map$Entry; → java.util.Map.Entry）
    const std::string stt = pbLen(1, pbInt(1, 1))
                            + pbLen(1, pbInt(3, 1) + pbLen(6, "app$Host$Inner"))
                            + pbLen(1, pbInt(3, 2) + pbLen(6, "Ljava/util/Map$Entry;"));
    const std::string cls = pbInt(3, 1) + pbLen(6, pbInt(6, 2));
    MetadataSpec spec;
    spec.payload = frameD1(stt, cls);
    spec.d2      = {"x", "y", "z"};
    return withKotlinMetadata(assembleClass("app/Inner", "java/lang/Object", kMinimalBody), spec);
}

/// k=2 文件门面扩展函数（app/ExtKt；7to8 模式编码）：接收者经预定义串表
///（predefined=8 → kotlin/Int）resolve——predefined 覆盖同位 d2 串。
Bytes extFunctionFixture()
{
    // d2: 0 plusInt / 1 flag（r1 predefined 覆盖） / 2 kotlin/Boolean /
    //     3 x / 4 kotlin/Long；records: r0 平铺 idx0；r1 predefined=8；
    //     r2 平铺 idx2..4
    const std::string stt = pbLen(1, pbInt(1, 1))
                            + pbLen(1, pbInt(2, 8))
                            + pbLen(1, pbInt(1, 3));
    const std::string fn = pbInt(2, 0)                          ///< name=plusInt
                           + pbLen(5, pbInt(6, 1))              ///< receiver_type kotlin/Int
                           + pbLen(3, pbInt(6, 2))              ///< return kotlin/Boolean
                           + pbLen(6, pbInt(2, 3) + pbLen(3, pbInt(6, 4)));
    const std::string pkg = pbLen(3, fn);
    MetadataSpec spec;
    spec.k       = 2;   ///< 文件门面（缺省 k=1 会把 Package 载荷送进类消息解析器）
    spec.payload = frameD1(stt, pkg);
    spec.d2      = {"plusInt", "flag", "kotlin/Boolean", "x", "kotlin/Long"};
    spec.use7to8 = true;
    return withKotlinMetadata(assembleClass("app/ExtKt", "java/lang/Object", kMinimalBody), spec);
}

/// k=3 lambda 合成类（app/Lambda；恰一条函数，name = 定义类二进制名）。
Bytes lambdaFixture()
{
    const std::string fn  = pbInt(2, 0) + pbLen(3, pbInt(6, 1));
    const std::string cls = fn;   ///< Function 消息本体（name/return_type 直挂）
    MetadataSpec spec;
    spec.k       = 3;   ///< 合成类/lambda（缺省 k=1 会把 Function 载荷送进类消息解析器）
    spec.payload = frameD1(plainTable(2), cls);
    spec.d2      = {"invoke", "kotlin/Unit"};
    return withKotlinMetadata(assembleClass("app/Lambda", "java/lang/Object", kMinimalBody), spec);
}

/// k=5 多文件类部件（app/MultiPart；Package 消息 + 属性）。
Bytes multiPartFixture()
{
    const std::string prop = pbInt(2, 0) + pbLen(3, pbInt(6, 1));
    const std::string pkg  = pbLen(4, prop);
    MetadataSpec spec;
    spec.k       = 5;   ///< 多文件类部件（缺省 k=1 会把 Package 载荷送进类消息解析器）
    spec.payload = frameD1(plainTable(2), pkg);
    spec.d2      = {"topProp", "kotlin/Int"};
    return withKotlinMetadata(assembleClass("app/MultiPart", "java/lang/Object", kMinimalBody),
                              spec);
}

}   // namespace

// ── 收集面：k=1 全形态模型 ──

TEST(MjPassKotlinMetadataCollectionTest, CollectsFullClassModel)
{
    ca::str::Utf8StringArena arena;
    auto                     parsed = parseOne(hostClassFixture(), arena);
    ASSERT_TRUE(parsed.is_ok()) << "k=1 全形态应收成功";
    KtClass model = std::move(parsed).unwrap();
    EXPECT_EQ(model.kind, KtClassKind::Class);
    EXPECT_EQ(model.extraFlags, 42);
    ASSERT_TRUE(model.hasName);
    EXPECT_EQ(model.name, "app$Host");
    ASSERT_TRUE(model.hasCompanionObjectName);
    EXPECT_EQ(model.companionObjectName, "Companion");

    // 父类型：id 表展开在前（proto 字段序），inline 在后；inline 显式 NonNull。
    ASSERT_EQ(model.superTypes.size(), 2u);
    EXPECT_EQ(model.superTypes[0].name, "java/util/List");
    EXPECT_EQ(model.superTypes[0].nullability, KtNullability::Unknown);
    EXPECT_EQ(model.superTypes[1].name, "java/lang/String");
    EXPECT_EQ(model.superTypes[1].nullability, KtNullability::NonNull);

    // 构造器参数（inline 类型形）。
    ASSERT_EQ(model.constructors.size(), 1u);
    ASSERT_EQ(model.constructors[0].parameters.size(), 1u);
    EXPECT_EQ(model.constructors[0].parameters[0].name, "first");
    ASSERT_TRUE(model.constructors[0].parameters[0].hasType);
    EXPECT_EQ(model.constructors[0].parameters[0].type.name, "java/lang/String");

    // 函数：getInt inline 返回；compute 经函数级 TypeTable id 展开（返回与参数）。
    ASSERT_EQ(model.functions.size(), 2u);
    EXPECT_EQ(model.functions[0].name, "getInt");
    ASSERT_TRUE(model.functions[0].hasReturnType);
    EXPECT_EQ(model.functions[0].returnType.name, "java/lang/String");
    EXPECT_EQ(mjbase::pass::ktFunctionDescriptor(model.functions[0]), "()Ljava/lang/String;");
    EXPECT_EQ(model.functions[1].name, "compute");
    ASSERT_TRUE(model.functions[1].hasReturnType);
    EXPECT_EQ(model.functions[1].returnType.name, "java/util/List");
    ASSERT_EQ(model.functions[1].parameters.size(), 1u);
    EXPECT_EQ(model.functions[1].parameters[0].name, "first");
    ASSERT_TRUE(model.functions[1].parameters[0].hasType);
    EXPECT_EQ(model.functions[1].parameters[0].type.name, "java/util/List");

    // 属性：first_nullable=0 强制覆盖内联 false（传播面实证）。
    ASSERT_EQ(model.properties.size(), 1u);
    EXPECT_EQ(model.properties[0].name, "size");
    ASSERT_TRUE(model.properties[0].hasType);
    EXPECT_EQ(model.properties[0].type.name, "java/lang/String");
    EXPECT_EQ(model.properties[0].type.nullability, KtNullability::Nullable);
}

TEST(MjPassKotlinMetadataCollectionTest, ResolvesRecordsOperationsBothForms)
{
    ca::str::Utf8StringArena arena;
    auto                     parsed = parseOne(recordOpsClassFixture(), arena);
    ASSERT_TRUE(parsed.is_ok());
    KtClass model = std::move(parsed).unwrap();
    ASSERT_TRUE(model.hasName);
    EXPECT_EQ(model.name, "app$Host$Inner") << "op=1 串记录（$→.）+ 内部名化";
    ASSERT_EQ(model.superTypes.size(), 1u);
    EXPECT_EQ(model.superTypes[0].name, "java/util/Map$Entry") << "op=2 DESC 记录（去 L;/→.）";
}

TEST(MjPassKotlinMetadataCollectionTest, CollectsFileFacadeExtension7to8Mode)
{
    ca::str::Utf8StringArena arena;
    auto                     parsed = parseOne(extFunctionFixture(), arena);
    ASSERT_TRUE(parsed.is_ok())
        << "7to8 模式 d1 应解码成功, 实际码: "
        << mjbase::pass::to_string(parsed.is_err() ? std::move(parsed).unwrap_err()
                                                   : KotlinMetadataError::Ok);
    KtClass model = std::move(parsed).unwrap();
    EXPECT_EQ(model.kind, KtClassKind::File);
    ASSERT_TRUE(model.hasName);
    EXPECT_EQ(model.name, "app/ExtKt") << "文件门面 name = 定义类二进制名";
    ASSERT_EQ(model.functions.size(), 1u);
    const KtFunction& fn = model.functions[0];
    EXPECT_EQ(fn.name, "plusInt");
    ASSERT_TRUE(fn.hasReceiver);
    EXPECT_EQ(fn.receiverType.name, "kotlin/Int") << "预定义串表命中（覆盖同位 d2 串）";
    ASSERT_TRUE(fn.hasReturnType);
    EXPECT_EQ(fn.returnType.name, "kotlin/Boolean");
    ASSERT_EQ(fn.parameters.size(), 1u);
    EXPECT_EQ(fn.parameters[0].name, "x");
    ASSERT_TRUE(fn.parameters[0].hasType);
    EXPECT_EQ(fn.parameters[0].type.name, "kotlin/Long");

    // 描述符闭环：扩展接收者作 JVM 首参（Recaf 原件丢弃此面的增强点）+
    // stdlib 替换（Int→I、Long→J、Boolean→Z）。
    EXPECT_EQ(mjbase::pass::ktFunctionDescriptor(fn),
              "(Lkotlin/Int;Lkotlin/Long;)Lkotlin/Boolean;");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor(mjbase::pass::ktFunctionDescriptor(fn)),
              "(IJ)Z");
}

TEST(MjPassKotlinMetadataCollectionTest, CollectsLambdaSyntheticClass)
{
    ca::str::Utf8StringArena arena;
    auto                     parsed = parseOne(lambdaFixture(), arena);
    ASSERT_TRUE(parsed.is_ok())
        << "k=3 lambda 应收成功, 实际码: "
        << mjbase::pass::to_string(parsed.is_err() ? std::move(parsed).unwrap_err()
                                                   : KotlinMetadataError::Ok);
    KtClass model = std::move(parsed).unwrap();
    EXPECT_EQ(model.kind, KtClassKind::SyntheticClass);
    ASSERT_TRUE(model.hasName);
    EXPECT_EQ(model.name, "app/Lambda") << "lambda name = 定义类二进制名";
    ASSERT_EQ(model.functions.size(), 1u);
    EXPECT_EQ(model.functions[0].name, "invoke");
    ASSERT_TRUE(model.functions[0].hasReturnType);
    EXPECT_EQ(model.functions[0].returnType.name, "kotlin/Unit");
    EXPECT_EQ(mjbase::pass::ktFunctionDescriptor(model.functions[0]),
              std::string("()Lkotlin/Unit;"));
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("()Lkotlin/Unit;"), "()V");
}

TEST(MjPassKotlinMetadataCollectionTest, CollectsMultiFilePart)
{
    ca::str::Utf8StringArena arena;
    auto                     parsed = parseOne(multiPartFixture(), arena);
    ASSERT_TRUE(parsed.is_ok())
        << "k=5 部件应收成功, 实际码: "
        << mjbase::pass::to_string(parsed.is_err() ? std::move(parsed).unwrap_err()
                                                   : KotlinMetadataError::Ok);
    KtClass model = std::move(parsed).unwrap();
    EXPECT_EQ(model.kind, KtClassKind::MultiFilePart);
    ASSERT_EQ(model.properties.size(), 1u);
    EXPECT_EQ(model.properties[0].name, "topProp");
    ASSERT_TRUE(model.properties[0].hasType);
    EXPECT_EQ(model.properties[0].type.name, "kotlin/Int");
}

// ── 容器收集：索引 + 排除 + 映射双方向 ──

TEST(MjPassKotlinMetadataCollectionTest, CollectsContainerIndexAndSkipsNonKotlin)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "app/Host.class", hostClassFixture());
    addClassResource(container, "app/Inner.class", recordOpsClassFixture());
    addClassResource(container, "JavaCls.class",
                     assembleClass("JavaCls", "java/lang/Object", kMinimalBody));
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const KotlinMetadataCollection& collection = runCollect(container, gam);
    ASSERT_EQ(collection.size(), 2u) << "无注解类排除";
    EXPECT_EQ(collection.kotlinNameOf("app/Host"), "app$Host");
    EXPECT_EQ(collection.binaryNameOf("app$Host"), "app/Host");
    EXPECT_TRUE(collection.kotlinNameOf("JavaCls").is_empty());
    EXPECT_EQ(collection.findClass("JavaCls"), nullptr);

    const KtClass* inner = collection.findClassByKotlinName("app$Host$Inner");
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->kind, KtClassKind::Class);
    EXPECT_EQ(collection.binaryNameOf("app$Host$Inner"), "app/Inner");
    EXPECT_EQ(collection.findClassByKotlinName("missing"), nullptr);
}

TEST(MjPassKotlinMetadataCollectionTest, CollectionObjectIndexesBothDirections)
{
    KotlinMetadataCollection collection;
    {
        KtClass model;
        model.kind    = KtClassKind::Class;
        model.name    = collection.arena().intern("app$Host");
        model.hasName = true;
        EXPECT_TRUE(collection.add(collection.arena().intern("app/Host"), std::move(model)));
    }
    {
        KtClass model;
        model.kind    = KtClassKind::File;
        model.name    = collection.arena().intern("app/ExtKt");
        model.hasName = true;
        EXPECT_TRUE(collection.add(collection.arena().intern("app/ExtKt"), std::move(model)));
    }
    ASSERT_EQ(collection.size(), 2u);
    ASSERT_NE(collection.findClass("app/Host"), nullptr);
    ASSERT_NE(collection.findClassByKotlinName("app$Host"), nullptr);
    EXPECT_EQ(collection.findClass("nope"), nullptr);
    EXPECT_EQ(collection.findClassByKotlinName("nope"), nullptr);
    // 同键二次写入拒绝（幂等二跑前提）。
    KtClass dup;
    dup.name    = collection.arena().intern("other");
    dup.hasName = true;
    EXPECT_FALSE(collection.add(collection.arena().intern("app/Host"), std::move(dup)));
    EXPECT_EQ(collection.size(), 2u);
}

// ── fail-closed 逐码 ──

TEST(MjPassKotlinMetadataCollectionTest, RejectsClassWithoutAnnotation)
{
    ca::str::Utf8StringArena arena;
    auto parsed = parseOne(assembleClass("Plain", "java/lang/Object", kMinimalBody), arena);
    ASSERT_TRUE(parsed.is_err());
    EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::NoAnnotation);
}

TEST(MjPassKotlinMetadataCollectionTest, RejectsBadAnnotationShape)
{
    const Bytes plain = assembleClass("T", "java/lang/Object", kMinimalBody);
    ca::str::Utf8StringArena arena;
    {
        // 缺 d1/d2：仅 k。
        auto parsed = parseOne(
            withMetadataElements(plain,
                                 [](ClassFile& cf) {
                                     std::vector<std::pair<std::string, AnnotationsAttr::ElemValue>>
                                         elements;
                                     elements.emplace_back("k", intElem(cf, 1));
                                     return elements;
                                 }),
            arena);
        ASSERT_TRUE(parsed.is_err());
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::BadAnnotationShape);
    }
    {
        // k 形状不符：'s' 串元素。
        auto parsed = parseOne(
            withMetadataElements(plain,
                                 [](ClassFile& cf) {
                                     AnnotationsAttr::ElemValue kStr;
                                     kStr.tag = 's';
                                     kStr.cp  = cf.cp.addString(cf.cp.addUtf8("x"));
                                     std::vector<std::pair<std::string, AnnotationsAttr::ElemValue>>
                                         elements;
                                     elements.emplace_back("k", std::move(kStr));
                                     return elements;
                                 }),
            arena);
        ASSERT_TRUE(parsed.is_err());
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::BadAnnotationShape);
    }
}

TEST(MjPassKotlinMetadataCollectionTest, RejectsCorruptData1Encoding)
{
    const Bytes plain = assembleClass("T", "java/lang/Object", kMinimalBody);
    ca::str::Utf8StringArena arena;
    // d1 串含非法 modified-UTF8 序列（孤续字节 0x80）→ 解码拒绝。
    // k 与 d2 须在位：形状门先行，仅 d1 在场会先落 BadAnnotationShape。
    auto parsed = parseOne(
        withMetadataElements(plain,
                             [](ClassFile& cf) {
                                 AnnotationsAttr::ElemValue kElem = intElem(cf, 1);
                                 AnnotationsAttr::ElemValue d1;
                                 d1.tag = '[';
                                 AnnotationsAttr::ElemValue corrupt;
                                 corrupt.tag = 's';
                                 corrupt.cp  = cf.cp.addString(cf.cp.addUtf8(std::string_view("\x80", 1)));
                                 d1.items.push_back(std::move(corrupt));
                                 AnnotationsAttr::ElemValue d2 = stringArrayElem(cf, {"x"});
                                 std::vector<std::pair<std::string, AnnotationsAttr::ElemValue>>
                                     elements;
                                 elements.emplace_back("k", std::move(kElem));
                                 elements.emplace_back("d1", std::move(d1));
                                 elements.emplace_back("d2", std::move(d2));
                                 return elements;
                             }),
        arena);
    ASSERT_TRUE(parsed.is_err()) << "实际码: "
                                 << mjbase::pass::to_string(parsed.is_err()
                                                                ? std::move(parsed).unwrap_err()
                                                                : KotlinMetadataError::Ok);
    EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::BadData1Encoding);
}

TEST(MjPassKotlinMetadataCollectionTest, RejectsBadStringTable)
{
    ca::str::Utf8StringArena arena;
    const Bytes plain = assembleClass("T", "java/lang/Object", kMinimalBody);
    MetadataSpec spec;
    // Σrange=2 > d2=1：展开越界。
    spec.payload = frameD1(pbLen(1, pbInt(1, 2)), pbInt(3, 0));
    spec.d2      = {"x"};
    {
        auto parsed = parseOne(withKotlinMetadata(plain, spec), arena);
        ASSERT_TRUE(parsed.is_err());
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::BadStringTable);
    }
    // range=0：非正。
    spec.payload = frameD1(pbLen(1, pbInt(1, 0)), pbInt(3, 0));
    {
        auto parsed = parseOne(withKotlinMetadata(plain, spec), arena);
        ASSERT_TRUE(parsed.is_err());
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::BadStringTable);
    }
}

TEST(MjPassKotlinMetadataCollectionTest, RejectsBadProto)
{
    ca::str::Utf8StringArena arena;
    const Bytes       plain = assembleClass("T", "java/lang/Object", kMinimalBody);
    // 主消息截断：fq_name 声明 len=127 但载荷只剩 1 字节。
    {
        MetadataSpec spec;
        spec.payload = frameD1(plainTable(1), std::string("\x1A\x7F", 2));
        spec.d2      = {"x"};
        auto parsed  = parseOne(withKotlinMetadata(plain, spec), arena);
        ASSERT_TRUE(parsed.is_err());
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::BadProto);
    }
    // required fq_name 缺席。
    {
        MetadataSpec spec;
        spec.payload = frameD1(plainTable(1), "");
        spec.d2      = {"x"};
        auto parsed  = parseOne(withKotlinMetadata(plain, spec), arena);
        ASSERT_TRUE(parsed.is_err());
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::BadProto);
    }
    // 串表索引越界（fq_name=5 超出记录数）。
    {
        MetadataSpec spec;
        spec.payload = frameD1(plainTable(1), pbInt(3, 5));
        spec.d2      = {"x"};
        auto parsed  = parseOne(withKotlinMetadata(plain, spec), arena);
        ASSERT_TRUE(parsed.is_err());
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::BadProto);
    }
}

TEST(MjPassKotlinMetadataCollectionTest, RejectsUnsupportedKind)
{
    ca::str::Utf8StringArena arena;
    const Bytes       plain = assembleClass("T", "java/lang/Object", kMinimalBody);
    for (const ca::u32 badKind : {ca::u32{4}, ca::u32{99}}) {
        MetadataSpec spec;
        spec.k       = badKind;
        spec.payload = frameD1(plainTable(1), "");
        spec.d2      = {"x"};
        auto parsed  = parseOne(withKotlinMetadata(plain, spec), arena);
        ASSERT_TRUE(parsed.is_err()) << "k=" << badKind;
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::UnsupportedKind);
    }
}

// ── 幂等 / 独立 / 只读 / 共存 ──

TEST(MjPassKotlinMetadataCollectionTest, IdempotentSecondRunHitsSameCache)
{
    mjbase::model::ResourceContainer   container;
    addClassResource(container, "app/Host.class", hostClassFixture());
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const KotlinMetadataCollection& first = runCollect(container, gam);
    ASSERT_EQ(first.size(), 1u);

    // 二跑：pass 壳重入 + getResult 同址缓存。
    const KotlinMetadataCollection& second = runCollect(container, gam);
    EXPECT_EQ(&second, &first) << "同 AM 二跑应命中缓存（同址）";
    EXPECT_EQ(second.size(), 1u);
    EXPECT_EQ(second.kotlinNameOf("app/Host"), "app$Host");
}

TEST(MjPassKotlinMetadataCollectionTest, IndependentGamRecomputesEqually)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "app/Host.class", hostClassFixture());
    addClassResource(container, "app/ExtKt.class", extFunctionFixture());

    ca::str::Utf8StringArena           arena1;
    mjbase::spi::GlobalAnalysisManager gam1(arena1);
    const KotlinMetadataCollection&    first = runCollect(container, gam1);
    ASSERT_EQ(first.size(), 2u);

    ca::str::Utf8StringArena           arena2;
    mjbase::spi::GlobalAnalysisManager gam2(arena2);
    const KotlinMetadataCollection&    second = runCollect(container, gam2);
    ASSERT_EQ(second.size(), first.size());
    EXPECT_EQ(second.kotlinNameOf("app/Host"), first.kotlinNameOf("app/Host"));
    EXPECT_EQ(second.kotlinNameOf("app/ExtKt"), first.kotlinNameOf("app/ExtKt"));
    ASSERT_NE(first.findClass("app/ExtKt"), nullptr);
    ASSERT_NE(second.findClass("app/ExtKt"), nullptr);
    ASSERT_EQ(first.findClass("app/ExtKt")->functions.size(), 1u);
    EXPECT_EQ(second.findClass("app/ExtKt")->functions[0].name, "plusInt");
}

TEST(MjPassKotlinMetadataCollectionTest, ContainerBytesUntouched)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "app/Host.class", hostClassFixture());
    addClassResource(container, "app/ExtKt.class", extFunctionFixture());
    const Bytes beforeHost   = resourceBytes(container, "app/Host.class");
    const Bytes beforeExtKt  = resourceBytes(container, "app/ExtKt.class");

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    ASSERT_EQ(runCollect(container, gam).size(), 2u);

    EXPECT_EQ(resourceBytes(container, "app/Host.class"), beforeHost) << "收集件不得改容器字节";
    EXPECT_EQ(resourceBytes(container, "app/ExtKt.class"), beforeExtKt);
}

TEST(MjPassKotlinMetadataCollectionTest, CoexistsIndependentlyWithKotlinMetadataRemover)
{
    const Bytes              bytes = hostClassFixture();
    ca::str::Utf8StringArena arena;
    // 收集面先读：模型可得（读槽）。
    {
        auto parsed = parseOne(bytes, arena);
        ASSERT_TRUE(parsed.is_ok());
        KtClass model = std::move(parsed).unwrap();
        EXPECT_EQ(model.name, "app$Host");
    }
    // 删除面删槽（独立组件，不经收集件）；删后收集面 NoAnnotation。
    {
        auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
        ASSERT_TRUE(made.is_ok());
        auto session = std::move(made).unwrap();
        ca::str::Utf8StringArena          arena2;
        mjbase::spi::ClassAnalysisManager cam(arena2);
        mjbase::pass::KotlinMetadataRemover remover;
        EXPECT_TRUE(remover.run(*session, cam).changed());
        auto reparsed = mjbase::pass::parseKotlinMetadata(session->backing(), arena);
        ASSERT_TRUE(reparsed.is_err());
        EXPECT_EQ(std::move(reparsed).unwrap_err(), KotlinMetadataError::NoAnnotation);
    }
}

TEST(MjPassKotlinMetadataCollectionTest, OptInNotSeededInDefaultRegistry)
{
    const auto found = mjbase::spi::defaultPassRegistry().find("KotlinMetadataCollector");
    EXPECT_TRUE(found.is_err()) << "收集是分析面，本批不进默认注册表";
}

// ── 描述符映射工具链 ──

TEST(MjPassKotlinMetadataCollectionTest, MapsKotlinTypesToDescriptors)
{
    ca::str::Utf8StringArena arena;
    const auto named = [&arena](std::string_view n) {
        KtType t;
        t.name    = arena.intern(n);
        t.hasName = true;
        return t;
    };
    EXPECT_EQ(mjbase::pass::ktTypeDescriptor(named("java/lang/String")),
              "Ljava/lang/String;");
    EXPECT_EQ(mjbase::pass::ktTypeDescriptor(named("kotlin/Int")), "Lkotlin/Int;");

    KtType star;   ///< 无名类型（星号投影等）
    EXPECT_EQ(mjbase::pass::ktTypeDescriptor(star), "Ljava/lang/Object;");

    KtType array;
    array.name    = arena.intern("kotlin/Array");
    array.hasName = true;
    array.arguments.push_back(named("kotlin/Int"));
    EXPECT_EQ(mjbase::pass::ktTypeDescriptor(array), "[Lkotlin/Int;") << "Array 特例取首实参";
    array.arguments.clear();
    EXPECT_EQ(mjbase::pass::ktTypeDescriptor(array), "Ljava/lang/Object;") << "首实参缺位退 Object";

    KtType arrayOfArray;
    arrayOfArray.name    = arena.intern("kotlin/Array");
    arrayOfArray.hasName = true;
    KtType innerArray;   ///< 新建内层数组（复用上面临清空实参的 array 会退 Object）
    innerArray.name    = arena.intern("kotlin/Array");
    innerArray.hasName = true;
    innerArray.arguments.push_back(named("kotlin/Int"));
    arrayOfArray.arguments.push_back(std::move(innerArray));
    EXPECT_EQ(mjbase::pass::ktTypeDescriptor(arrayOfArray), "[[Lkotlin/Int;") << "递归拼数组前缀";
}

TEST(MjPassKotlinMetadataCollectionTest, MapsFunctionDescriptorWithReceiverFirst)
{
    ca::str::Utf8StringArena arena;
    const auto named = [&arena](std::string_view n) {
        KtType t;
        t.name    = arena.intern(n);
        t.hasName = true;
        return t;
    };
    // 扩展函数 Long.extend(Int): Boolean → 接收者作 JVM 首参。
    KtFunction ext;
    ext.name             = arena.intern("extend");
    ext.hasReceiver      = true;
    ext.receiverType     = named("kotlin/Int");
    ext.hasReturnType    = true;
    ext.returnType       = named("kotlin/Boolean");
    KtVariable param;
    param.name      = arena.intern("x");
    param.type      = named("kotlin/Long");
    param.hasType   = true;
    ext.parameters.push_back(std::move(param));
    EXPECT_EQ(mjbase::pass::ktFunctionDescriptor(ext),
              "(Lkotlin/Int;Lkotlin/Long;)Lkotlin/Boolean;");

    // 无返回类型落 Object；参数缺类型落 Object。
    KtFunction partial;
    partial.name = arena.intern("partial");
    KtVariable untyped;
    untyped.name = arena.intern("p");
    partial.parameters.push_back(std::move(untyped));
    EXPECT_EQ(mjbase::pass::ktFunctionDescriptor(partial),
              "(Ljava/lang/Object;)Ljava/lang/Object;");
}

TEST(MjPassKotlinMetadataCollectionTest, SubstitutesKotlinStdlibDescriptors)
{
    // 方法描述符逐段替换。
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("(Lkotlin/Int;Lkotlin/String;)Lkotlin/Unit;"),
              "(ILjava/lang/String;)V");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor(
                  "(Lkotlin/collections/List;Lkotlin/Any;)Lkotlin/Boolean;"),
              "(Ljava/util/List;Ljava/lang/Object;)Z");
    // 字段描述符与数组前缀：数组段剥前缀后逐表替换再回拼
    // （[Lkotlin/IntArray; = Array<IntArray> = int[][] → [[I）。
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("Lkotlin/collections/Map;"),
              "Ljava/util/Map;");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("[Lkotlin/IntArray;"), "[[I");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("[[Lkotlin/Int;"), "[[I");
    // FunctionN 归位 kotlin.jvm.functions。
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("Lkotlin/Function2;"),
              "Lkotlin/jvm/functions/Function2;");
    // 未命中段原样；原语/普通形态不动。
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("Lfoo/Bar;"), "Lfoo/Bar;");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("(IZ)V"), "(IZ)V");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("()V"), "()V");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("Ljava/lang/Object;"),
              "Ljava/lang/Object;");
    // 非描述符文法输入原样返回（fail-closed）。
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("java/lang/String"), "java/lang/String");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor("Lfoo/Bar"), "Lfoo/Bar");
    EXPECT_EQ(mjbase::pass::kotlinToJvmDescriptor(""), "");
}

TEST(MjPassKotlinMetadataCollectionTest, ReverseMapsDescriptorByCollectedNames)
{
    KotlinMetadataCollection collection;
    KtClass model;
    model.kind    = KtClassKind::Class;
    model.name    = collection.arena().intern("app$Host$Inner");
    model.hasName = true;
    EXPECT_TRUE(collection.add(collection.arena().intern("app/Inner"), std::move(model)));

    EXPECT_EQ(mjbase::pass::reverseMapDescriptor(collection,
                                                 "(Lapp$Host$Inner;)Lapp$Host$Inner;"),
              "(Lapp/Inner;)Lapp/Inner;");
    EXPECT_EQ(mjbase::pass::reverseMapDescriptor(collection, "[Lapp$Host$Inner;"),
              "[Lapp/Inner;");
    EXPECT_EQ(mjbase::pass::reverseMapDescriptor(collection, "(Lapp/Inner;)V"),
              "(Lapp/Inner;)V") << "二进制名段不在真名表，原样";
    EXPECT_EQ(mjbase::pass::reverseMapDescriptor(collection, "(LFoo;)V"), "(LFoo;)V");
    EXPECT_EQ(mjbase::pass::reverseMapDescriptor(collection, "malformed"), "malformed");
}

// ── javac 真类消费闭环 ──

TEST(MjPassKotlinMetadataCollectionTest, ConsumesJavacRealClass)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }
    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate_kt_meta";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";
    writeSource(srcDir / "hb/ktmeta/Plain.java", kPlainJavaSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/ktmeta/Plain.java", binDir, &compileErr))
        << compileErr;
    const Bytes javacBytes = readClassBytes(binDir / "hb/ktmeta/Plain.class");
    ASSERT_FALSE(javacBytes.empty());

    // 无注解真类：收集面排除（NoAnnotation）。
    ca::str::Utf8StringArena arena;
    {
        auto parsed = parseOne(javacBytes, arena);
        ASSERT_TRUE(parsed.is_err());
        EXPECT_EQ(std::move(parsed).unwrap_err(), KotlinMetadataError::NoAnnotation);
    }
    // 挂真注解（CF 写面）→ 解析面在 javac 产物上同型成立。
    {
        MetadataSpec spec;
        spec.payload = frameD1(plainTable(1), pbInt(3, 0));
        spec.d2      = {"hb.ktmeta.Plain"};
        auto parsed  = parseOne(withKotlinMetadata(javacBytes, spec), arena);
        ASSERT_TRUE(parsed.is_ok());
        KtClass model = std::move(parsed).unwrap();
        EXPECT_EQ(model.kind, KtClassKind::Class);
        EXPECT_EQ(model.name, "hb$ktmeta$Plain");
    }
}
