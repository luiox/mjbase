// DevirtualizeSingleTarget 单测：jasm 装配夹具覆盖改写面（自类唯一目标 /
// 继承唯一目标 / 同类私有目标）与放弃面全清单（多目标、外部 owner、接口
// 目标、非 ALOAD_0 接收者、锚定窗、bridge 目标、跨 nest 私有、调用者
// major<52、目标不在调用者 super 链），另含幂等与分析未注册契约。真 JVM
// verifier/行为等价硬验收在同名 *_jvm_test.cpp。
//
// 夹具纪律：类与方法一律 jasm 文本装配（JvmJasmService），原
// structural_pass_util 手拼载体退役；Code 字节/CP 条目断言仍经 libjbc
// 读回完成。四处按语义合法重表达：
// - 接口目标面：jasm 原生 invokeinterface（发 InterfaceMethodref + count
//   操作数）替代原 Methodref 交叉形态，站点门语义同实（原形态注记失效）；
// - 非 ALOAD_0 接收者面：aload 1 取 1 槽需有定义——caller 带一个
//   Object 实参（1 槽即实参，帧自洽），接收者非 this 的门面同实；
// - 锚定窗面：原 ExcSpec 手填 handler 指向调用点 pc，以 jasm 原生
//   exceptions: 段同位表达（{ L0, Lb, Lb, * }：区间止于调用点前缘、
//   handler 锚调用点自身，labelsAt 同源留锚）；
// - bridge 目标面：jasm 修饰集未证 bridge 记号——例外①申报：装配合法
//   基类后单点 access-flag 翻转（0x0040 ACC_BRIDGE），翻转前后各做一次
//   relaxed validate 自检（pass 读 flags 不读文本，面同实）。
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/reference/resolved_call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/devirtualize_single_target.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u8 kOpInvokeVirtual   = 0xb6;
constexpr ca::u8 kOpInvokeSpecial   = 0xb7;
constexpr ca::u8 kOpInvokeInterface = 0xb9;

/// 类便捷装配：super 为超类 internal name，classMods 形如 "public super"，
/// methodsText 为若干 `.method` 声明文本。
Bytes asmClass(const std::string& cls, const std::string& super,
               std::string_view classMods, std::string_view methodsText)
{
    const std::string src = R"JASM(.super )JASM" + super + R"JASM(
.class )JASM" + std::string(classMods) + " " + cls + R"JASM( {
)JASM" + std::string(methodsText) + R"JASM(}
)JASM";
    // major 61：改写面受 pass 版本门（<52 不改写）约束，须 52+；61 走装配器
    // 全帧分析，体面帧自洽。
    return assembleJasm(src, 61);
}

/// public m()V（return 体）声明文本（唯一具体目标 / 基类声明）。
std::string_view declM()
{
    return "  .method public m ()V {\n    code: {\n      return\n    }\n  }\n";
}

/// this 直调唯一目标的 caller 文本（receiver 形如 "aload 0"；tail 为
/// return 前尾段，默认空；excText 为 exceptions 段条目文本，默认空）。
std::string callerMethod(std::string_view receiver, std::string_view inv,
                         std::string_view tail = "",
                         std::string_view excText = "")
{
    const std::string excSeg = excText.empty()
        ? std::string()
        : "    exceptions: {\n" + std::string(excText) + "    },\n";
    std::string s;
    s += "  .method public caller ()V {\n";
    s += excSeg;
    s += "    code: {\n";
    s += "      " + std::string(receiver) + '\n';
    s += "      " + std::string(inv) + '\n';
    s += std::string(tail);
    s += "      return\n    }\n  }\n";
    return s;
}

/// 唯一具体声明 + this 直调唯一目标的载体类（各夹具用独立类名避免跨用例
/// 覆盖族串扰）。
Bytes uniquePairClass(const std::string& cls)
{
    return asmClass(cls, "java/lang/Object", "public super",
                    std::string(declM()) + callerMethod("aload 0", "invokevirtual " + cls + ".m ()V"));
}

/// 读某方法 Code 属性的指令字节（夹具方法恒有 Code）。
Bytes codeOf(const Bytes& classBytes, const std::string& methodName)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(classBytes));
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return {};
    jbc::core::ReadResult result = std::move(read).unwrap();
    jbc::core::ClassFile& cf     = *result.file;
    for (jbc::core::MemberInfo& m : cf.methods) {
        if (cf.nameOf(m) != methodName) continue;
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
            const auto decoded = cf.decodeAttr(slot);
            EXPECT_TRUE(decoded.is_ok());
            if (decoded.is_err()) return {};
            if (decoded.unwrap()->kind() != jbc::core::AttrKind::Code) continue;
            const auto& code = static_cast<const jbc::core::CodeAttr&>(*decoded.unwrap());
            return Bytes(code.code.begin(), code.code.end());
        }
    }
    ADD_FAILURE() << "no Code attr: " << methodName;
    return {};
}

