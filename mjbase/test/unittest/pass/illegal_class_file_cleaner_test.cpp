/// IllegalClassFileCleaner 单元测试：清洗器判据验收 + 补齐独立直接用例。
/// 断言口径：不可解析字节原样保留；可解析资源清洗后仍可解析；改动走两阶段
/// 提交；PA 与实际改动一致。
/// F 线清洗批起规则面为两级流水全量：libjclassfile 前置级 5 条挂账规则
/// （属性级 CP 校验剔除 / 越界跳转与 LDC 桩替 / LVT·LVTT·LNT 修剪 / BSM 压缩
/// 与 condy 手术 / 无效句柄出池）+ libjbc 级 2 条结构规则；craft 畸形样本
/// 逐规则验收 + 合法输入字节逐位不动断言。
///
/// 夹具纪律：合法基座（含正面控制组）一律 jasm 文本装配；本件的被测形态
/// 是「清洗器吃下的非法类」，判据落法：
/// - 例外①（装配器可判的非法形）：装配合法基座 → mutateClassFile DOM 单点
///   偏差（越界跳转偏移 / LDC2_W 槽类别错挂 / LDC 悬垂索引），改写前后
///   relaxed validate 自检 Ok→Fail 内置。
/// - 例外①申报（前置级规则面，validate 语义未覆盖）：异常表越界/空区间、
///   成员描述符残缺、BSM/句柄/condy 深层 CP 形——偏差后不做 Fail 断言
///  （relaxed validate 不含此三门），以清洗 pass 实跑为最终证明。
/// - 例外②（jasm 无属性指令面）：ConstantValue / BootstrapMethods /
///   LNT·LVT·LVTT 载荷经 CP/AttrSlot DOM 挂 Raw 槽（载荷 putU16 拼装属
///   属性载荷构建而非类构造）。
/// 原 CraftBuilder 手拼载体随字节级 builder 一并退役。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
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
#include "shared/jasm_fixture.hpp"

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

// ── jasm 合法基座便捷形 ─────────────────────────────────────────────

/// 单方法类装配（public super 骨架，超 java/lang/Object）。
Bytes asmOne(const std::string& cls, std::string_view mods, std::string_view name,
             std::string_view desc, std::string_view body)
{
    const std::string src = R"JASM(.super java/lang/Object
.class public super )JASM" + cls + R"JASM( {
  .method )JASM" + std::string(mods) + " " + std::string(name) + " " + std::string(desc)
        + R"JASM( {
    code: {
)JASM" + std::string(body) + R"JASM(    }
  }
}
)JASM";
    return assembleJasm(src);
}

/// 字段声明 + 方法集装配（正面控制组与多成员基座用）。
Bytes asmT(const std::string& cls, const std::vector<std::string>& fields,
           const std::vector<std::tuple<std::string_view, std::string_view, std::string_view,
                                        std::string_view>>& ms)
{
    std::string src = ".super java/lang/Object\n.class public super ";
    src.append(cls).append(" {\n");
    for (const std::string& f : fields) src += "  .field " + f + "\n";
    for (const auto& [mods, name, desc, body] : ms) {
        src += "  .method ";
        src.append(mods).append(" ").append(name).append(" ").append(desc);
        src += " {\n    code: {\n";
        src.append(body);
        src += "    }\n  }\n";
    }
    src += "}\n";
    return assembleJasm(src);
}

// ── DOM 单点偏差面（例外①载体）────────────────────────────────────

/// 把首方法 Code 里第一条 2 字节偏移分支（opcode）的操作数改写为 off。
/// 前后 relaxed validate 自检内置：改写前 Ok（装配基座），改写后 Fail
///（越界分支落点为验证级非法形）。
Bytes rewriteBranchOffset(const Bytes& bytes, ca::u8 op, ca::u16 off)
{
    expectValidateOk(bytes);
    Bytes deviated = mutateClassFile(bytes, "rewriteBranchOffset",
                                     [&](jbc::core::ClassFile& cf) {
        for (jbc::core::MemberInfo& m : cf.methods) {
            for (jbc::core::AttrSlot& slot : m.attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
                if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return;
                auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
                if (code == nullptr) return;
                for (ca::u32 pc = 0; pc + 2 < code->code.size(); ++pc) {
                    if (code->code[pc] != op) continue;
                    code->code[pc + 1] = static_cast<ca::u8>(off >> 8);
                    code->code[pc + 2] = static_cast<ca::u8>(off);
                    return;
                }
            }
        }
    });
    EXPECT_NE(deviated.size(), 0u) << "rewriteBranchOffset: 未找到目标分支";
    if (deviated.empty()) return {};
    expectValidateFail(deviated);
    return deviated;
}

