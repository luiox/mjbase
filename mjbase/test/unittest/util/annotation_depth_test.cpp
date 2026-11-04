/// @file        annotation_depth_test.cpp
/// @brief       注解族嵌套深度预检测试：深嵌敌意注解拦下、合法注解放行、
///              歧义/截断 fail-open 与 libjbc 解码面错误通道相干。
///
/// 背景（doc_ai/upstream-libjbc-annotation-recursion.md）：libjbc 0.4.22
/// attr_codec 的 readAnnotation/readElemValue 互递归无界，AAC 壳 oo.class
/// 的 2999 层 '@' 嵌套注解使 decodeAttr 栈溢出。本测试以字节夹具钉住
/// util/annotation_depth.hpp 守卫的裁决口径；宿主进程被敌意输入打爆的
/// 形态不可在单测内直解（栈溢出不可恢复），故深嵌载体只验预检与装载
/// 面（lazy，不解码）。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/util/annotation_depth.hpp"

using jbc::core::AttrSlot;
using jbc::core::ByteView;
using jbc::core::ClassFile;
using mjbase::util::AnnotationAttrKind;
using mjbase::util::attrSafeToDecode;
using mjbase::util::annotationNestingWithinLimit;

namespace {

void putU2(std::vector<ca::u8>& out, ca::u16 v)
{
    out.push_back(static_cast<ca::u8>(v >> 8));
    out.push_back(static_cast<ca::u8>(v & 0xFF));
}

void putU4(std::vector<ca::u8>& out, ca::u32 v)
{
    out.push_back(static_cast<ca::u8>(v >> 24));
    out.push_back(static_cast<ca::u8>(v >> 16));
    out.push_back(static_cast<ca::u8>(v >> 8));
    out.push_back(static_cast<ca::u8>(v & 0xFF));
}

void putUtf8(std::vector<ca::u8>& cp, std::string_view s)
{
    cp.push_back(1);
    putU2(cp, static_cast<ca::u16>(s.size()));
    cp.insert(cp.end(), s.begin(), s.end());
}

/// @brief 最小 classfile（V1_8）：this=T、super=java/lang/Object、零成员、
/// 单个类级属性（name + 原始载荷）。属性名/类名 CP 布局固定：
/// #1 Utf8 "T"，#2 Class(#1)，#3 Utf8 "java/lang/Object"，#4 Class(#3)，
/// #5 Utf8 属性名。
std::vector<ca::u8> minimalClassWithAttr(std::string_view attrName,
                                         const std::vector<ca::u8>& payload)
{
    std::vector<ca::u8> cp;
    putUtf8(cp, "T");                                  // #1
    cp.push_back(7);                                   // #2 Class
    putU2(cp, 1);
    putUtf8(cp, "java/lang/Object");                   // #3
    cp.push_back(7);                                   // #4 Class
    putU2(cp, 3);
    putUtf8(cp, attrName);                             // #5

    std::vector<ca::u8> out;
    putU4(out, 0xCAFEBABEu);
    putU2(out, 0);
    putU2(out, 0x34);
    putU2(out, static_cast<ca::u16>(6));               // cpCount（slot 0 无效）
    out.insert(out.end(), cp.begin(), cp.end());
    putU2(out, 0x0021);                                // public super
    putU2(out, 2);                                     // this = #2
    putU2(out, 4);                                     // super = #4
    putU2(out, 0);                                     // interfaces
    putU2(out, 0);                                     // fields
    putU2(out, 0);                                     // methods
    putU2(out, 1);                                     // class attrs = 1
    putU2(out, 5);                                     // name = #5
    putU4(out, static_cast<ca::u32>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// @brief Runtime{Visible,Invisible}Annotations 载荷：1 条注解，单 pair，
/// 元素值 '@' 连嵌 depth 层后以 's' 叶收尾。CP 下标一律指向 #5（Utf8），
/// 预检走查不校验 CP，浅嵌对照会真解码故下标合法。
std::vector<ca::u8> nestedAnnoPayload(ca::u32 depth)
{
    std::vector<ca::u8> p;
    putU2(p, 1);          // num_annotations
    putU2(p, 5);          // type_index
    putU2(p, 1);          // num_pairs
    for (ca::u32 i = 0; i < depth; ++i) {
        putU2(p, 5);      // element_name
        p.push_back('@'); // nested annotation
        putU2(p, 5);      // type_index
        putU2(p, 1);      // num_pairs
    }
    putU2(p, 5);          // element_name（叶）
    p.push_back('s');     // string 值
    putU2(p, 5);          // const_value_index
    return p;
}

/// @brief RVTA 载荷：1 条 type_annotation，target_type=0x13（字段，empty），
/// type_path 空，单 pair 元素值 '@' 连嵌 depth 层后 's' 叶收尾。
std::vector<ca::u8> typeAnnoPayload(ca::u32 depth)
{
    std::vector<ca::u8> p;
    putU2(p, 1);          // num_annotations
    p.push_back(0x13);    // target_type: field（empty target_info）
    p.push_back(0);       // type_path 长度
    putU2(p, 5);          // type_index
    putU2(p, 1);          // num_pairs
    for (ca::u32 i = 0; i < depth; ++i) {
        putU2(p, 5);
        p.push_back('@');
        putU2(p, 5);
        putU2(p, 1);
    }
    putU2(p, 5);
    p.push_back('s');
    putU2(p, 5);
    return p;
}

/// @brief RVPA 载荷：1 参数 1 条注解，元素值 '@' 连嵌 depth 层。
std::vector<ca::u8> paramAnnoPayload(ca::u32 depth)
{
    std::vector<ca::u8> p;
    p.push_back(1);       // num_parameters
    putU2(p, 1);          // num_annotations
    putU2(p, 5);
    putU2(p, 1);
    for (ca::u32 i = 0; i < depth; ++i) {
        putU2(p, 5);
        p.push_back('@');
        putU2(p, 5);
        putU2(p, 1);
    }
    putU2(p, 5);
    p.push_back('s');
    putU2(p, 5);
    return p;
}

/// @brief AnnotationDefault 载荷：单个元素值 '@' 连嵌 depth 层。
std::vector<ca::u8> annoDefaultPayload(ca::u32 depth)
{
    std::vector<ca::u8> p;
    for (ca::u32 i = 0; i < depth; ++i) {
        p.push_back('@');
        putU2(p, 5);
        putU2(p, 1);
        putU2(p, 5);      // element_name
    }
    p.push_back('s');
    putU2(p, 5);
    return p;
}

/// 读类并取首个（唯一）属性槽。
const AttrSlot* soleAttr(const ClassFile& cf)
{
    return cf.attrs.empty() ? nullptr : &cf.attrs[0];
}

}   // namespace

TEST(AnnotationDepth, DeepNestingRejectedAcrossFamilies)
{
    constexpr ca::u32 kHostileDepth = 2999;   // oo.class 实测嵌套量级
    for (const char* name : {"RuntimeVisibleAnnotations", "RuntimeInvisibleAnnotations"}) {
        EXPECT_FALSE(annotationNestingWithinLimit(
            ByteView{nestedAnnoPayload(kHostileDepth)}, AnnotationAttrKind::Annotations,
            mjbase::util::kMaxAnnotationNesting)) << name;
    }
    EXPECT_FALSE(annotationNestingWithinLimit(
        ByteView{typeAnnoPayload(kHostileDepth)}, AnnotationAttrKind::TypeAnnotations,
        mjbase::util::kMaxAnnotationNesting));
    EXPECT_FALSE(annotationNestingWithinLimit(
        ByteView{paramAnnoPayload(kHostileDepth)},
        AnnotationAttrKind::ParameterAnnotations, mjbase::util::kMaxAnnotationNesting));
    EXPECT_FALSE(annotationNestingWithinLimit(
        ByteView{annoDefaultPayload(kHostileDepth)},
        AnnotationAttrKind::AnnotationDefault, mjbase::util::kMaxAnnotationNesting));
}

TEST(AnnotationDepth, ShallowLegalNestingAccepted)
{
    for (const char* name : {"RuntimeVisibleAnnotations", "RuntimeInvisibleAnnotations"}) {
        EXPECT_TRUE(annotationNestingWithinLimit(
            ByteView{nestedAnnoPayload(3)}, AnnotationAttrKind::Annotations,
            mjbase::util::kMaxAnnotationNesting)) << name;
    }
    EXPECT_TRUE(annotationNestingWithinLimit(ByteView{typeAnnoPayload(3)},
                                             AnnotationAttrKind::TypeAnnotations,
                                             mjbase::util::kMaxAnnotationNesting));
    EXPECT_TRUE(annotationNestingWithinLimit(ByteView{paramAnnoPayload(3)},
                                             AnnotationAttrKind::ParameterAnnotations,
                                             mjbase::util::kMaxAnnotationNesting));
    EXPECT_TRUE(annotationNestingWithinLimit(ByteView{annoDefaultPayload(3)},
                                             AnnotationAttrKind::AnnotationDefault,
                                             mjbase::util::kMaxAnnotationNesting));
}

TEST(AnnotationDepth, BoundaryDepthIsExclusive)
{
    // 255 层 = 恰在限内放行；256 层 = 确证越限拒绝（与 signature kMaxNesting
    // 同档口径：kMaxNesting 取等仍可解析）。
    EXPECT_TRUE(annotationNestingWithinLimit(ByteView{nestedAnnoPayload(255)},
                                             AnnotationAttrKind::Annotations,
                                             mjbase::util::kMaxAnnotationNesting));
    EXPECT_FALSE(annotationNestingWithinLimit(ByteView{nestedAnnoPayload(256)},
                                              AnnotationAttrKind::Annotations,
                                              mjbase::util::kMaxAnnotationNesting));
}

TEST(AnnotationDepth, TruncatedPayloadFailsOpen)
{
    // 截断载荷：走查按歧义放行；libjbc 解码面对同载荷回 Err——两道裁决
    // 相干，放行不会把栈炸进进程。
    std::vector<ca::u8> truncated = nestedAnnoPayload(4);
    truncated.resize(6);   // 只剩 num_annotations + 注解体头部一半
    EXPECT_TRUE(annotationNestingWithinLimit(ByteView{truncated},
                                             AnnotationAttrKind::Annotations,
                                             mjbase::util::kMaxAnnotationNesting));

    auto bytes = minimalClassWithAttr("RuntimeVisibleAnnotations", truncated);
    auto read  = jbc::core::readClass(ByteView{bytes.data(), bytes.size()});
    ASSERT_TRUE(read.is_ok());
    jbc::core::ReadResult parsed = std::move(read).unwrap();
    ClassFile& cf = *parsed.file;   // 产物先落名为实体再取引用（move-only 纪律）
    const AttrSlot* slot = soleAttr(cf);
    ASSERT_NE(slot, nullptr);
    EXPECT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*slot)).is_err());
}