/// 解析 Methodref 形态 CP 条目的三段文本（畸形条目返回 nullopt）。
struct RefText
{
    std::string owner;
    std::string name;
    std::string desc;
};

std::optional<RefText> methodRefOf(const Bytes& classBytes, ca::u16 cpIndex)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(classBytes));
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return std::nullopt;
    jbc::core::ReadResult       result = std::move(read).unwrap();
    jbc::core::ClassFile&       cf     = *result.file;
    if (cpIndex == 0 || cpIndex >= cf.cp.size()) return std::nullopt;
    const jbc::core::ConstantPool::Entry entry = cf.cp.at(cpIndex);
    if (entry.tag != jbc::core::CpTag::Methodref) return std::nullopt;
    RefText text;
    // Methodref.a → Class 条目 → Utf8 名字索引（两跳）。
    const jbc::core::ConstantPool::Entry cls = cf.cp.at(entry.a);
    if (cls.tag != jbc::core::CpTag::Class) return std::nullopt;
    text.owner.assign(cf.cp.utf8Of(cls.a));
    const jbc::core::ConstantPool::Entry nat = cf.cp.at(entry.b);
    if (nat.tag != jbc::core::CpTag::NameAndType) return std::nullopt;
    text.name.assign(cf.cp.utf8Of(nat.a));
    text.desc.assign(cf.cp.utf8Of(nat.b));
    return text;
}

mjbase::spi::PreservedAnalyses runDevirtualize(mjbase::model::ResourceContainer& container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::HierarchyBundleAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedCallGraphAnalysis>();
    mjbase::pass::DevirtualizeSingleTarget pass;
    return pass.run(container, gam);
}

/// 读容器内 class 资源字节（断言在位且确为 class 资源）。
Bytes classBytesOf(mjbase::model::ResourceContainer& container, const char* uri)
{
    auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr) << uri;
    auto* cls = resource == nullptr
                    ? nullptr
                    : ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource);
    EXPECT_NE(cls, nullptr) << uri;
    return cls == nullptr ? Bytes{} : cls->get();
}

}   // namespace

// ── 改写面 ──────────────────────────────────────────────────────────────────

// 自类唯一目标：invokevirtual A.m（A 声明、无其他族成员）→ invokespecial A.m，
// CP 条目复用（操作数不变，仅 opcode 字节变化）；二次运行严格 no-op。
TEST(MjPassDevirtualizeSingleTargetTest, RewritesUniqueSelfTargetAndIsIdempotent)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "A.class", uniquePairClass("A"));

    const Bytes before = classBytesOf(container, "A.class");
    const Bytes beforeCaller = codeOf(before, "caller");
    ASSERT_EQ(beforeCaller.size(), 5u);   // aload_0; invokevirtual(3); return
    ASSERT_EQ(beforeCaller[1], kOpInvokeVirtual);

    const auto pa = runDevirtualize(container);
    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(pa.changed());

    const Bytes after        = classBytesOf(container, "A.class");
    const Bytes afterCaller  = codeOf(after, "caller");
    ASSERT_EQ(afterCaller.size(), beforeCaller.size());
    EXPECT_EQ(afterCaller[1], kOpInvokeSpecial);
    // CP 复用：Methodref A.m 既有条目原样指向（操作数不变）。
    EXPECT_EQ(afterCaller[2], beforeCaller[2]);
    EXPECT_EQ(afterCaller[3], beforeCaller[3]);
    const auto ref = methodRefOf(after,
                                 static_cast<ca::u16>((afterCaller[2] << 8) | afterCaller[3]));
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(ref->owner, "A");
    EXPECT_EQ(ref->name, "m");

    // 幂等：二次运行严格 no-op。
    const Bytes settled = classBytesOf(container, "A.class");
    const auto  pa2     = runDevirtualize(container);
    EXPECT_TRUE(pa2.areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "A.class"), settled);
}