/// 把首方法 Code 里第一条 ldc/ldc2_w（op）的 CP 操作数改写为 newIdx。
/// 改写后 expectValidateFail（槽类别错挂/悬垂索引为验证级非法形）。
Bytes rewriteLdcOperand(const Bytes& bytes, ca::u8 op, ca::u16 newIdx)
{
    expectValidateOk(bytes);
    const bool wide = op == 0x14;   // LDC2_W 带 2 字节索引
    Bytes deviated = mutateClassFile(bytes, "rewriteLdcOperand",
                                     [&](jbc::core::ClassFile& cf) {
        for (jbc::core::MemberInfo& m : cf.methods) {
            for (jbc::core::AttrSlot& slot : m.attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
                if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return;
                auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
                if (code == nullptr) return;
                for (ca::u32 pc = 0; pc < code->code.size(); ++pc) {
                    if (code->code[pc] != op) continue;
                    if (wide) {
                        code->code[pc + 1] = static_cast<ca::u8>(newIdx >> 8);
                        code->code[pc + 2] = static_cast<ca::u8>(newIdx);
                    } else {
                        code->code[pc + 1] = static_cast<ca::u8>(newIdx);
                    }
                    return;
                }
            }
        }
    });
    EXPECT_NE(deviated.size(), 0u) << "rewriteLdcOperand: 未找到目标 LDC";
    if (deviated.empty()) return {};
    expectValidateFail(deviated);
    return deviated;
}

// ── 属性载荷构建（例外②；putU16 属载荷拼装而非类构造）─────────────

/// BootstrapMethods 属性体：entries = (handleIdx, args...) 列表。
Bytes bsmBody(const std::vector<std::pair<ca::u16, std::vector<ca::u16>>>& entries)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>(entries.size() >> 8));
    b.push_back(static_cast<ca::u8>(entries.size()));
    for (const auto& [handle, args] : entries) {
        b.push_back(static_cast<ca::u8>(handle >> 8));
        b.push_back(static_cast<ca::u8>(handle));
        b.push_back(static_cast<ca::u8>(args.size() >> 8));
        b.push_back(static_cast<ca::u8>(args.size()));
        for (const ca::u16 arg : args) {
            b.push_back(static_cast<ca::u8>(arg >> 8));
            b.push_back(static_cast<ca::u8>(arg));
        }
    }
    return b;
}

/// LineNumberTable 属性体：(startPc, line) 列表。
Bytes lntBody(const std::vector<std::pair<ca::u16, ca::u16>>& entries)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>(entries.size() >> 8));
    b.push_back(static_cast<ca::u8>(entries.size()));
    for (const auto& [startPc, line] : entries) {
        b.push_back(static_cast<ca::u8>(startPc >> 8));
        b.push_back(static_cast<ca::u8>(startPc));
        b.push_back(static_cast<ca::u8>(line >> 8));
        b.push_back(static_cast<ca::u8>(line));
    }
    return b;
}

/// LVT/LVTT 属性体（同一布局）：(startPc, length, name, desc/sig, slot)。
Bytes lvtBody(const std::vector<std::tuple<ca::u16, ca::u16, std::string_view,
                                           std::string_view, ca::u16>>& entries,
              jbc::core::ClassFile& scratch)
{
    auto put16 = [](Bytes& out, ca::u16 v) {
        out.push_back(static_cast<ca::u8>(v >> 8));
        out.push_back(static_cast<ca::u8>(v));
    };
    auto putUtf8Idx = [&scratch](Bytes& out, std::string_view s) {
        const ca::u32 idx = scratch.cp.addUtf8(s);
        out.push_back(static_cast<ca::u8>(idx >> 8));
        out.push_back(static_cast<ca::u8>(idx));
    };
    Bytes b;
    put16(b, static_cast<ca::u16>(entries.size()));
    for (const auto& [startPc, length, name, desc, slot] : entries) {
        put16(b, startPc);
        put16(b, length);
        putUtf8Idx(b, name);
        putUtf8Idx(b, desc);
        put16(b, slot);
    }
    return b;
}