TEST(AnnotationDepth, LegalShallowAttrDecodesCleanlyThroughHost)
{
    // 守卫不误拒：浅嵌合法注解走查放行，且 libjbc 真解码成功——保守方向
    // 的证明面（合法类在守卫下零行为差）。
    auto   bytes = minimalClassWithAttr("RuntimeVisibleAnnotations", nestedAnnoPayload(3));
    auto   read  = jbc::core::readClass(ByteView{bytes.data(), bytes.size()});
    ASSERT_TRUE(read.is_ok());
    jbc::core::ReadResult parsed = std::move(read).unwrap();
    ClassFile& cf = *parsed.file;   // 产物先落名为实体再取引用（move-only 纪律）
    const AttrSlot* slot = soleAttr(cf);
    ASSERT_NE(slot, nullptr);
    EXPECT_TRUE(attrSafeToDecode(cf, *slot));
    EXPECT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*slot)).is_ok());
}

TEST(AnnotationDepth, SessionLoadingOfHostileClassStaysLazyAndSafe)
{
    // oo.class 形态回归钉：readClass/makeSessionFromBytes 接受深嵌注解类
    // （lazy 面），宿主不解码即不炸；守卫在该类上拒绝注解族槽。
    auto   bytes = minimalClassWithAttr("RuntimeVisibleAnnotations", nestedAnnoPayload(2999));
    auto   session = jbc::ir::makeSessionFromBytes(ByteView{bytes.data(), bytes.size()});
    ASSERT_TRUE(session.is_ok());
    ClassFile& cf = session.unwrap()->backing();
    const AttrSlot* slot = soleAttr(cf);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(attrSafeToDecode(cf, *slot));
}