// 继承唯一目标：encoded owner 未声明（Sub.m），沿 super 链落解到 Sup.m；
// 改写操作数换成指向声明者的 Methodref（新池条目）。
TEST(MjPassDevirtualizeSingleTargetTest, RewritesInheritedUniqueTarget)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Sup.class",
                     asmClass("Sup", "java/lang/Object", "public super", declM()));
    addClassResource(container, "Sub.class",
                     asmClass("Sub", "Sup", "public super",
                              callerMethod("aload 0", "invokevirtual Sub.m ()V")));

    const auto pa = runDevirtualize(container);
    EXPECT_FALSE(pa.areAllPreserved());

    const Bytes after       = classBytesOf(container, "Sub.class");
    const Bytes afterCaller = codeOf(after, "caller");
    ASSERT_EQ(afterCaller.size(), 5u);
    EXPECT_EQ(afterCaller[1], kOpInvokeSpecial);
    const auto ref = methodRefOf(after,
                                 static_cast<ca::u16>((afterCaller[2] << 8) | afterCaller[3]));
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(ref->owner, "Sup");   // 命名类 = 唯一目标声明类
    EXPECT_EQ(ref->name, "m");
    EXPECT_EQ(ref->desc, "()V");
}

// 同类私有目标（javac 同类私有调用的 invokevirtual 形态）：C_t == 调用者，
// invokespecial 选择 = 落解方法，改写。
TEST(MjPassDevirtualizeSingleTargetTest, RewritesPrivateSelfTarget)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "Priv.class",
        asmClass("Priv", "java/lang/Object", "public super",
                 "  .method private secret ()V {\n    code: {\n      return\n    }\n  }\n"
                     + callerMethod("aload 0", "invokevirtual Priv.secret ()V")));

    const auto pa = runDevirtualize(container);
    EXPECT_FALSE(pa.areAllPreserved());
    const Bytes afterCaller = codeOf(classBytesOf(container, "Priv.class"), "caller");
    ASSERT_EQ(afterCaller.size(), 5u);
    EXPECT_EQ(afterCaller[1], kOpInvokeSpecial);
}

// ── 放弃面 ──────────────────────────────────────────────────────────────────

// 多目标：子类覆写使覆盖族非唯一 → 保守放弃（基类与调用者字节均原样）。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsMultiTargetSites)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "MBase.class",
                     asmClass("MBase", "java/lang/Object", "public super", declM()));
    addClassResource(container, "MChild.class",
                     asmClass("MChild", "MBase", "public super", declM()));
    addClassResource(container, "MCaller.class",
                     asmClass("MCaller", "java/lang/Object", "public super",
                              callerMethod("aload 0", "invokevirtual MBase.m ()V")));

    const Bytes callerBefore = classBytesOf(container, "MCaller.class");
    const auto  pa           = runDevirtualize(container);
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "MCaller.class"), callerBefore);
    EXPECT_EQ(codeOf(classBytesOf(container, "MCaller.class"), "caller")[1], kOpInvokeVirtual);
}

// 外部 owner（降级编码目标，目标不在容器声明）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsExternalOwnerSites)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "Ext.class",
        asmClass("Ext", "java/lang/Object", "public super",
                 callerMethod("aload 0",
                              "invokevirtual java/lang/Object.toString ()Ljava/lang/String;",
                              "      pop\n")));

    const Bytes before = classBytesOf(container, "Ext.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "Ext.class"), before);
}

// 接口目标（virtual 与 interface 两种 opcode 形态）：唯一族成员是接口自身
// 声明——类链优先选择使其不可证，一律放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsInterfaceTargetSites)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "IFunc.class",
                     asmClass("IFunc", "java/lang/Object", "public interface abstract",
                              declM()));
    addClassResource(container, "IfaceUserV.class",
                     asmClass("IfaceUserV", "java/lang/Object", "public super",
                              callerMethod("aload 0", "invokevirtual IFunc.m ()V")));
    addClassResource(container, "IfaceUserI.class",
                     asmClass("IfaceUserI", "java/lang/Object", "public super",
                              callerMethod("aload 0", "invokeinterface IFunc.m ()V")));

    const Bytes vBefore = classBytesOf(container, "IfaceUserV.class");
    const Bytes iBefore = classBytesOf(container, "IfaceUserI.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "IfaceUserV.class"), vBefore);
    EXPECT_EQ(classBytesOf(container, "IfaceUserI.class"), iBefore);
    EXPECT_EQ(codeOf(classBytesOf(container, "IfaceUserI.class"), "caller")[1],
              kOpInvokeInterface);
}

// 非 ALOAD_0 接收者（aload 1 局部可能是兄弟类型，verifier 接收者约束）→ 放弃。
// caller 带 Object 实参使 1 槽有定义（帧自洽），接收者非 this 的门面同实。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsNonThisReceiverSites)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "Rcv.class",
        asmClass("Rcv", "java/lang/Object", "public super",
                 std::string(declM())
                     + "  .method public caller (Ljava/lang/Object;)V {\n"
                       "    code: {\n      aload 1\n      invokevirtual Rcv.m ()V\n"
                       "      return\n    }\n  }\n"));

    const Bytes before = classBytesOf(container, "Rcv.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "Rcv.class"), before);
}