/// 向类/方法/Code 内嵌属性表挂 Raw 新槽（例外②附着面）。
void attachRawAttr(jbc::core::ClassFile& cf, std::vector<jbc::core::AttrSlot>& attrs,
                   std::string_view name, Bytes body)
{
    attrs.push_back(jbc::core::AttrSlot::makeNew(
        cf.cp.addUtf8(name), std::make_unique<jbc::core::RawAttr>(std::move(body))));
}

}   // namespace

// 用例：3 字节垃圾不可解析 → PA all()、原字节原样保留。
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
// 例外①申报面：描述符残缺（"HI" / "(V)V"）为前置级规则面——装配器词表不收，
// 从装配合法四成员基座出发经 DOM 单点改写 descCp（addUtf8 新条目不共池），
// 偏差后不做 Fail 断言（relaxed validate 不判描述符残缺），清洗实跑为最终证明。
TEST(MjPassIllegalClassFileCleanerTest, StripsIllegalDescriptorMembers)
{
    const Bytes base = asmT("T", {"goodField I", "badField I"},
        {
            {"public static", "goodMethod", "()V", "      return\n"},
            {"public static", "badMethod", "()V", "      return\n"},
        });

    Bytes bytes = mutateClassFile(base, "illegalDescriptors",
                                  [&](jbc::core::ClassFile& cf) {
        for (jbc::core::MemberInfo& f : cf.fields) {
            if (cf.utf8(f.nameCp) == "badField") f.descCp = cf.cp.addUtf8("HI");
        }
        for (jbc::core::MemberInfo& m : cf.methods) {
            if (cf.utf8(m.nameCp) == "badMethod") m.descCp = cf.cp.addUtf8("(V)V");
        }
    });
    ASSERT_NE(bytes.size(), 0u);

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
// 合法条目取 (0,1,1)——前置级语义以「最后一条指令起始偏移」为上界，夹具取
// 两边界语义下都合法的形态。例外①申报面：越界/空区间条目为前置级规则面
//（validate 不判），合法条目 jasm 表达（{L0, L1, L1, *}=pc(0,1,1)），垃圾
// 条目经 DOM 追加，清洗实跑为最终证明。
TEST(MjPassIllegalClassFileCleanerTest, PrunesJunkExceptionTableEntries)
{
    const std::string src = R"JASM(.super java/lang/Object
.class public super T {
  .method public static t ()V {
    exceptions: {
      { L0, L1, L1, * }
    }
    code: {
      L0:
      nop
      L1:
      return
    }
  }
}
)JASM";
    const Bytes base = assembleJasm(src);
    ASSERT_NE(base.size(), 0u);
    expectValidateOk(base);

    // DOM 追加垃圾条目：(5,7,1) 全越界剪除；(1,1,1) 空区间剪除。
    Bytes bytes = mutateClassFile(base, "junkExcEntries", [&](jbc::core::ClassFile& cf) {
        for (jbc::core::MemberInfo& m : cf.methods) {
            for (jbc::core::AttrSlot& slot : m.attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
                if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return;
                auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
                if (code == nullptr) return;
                code->excTable.push_back({5, 7, 1, 0});
                code->excTable.push_back({1, 1, 1, 0});
            }
        }
    });
    ASSERT_NE(bytes.size(), 0u);

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
    const Bytes bytes = asmT("T", {"f I"},
        {
            {"public static", "t", "()V", "      nop\n      return\n"},
            {"public static", "u", "()V", "      return\n"},
        });
    ASSERT_NE(bytes.size(), 0u);

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

// 用例（proguard 真实样本冒烟）：逐资源断言「字节未动或改后仍可解析」
// 与 PA 一致性。changed>0 断言绑全量 IllegalStrippingTransformer 规则集
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
// craft 畸形样本逐规则验收 + 合法输入字节不动。合法基座 jasm 装配；非法形
// 经 mutateClassFile DOM 单点偏差（CP 物化走 libjbc add* 追加面），产物级
// 断言经 libjclassfile 模型重读 + libjbc 可重读双面验收。

namespace {

using jclassfile::model::Attribute;
using jclassfile::model::AttributeConstants;
using jclassfile::model::ClassFile;
using jclassfile::model::CodeAttribute;
using jclassfile::model::CpTag;
using CjMethod = jclassfile::model::Method;

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

/// 首方法 Code 的可变视图（mutateClassFile 变换器用；未找到为 nullptr）。
jbc::core::CodeAttr* mutableFirstCode(jbc::core::ClassFile& cf)
{
    for (jbc::core::MemberInfo& m : cf.methods) {
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
            if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return nullptr;
            return ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
        }
    }
    return nullptr;
}

}   // namespace

