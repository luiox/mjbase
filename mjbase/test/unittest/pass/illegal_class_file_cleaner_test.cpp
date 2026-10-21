/// 段（889 行起），并按任务补齐旧栈缺失的独立直接用例。断言口径：
/// 不可解析字节原样保留；可解析资源清洗后仍可解析；改动走两阶段提交；
/// PA 与实际改动一致。
/// F 线清洗批起规则面为两级流水全量：libjclassfile 前置级 5 条挂账规则
/// （属性级 CP 校验剔除 / 越界跳转与 LDC 桩替 / LVT·LVTT·LNT 修剪 / BSM 压缩
/// 与 condy 手术 / 无效句柄出池）+ libjbc 级 2 条结构规则；craft 畸形样本
/// 逐规则验收 + 合法输入字节逐位不动断言，载体见文件尾部 CraftBuilder。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jclassfile/io/class_file_reader.hpp"
#include "jclassfile/model/attribute/attribute_constants.hpp"
#include "jclassfile/model/attribute/bootstrap_methods_attribute.hpp"
#include "jclassfile/model/attribute/code_attribute.hpp"
#include "jclassfile/model/attribute/line_number_table_attribute.hpp"
#include "jclassfile/model/attribute/local_variable_table_attribute.hpp"
#include "jclassfile/model/attribute/local_variable_type_table_attribute.hpp"
#include "jclassfile/model/const_pool.hpp"
#include "jclassfile/model/constant/cp_entry.hpp"
#include "jclassfile/model/constant/const_dynamic.hpp"
#include "jclassfile/model/constant/cp_method_handle.hpp"
#include "jclassfile/model/constant_pool_constants.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/illegal_class_file_cleaner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

std::vector<ca::u8> readSampleBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<ca::u8>(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
}

/// 重读 class 字节，返回解析产物与第 methodIdx 个方法的已解码 Code 属性。
/// ReadResult 持有模型（move-only）：产物随结构体整体带回调用方，Code 引用
/// 的寿命由调用方的 ParsedCode 变量保证。
struct ParsedCode
{
    jbc::core::ReadResult       parsed;
    const jbc::core::CodeAttr*  code = nullptr;
};

ParsedCode parsedCodeOf(const std::vector<ca::u8>& bytes, ca::usize methodIdx)
{
    ParsedCode out;
    auto       read = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(read.is_ok()) << "cleaned bytes must stay parseable";
    if (read.is_err()) return out;
    out.parsed = std::move(read).unwrap();
    jbc::core::ClassFile& cf = *out.parsed.file;
    EXPECT_LT(methodIdx, cf.methods.size());
    if (methodIdx >= cf.methods.size()) return out;
    jbc::core::MemberInfo& member = cf.methods[methodIdx];
    jbc::core::AttrSlot*   slot   = nullptr;
    for (jbc::core::AttrSlot& s : member.attrs) {
        if (cf.cp.utf8Equals(s.nameCp, "Code")) slot = &s;
    }
    EXPECT_NE(slot, nullptr) << "method has no Code attribute";
    if (slot == nullptr) return out;
    if (!slot->decoded()) {
        EXPECT_TRUE(cf.decodeAttr(*slot).is_ok()) << "Code attribute must decode";
        if (!slot->decoded()) return out;
    }
    out.code = ca::core::tag_cast::cast<const jbc::core::CodeAttr>(slot->peek());
    EXPECT_NE(out.code, nullptr);
    return out;
}

}   // namespace

// 旧栈用例移植：3 字节垃圾不可解析 → PA all()、原字节原样保留。
TEST(MjPassIllegalClassFileCleanerTest, PreservesUnparseableClassBytes)
{
    const std::vector<ca::u8> invalidBytes{0xCA, 0xFE, 0xBA};
    mjbase::model::ResourceContainer container;
    container.addResource("Bad.class",
                          std::make_unique<mjbase::model::ClassResource>("Bad.class",
                                                                         invalidBytes));

    mjbase::pass::IllegalClassFileCleaner pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_FALSE(pa.changed());
    auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource>(
        container.getResource("Bad.class"));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->get(), invalidBytes);
}

// 补齐直接用例：非法描述符成员整条剔除，合法成员保留；PA none()；产物可重读。
TEST(MjPassIllegalClassFileCleanerTest, StripsIllegalDescriptorMembers)
{
    FixtureBuilder builder("T");
    builder.addField({"goodField", "I"});
    builder.addField({"badField", "HI"});      // 'H' 非字段类型起始
    builder.addMethod({"goodMethod", "()V", bareReturn()});
    builder.addMethod({"badMethod", "(V)V", bareReturn()});   // V 不在参数位
    auto bytes = builder.assemble();

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    mjbase::pass::IllegalClassFileCleaner pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(pa.changed());
    auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource>(
        container.getResource("T.class"));
    ASSERT_NE(res, nullptr);
    const auto& output = res->get();

    auto read = jbc::core::readClass(jbc::core::ByteView(output));
    ASSERT_TRUE(read.is_ok());
    jbc::core::ReadResult parsed = std::move(read).unwrap();
    jbc::core::ClassFile& cf     = *parsed.file;
    ASSERT_EQ(cf.fields.size(), 1u);
    EXPECT_EQ(cf.nameOf(cf.fields[0]), "goodField");
    ASSERT_EQ(cf.methods.size(), 1u);
    EXPECT_EQ(cf.nameOf(cf.methods[0]), "goodMethod");
}

