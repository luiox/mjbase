/// @file        shadow_token_test.cpp
/// @brief       #1156 shadow 引用 token T2：resolve(s, pos) 单一收口 + name 槽
///              文法专测（mjbase 移植版；表/条目 API 为驻留视图口径）。
///
/// 钉扎面（规范 doc_ai/shadow-reference-token-spec.md §2/§3/§6）：
/// - §2 文法唯一形态：ClassRef/MemberRef 形状矩阵 + $${ 转义；
/// - §3 位置三分：name 槽整串二选一、无子串扫描；字符串常量恒等（含 ${...}
///   的常量文本永不判为 Ref —— 文法性质，非运行时功能）；
/// - §6 错误四分类全分支 + 缺 sidecar 硬报错；两错误码永不 fall-through
///   （同一输入两码互斥，矩阵级专测）；
/// - MemberRef 二元 handle：类/成员 token 都要验；重载歧义 → ambiguous-member。
/// 测试全部自造字符串，零样本字节（法务红线）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/renamer/shadow_alias.hpp"
#include "mjbase/renamer/shadow_token.hpp"

namespace mjbase::renamer {

namespace {

/// 手工表（全部自造名/号）：
///   Class_1 = a/b/Alpha（token 11111111），方法 m_1 = fly ()V（token 22222222），
///   字段 f_1 = weight I（token 33333333）；Class_2 = a/b/Beta（token 44444444）。
ShadowAliasTable makeTable()
{
    ShadowAliasTable table;
    table.addClass("a/b/Alpha", "Class_1", "11111111");
    table.addMethod("a/b/Alpha", "()V", "fly", "m_1", "22222222");
    table.addField("a/b/Alpha", "I", "weight", "f_1", "33333333");
    table.addClass("a/b/Beta", "Class_2", "44444444");
    return table;
}

/// 重载歧义表：同 owner 同影子名两条 desc（手编 v2 可达状态；生成器不会产）。
ShadowAliasTable makeAmbiguousTable()
{
    ShadowAliasTable table;
    table.addClass("a/b/Alpha", "Class_1", "11111111");
    table.addMethod("a/b/Alpha", "(I)V", "fly", "m_1", "22222222");
    table.addMethod("a/b/Alpha", "(J)V", "fly", "m_1", "55555555");
    return table;
}

ShadowSlotResult resolve(const ShadowAliasTable& table, const std::string& input)
{
    return resolveShadowSlot(table, input);
}

ShadowSlotResult resolveMember(const ShadowAliasTable& table,
                               const std::string&      owner,
                               const std::string&      input)
{
    ShadowSlotQuery query;
    query.member = true;
    query.owner  = owner;
    return resolveShadowSlot(table, input, query);
}

const std::string& text(const ShadowSlotResult& result)
{
    return result.message;
}

constexpr const char* kClassRef  = "${Class_1.11111111}";
constexpr const char* kMemberRef = "${Class_1.11111111.m_1.22222222}";

}   // namespace

// ───────────────────────── §2 文法形状（纯词法） ─────────────────────────

TEST(MjShadowTokenTest, ParseShapeClassificationMatrix)
{
    struct Case
    {
        const char*   input;
        ShadowRefForm form;
    };
    const Case cases[] = {
        {"a/b/Alpha", ShadowRefForm::Raw},
        {"", ShadowRefForm::Raw},
        {"$${Class_1.11111111}", ShadowRefForm::EscapedLiteral},
        {"$${x", ShadowRefForm::EscapedLiteral},
        {kClassRef, ShadowRefForm::ClassRef},
        {"${Class_12.abcdef01}", ShadowRefForm::ClassRef},
        {kMemberRef, ShadowRefForm::MemberRef},
        {"${Class_1.11111111.handle_3.9a1b2c3d}", ShadowRefForm::MemberRef},
        // 形状违约（§6 malformed-handle 语义域）：
        {"${Class_1}", ShadowRefForm::Malformed},                       // 缺 token 后缀
        {"${Class_1.1111111}", ShadowRefForm::Malformed},               // 7 位 token
        {"${Class_1.111111111}", ShadowRefForm::Malformed},             // 9 位 token
        {"${Class_1.1111111g}", ShadowRefForm::Malformed},              // 坏 hex
        {"${Class_1.1111111F}", ShadowRefForm::Malformed},              // 大写 hex
        {"${CLASS_1.11111111}", ShadowRefForm::Malformed},              // 前缀大小写违约
        {"${Class_.11111111}", ShadowRefForm::Malformed},               // 空序号
        {"${Class}", ShadowRefForm::Malformed},                         // 裸 ${ 开头
        {"${", ShadowRefForm::Malformed},
        {"${}", ShadowRefForm::Malformed},
        {"${Class_1.11111111}x", ShadowRefForm::Malformed},             // 尾随垃圾
        {"${Class_1.11111111.m_1.22222222}x", ShadowRefForm::Malformed},
        {"${Class_1.11111111.m_1.22222222.extra.33333333}", ShadowRefForm::Malformed},
        {"${Class_1.11111111..22222222}", ShadowRefForm::Malformed},    // 空影子段
    };
    for (const auto& item : cases) {
        EXPECT_EQ(item.form, parseShadowRef(item.input).form) << item.input;
    }
}

TEST(MjShadowTokenTest, MidStringDollarBraceIsRawNotRef)
{
    // §3 name 槽无子串扫描：`${` 不在串首只是裸名字节，绝不离析出 Ref。
    EXPECT_EQ(ShadowRefForm::Raw, parseShadowRef("ldc \"${Class_1.11111111}\"").form);
    EXPECT_EQ(ShadowRefForm::Raw, parseShadowRef("a${Class_1.11111111}").form);
    EXPECT_EQ(ShadowRefForm::Raw, parseShadowRef("prefix ${var} suffix").form);
}

// ───────────────────────── §3 name 槽解析全分支 ─────────────────────────

TEST(MjShadowTokenTest, ClassRefResolvesToRawInternalName)
{
    const auto result = resolve(makeTable(), kClassRef);
    EXPECT_EQ(ShadowSlotError::None, result.error);
    EXPECT_EQ("a/b/Alpha", result.raw);
    EXPECT_TRUE(result.fromHandle);
    EXPECT_EQ(ShadowKind::Class, result.kind);
    EXPECT_TRUE(text(result).empty());
}

TEST(MjShadowTokenTest, MemberRefResolvesToRawNameAndTableDesc)
{
    const auto result = resolveMember(makeTable(), "a/b/Alpha", kMemberRef);
    EXPECT_EQ(ShadowSlotError::None, result.error);
    EXPECT_EQ("fly", result.raw);
    EXPECT_EQ("()V", result.descriptor);
    EXPECT_TRUE(result.fromHandle);
    EXPECT_EQ(ShadowKind::Method, result.kind);
}

TEST(MjShadowTokenTest, RawExistsPassesIdentityAndRawMissingIsRawNotFound)
{
    const auto hit = resolve(makeTable(), "a/b/Alpha");
    EXPECT_EQ(ShadowSlotError::None, hit.error);
    EXPECT_EQ("a/b/Alpha", hit.raw);
    EXPECT_FALSE(hit.fromHandle);

    const auto miss = resolve(makeTable(), "no/such/Name");
    EXPECT_EQ(ShadowSlotError::RawNotFound, miss.error);
    EXPECT_NE(std::string::npos, text(miss).find("raw-not-found"));
    EXPECT_NE(std::string::npos, text(miss).find("raw_to_shadow"));
}

TEST(MjShadowTokenTest, RawMemberSlotIsOwnerScoped)
{
    const auto hit = resolveMember(makeTable(), "a/b/Alpha", "fly");
    EXPECT_EQ(ShadowSlotError::None, hit.error);
    EXPECT_EQ("fly", hit.raw);

    const auto miss = resolveMember(makeTable(), "a/b/Beta", "fly");
    EXPECT_EQ(ShadowSlotError::RawNotFound, miss.error);
    // owner 上下文进诊断（§6 可诊断要求）。
    EXPECT_NE(std::string::npos, text(miss).find("a/b/Beta"));
}

TEST(MjShadowTokenTest, EscapedLiteralIsRawSemantics)
{
    // $${...} = 字面 ${...}：注册了该字面名 → 恒等放行。
    ShadowAliasTable table;
    table.addClass("${Class_1.deadbeef}", "Class_9", "aaaaaaaa");
    const auto hit = resolve(table, "$${Class_1.deadbeef}");
    EXPECT_EQ(ShadowSlotError::None, hit.error);
    EXPECT_EQ("${Class_1.deadbeef}", hit.raw);
    EXPECT_FALSE(hit.fromHandle);

    // 未注册 → raw-not-found（不是 malformed/unknown）。
    const auto miss = resolve(makeTable(), "$${Class_1.11111111}");
    EXPECT_EQ(ShadowSlotError::RawNotFound, miss.error);
}

TEST(MjShadowTokenTest, EmptyTableRawPathStaysIdentityZeroDrift)
{
    const ShadowAliasTable empty;
    const auto             raw = resolve(empty, "a/b/Alpha");
    EXPECT_EQ(ShadowSlotError::None, raw.error);
    EXPECT_EQ("a/b/Alpha", raw.raw);

    const auto escaped = resolve(empty, "$${anything}");
    EXPECT_EQ(ShadowSlotError::None, escaped.error);
    EXPECT_EQ("${anything}", escaped.raw);
}

TEST(MjShadowTokenTest, EmptyTableAnyReferenceShapeIsMissingSidecarHardError)
{
    const ShadowAliasTable empty;
    for (const auto* input : {"${Class_1.11111111}", kMemberRef, "${Class_1}", "${"}) {
        const auto result = resolve(empty, input);
        EXPECT_EQ(ShadowSlotError::MissingSidecar, result.error) << input;
        EXPECT_NE(std::string::npos, text(result).find("missing-sidecar")) << input;
        // 硬报错带修复动作（§6：提示需加载 sidecar）。
        EXPECT_NE(std::string::npos, text(result).find("shadow_generate")) << input;
    }
}

TEST(MjShadowTokenTest, NonNamePositionsDispatch)
{
    // Descriptor 已接线：裸 handle 不是描述符 → 文法违约（MalformedHandle），
    // 不再是「未接线」占位拒绝。
    ShadowSlotQuery query;
    query.position = ShadowSlotPosition::Descriptor;
    const auto desc = resolveShadowSlot(makeTable(), kClassRef, query);
    EXPECT_EQ(ShadowSlotError::MalformedHandle, desc.error);
    EXPECT_EQ(std::string::npos, text(desc).find("does not accept shadow references"));

    // JasmBody 仍未接线：fail-loud（MalformedHandle + 指引）。
    query.position = ShadowSlotPosition::JasmBody;
    const auto jasm = resolveShadowSlot(makeTable(), kClassRef, query);
    EXPECT_EQ(ShadowSlotError::MalformedHandle, jasm.error);
    EXPECT_NE(std::string::npos, text(jasm).find("does not accept shadow references"));
}

// ───────────────────────── §6 错误四分类（Ref 面） ─────────────────────────

TEST(MjShadowTokenTest, ClassRefTokenMismatchAndUnknownIndexAreUnknownHandle)
{
    const auto mismatch = resolve(makeTable(), "${Class_1.22222222}");
    EXPECT_EQ(ShadowSlotError::UnknownHandle, mismatch.error);
    EXPECT_NE(std::string::npos, text(mismatch).find("unknown-handle"));

    const auto unknown = resolve(makeTable(), "${Class_99.11111111}");
    EXPECT_EQ(ShadowSlotError::UnknownHandle, unknown.error);

    // 前导零 = 另一串名（不规范化）→ 响亮 unknown-handle，绝不静默命中。
    const auto padded = resolve(makeTable(), "${Class_01.11111111}");
    EXPECT_EQ(ShadowSlotError::UnknownHandle, padded.error);
}

TEST(MjShadowTokenTest, MalformedVariantsAreMalformedHandle)
{
    for (const auto* input :
         {"${Class_1}", "${Class_1.1111111g}", "${CLASS_1.11111111}", "${", "${Class"}) {
        const auto result = resolve(makeTable(), input);
        EXPECT_EQ(ShadowSlotError::MalformedHandle, result.error) << input;
        EXPECT_NE(std::string::npos, text(result).find("malformed-handle")) << input;
        EXPECT_NE(std::string::npos, text(result).find("rebuild")) << input;
    }
}

TEST(MjShadowTokenTest, MemberRefBothTokensVerified)
{
    // 成员 token 不匹配 → unknown-handle（形状对、表里有影子名但号不符）。
    const auto memberMismatch =
        resolveMember(makeTable(), "a/b/Alpha", "${Class_1.11111111.m_1.33333333}");
    EXPECT_EQ(ShadowSlotError::UnknownHandle, memberMismatch.error);

    // 类 token 不匹配（成员段全对）→ unknown-handle。
    const auto classMismatch =
        resolveMember(makeTable(), "a/b/Alpha", "${Class_1.22222222.m_1.22222222}");
    EXPECT_EQ(ShadowSlotError::UnknownHandle, classMismatch.error);

    // 成员影子名不在该 owner 名下 → unknown-handle（诊断含 owner 上下文）。
    const auto unknownShadow =
        resolveMember(makeTable(), "a/b/Alpha", "${Class_1.11111111.nope_1.22222222}");
    EXPECT_EQ(ShadowSlotError::UnknownHandle, unknownShadow.error);
    EXPECT_NE(std::string::npos, text(unknownShadow).find("a/b/Alpha"));
}

TEST(MjShadowTokenTest, AmbiguousOverloadsDemandDescriptor)
{
    const auto result = resolveMember(makeAmbiguousTable(), "a/b/Alpha", kMemberRef);
    EXPECT_EQ(ShadowSlotError::AmbiguousMember, result.error);
    EXPECT_NE(std::string::npos, text(result).find("ambiguous-member"));
    // 修复动作指引补 desc（§6），并说明本卡只收 raw desc。
    EXPECT_NE(std::string::npos, text(result).find("descriptor"));
    EXPECT_NE(std::string::npos, text(result).find("raw descriptor"));
}

// ───────────────────── 不 fall-through 专测（§3 红线） ─────────────────────

TEST(MjShadowTokenTest, ErrorCodesNeverFallThroughMutualExclusionMatrix)
{
    // 同一输入的两码互斥：Ref 形状永不产生 raw-not-found；raw 形状永不产生
    // unknown-handle / malformed-handle / ambiguous-member。
    const ShadowAliasTable table = makeTable();
    const char* refShaped[]    = {
        "${Class_1.11111111}", "${Class_99.11111111}", "${Class_1.22222222}",
        kMemberRef, "${Class_1.11111111.m_1.33333333}", "${Class_1}", "${",
        "${Class_1.11111111.m_1.22222222"};
    const char* rawShaped[] = {
        "a/b/Alpha", "fly", "no/such/Name", "no/such/member", "", "x/y ${Class_1.11111111}"};
    for (const auto* input : refShaped) {
        const auto slot = resolve(table, input);
        if (slot.error == ShadowSlotError::None) continue;
        EXPECT_NE(ShadowSlotError::RawNotFound, slot.error) << input;
        EXPECT_EQ(std::string::npos, text(slot).find("raw-not-found")) << input;
    }
    for (const auto* input : rawShaped) {
        if (input[0] == '\0') {
            // 空串由接线层直通（resolve 语义 = raw 恒等），此处跳过。
            continue;
        }
        const auto slot = resolve(table, input);
        EXPECT_NE(ShadowSlotError::UnknownHandle, slot.error) << input;
        EXPECT_NE(ShadowSlotError::MalformedHandle, slot.error) << input;
        EXPECT_NE(ShadowSlotError::AmbiguousMember, slot.error) << input;
        EXPECT_EQ(std::string::npos, text(slot).find("unknown-handle")) << input;
        EXPECT_EQ(std::string::npos, text(slot).find("malformed-handle")) << input;
        EXPECT_EQ(std::string::npos, text(slot).find("ambiguous-member")) << input;
    }
}

// ───────────────────── 字符串常量恒等（文法性质钉扎） ─────────────────────

TEST(MjShadowTokenTest, StringConstantTextNeverClassifiesAsReference)
{
    // 含 ${var} / ${Class_N.token8} 的字符串常量文本：在 name 槽的视角里要么
    // 是裸名（${ 不在串首），要么是形状违约——**永不展开**。解析展开只发生
    // 在文法标识槽，常量位置根本不进本收口（接线层结构性保证，这里钉形状
    // 判定不越界）。
    const ShadowAliasTable table = makeTable();
    const char* constants[] = {
        "\"${var}\"",
        "\"value: ${Class_1.11111111}\"",
        "String s = \"${Class_1.deadbeef}\"; // ${Class_2.44444444}",
        "concat ${var} and ${Class_1.11111111.m_1.22222222} in one literal",
    };
    for (const auto* constant : constants) {
        const auto parts = parseShadowRef(constant);
        EXPECT_NE(ShadowRefForm::ClassRef, parts.form) << constant;
        EXPECT_NE(ShadowRefForm::MemberRef, parts.form) << constant;
        // 常量文本经收口后 raw 恒等（或 raw-not-found 诊断），但绝不被展开成
        // 别的名字：fromHandle 恒 false。
        const auto slot = resolve(table, constant);
        EXPECT_FALSE(slot.fromHandle) << constant;
        if (slot.error == ShadowSlotError::None) {
            EXPECT_EQ(constant, slot.raw) << constant;
        }
    }
}

// ───────────────────────── handle 发放（回显工具面） ─────────────────────────

TEST(MjShadowTokenTest, HandleIssuanceCoversClassAndMemberEntries)
{
    const auto table = makeTable();
    ASSERT_EQ(4U, table.size());
    std::string classHandle;
    std::string memberHandle;
    for (const auto& entry : table.entries()) {
        const auto handle = shadowRefHandleOf(table, entry);
        if (entry.kind == ShadowKind::Class && entry.rawName == "a/b/Alpha") {
            classHandle = handle;
        }
        if (entry.kind == ShadowKind::Method) {
            memberHandle = handle;
            EXPECT_EQ("${Class_1.11111111.m_1.22222222}", handle);
        }
    }
    EXPECT_EQ(kClassRef, classHandle);

    // 发出的句柄必可解析回同一符号（发放/解析闭环）。
    const auto back = resolve(table, memberHandle);
    EXPECT_EQ(ShadowSlotError::None, back.error);
    EXPECT_EQ("fly", back.raw);
}

TEST(MjShadowTokenTest, HandleIssuanceSkipsTokenlessAndManualNames)
{
    ShadowAliasTable table;
    table.addClass("x/y/NoToken", "Class_3");                    // v1 遗留缺号
    table.addClass("x/y/Manual", "HandsomeName", "bbbbbbbb");    // 手工类名
    table.addClass("x/y/Ok", "Class_4", "cccccccc");
    table.addMethod("x/y/Ok", "()V", "run", "Weird Shadow", "dddddddd");
    for (const auto& entry : table.entries()) {
        const auto handle = shadowRefHandleOf(table, entry);
        if (entry.rawName == "x/y/Ok") {
            EXPECT_EQ("${Class_4.cccccccc}", handle);
        }
        else if (entry.kind == ShadowKind::Method) {
            EXPECT_TRUE(handle.empty());   // 影子名出文法字符 → 不发句柄
        }
        else {
            EXPECT_TRUE(handle.empty());   // 缺 token / 手工名 → 不发句柄
        }
    }
}


// ── Descriptor 槽（A 批 A1：类型位展开）──────────────────────────

namespace {

/// Descriptor 槽快捷入口。
ShadowSlotResult resolveDesc(const ShadowAliasTable& table, const std::string& input)
{
    ShadowSlotQuery query;
    query.position = ShadowSlotPosition::Descriptor;
    return resolveShadowSlot(table, input, query);
}

}   // namespace

TEST(MjShadowTokenTest, DescriptorPlainIdentityZeroDrift)
{
    const auto table = makeTable();
    // 无 token 的纯描述符恒等直通（java/lang/String 不在表内也零漂移）。
    const auto r1 = resolveDesc(table, "(Ljava/lang/String;I)V");
    ASSERT_EQ(r1.error, ShadowSlotError::None);
    EXPECT_EQ(r1.raw, "(Ljava/lang/String;I)V");
    EXPECT_FALSE(r1.fromHandle);

    const auto r2 = resolveDesc(table, "Ljava/lang/Object;");
    ASSERT_EQ(r2.error, ShadowSlotError::None);
    EXPECT_EQ(r2.raw, "Ljava/lang/Object;");
    EXPECT_FALSE(r2.fromHandle);
}

TEST(MjShadowTokenTest, DescriptorClassRefExpandsInTypePosition)
{
    const auto table = makeTable();
    const auto r = resolveDesc(table, "(L" + std::string(kClassRef) + ";I)V");
    ASSERT_EQ(r.error, ShadowSlotError::None) << text(r);
    EXPECT_EQ(r.raw, "(La/b/Alpha;I)V");
    EXPECT_TRUE(r.fromHandle);
}

TEST(MjShadowTokenTest, DescriptorArrayAndFieldForms)
{
    const auto table = makeTable();
    // 字段描述符：数组 + handle 类型位。
    const auto r = resolveDesc(table, "[L" + std::string(kClassRef) + ";");
    ASSERT_EQ(r.error, ShadowSlotError::None) << text(r);
    EXPECT_EQ(r.raw, "[La/b/Alpha;");
    EXPECT_TRUE(r.fromHandle);

    // 双重数组 + 裸类型恒等。
    const auto r2 = resolveDesc(table, "[[La/b/Beta;");
    ASSERT_EQ(r2.error, ShadowSlotError::None) << text(r2);
    EXPECT_EQ(r2.raw, "[[La/b/Beta;");
    EXPECT_FALSE(r2.fromHandle);

    // 方法多参数：数组参数 + 基类型参数（连续字段类型）。
    const auto r3 = resolveDesc(table, "([L" + std::string(kClassRef) + ";[I[I)V");
    ASSERT_EQ(r3.error, ShadowSlotError::None) << text(r3);
    EXPECT_EQ(r3.raw, "([La/b/Alpha;[I[I)V");
    EXPECT_TRUE(r3.fromHandle);

    // 单字段描述符带尾随第二类型 = 文法违约（字段只有一个类型）。
    EXPECT_EQ(resolveDesc(table, "[L" + std::string(kClassRef) + ";[I").error,
              ShadowSlotError::MalformedHandle);
}

TEST(MjShadowTokenTest, DescriptorMultiExpansionAndDispatchViaSlot)
{
    const auto table = makeTable();
    const std::string input =
        "(L" + std::string(kClassRef) + ";L" + std::string(kClassRef) + ";)L" +
        std::string(kClassRef) + ";";
    const auto r = resolveDesc(table, input);
    ASSERT_EQ(r.error, ShadowSlotError::None) << text(r);
    EXPECT_EQ(r.raw, "(La/b/Alpha;La/b/Alpha;)La/b/Alpha;");
    EXPECT_TRUE(r.fromHandle);
}

TEST(MjShadowTokenTest, DescriptorMissingSidecarHardFails)
{
    const ShadowAliasTable empty;
    const auto r = resolveDesc(empty, "(L" + std::string(kClassRef) + ";)V");
    EXPECT_EQ(r.error, ShadowSlotError::MissingSidecar);
}

TEST(MjShadowTokenTest, DescriptorUnknownHandlePropagates)
{
    const auto table = makeTable();
    // token 错 → unknown-handle 原样上抛。
    const auto r = resolveDesc(table, "(L${Class_1.deadbeef};)V");
    EXPECT_EQ(r.error, ShadowSlotError::UnknownHandle);
}

TEST(MjShadowTokenTest, DescriptorMemberRefRejectedInTypePosition)
{
    const auto table = makeTable();
    const auto r = resolveDesc(table, "(L" + std::string(kMemberRef) + ";)V");
    EXPECT_EQ(r.error, ShadowSlotError::MalformedHandle);
    EXPECT_NE(r.message.find("type position"), std::string::npos);
}

TEST(MjShadowTokenTest, DescriptorGenericSignatureFailsLoud)
{
    const auto table = makeTable();
    // 泛型签名标记 fail-loud（Sig v2 另批），不静默替换。
    const auto r = resolveDesc(table, "(L" + std::string(kClassRef) + "<TT;>;)V");
    EXPECT_EQ(r.error, ShadowSlotError::MalformedHandle);
    EXPECT_NE(r.message.find("generic signature"), std::string::npos);
}

TEST(MjShadowTokenTest, DescriptorGrammarGarbageFailsLoud)
{
    const auto table = makeTable();
    // 裸 handle 不是描述符。
    EXPECT_EQ(resolveDesc(table, kClassRef).error, ShadowSlotError::MalformedHandle);
    // 尾随垃圾。
    EXPECT_EQ(resolveDesc(table, "(I)VX").error, ShadowSlotError::MalformedHandle);
    // 未闭合参数表。
    EXPECT_EQ(resolveDesc(table, "(I").error, ShadowSlotError::MalformedHandle);
    // V 不出现在字段描述符/参数位。
    EXPECT_EQ(resolveDesc(table, "VI").error, ShadowSlotError::MalformedHandle);
    // 空串。
    EXPECT_EQ(resolveDesc(table, "").error, ShadowSlotError::MalformedHandle);
    // 段内 Malformed 引用。
    EXPECT_EQ(resolveDesc(table, "(L${;I)V").error, ShadowSlotError::MalformedHandle);
}

TEST(MjShadowTokenTest, DescriptorEscapedLiteralKeepsLiteralBraces)
{
    const auto table = makeTable();
    // $${…} = 字面请求：剥 $ 后的字面名即含 ${ 的名字本身（与 Name 槽
    // 剥皮语义一致——字面类名按原样输出，不查表）。
    const auto r = resolveDesc(table, "(L$${a/b/Weird};)V");
    ASSERT_EQ(r.error, ShadowSlotError::None) << text(r);
    EXPECT_EQ(r.raw, "(L${a/b/Weird};)V");
    EXPECT_FALSE(r.fromHandle);
}

}   // namespace mjbase::renamer