// 锚定窗：异常表 handler 落在调用点前缘（Boundary 标签锚定）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsAnchoredSites)
{
    mjbase::model::ResourceContainer container;
    // 异常表 start 标签锚在调用点前置边界（原字节面 handler=调用点 pc；
    // 61 帧分析下 handler 必须独立成块收 Throwable，锚定面由 start 标签
    // 同样触发 labelsAnchorAt 前置边界守卫——行为契约同实）。
    const std::string caller =
        "  .method public caller ()V {\n"
        "    exceptions: {\n"
        "      { Ls, Le, H, * },\n"
        "    },\n"
        "    code: {\n"
        "      aload 0\n"
        "      Ls:\n"
        "      invokevirtual Anc.m ()V\n"
        "      Le:\n"
        "      return\n"
        "      H:\n"
        "      pop\n"
        "      return\n"
        "    }\n"
        "  }\n";
    addClassResource(container, "Anc.class",
                     asmClass("Anc", "java/lang/Object", "public super",
                              std::string(declM()) + caller));

    const Bytes before = classBytesOf(container, "Anc.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "Anc.class"), before);
}

// bridge 目标（签名/泛型桥成员禁改红线；abstract 门同行承载）→ 放弃。
// 例外①申报：jasm 修饰集未证 bridge 记号——装配合法基类后单点
// access-flag 翻转（0x0040 ACC_BRIDGE），翻转前后各做一次 relaxed validate
// 自检；pass 读 flags 不读文本，门面同实。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsBridgeTargetSites)
{
    mjbase::model::ResourceContainer container;
    const Bytes base = asmClass("Brd", "java/lang/Object", "public super",
                                std::string(declM()) + callerMethod("aload 0", "invokevirtual Brd.m ()V"));
    expectValidateOk(base);
    // declM 声明在前（物理序 0）：单点翻转 m 的 ACC_BRIDGE。
    const Bytes bridged = flipMemberAccess(base, false, 0, 0x0040, 0, "bridge flag");
    expectValidateOk(bridged);
    addClassResource(container, "Brd.class", bridged);

    const Bytes before = classBytesOf(container, "Brd.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "Brd.class"), before);
}

// 跨 nest 私有目标（C_t != 调用者）：invokespecial 超类形态的选择扫描可能
// 命中中间类同签名声明（JEP 181 选 invokevirtual 的根因）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsCrossNestPrivateTargetSites)
{
    mjbase::model::ResourceContainer container;
    addClassResource(
        container, "NestA.class",
        asmClass("NestA", "java/lang/Object", "public super",
                 "  .method private hidden ()V {\n    code: {\n      return\n    }\n  }\n"));
    addClassResource(container, "NestB.class",
                     asmClass("NestB", "NestA", "public super",
                              callerMethod("aload 0", "invokevirtual NestA.hidden ()V")));

    const Bytes before = classBytesOf(container, "NestB.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "NestB.class"), before);
}

// 调用者类 major < 52：ACC_SUPER 语义按宿主分叉（JVMS §4.1）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsCallerBelowClassVersion52)
{
    mjbase::model::ResourceContainer container;
    Bytes bytes = uniquePairClass("OldV");
    ASSERT_GT(bytes.size(), 8u);
    bytes[6] = 0x00;   // major 高字节
    bytes[7] = 0x32;   // major 低字节 = 50
    addClassResource(container, "OldV.class", std::move(bytes));

    const Bytes before = classBytesOf(container, "OldV.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "OldV.class"), before);
}

// 目标声明类不在调用者 super 链（兄弟类型接收者的 verifier 陷阱形态）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsTargetOffCallerChain)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "OffTgt.class",
                     asmClass("OffTgt", "java/lang/Object", "public super", declM()));
    addClassResource(container, "OffCaller.class",
                     asmClass("OffCaller", "java/lang/Object", "public super",
                              callerMethod("aload 0", "invokevirtual OffTgt.m ()V")));

    const Bytes before = classBytesOf(container, "OffCaller.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "OffCaller.class"), before);
}

// ── 契约面 ──────────────────────────────────────────────────────────────────

// 分析未注册 = 调用方契约错误 → std::logic_error，容器无改写。
TEST(MjPassDevirtualizeSingleTargetTest, UnregisteredAnalysisPropagatesWithoutRewrite)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Ct.class", uniquePairClass("Ct"));

    const Bytes before                     = classBytesOf(container, "Ct.class");
    ca::str::Utf8StringArena               arena;
    mjbase::spi::GlobalAnalysisManager     gam(arena);   // 无任何注册
    mjbase::pass::DevirtualizeSingleTarget pass;
    EXPECT_THROW(pass.run(container, gam), std::logic_error);
    EXPECT_EQ(classBytesOf(container, "Ct.class"), before);
}