// 补齐直接用例：越界/空区间异常表条目剪除，合法条目保留。
// 注意：合法条目取 (0,1,1)——前置级旧栈语义以「最后一条指令起始偏移」为
// startPc/endPc/handlerPc 上界（endPc==code_length 越过该上界的条目也会被
// 剪），夹具取两边界语义下都合法的形态，验证「垃圾剪除、合法保留」本身。
TEST(MjPassIllegalClassFileCleanerTest, PrunesJunkExceptionTableEntries)
{
    FixtureBuilder builder("T");
    FixtureMethod  m;
    m.name = "t";
    m.desc = "()V";
    m.code = nopReturn();   // code_length = 2，最后一条指令起始偏移 = 1
    // (0,1,1) 界内合法保留；(5,7,1) 全越界剪除；(1,1,1) 空区间剪除。
    m.exc  = {ExcSpec{0, 1, 1, 0}, ExcSpec{5, 7, 1, 0}, ExcSpec{1, 1, 1, 0}};
    builder.addMethod(m);
    auto bytes = builder.assemble();

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    mjbase::pass::IllegalClassFileCleaner pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    EXPECT_TRUE(pa.changed());
    auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource>(
        container.getResource("T.class"));
    ASSERT_NE(res, nullptr);
    const ParsedCode parsed = parsedCodeOf(res->get(), 0);
    ASSERT_NE(parsed.code, nullptr);
    ASSERT_EQ(parsed.code->excTable.size(), 1u);
    EXPECT_EQ(parsed.code->excTable[0].startPc, 0u);
    EXPECT_EQ(parsed.code->excTable[0].endPc, 1u);
    EXPECT_EQ(parsed.code->excTable[0].handlerPc, 1u);
}

// 补齐直接用例：结构本就干净的可解析类 → 字节逐位不变、PA all()。
TEST(MjPassIllegalClassFileCleanerTest, LeavesValidClassUntouched)
{
    FixtureBuilder builder("T");
    builder.addField({"f", "I"});
    builder.addMethod({"t", "()V", nopReturn()});
    builder.addMethod({"u", "()V", bareReturn()});
    auto bytes = builder.assemble();

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    mjbase::pass::IllegalClassFileCleaner pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_FALSE(pa.changed());
    auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource>(
        container.getResource("T.class"));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->get(), bytes);
}

// 补齐直接用例：非 class 资源不触碰（旧契约「非 class 资源不触碰」的直接验收）。
TEST(MjPassIllegalClassFileCleanerTest, SkipsNonClassResources)
{
    mjbase::model::ResourceContainer container;
    const std::vector<ca::u8>        unknownBytes{0x01, 0x02, 0x03};
    container.addResource("data.bin",
                          std::make_unique<mjbase::model::UnknownResource>("data.bin",
                                                                           unknownBytes));

    mjbase::pass::IllegalClassFileCleaner pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    EXPECT_TRUE(pa.areAllPreserved());
    auto* res = ca::core::tag_cast::cast<mjbase::model::UnknownResource>(
        container.getResource("data.bin"));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->get(), unknownBytes);
}

// 旧栈用例移植（proguard 真实样本冒烟）：逐资源断言「字节未动或改后仍可解析」
// 与 PA 一致性。旧栈 changed>0 断言绑全量 IllegalStrippingTransformer 规则集
// （属性级清洗），不在本批对齐子集的保证面内，故不照搬。
TEST(MjPassIllegalClassFileCleanerTest, ProguardFixtureStaysParseable)
{
    const auto root = std::filesystem::path("libs/libjclassfile/test_resource/obfuscated/proguard");
    if (!std::filesystem::exists(root)) {
        GTEST_SKIP() << "proguard classfile samples not found: " << root.string();
    }

    std::vector<std::vector<ca::u8>> originalBytes;
    mjbase::model::ResourceContainer container;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".class") continue;
        auto bytes = readSampleBytes(entry.path());
        if (bytes.empty()) continue;
        originalBytes.push_back(bytes);
        const auto  uri = entry.path().filename().generic_string();
        container.addResource(uri,
                              std::make_unique<mjbase::model::ClassResource>(uri,
                                                                             std::move(bytes)));
    }
    ASSERT_FALSE(originalBytes.empty());

    mjbase::pass::IllegalClassFileCleaner pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    const auto                            pa = pass.run(container, gam);

    bool changed = false;
    container.visitResources([&](const std::string_view, const mjbase::model::IResource& resource) {
        const auto* cls = ca::core::tag_cast::cast<const mjbase::model::ClassResource>(&resource);
        ASSERT_NE(cls, nullptr);
        bool untouched = false;
        for (const auto& before : originalBytes) {
            if (before == cls->get()) {
                untouched = true;
                break;
            }
        }
        if (untouched) return;
        changed = true;
        // 改动过的资源必须仍可解析（清洗不产生新的不可解析形态）。
        auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
        EXPECT_TRUE(read.is_ok()) << "cleaned sample must stay parseable";
    });
    EXPECT_EQ(changed, pa.changed());
}