// 规则 1：属性级 CP 校验剔除——ConstantValue 引用的条目种类与字段类型不符
// （int 字段挂 Class 条目）→ 该属性剔除，字段本体保留。
// 例外②：ConstantValue 经 DOM 挂 Raw 槽（u2 CP 指针载荷），错挂条目为
// Class 条目（addClass 物化）。
TEST(MjPassIllegalClassFileCleanerTest, DropsConstantValueWithWrongCpKind)
{
    const Bytes base = asmT("T", {"cvGood I"}, {});   // 占位，deviate 时改字段名
    ASSERT_NE(base.size(), 0u);
    expectValidateOk(base);
    Bytes bytes = mutateClassFile(base, "wrongKindConstantValue",
                                  [&](jbc::core::ClassFile& cf) {
        cf.fields[0].nameCp = cf.cp.addUtf8("cvBad");
        const ca::u32 clsCp = cf.cp.addClass(cf.cp.addUtf8("java/lang/String"));
        attachRawAttr(cf, cf.fields[0].attrs, "ConstantValue",
                      Bytes{static_cast<ca::u8>(clsCp >> 8), static_cast<ca::u8>(clsCp)});
    });
    ASSERT_NE(bytes.size(), 0u);

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
// 例外②：合法 ConstantValue 同面 DOM 挂槽（Integer 条目 addInteger 物化）。
TEST(MjPassIllegalClassFileCleanerTest, KeepsValidConstantValueUntouched)
{
    const Bytes base = asmT("T", {"cvGood I"}, {});
    ASSERT_NE(base.size(), 0u);
    expectValidateOk(base);
    const Bytes bytes = mutateClassFile(base, "validConstantValue",
                                        [&](jbc::core::ClassFile& cf) {
        const ca::u32 intCp = cf.cp.addInteger(42);
        attachRawAttr(cf, cf.fields[0].attrs, "ConstantValue",
                      Bytes{static_cast<ca::u8>(intCp >> 8), static_cast<ca::u8>(intCp)});
    });
    ASSERT_NE(bytes.size(), 0u);

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
// 例外①：合法基座 goto Lend（偏移 +3），DOM 单点改写偏移为 +100（越界），
// 前后 validate Ok→Fail 自检内置。
TEST(MjPassIllegalClassFileCleanerTest, StubsOutOfBoundsJumpWithReturn)
{
    const Bytes base = asmOne("T", "public static", "j", "()V",
                              "      goto Lend\n      Lend:\n      return\n");
    ASSERT_NE(base.size(), 0u);
    const Bytes bytes = rewriteBranchOffset(base, 0xA7, 100);
    ASSERT_NE(bytes.size(), 0u);

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

// 规则 2a 正面控制：界内 GOTO 不触发，字节逐位不动。纯 jasm 表达
//（goto Lskip 落 pc3 NOP，偏移恰 +3）。
TEST(MjPassIllegalClassFileCleanerTest, KeepsInBoundsJumpUntouched)
{
    const Bytes bytes = asmOne("T", "public static", "j", "()V",
                               "      goto Lskip\n      nop\n      Lskip:\n      return\n");
    ASSERT_NE(bytes.size(), 0u);
    expectValidateOk(bytes);

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.areAllPreserved()) << "in-bounds jump must not trigger re-encode";
    EXPECT_EQ(cleanedBytes(container, "T.class"), bytes);
}

// 规则 2b：LDC 族与常量种类不符（LDC2_W 指向 Integer）与悬垂索引（LDC 指向
// 越界 CP 槽）→ 一律 RETURN 桩替换。
// 例外①：合法基座 ldc2_w 1L / ldc 7，DOM 单点改写 CP 操作数（Integer 条目
// / 越界槽 127），前后 validate Ok→Fail 自检内置。
TEST(MjPassIllegalClassFileCleanerTest, StubsIllegalLdcInstructions)
{
    // wide：LDC2_W 挂 Integer 条目（cat2 槽挂 cat1 条目）。
    ca::u16 intIdx = 0;
    const Bytes wideBase = mutateClassFile(
        asmOne("T", "public static", "w", "()V", "      ldc2_w 1L\n      return\n"),
        "internInt", [&](jbc::core::ClassFile& cf) {
            intIdx = static_cast<ca::u16>(cf.cp.addInteger(7));
        });
    ASSERT_NE(wideBase.size(), 0u);
    const Bytes wide = rewriteLdcOperand(wideBase, 0x14, intIdx);
    ASSERT_NE(wide.size(), 0u);

    // dangling：LDC 指向越出池界的槽 127。
    const Bytes dangling =
        rewriteLdcOperand(asmOne("T", "public static", "d", "()V", "      ldc 7\n      return\n"),
                          0x12, 127);
    ASSERT_NE(dangling.size(), 0u);

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", wide);
    addClassResource(container, "Td.class", dangling);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "illegal LDC forms must be stubbed";

    const ParsedCode wideParsed = parsedCodeOf(cleanedBytes(container, "T.class"), 0);
    ASSERT_NE(wideParsed.code, nullptr);
    // LDC2_W(3B) → NOP NOP RETURN；原 RETURN 保留。
    EXPECT_EQ(wideParsed.code->code, (Bytes{0x00, 0x00, 0xB1, 0xB1}));

    const ParsedCode danglingParsed = parsedCodeOf(cleanedBytes(container, "Td.class"), 0);
    ASSERT_NE(danglingParsed.code, nullptr);
    // LDC(2B) → NOP RETURN；原 RETURN 保留。
    EXPECT_EQ(danglingParsed.code->code, (Bytes{0x00, 0xB1, 0xB1}));
}

// 规则 3：LVT/LVTT/LineNumberTable 越界/悬垂条目修剪，合法条目保留。
// 基座 jasm（istore 0 使 maxLocals 真值 = 1，LVT slot-0 合法门成立）；三张
// 调试表经例外② DOM 挂 Raw 槽（载荷 putU16 拼装）。
TEST(MjPassIllegalClassFileCleanerTest, PrunesLvtLvttLntEntries)
{
    // code_length = 4（iconst_0; istore_0; nop; return），末条指令起始偏移 3。
    const Bytes base =
        asmOne("T", "public static", "d", "()V",
               "      iconst_0\n      istore 0\n      nop\n      return\n");
    ASSERT_NE(base.size(), 0u);
    expectValidateOk(base);

    Bytes bytes = mutateClassFile(base, "debugTables", [&](jbc::core::ClassFile& cf) {
        jbc::core::CodeAttr* code = mutableFirstCode(cf);
        ASSERT_NE(code, nullptr);
        attachRawAttr(cf, code->attrs, "LineNumberTable",
                      lntBody({{0, 10}, {5, 20}}));   // (5,20) 越界
        attachRawAttr(cf, code->attrs, "LocalVariableTable",
                      lvtBody({{0, 2, "a", "I", 0},      // 合法
                               {0, 2, "b", "J", 9},      // slot >= maxLocals
                               {5, 1, "c", "I", 0}},     // 区间越界
                              cf));
        attachRawAttr(cf, code->attrs, "LocalVariableTypeTable",
                      lvtBody({{0, 2, "s", "Ljava/util/List;", 0},   // 合法
                               {0, 2, "t", "J", 9}},                 // wide 且 slot 越界
                              cf));
    });
    ASSERT_NE(bytes.size(), 0u);

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
// 例外②申报面：BootstrapMethods/句柄/condy 深层 CP 形装配器不建模——合法
// 基座（ldc 7）经 DOM 物化 CP 追加面 + Raw 类属性挂槽 + LDC 操作数单点改写
// 产出被测形态；validate 不判句柄 kind 深语义，清洗实跑为最终证明。
TEST(MjPassIllegalClassFileCleanerTest, CompactsBsmAndDropsCondyChain)
{
    ca::u16 condyBad = 0;
    const Bytes plain = asmOne("T", "public static", "c", "()V", "      ldc 7\n      return\n");
    ASSERT_NE(plain.size(), 0u);
    expectValidateOk(plain);
    const Bytes base = mutateClassFile(plain, "condyChain", [&](jbc::core::ClassFile& cf) {
            // 非法句柄：REF_INVOKE_INTERFACE（要求 InterfaceMethodref）挂在
            // Methodref 上。
            const ca::u32 clsB    = cf.cp.addClass(cf.cp.addUtf8("B"));
            const ca::u32 nt      = cf.cp.addNameAndType(cf.cp.addUtf8("x"),
                                                         cf.cp.addUtf8("I"));
            const ca::u32 str     = cf.cp.addString(cf.cp.addUtf8("A"));
            const ca::u32 mref    = cf.cp.addRef(jbc::core::CpTag::Methodref, clsB, nt);
            const ca::u32 mhBad   = cf.cp.addMethodHandle(9, mref);
            const ca::u32 mhGood  = cf.cp.addMethodHandle(6, mref);
            condyBad = static_cast<ca::u16>(cf.cp.addDynamic(0, nt));
            attachRawAttr(cf, cf.attrs, "BootstrapMethods",
                          bsmBody({{static_cast<ca::u16>(mhBad), {static_cast<ca::u16>(str)}},
                                   {static_cast<ca::u16>(mhGood),
                                    {static_cast<ca::u16>(str)}}}));
            jbc::core::CodeAttr* code = mutableFirstCode(cf);
            ASSERT_NE(code, nullptr);
            for (ca::u32 pc = 0; pc < code->code.size(); ++pc) {
                if (code->code[pc] != 0x12) continue;
                code->code[pc + 1] = static_cast<ca::u8>(condyBad);
                break;
            }
        });
    ASSERT_NE(base.size(), 0u);

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", base);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "condy chain over invalid BSM must be cleaned";

    auto model = rereadModel(cleanedBytes(container, "T.class"));
    ASSERT_NE(model, nullptr);
    auto read = jbc::core::readClass(jbc::core::ByteView(cleanedBytes(container, "T.class")));
    ASSERT_TRUE(read.is_ok()) << "cleaned bytes must stay parseable";

    EXPECT_EQ(findAttrByName(model->attributes(), AttributeConstants::BOOTSTRAP_METHODS),
              nullptr)
        << "BSM table must be removed once no code references a dynamic constant";
    EXPECT_EQ(countPoolTag(*model, jclassfile::model::CpTag::DYNAMIC), 0u)
        << "orphan condy must leave the pool";
    EXPECT_EQ(countPoolTag(*model, jclassfile::model::CpTag::METHOD_HANDLE), 1u)
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
// 例外②申报面同上。
TEST(MjPassIllegalClassFileCleanerTest, RemapsCondyAfterBsmCompaction)
{
    ca::u16 condy = 0;
    const Bytes plain = asmOne("T", "public static", "c", "()V", "      ldc 7\n      return\n");
    ASSERT_NE(plain.size(), 0u);
    expectValidateOk(plain);
    const Bytes base = mutateClassFile(plain, "condyRemap", [&](jbc::core::ClassFile& cf) {
            const ca::u32 clsB  = cf.cp.addClass(cf.cp.addUtf8("B"));
            const ca::u32 nt    = cf.cp.addNameAndType(cf.cp.addUtf8("x"),
                                                       cf.cp.addUtf8("I"));
            const ca::u32 str   = cf.cp.addString(cf.cp.addUtf8("A"));
            const ca::u32 mref  = cf.cp.addRef(jbc::core::CpTag::Methodref, clsB, nt);
            const ca::u32 mhGood = cf.cp.addMethodHandle(6, mref);
            const ca::u32 mhBad  = cf.cp.addMethodHandle(9, mref);
            condy = static_cast<ca::u16>(cf.cp.addDynamic(0, nt));
            attachRawAttr(cf, cf.attrs, "BootstrapMethods",
                          bsmBody({{static_cast<ca::u16>(mhGood),
                                    {static_cast<ca::u16>(str)}},
                                   {static_cast<ca::u16>(mhBad),
                                    {static_cast<ca::u16>(str)}}}));
            jbc::core::CodeAttr* code = mutableFirstCode(cf);
            ASSERT_NE(code, nullptr);
            for (ca::u32 pc = 0; pc < code->code.size(); ++pc) {
                if (code->code[pc] != 0x12) continue;
                code->code[pc + 1] = static_cast<ca::u8>(condy);
                break;
            }
        });
    ASSERT_NE(base.size(), 0u);

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", base);

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
    EXPECT_EQ(countPoolTag(*model, jclassfile::model::CpTag::DYNAMIC), 1u)
        << "live condy must stay";
    EXPECT_EQ(countPoolTag(*model, jclassfile::model::CpTag::METHOD_HANDLE), 1u)
        << "only the valid handle stays";

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
// 例外②：孤儿句柄经 DOM CP 追加面物化（kind 5 REF_INVOKE_VIRTUAL 挂
// Fieldref → 非法）。
TEST(MjPassIllegalClassFileCleanerTest, RemovesInvalidMethodHandleFromPool)
{
    const Bytes plain = asmOne("T", "public static", "m", "()V", "      nop\n      return\n");
    ASSERT_NE(plain.size(), 0u);
    expectValidateOk(plain);
    const Bytes bytes = mutateClassFile(plain, "orphanBadHandle", [&](jbc::core::ClassFile& cf) {
            const ca::u32 clsB = cf.cp.addClass(cf.cp.addUtf8("B"));
            const ca::u32 nt   = cf.cp.addNameAndType(cf.cp.addUtf8("f"),
                                                      cf.cp.addUtf8("I"));
            const ca::u32 fref = cf.cp.addRef(jbc::core::CpTag::Fieldref, clsB, nt);
            (void)cf.cp.addMethodHandle(5, fref);   // 池中无主条目
        });
    ASSERT_NE(bytes.size(), 0u);

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.changed()) << "invalid method handle must leave the pool";

    auto model = rereadModel(cleanedBytes(container, "T.class"));
    ASSERT_NE(model, nullptr);
    auto read = jbc::core::readClass(jbc::core::ByteView(cleanedBytes(container, "T.class")));
    ASSERT_TRUE(read.is_ok()) << "cleaned bytes must stay parseable";
    EXPECT_EQ(countPoolTag(*model, jclassfile::model::CpTag::METHOD_HANDLE), 0u);
}

// 规则 5 正面控制：合法的无主 MethodHandle 保留，字节逐位不动。
// 例外②：孤儿合法句柄（kind 6 REF_INVOKESTATIC 挂 Methodref）同面物化。
TEST(MjPassIllegalClassFileCleanerTest, KeepsValidFloatingMethodHandleUntouched)
{
    const Bytes plain = asmOne("T", "public static", "m", "()V", "      nop\n      return\n");
    ASSERT_NE(plain.size(), 0u);
    expectValidateOk(plain);
    const Bytes bytes = mutateClassFile(plain, "orphanGoodHandle", [&](jbc::core::ClassFile& cf) {
            const ca::u32 clsB = cf.cp.addClass(cf.cp.addUtf8("B"));
            const ca::u32 nt   = cf.cp.addNameAndType(cf.cp.addUtf8("bsm"),
                                                      cf.cp.addUtf8("()V"));
            const ca::u32 mref = cf.cp.addRef(jbc::core::CpTag::Methodref, clsB, nt);
            (void)cf.cp.addMethodHandle(6, mref);
        });
    ASSERT_NE(bytes.size(), 0u);

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", bytes);

    const auto pa = runCleaner(container);
    EXPECT_TRUE(pa.areAllPreserved()) << "valid floating handle must not trigger re-encode";
    EXPECT_EQ(cleanedBytes(container, "T.class"), bytes);
}
