/// DevirtualizeSingleTarget 单测：手工 classfile 夹具（structural_pass_util）
/// 覆盖改写面（自类唯一目标 / 继承唯一目标 / 同类私有目标）与放弃面全清单
/// （多目标、外部 owner、接口目标、非 ALOAD_0 接收者、锚定窗、bridge 目标、
/// 跨 nest 私有、调用者 major<52、目标不在调用者 super 链），另含幂等与
/// 分析未注册契约。真 JVM verifier/行为等价硬验收在同名 *_jvm_test.cpp。
///
/// 形态注记：INVOKEINTERFACE 用例的操作数指向 Methodref 形态 CP 条目（共享
/// 夹具构造器无 InterfaceMethodref 入口）——libjbc 读取面容忍两种 ref 形态
/// 交叉（JVMS 5.4.3.3 同源口径），分析只消费 opcode 与 owner/name/desc 文本，
/// 该简化不影响站点门语义。abstract 目标门与 bridge 门同一位掩码行，由
/// bridge 用例承载（abstract 方法按夹具构造器形态无法表达「无 Code 属性」）。
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublic    = 0x0001;
constexpr ca::u16 kAccPrivate   = 0x0002;
constexpr ca::u16 kAccBridge    = 0x0040;
constexpr ca::u16 kAccInterface = 0x0200;

constexpr ca::u8 kOpAload0          = 0x2a;
constexpr ca::u8 kOpAload1          = 0x2b;
constexpr ca::u8 kOpInvokeVirtual   = 0xb6;
constexpr ca::u8 kOpInvokeSpecial   = 0xb7;
constexpr ca::u8 kOpInvokeInterface = 0xb9;
constexpr ca::u8 kOpReturn          = 0xb1;

Bytes selfCallCode(ca::u16 cp)
{
    return {kOpAload0, kOpInvokeVirtual, static_cast<ca::u8>(cp >> 8),
            static_cast<ca::u8>(cp), kOpReturn};
}

Bytes interfaceCallCode(ca::u16 cp)
{
    return {kOpAload0, kOpInvokeInterface, static_cast<ca::u8>(cp >> 8),
            static_cast<ca::u8>(cp), /*count=*/0x01, /*zero=*/0x00, kOpReturn};
}

/// 唯一具体声明 + this 直调唯一目标的载体类（各夹具用独立类名避免跨用例
/// 覆盖族串扰）。
void addUniquePair(FixtureBuilder& builder, std::string_view encodedOwner)
{
    FixtureMethod target;
    target.name   = "m";
    target.desc   = "()V";
    target.access = kAccPublic;
    target.code   = bareReturn();
    builder.addMethod(target);

    const ca::u16 cp = builder.methodRef(encodedOwner, "m", "()V");
    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    builder.addMethod(caller);
}