// ==================== F 线清洗批：libjclassfile 前置级 5 条挂账规则 ====================
// craft 畸形样本逐规则验收 + 合法输入字节不动。craft 载体为文件局部的
// CraftBuilder（共享 structural_pass_util 不建模属性级/BSM/condy 形态），
// 产物级断言经 libjclassfile 模型重读 + libjbc 可重读双面验收。

namespace {

using jclassfile::model::Attribute;
using jclassfile::model::AttributeConstants;
using jclassfile::model::ClassFile;
using jclassfile::model::CodeAttribute;
using jclassfile::model::CpTag;
using CjMethod = jclassfile::model::Method;

/// 任意属性（名字 + 体字节；长度由组装器落）。
struct RawAttr
{
    std::string name;
    Bytes       body;
};

struct CraftField
{
    std::string          name;
    std::string          desc;
    ca::u16              access = 0x0019;   // public static final
    std::vector<RawAttr> attrs;
};

struct CraftMethod
{
    std::string          name;
    std::string          desc;
    Bytes                code;
    ca::u16              access = 0x0009;   // public static
    std::vector<ExcSpec> exc;
    ca::u16              maxStack  = 1;
    ca::u16              maxLocals = 1;
    std::vector<RawAttr> codeAttrs;    // Code 内嵌套属性（LNT/LVT/LVTT 载体）
    std::vector<RawAttr> methodAttrs;  // Code 之外的方法属性
};

/// @brief 手工拼 classfile 的 craft 载体：支持字段/类属性、Code 内嵌属性、
/// MethodHandle / CONSTANT_Dynamic 等全部测试面 CP 条目。
///
/// methodHandle 的 kind 与 reference 索引由用例显式给定——允许构造 kind/tag
/// 不匹配的非法句柄（规则 4/5 的触发面）；condy 的 bsm 索引同理。
class CraftBuilder
{
public:
    explicit CraftBuilder(std::string thisClass)
        : thisClass_(std::move(thisClass))
    {
        thisIdx_  = cls(thisClass_);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
    }

    void addField(CraftField f) { fields_.push_back(std::move(f)); }
    void addMethod(CraftMethod m) { methods_.push_back(std::move(m)); }
    void addClassAttr(RawAttr a) { classAttrs_.push_back(std::move(a)); }

    Bytes assemble()
    {
        struct NameDesc
        {
            ca::u16 name;
            ca::u16 desc;
        };
        // 第一遍：全部 CP 条目（含各属性名 utf8）必须在写头部 cpCount 前物化——
        // cpCount 冻结后再物化会少算条目，整池错位。
        std::vector<NameDesc> fieldSlots;
        fieldSlots.reserve(fields_.size());
        for (const CraftField& f : fields_) {
            fieldSlots.push_back({utf8(f.name), utf8(f.desc)});
            for (const RawAttr& a : f.attrs) utf8(a.name);
        }
        std::vector<NameDesc> methodSlots;
        methodSlots.reserve(methods_.size());
        for (const CraftMethod& m : methods_) {
            methodSlots.push_back({utf8(m.name), utf8(m.desc)});
            for (const RawAttr& a : m.methodAttrs) utf8(a.name);
            for (const RawAttr& a : m.codeAttrs) utf8(a.name);
        }
        for (const RawAttr& a : classAttrs_) utf8(a.name);

        Bytes out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);   // V1_8
        putU16(out, next_);
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, 0x0021);
        putU16(out, thisIdx_);
        putU16(out, superIdx_);
        putU16(out, 0);   // interfaces
        putU16(out, static_cast<ca::u16>(fields_.size()));
        for (ca::usize i = 0; i < fields_.size(); ++i) {
            const CraftField& f = fields_[i];
            putU16(out, f.access);
            putU16(out, fieldSlots[i].name);
            putU16(out, fieldSlots[i].desc);
            putAttrs(out, f.attrs);
        }
        putU16(out, static_cast<ca::u16>(methods_.size()));
        for (ca::usize i = 0; i < methods_.size(); ++i) {
            const CraftMethod& m = methods_[i];
            putU16(out, m.access);
            putU16(out, methodSlots[i].name);
            putU16(out, methodSlots[i].desc);
            std::vector<RawAttr> methodAttributes = m.methodAttrs;
            methodAttributes.push_back(codeAttrOf(m));
            putAttrs(out, methodAttributes);
        }
        putAttrs(out, classAttrs_);
        return out;
    }

    // ---- CP 物化入口 ----

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

    ca::u16 fieldRef(std::string_view owner, std::string_view n, std::string_view d)
    {
        return memberRef(0x09, owner, n, d, fieldRefs_);
    }

    ca::u16 methodRef(std::string_view owner, std::string_view n, std::string_view d)
    {
        return memberRef(0x0a, owner, n, d, methodRefs_);
    }

    /// MethodHandle（tag 15）：kind 与 reference 索引显式给定，允许非法组合。
    ca::u16 methodHandle(ca::u8 kind, ca::u16 refIdx)
    {
        std::string key = "mh:" + std::to_string(kind) + ":" + std::to_string(refIdx);
        const auto  it = handles_.find(key);
        if (it != handles_.end()) return it->second;
        putU8(cp_, 0x0f);
        putU8(cp_, kind);
        putU16(cp_, refIdx);
        const ca::u16 idx = next_++;
        handles_.emplace(std::move(key), idx);
        return idx;
    }

    /// CONSTANT_Dynamic（tag 17）。
    ca::u16 condy(ca::u16 bsmIdx, ca::u16 ntIdx)
    {
        std::string key = "condy:" + std::to_string(bsmIdx) + ":" + std::to_string(ntIdx);
        const auto  it = condies_.find(key);
        if (it != condies_.end()) return it->second;
        putU8(cp_, 0x11);
        putU16(cp_, bsmIdx);
        putU16(cp_, ntIdx);
        const ca::u16 idx = next_++;
        condies_.emplace(std::move(key), idx);
        return idx;
    }

    // ---- 属性体片段（用例侧拼 RawAttr::body 用）----

    Bytes u16Body(ca::u16 v)
    {
        Bytes b;
        putU16(b, v);
        return b;
    }

    /// BootstrapMethods 属性体：entries = (handleIdx, args...) 列表。
    Bytes bsmBody(const std::vector<std::pair<ca::u16, std::vector<ca::u16>>>& entries)
    {
        Bytes b;
        putU16(b, static_cast<ca::u16>(entries.size()));
        for (const auto& [handle, args] : entries) {
            putU16(b, handle);
            putU16(b, static_cast<ca::u16>(args.size()));
            for (const ca::u16 arg : args) putU16(b, arg);
        }
        return b;
    }

    /// LineNumberTable 属性体：(startPc, line) 列表。
    Bytes lntBody(const std::vector<std::pair<ca::u16, ca::u16>>& entries)
    {
        Bytes b;
        putU16(b, static_cast<ca::u16>(entries.size()));
        for (const auto& [startPc, line] : entries) {
            putU16(b, startPc);
            putU16(b, line);
        }
        return b;
    }

    /// LVT/LVTT 属性体（同一布局）：(startPc, length, name, desc/sig, slot)。
    Bytes lvtBody(const std::vector<std::tuple<ca::u16, ca::u16, std::string_view,
                                               std::string_view, ca::u16>>& entries)
    {
        Bytes b;
        putU16(b, static_cast<ca::u16>(entries.size()));
        for (const auto& [startPc, length, name, desc, slot] : entries) {
            putU16(b, startPc);
            putU16(b, length);
            putU16(b, utf8(name));
            putU16(b, utf8(desc));
            putU16(b, slot);
        }
        return b;
    }

private:
    ca::u16 memberRef(ca::u8 tag, std::string_view owner, std::string_view n,
                      std::string_view d, std::unordered_map<std::string, ca::u16>& cache)
    {
        std::string key(std::string(owner).append("|").append(n).append("|").append(d));
        const auto  it = cache.find(key);
        if (it != cache.end()) return it->second;
        const ca::u16 ci = cls(owner);
        const ca::u16 nt = nameType(n, d);
        putU8(cp_, tag);
        putU16(cp_, ci);
        putU16(cp_, nt);
        const ca::u16 idx = next_++;
        cache.emplace(std::move(key), idx);
        return idx;
    }

    Bytes attrBytes(const RawAttr& a)
    {
        Bytes b;
        putU16(b, utf8(a.name));
        putU32(b, static_cast<ca::u32>(a.body.size()));
        b.insert(b.end(), a.body.begin(), a.body.end());
        return b;
    }

    void putAttrs(Bytes& out, const std::vector<RawAttr>& attrs)
    {
        putU16(out, static_cast<ca::u16>(attrs.size()));
        for (const RawAttr& a : attrs) appendBytes(out, attrBytes(a));
    }

    RawAttr codeAttrOf(const CraftMethod& m)
    {
        Bytes body;
        putU16(body, m.maxStack);
        putU16(body, m.maxLocals);
        putU32(body, static_cast<ca::u32>(m.code.size()));
        body.insert(body.end(), m.code.begin(), m.code.end());
        putU16(body, static_cast<ca::u16>(m.exc.size()));
        for (const ExcSpec& e : m.exc) {
            putU16(body, e.startPc);
            putU16(body, e.endPc);
            putU16(body, e.handlerPc);
            putU16(body, e.catchCp);
        }
        putAttrs(body, m.codeAttrs);
        return RawAttr{"Code", std::move(body)};
    }

    Bytes   cp_;
    ca::u16 next_ = 1;
    std::string thisClass_;
    std::unordered_map<std::string, ca::u16> utf8s_;
    std::unordered_map<std::string, ca::u16> classes_;
    std::unordered_map<std::string, ca::u16> nameTypes_;
    std::unordered_map<std::string, ca::u16> strs_;
    std::unordered_map<std::string, ca::u16> ints_;
    std::unordered_map<std::string, ca::u16> fieldRefs_;
    std::unordered_map<std::string, ca::u16> methodRefs_;
    std::unordered_map<std::string, ca::u16> handles_;
    std::unordered_map<std::string, ca::u16> condies_;
    ca::u16 thisIdx_  = 0;
    ca::u16 superIdx_ = 0;
    ca::u16 codeName_ = 0;
    std::vector<CraftField>  fields_;
    std::vector<CraftMethod> methods_;
    std::vector<RawAttr>     classAttrs_;
};