/// 声明 m()V 的基类（public 具体）。
void addDeclaringBase(FixtureBuilder& builder)
{
    FixtureMethod target;
    target.name   = "m";
    target.desc   = "()V";
    target.access = kAccPublic;
    target.code   = bareReturn();
    builder.addMethod(target);
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
    FixtureBuilder                   builder("A");
    addUniquePair(builder, "A");
    addClassResource(container, "A.class", builder.assemble());

    const Bytes before = classBytesOf(container, "A.class");
    const Bytes beforeCaller = codeOf(before, "caller");
    ASSERT_EQ(beforeCaller.size(), 5u);
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
    FixtureBuilder                   sup("Sup");
    addDeclaringBase(sup);
    addClassResource(container, "Sup.class", sup.assemble());

    FixtureBuilder sub("Sub");
    sub.setSuper("Sup");
    const ca::u16 cp = sub.methodRef("Sub", "m", "()V");
    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    sub.addMethod(caller);
    addClassResource(container, "Sub.class", sub.assemble());

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
    FixtureBuilder                   builder("Priv");
    FixtureMethod secret;
    secret.name   = "secret";
    secret.desc   = "()V";
    secret.access = kAccPrivate;
    secret.code   = bareReturn();
    builder.addMethod(secret);

    const ca::u16 cp = builder.methodRef("Priv", "secret", "()V");
    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    builder.addMethod(caller);
    addClassResource(container, "Priv.class", builder.assemble());

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
    FixtureBuilder                   base("MBase");
    addDeclaringBase(base);
    addClassResource(container, "MBase.class", base.assemble());

    FixtureBuilder child("MChild");
    child.setSuper("MBase");
    addDeclaringBase(child);
    addClassResource(container, "MChild.class", child.assemble());

    FixtureBuilder callerCls("MCaller");
    const ca::u16  cp = callerCls.methodRef("MBase", "m", "()V");
    FixtureMethod  caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    callerCls.addMethod(caller);
    addClassResource(container, "MCaller.class", callerCls.assemble());

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
    FixtureBuilder                   builder("Ext");
    const ca::u16 cp = builder.methodRef("java/lang/Object", "toString",
                                         "()Ljava/lang/String;");
    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    builder.addMethod(caller);
    addClassResource(container, "Ext.class", builder.assemble());

    const Bytes before = classBytesOf(container, "Ext.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "Ext.class"), before);
}

// 接口目标（virtual 与 interface 两种 opcode 形态）：唯一族成员是接口自身
// 声明——类链优先选择使其不可证，一律放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsInterfaceTargetSites)
{
    mjbase::model::ResourceContainer container;
    FixtureBuilder                   iface("IFunc");
    iface.setClassAccess(static_cast<ca::u16>(kAccPublic | kAccInterface));
    addDeclaringBase(iface);
    addClassResource(container, "IFunc.class", iface.assemble());

    FixtureBuilder virtCaller("IfaceUserV");
    const ca::u16  virtCp = virtCaller.methodRef("IFunc", "m", "()V");
    FixtureMethod  virt;
    virt.name      = "caller";
    virt.desc      = "()V";
    virt.access    = kAccPublic;
    virt.maxStack  = 1;
    virt.maxLocals = 1;
    virt.code      = selfCallCode(virtCp);
    virtCaller.addMethod(virt);
    addClassResource(container, "IfaceUserV.class", virtCaller.assemble());

    FixtureBuilder itfCaller("IfaceUserI");
    const ca::u16  itfCp = itfCaller.methodRef("IFunc", "m", "()V");
    FixtureMethod  itf;
    itf.name      = "caller";
    itf.desc      = "()V";
    itf.access    = kAccPublic;
    itf.maxStack  = 1;
    itf.maxLocals = 1;
    itf.code      = interfaceCallCode(itfCp);
    itfCaller.addMethod(itf);
    addClassResource(container, "IfaceUserI.class", itfCaller.assemble());

    const Bytes vBefore = classBytesOf(container, "IfaceUserV.class");
    const Bytes iBefore = classBytesOf(container, "IfaceUserI.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "IfaceUserV.class"), vBefore);
    EXPECT_EQ(classBytesOf(container, "IfaceUserI.class"), iBefore);
    EXPECT_EQ(codeOf(classBytesOf(container, "IfaceUserI.class"), "caller")[1],
              kOpInvokeInterface);
}

// 非 ALOAD_0 接收者（aload_1 局部可能是兄弟类型，verifier 接收者约束）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsNonThisReceiverSites)
{
    mjbase::model::ResourceContainer container;
    FixtureBuilder                   builder("Rcv");
    addDeclaringBase(builder);

    const ca::u16 cp = builder.methodRef("Rcv", "m", "()V");
    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 2;
    caller.code      = selfCallCode(cp);
    caller.code[0]   = kOpAload1;   // 接收者不是 this
    builder.addMethod(caller);
    addClassResource(container, "Rcv.class", builder.assemble());

    const Bytes before = classBytesOf(container, "Rcv.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "Rcv.class"), before);
}

// 锚定窗：异常表 handler 落在调用点前缘（Boundary 标签锚定）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsAnchoredSites)
{
    mjbase::model::ResourceContainer container;
    FixtureBuilder                   builder("Anc");
    addDeclaringBase(builder);

    const ca::u16 cp = builder.methodRef("Anc", "m", "()V");
    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    // handler 指向调用点 pc=1：标签锚定调用点前置边界。
    caller.exc.push_back(ExcSpec{0, 1, 1, 0});
    builder.addMethod(caller);
    addClassResource(container, "Anc.class", builder.assemble());

    const Bytes before = classBytesOf(container, "Anc.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "Anc.class"), before);
}

// bridge 目标（签名/泛型桥成员禁改红线；abstract 门同行承载）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsBridgeTargetSites)
{
    mjbase::model::ResourceContainer container;
    FixtureBuilder                   builder("Brd");
    FixtureMethod bridge;
    bridge.name   = "m";
    bridge.desc   = "()V";
    bridge.access = static_cast<ca::u16>(kAccPublic | kAccBridge);
    bridge.code   = bareReturn();
    builder.addMethod(bridge);

    const ca::u16 cp = builder.methodRef("Brd", "m", "()V");
    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    builder.addMethod(caller);
    addClassResource(container, "Brd.class", builder.assemble());

    const Bytes before = classBytesOf(container, "Brd.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "Brd.class"), before);
}

// 跨 nest 私有目标（C_t != 调用者）：invokespecial 超类形态的选择扫描可能
// 命中中间类同签名声明（JEP 181 选 invokevirtual 的根因）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsCrossNestPrivateTargetSites)
{
    mjbase::model::ResourceContainer container;
    FixtureBuilder                   host("NestA");
    FixtureMethod hidden;
    hidden.name   = "hidden";
    hidden.desc   = "()V";
    hidden.access = kAccPrivate;
    hidden.code   = bareReturn();
    host.addMethod(hidden);
    addClassResource(container, "NestA.class", host.assemble());

    FixtureBuilder sub("NestB");
    sub.setSuper("NestA");
    const ca::u16 cp = sub.methodRef("NestA", "hidden", "()V");
    FixtureMethod caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    sub.addMethod(caller);
    addClassResource(container, "NestB.class", sub.assemble());

    const Bytes before = classBytesOf(container, "NestB.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "NestB.class"), before);
}

// 调用者类 major < 52：ACC_SUPER 语义按宿主分叉（JVMS §4.1）→ 放弃。
TEST(MjPassDevirtualizeSingleTargetTest, KeepsCallerBelowClassVersion52)
{
    mjbase::model::ResourceContainer container;
    FixtureBuilder                   builder("OldV");
    addUniquePair(builder, "OldV");
    Bytes bytes = builder.assemble();
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
    FixtureBuilder                   tgt("OffTgt");
    addDeclaringBase(tgt);
    addClassResource(container, "OffTgt.class", tgt.assemble());

    FixtureBuilder callerCls("OffCaller");
    const ca::u16  cp = callerCls.methodRef("OffTgt", "m", "()V");
    FixtureMethod  caller;
    caller.name      = "caller";
    caller.desc      = "()V";
    caller.access    = kAccPublic;
    caller.maxStack  = 1;
    caller.maxLocals = 1;
    caller.code      = selfCallCode(cp);
    callerCls.addMethod(caller);
    addClassResource(container, "OffCaller.class", callerCls.assemble());

    const Bytes before = classBytesOf(container, "OffCaller.class");
    EXPECT_TRUE(runDevirtualize(container).areAllPreserved());
    EXPECT_EQ(classBytesOf(container, "OffCaller.class"), before);
}

// ── 契约面 ──────────────────────────────────────────────────────────────────

// 分析未注册 = 调用方契约错误 → std::logic_error，容器无改写。
TEST(MjPassDevirtualizeSingleTargetTest, UnregisteredAnalysisPropagatesWithoutRewrite)
{
    mjbase::model::ResourceContainer container;
    FixtureBuilder                   builder("Ct");
    addUniquePair(builder, "Ct");
    addClassResource(container, "Ct.class", builder.assemble());

    const Bytes before = classBytesOf(container, "Ct.class");
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);   // 无任何注册
    mjbase::pass::DevirtualizeSingleTarget pass;
    EXPECT_THROW(pass.run(container, gam), std::logic_error);
    EXPECT_EQ(classBytesOf(container, "Ct.class"), before);
}