/// 跑清洗 pass，返回 PA。
mjbase::spi::PreservedAnalyses runCleaner(mjbase::model::ResourceContainer& container)
{
    mjbase::pass::IllegalClassFileCleaner pass;
    ca::str::Utf8StringArena              arena;
    mjbase::spi::GlobalAnalysisManager    gam(arena);
    return pass.run(container, gam);
}

/// 清洗后取回资源 bytes。
const Bytes& cleanedBytes(mjbase::model::ResourceContainer& container, const char* uri)
{
    const auto* res = ca::core::tag_cast::cast<const mjbase::model::ClassResource>(
        container.getResource(uri));
    EXPECT_NE(res, nullptr);
    return res->get();
}

/// libjclassfile 模型重读（模型级断言用）；读失败返回 null。
std::unique_ptr<ClassFile> rereadModel(const Bytes& bytes)
{
    try {
        return std::make_unique<ClassFile>(jclassfile::io::ClassFileReader().read(bytes));
    }
    catch (...) {
        return nullptr;
    }
}

const Attribute* findAttrByName(const std::vector<const Attribute*>& attrs, std::string_view name)
{
    for (const Attribute* attr : attrs) {
        if (attr != nullptr && attr->name().text() == name) return attr;
    }
    return nullptr;
}

const CodeAttribute* findCodeOf(const ClassFile& model, std::string_view methodName)
{
    for (const CjMethod* method : model.getMethods()) {
        if (method->name() == nullptr || method->name()->text() != methodName) continue;
        return static_cast<const CodeAttribute*>(
            findAttrByName(method->attributes(), AttributeConstants::CODE));
    }
    return nullptr;
}

ca::usize countPoolTag(const ClassFile& model, CpTag tag)
{
    ca::usize count = 0;
    model.getPool().iterator().forEach([&](int, const jclassfile::model::CpEntry* entry) {
        if (entry != nullptr && entry->tag() == tag) ++count;
    });
    return count;
}

}   // namespace

// 规则 1：属性级 CP 校验剔除——ConstantValue 引用的条目种类与字段类型不符
// （int 字段挂 Class 条目）→ 该属性剔除，字段本体保留。
TEST(MjPassIllegalClassFileCleanerTest, DropsConstantValueWithWrongCpKind)
{
    CraftBuilder builder("T");
    CraftField   bad;
    bad.name  = "cvBad";
    bad.desc  = "I";
    bad.attrs.push_back({"ConstantValue", builder.u16Body(builder.cls("java/lang/String"))});
    builder.addField(bad);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "wrong-kind ConstantValue must be stripped";
    EXPECT_FALSE(pa.areAllPreserved());

    const Bytes& output = cleanedBytes(container, "T.class");
    auto             model = rereadModel(output);
    ASSERT_NE(model, nullptr) << "cleaned bytes must stay readable by libjclassfile";
    auto read = jbc::core::readClass(jbc::core::ByteView(output));
    ASSERT_TRUE(read.is_ok()) << "cleaned bytes must stay parseable by libjbc";
    ASSERT_EQ(model->getFields().size(), 1u);
    EXPECT_EQ(model->getFields()[0]->name()->text(), "cvBad");
    EXPECT_EQ(model->getFields()[0]->attributes().size(), 0u)
        << "ConstantValue with wrong CP kind must be dropped";
}

// 规则 1 正面控制：合法 ConstantValue（int 字段挂 Integer 条目）→ 字节逐位不动。
TEST(MjPassIllegalClassFileCleanerTest, KeepsValidConstantValueUntouched)
{
    CraftBuilder builder("T");
    CraftField   good;
    good.name = "cvGood";
    good.desc = "I";
    good.attrs.push_back({"ConstantValue", builder.u16Body(builder.intConst(42))});
    builder.addField(good);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.areAllPreserved()) << "valid ConstantValue must not trigger re-encode";
    const Bytes& output = cleanedBytes(container, "T.class");
    if (output != bytes) {
        ca::usize diff = 0;
        while (diff < output.size() && diff < bytes.size() && output[diff] == bytes[diff]) ++diff;
        ADD_FAILURE() << "bytes diverge at offset " << diff << " (in=" << bytes.size()
                      << " out=" << output.size() << ")";
    }
    EXPECT_EQ(output, bytes);
}

// 规则 2a：越界跳转（GOTO 目标越过代码体）→ RETURN 桩替换，代码长度不变。
TEST(MjPassIllegalClassFileCleanerTest, StubsOutOfBoundsJumpWithReturn)
{
    CraftBuilder builder("T");
    CraftMethod  m;
    m.name = "j";
    m.desc = "()V";
    m.code = {0xA7, 0x00, 0x64, 0xB1};   // GOTO +100（越界）; RETURN
    builder.addMethod(m);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "out-of-bounds jump must be stubbed";

    const ParsedCode parsed = parsedCodeOf(cleanedBytes(container, "T.class"), 0);
    ASSERT_NE(parsed.code, nullptr);
    ASSERT_EQ(parsed.code->code.size(), 4u);
    // GOTO(3B) → NOP NOP RETURN；原 RETURN 保留。
    EXPECT_EQ(parsed.code->code, (Bytes{0x00, 0x00, 0xB1, 0xB1}));
}

// 规则 2a 正面控制：界内 GOTO 不触发，字节逐位不动。
TEST(MjPassIllegalClassFileCleanerTest, KeepsInBoundsJumpUntouched)
{
    CraftBuilder builder("T");
    CraftMethod  m;
    m.name = "j";
    m.desc = "()V";
    m.code = {0xA7, 0x00, 0x03, 0x00, 0xB1};   // GOTO +3（指向界内 NOP）; NOP; RETURN
    builder.addMethod(m);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.areAllPreserved()) << "in-bounds jump must not trigger re-encode";
    EXPECT_EQ(cleanedBytes(container, "T.class"), bytes);
}

// 规则 2b：LDC 族与常量种类不符（LDC2_W 指向 Integer）与悬垂索引（LDC 指向
// 越界 CP 槽）→ 一律 RETURN 桩替换。
TEST(MjPassIllegalClassFileCleanerTest, StubsIllegalLdcInstructions)
{
    CraftBuilder builder("T");
    const ca::u16 intIdx = builder.intConst(7);
    CraftMethod   wide;
    wide.name = "w";
    wide.desc = "()V";
    wide.code = {0x14, static_cast<ca::u8>(intIdx >> 8), static_cast<ca::u8>(intIdx),
                 0xB1};   // LDC2_W(0x14) <Integer>（cat2 槽挂 cat1 条目）; RETURN
    builder.addMethod(wide);

    CraftMethod dangling;
    dangling.name = "d";
    dangling.desc = "()V";
    dangling.code = {0x12, 0x7F, 0xB1};   // LDC 127（越出池界 → null entry）; RETURN
    builder.addMethod(dangling);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "illegal LDC forms must be stubbed";

    const ParsedCode wideParsed = parsedCodeOf(cleanedBytes(container, "T.class"), 0);
    ASSERT_NE(wideParsed.code, nullptr);
    // LDC2_W(3B) → NOP NOP RETURN；原 RETURN 保留。
    EXPECT_EQ(wideParsed.code->code, (Bytes{0x00, 0x00, 0xB1, 0xB1}));

    const ParsedCode danglingParsed = parsedCodeOf(cleanedBytes(container, "T.class"), 1);
    ASSERT_NE(danglingParsed.code, nullptr);
    // LDC(2B) → NOP RETURN；原 RETURN 保留。
    EXPECT_EQ(danglingParsed.code->code, (Bytes{0x00, 0xB1, 0xB1}));
}

// 规则 3：LVT/LVTT/LineNumberTable 越界/悬垂条目修剪，合法条目保留。
TEST(MjPassIllegalClassFileCleanerTest, PrunesLvtLvttLntEntries)
{
    CraftBuilder builder("T");
    CraftMethod  m;
    m.name      = "d";
    m.desc      = "()V";
    m.code      = nopReturn();   // code_length = 2, maxLocals = 1
    m.codeAttrs.push_back({"LineNumberTable",
                           builder.lntBody({{0, 10}, {5, 20}})});   // (5,20) 越界
    m.codeAttrs.push_back({"LocalVariableTable",
                           builder.lvtBody({{0, 2, "a", "I", 0},      // 合法
                                            {0, 2, "b", "J", 9},      // slot >= maxLocals
                                            {5, 1, "c", "I", 0}})});  // 区间越界
    m.codeAttrs.push_back({"LocalVariableTypeTable",
                           builder.lvtBody({{0, 2, "s", "Ljava/util/List;", 0},   // 合法
                                            {0, 2, "t", "J", 9}})});              // wide 且 slot 越界
    builder.addMethod(m);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "out-of-bounds debug-table entries must be pruned";

    auto model = rereadModel(cleanedBytes(container, "T.class"));
    ASSERT_NE(model, nullptr);
    auto read = jbc::core::readClass(jbc::core::ByteView(cleanedBytes(container, "T.class")));
    ASSERT_TRUE(read.is_ok()) << "cleaned bytes must stay parseable";
    const CodeAttribute* code = findCodeOf(*model, "d");
    ASSERT_NE(code, nullptr);

    const auto* lnt = static_cast<const jclassfile::model::LineNumberTableAttribute*>(
        findAttrByName(code->attributes(), AttributeConstants::LINE_NUMBER_TABLE));
    ASSERT_NE(lnt, nullptr);
    ASSERT_EQ(lnt->entries().size(), 1u);
    EXPECT_EQ(lnt->entries()[0].startPc(), 0);

    const auto* lvt = static_cast<const jclassfile::model::LocalVariableTableAttribute*>(
        findAttrByName(code->attributes(), AttributeConstants::LOCAL_VARIABLE_TABLE));
    ASSERT_NE(lvt, nullptr);
    ASSERT_EQ(lvt->entries().size(), 1u);
    EXPECT_EQ(lvt->entries()[0].index(), 0);

    const auto* lvtt = static_cast<const jclassfile::model::LocalVariableTypeTableAttribute*>(
        findAttrByName(code->attributes(), AttributeConstants::LOCAL_VARIABLE_TYPE_TABLE));
    ASSERT_NE(lvtt, nullptr);
    ASSERT_EQ(lvtt->entries().size(), 1u);
    EXPECT_EQ(lvtt->entries()[0].index(), 0);
}

// 规则 4（级联形态）：LDC 引用指向无效 BSM 条目的 condy → LDC 桩替；该 condy
// 不再被任何代码引用 → BootstrapMethods 整表移除 + 池内无主 condy 出池 +
// 无效句柄出池。
TEST(MjPassIllegalClassFileCleanerTest, CompactsBsmAndDropsCondyChain)
{
    CraftBuilder builder("T");
    // 非法句柄：REF_INVOKE_INTERFACE（要求 InterfaceMethodRef）挂在 Methodref 上。
    const ca::u16 mhBad  = builder.methodHandle(9, builder.methodRef("B", "bsm", "()V"));
    const ca::u16 mhGood = builder.methodHandle(6, builder.methodRef("B", "bsm", "()V"));
    const ca::u16 nt     = builder.nameType("x", "I");
    const ca::u16 str    = builder.strConst("A");
    const ca::u16 condyBad = builder.condy(0, nt);   // 指向 BSM 条目 0（非法）
    builder.addClassAttr(
        {"BootstrapMethods",
         builder.bsmBody({{mhBad, {str}}, {mhGood, {str}}})});

    CraftMethod m;
    m.name = "c";
    m.desc = "()V";
    m.code = {0x12, static_cast<ca::u8>(condyBad), 0xB1};   // LDC <condy>; RETURN
    builder.addMethod(m);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "condy chain over invalid BSM must be cleaned";

    auto model = rereadModel(cleanedBytes(container, "T.class"));
    ASSERT_NE(model, nullptr);
    auto read = jbc::core::readClass(jbc::core::ByteView(cleanedBytes(container, "T.class")));
    ASSERT_TRUE(read.is_ok()) << "cleaned bytes must stay parseable";

    EXPECT_EQ(findAttrByName(model->attributes(), AttributeConstants::BOOTSTRAP_METHODS),
              nullptr)
        << "BSM table must be removed once no code references a dynamic constant";
    EXPECT_EQ(countPoolTag(*model, CpTag::DYNAMIC), 0u) << "orphan condy must leave the pool";
    EXPECT_EQ(countPoolTag(*model, CpTag::METHOD_HANDLE), 1u)
        << "invalid handle must leave the pool, valid one stays";

    const CodeAttribute* code = findCodeOf(*model, "c");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->instructions().size(), 3u);
    EXPECT_EQ(code->instructions()[0]->getOpcode(), 0x00) << "LDC must be stubbed to NOP";
    EXPECT_EQ(code->instructions()[1]->getOpcode(), 0xB1);
    EXPECT_EQ(code->instructions()[2]->getOpcode(), 0xB1);
}

// 规则 4（压缩+重映射形态）：condy 指向合法 BSM 条目、表内另有非法条目 →
// 非法条目剔除、表压缩、condy 的 bootstrap 索引重映射后仍指合法条目，LDC 保留。
TEST(MjPassIllegalClassFileCleanerTest, RemapsCondyAfterBsmCompaction)
{
    CraftBuilder builder("T");
    const ca::u16 mhGood = builder.methodHandle(6, builder.methodRef("B", "bsm", "()V"));
    const ca::u16 mhBad  = builder.methodHandle(9, builder.methodRef("B", "bsm", "()V"));
    const ca::u16 nt     = builder.nameType("x", "I");
    const ca::u16 str    = builder.strConst("A");
    const ca::u16 condy   = builder.condy(0, nt);   // 指向 BSM 条目 0（合法）
    builder.addClassAttr(
        {"BootstrapMethods",
         builder.bsmBody({{mhGood, {str}}, {mhBad, {str}}})});

    CraftMethod m;
    m.name = "c";
    m.desc = "()V";
    m.code = {0x12, static_cast<ca::u8>(condy), 0xB1};   // LDC <condy>; RETURN
    builder.addMethod(m);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "invalid BSM entry must compact the table";


    auto model = rereadModel(cleanedBytes(container, "T.class"));
    ASSERT_NE(model, nullptr);
    auto read = jbc::core::readClass(jbc::core::ByteView(cleanedBytes(container, "T.class")));
    ASSERT_TRUE(read.is_ok()) << "cleaned bytes must stay parseable";

    const auto* bsm = static_cast<const jclassfile::model::BootstrapMethodsAttribute*>(
        findAttrByName(model->attributes(), AttributeConstants::BOOTSTRAP_METHODS));
    ASSERT_NE(bsm, nullptr) << "BSM table must survive: a live condy still references it";
    ASSERT_EQ(bsm->bootstrapMethods().size(), 1u);
    ASSERT_NE(bsm->bootstrapMethods()[0].bsmMethodRef(), nullptr);
    EXPECT_EQ(bsm->bootstrapMethods()[0].bsmMethodRef()->kind(),
              jclassfile::model::CpMethodHandle::REF_INVOKE_STATIC);
    EXPECT_EQ(countPoolTag(*model, CpTag::DYNAMIC), 1u) << "live condy must stay";
    EXPECT_EQ(countPoolTag(*model, CpTag::METHOD_HANDLE), 1u) << "only the valid handle stays";

    // 重映射后 condy 的 bootstrap 索引仍指向压缩表的合法条目。
    const jclassfile::model::CpEntry* poolCondy = nullptr;
    model->getPool().iterator().forEach([&](int, const jclassfile::model::CpEntry* entry) {
        if (entry != nullptr && entry->isConstDynamic()) poolCondy = entry;
    });
    ASSERT_NE(poolCondy, nullptr);
    EXPECT_EQ(static_cast<const jclassfile::model::ConstDynamic*>(poolCondy)->bsmIndex(), 0u);

    const CodeAttribute* code = findCodeOf(*model, "c");
    ASSERT_NE(code, nullptr);
    ASSERT_FALSE(code->instructions().empty());
    EXPECT_EQ(code->instructions()[0]->getOpcode(), 0x12) << "valid LDC must survive";
}

// 规则 5：无主且 reference kind 与被引条目不符的 MethodHandle → 出池。
TEST(MjPassIllegalClassFileCleanerTest, RemovesInvalidMethodHandleFromPool)
{
    CraftBuilder builder("T");
    // REF_INVOKE_VIRTUAL（要求 Methodref）挂在 Fieldref 上 → 非法句柄。
    const ca::u16 mhBad =
        builder.methodHandle(static_cast<ca::u8>(5), builder.fieldRef("B", "f", "I"));
    (void)mhBad;   // 池中无主条目：不被任何结构引用。
    CraftMethod m;
    m.name = "m";
    m.desc = "()V";
    m.code = nopReturn();
    builder.addMethod(m);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "invalid method handle must leave the pool";

    auto model = rereadModel(cleanedBytes(container, "T.class"));
    ASSERT_NE(model, nullptr);
    auto read = jbc::core::readClass(jbc::core::ByteView(cleanedBytes(container, "T.class")));
    ASSERT_TRUE(read.is_ok()) << "cleaned bytes must stay parseable";
    EXPECT_EQ(countPoolTag(*model, CpTag::METHOD_HANDLE), 0u);
}

// 规则 5 正面控制：合法的无主 MethodHandle 保留，字节逐位不动。
TEST(MjPassIllegalClassFileCleanerTest, KeepsValidFloatingMethodHandleUntouched)
{
    CraftBuilder builder("T");
    const ca::u16 mhGood =
        builder.methodHandle(static_cast<ca::u8>(6), builder.methodRef("B", "bsm", "()V"));
    (void)mhGood;
    CraftMethod m;
    m.name = "m";
    m.desc = "()V";
    m.code = nopReturn();
    builder.addMethod(m);

    auto bytes = builder.assemble();
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.areAllPreserved()) << "valid floating handle must not trigger re-encode";
    EXPECT_EQ(cleanedBytes(container, "T.class"), bytes);
}
