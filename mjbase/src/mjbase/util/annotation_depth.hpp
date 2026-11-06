/// @file        annotation_depth.hpp
/// @brief       注解族属性解码前的嵌套深度保守预检（迭代走查、显式栈）。
///
/// 背景（doc:upstream-annotation-recursion）：libjbc 0.4.22 的
/// readAnnotation/readElemValue 互递归无深度上限，敌意类可携约 3000 层 '@'
/// 嵌套注解使 decodeAttr 栈溢出（AAC 壳 oo.class 实证，MSVC ASAN 抓到第一
/// 现场）。同族解码面均已有界：jbc/type/signature.cpp kMaxNesting=255、
/// libjclassfile annotation_reader DepthGuard——唯 attr_codec 注解族缺守卫
/// （libjbc 侧尚未补，本守卫为其代偿；上游修复合入后移除本守卫）。
///
/// 本预检按同一语法对原始属性字节做迭代走查（显式栈，不递归），只在能
/// 证明嵌套超限时拒绝；形状歧义 / 截断 / 未知 target_type 一律放行——那些
/// 形态走 libjbc 解码面自身的 Result 错误通道（Truncated /
/// UnsupportedFeature），是优雅的。保守方向：不拒任何 libjbc 能正常解出
/// 的合法注解；拒绝仅当元素值嵌套深度确证超限。
///
/// 覆盖属性名（全部经 readElemValue 递归，JVMS 4.7.16 / 4.7.20 / 4.7.22 /
/// 4.7.24）：Runtime{Visible,Invisible}Annotations、Runtime{Visible,
/// Invisible}TypeAnnotations、Runtime{Visible,Invisible}ParameterAnnotations、
/// AnnotationDefault。
#pragma once

#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/bytes.hpp"
#include "jbc/core/class_file.hpp"

namespace mjbase::util {

/// @brief 嵌套上限。与 libjbc type/signature.cpp 的 kMaxNesting 同档（族内
/// 单一口径）；JVMS 未定义注解嵌套上界，255 远超合法语义需要。
inline constexpr ca::u32 kMaxAnnotationNesting = 255;

/// @brief 注解族属性形状（与七名一一映射）。
enum class AnnotationAttrKind : ca::u8 {
    Annotations,             ///< Runtime{Visible,Invisible}Annotations
    ParameterAnnotations,    ///< Runtime{Visible,Invisible}ParameterAnnotations
    TypeAnnotations,         ///< Runtime{Visible,Invisible}TypeAnnotations
    AnnotationDefault,       ///< AnnotationDefault
};

/// @brief 属性名 → 形状；非注解族名返回 false。
inline bool annotationAttrKind(const jbc::core::ClassFile& cf,
                               const jbc::core::AttrSlot&  slot,
                               AnnotationAttrKind&         out) noexcept
{
    if (cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleAnnotations")
        || cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleAnnotations")) {
        out = AnnotationAttrKind::Annotations;
        return true;
    }
    if (cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleParameterAnnotations")
        || cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleParameterAnnotations")) {
        out = AnnotationAttrKind::ParameterAnnotations;
        return true;
    }
    if (cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleTypeAnnotations")
        || cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleTypeAnnotations")) {
        out = AnnotationAttrKind::TypeAnnotations;
        return true;
    }
    if (cf.cp.utf8Equals(slot.nameCp, "AnnotationDefault")) {
        out = AnnotationAttrKind::AnnotationDefault;
        return true;
    }
    return false;
}

namespace detail {

/// @brief 有界游标：任一读失败返回 false（调用方按歧义放行）。
class DepthCursor
{
public:
    explicit DepthCursor(jbc::core::ByteView v) noexcept : p_{v.data()}, n_{v.size()} {}

    bool u1(ca::u8& out) noexcept
    {
        if (pos_ >= n_) return false;
        out = p_[pos_++];
        return true;
    }

    bool u2(ca::u16& out) noexcept
    {
        ca::u8 hi = 0;
        ca::u8 lo = 0;
        if (!u1(hi) || !u1(lo)) return false;
        out = static_cast<ca::u16>((static_cast<ca::u16>(hi) << 8) | lo);
        return true;
    }

private:
    const ca::u8* p_   = nullptr;
    ca::usize    n_   = 0;
    ca::usize    pos_ = 0;
};

/// @brief 任务形态。元素值自定界，显式栈一帧一个待展开层。
enum TaskKind : ca::u8 {
    kTaskAnnotation = 0,   ///< 注解体：u2 type + u2 nv → nv 个（u2 名 + 元素值）
    kTaskPair       = 1,   ///< u2 名 + 元素值
    kTaskElement    = 2,   ///< 元素值
};

struct Task
{
    ca::u32 remaining;
    ca::u8  kind;
    ca::u32 depth;   ///< 本帧展开出的元素值上方 '@'/'[' 容器层数（顶层对 0）
};

/// @brief 消费一个注解体头部（u2 type_index + u2 num_pairs）。
inline bool readAnnotationHead(DepthCursor& r, ca::u16& numPairs) noexcept
{
    ca::u16 typeIdx = 0;
    return r.u2(typeIdx) && r.u2(numPairs);
}

/// @brief 跳过一个 type_annotation 的 target_info + type_path + u2 type_index
///（JVMS 4.7.20 表 A/B，形状镜像 libjbc readTargetInfo/readTypePath；未知
/// target_type 视为歧义放行——libjbc 解码面对它回 UnsupportedFeature）。
inline bool skipTypeAnnotationHead(DepthCursor& r) noexcept
{
    using jbc::core::TypeAnnotationsAttr;
    ca::u8 targetType = 0;
    if (!r.u1(targetType)) return false;
    if (targetType == TypeAnnotationsAttr::kClassTypeParameter
        || targetType == TypeAnnotationsAttr::kMethodTypeParameter) {
        ca::u8 idx = 0;
        if (!r.u1(idx)) return false;
    }
    else if (targetType == TypeAnnotationsAttr::kSupertype) {
        ca::u16 idx = 0;
        if (!r.u2(idx)) return false;
    }
    else if (targetType == TypeAnnotationsAttr::kClassTypeParameterBound
             || targetType == TypeAnnotationsAttr::kMethodTypeParameterBound) {
        ca::u8 paramIdx = 0;
        ca::u8 boundIdx = 0;
        if (!r.u1(paramIdx) || !r.u1(boundIdx)) return false;
    }
    else if (targetType == TypeAnnotationsAttr::kField
             || targetType == TypeAnnotationsAttr::kMethodReturn
             || targetType == TypeAnnotationsAttr::kMethodReceiver) {
        // empty
    }
    else if (targetType == TypeAnnotationsAttr::kFormalsParameter) {
        ca::u8 idx = 0;
        if (!r.u1(idx)) return false;
    }
    else if (targetType == TypeAnnotationsAttr::kThrows) {
        ca::u16 idx = 0;
        if (!r.u2(idx)) return false;
    }
    else if (targetType == TypeAnnotationsAttr::kLocalVar
             || targetType == TypeAnnotationsAttr::kLocalVarResource) {
        ca::u16 n = 0;
        if (!r.u2(n)) return false;
        for (ca::u16 i = 0; i < n; ++i) {
            ca::u16 a = 0;
            ca::u16 b = 0;
            ca::u16 c = 0;
            if (!r.u2(a) || !r.u2(b) || !r.u2(c)) return false;
        }
    }
    else if (targetType == TypeAnnotationsAttr::kCatch) {
        ca::u16 idx = 0;
        if (!r.u2(idx)) return false;
    }
    else if (targetType >= TypeAnnotationsAttr::kOffsetBase
             && targetType <= TypeAnnotationsAttr::kOffsetMax) {
        ca::u16 offset = 0;
        if (!r.u2(offset)) return false;
    }
    else if (targetType == TypeAnnotationsAttr::kCast
             || (targetType >= TypeAnnotationsAttr::kTypeArgBase
                 && targetType <= TypeAnnotationsAttr::kTypeArgMax)) {
        ca::u16 offset = 0;
        ca::u8  idx    = 0;
        if (!r.u2(offset) || !r.u1(idx)) return false;
    }
    else {
        return false;   // 未知 target_type → 歧义放行
    }

    // type_path：u1 长度 → 每段 (u1 kind, u1 type_argument_index)。
    ca::u8 pathLen = 0;
    if (!r.u1(pathLen)) return false;
    for (ca::u8 i = 0; i < pathLen; ++i) {
        ca::u8 kind = 0;
        ca::u8 idx  = 0;
        if (!r.u1(kind) || !r.u1(idx)) return false;
    }

    ca::u16 typeIdx = 0;
    return r.u2(typeIdx);
}

/// @brief 共享走查循环：展开任务栈到空，或证明深度超限。
///
/// true  = 限内 / 形状歧义 / 截断（fail-open）；false = 嵌套深度确证超限。
/// 帧取值语义（先出栈再展开），子帧压栈不影响已取出的帧。
inline bool walkTasks(DepthCursor& r, std::vector<Task>& stack, ca::u32 maxDepth) noexcept
{
    while (!stack.empty()) {
        const Task f = stack.back();
        stack.pop_back();
        if (f.remaining == 0) continue;

        switch (f.kind) {
        case kTaskAnnotation: {
            ca::u16 numPairs = 0;
            if (!readAnnotationHead(r, numPairs)) return true;   // 歧义放行
            stack.push_back(Task{numPairs, kTaskPair, f.depth});
            break;
        }
        case kTaskPair: {
            ca::u16 nameIdx = 0;
            if (!r.u2(nameIdx)) return true;
            stack.push_back(Task{1, kTaskElement, f.depth});
            break;
        }
        case kTaskElement: {
            // 元素值深度越限即确证（本帧 remaining 已减，值必然存在）。
            if (f.depth > maxDepth) return false;
            ca::u8 tag = 0;
            if (!r.u1(tag)) return true;
            if (tag == 'e') {
                ca::u16 typeName = 0;
                ca::u16 constNm  = 0;
                if (!r.u2(typeName) || !r.u2(constNm)) return true;
            }
            else if (tag == '@') {
                ca::u16 numPairs = 0;
                if (!readAnnotationHead(r, numPairs)) return true;
                stack.push_back(Task{numPairs, kTaskPair, f.depth + 1});
            }
            else if (tag == '[') {
                ca::u16 n = 0;
                if (!r.u2(n)) return true;
                stack.push_back(Task{n, kTaskElement, f.depth + 1});
            }
            else {
                ca::u16 cp = 0;
                if (!r.u2(cp)) return true;
            }
            break;
        }
        default:
            return true;   // 不可达；防御性放行
        }
    }
    return true;
}

}   // namespace detail

/// @brief 注解族属性载荷的元素值嵌套深度是否在限内。
///
/// 按形状分派顶层装填：
///   - Annotations：u2 n → n × 注解体；
///   - ParameterAnnotations：u1 参数数 → 每参数 (u2 n → n × 注解体)；
///   - TypeAnnotations：u2 n → n × (target_info + type_path + u2 type_index
///     + u2 nv → 元素值对)；
///   - AnnotationDefault：单个元素值。
///
/// true  = 限内 / 形状歧义 / 截断（fail-open，交给 libjbc 解码面裁决）；
/// false = 已确证元素值嵌套深度 > maxDepth（decode 会栈溢出，必须拦截）。
inline bool annotationNestingWithinLimit(const jbc::core::ByteView& payload,
                                         AnnotationAttrKind         kind,
                                         ca::u32                    maxDepth) noexcept
{
    detail::DepthCursor r{payload};

    std::vector<detail::Task> stack;
    stack.reserve(8);

    switch (kind) {
    case AnnotationAttrKind::Annotations: {
        ca::u16 n = 0;
        if (!r.u2(n)) return true;
        stack.push_back(detail::Task{n, detail::kTaskAnnotation, 0});
        break;
    }
    case AnnotationAttrKind::ParameterAnnotations: {
        ca::u8 nparams = 0;
        if (!r.u1(nparams)) return true;
        bool headOk = true;
        for (ca::u32 p = 0; p < nparams; ++p) {
            ca::u16 n = 0;
            if (!r.u2(n)) {
                headOk = false;
                break;
            }
            stack.push_back(detail::Task{n, detail::kTaskAnnotation, 0});
        }
        if (!headOk) return true;
        break;
    }
    case AnnotationAttrKind::TypeAnnotations: {
        ca::u16 n = 0;
        if (!r.u2(n)) return true;
        for (ca::u32 i = 0; i < n; ++i) {
            ca::u16 numPairs = 0;
            if (!detail::skipTypeAnnotationHead(r)) return true;   // 歧义放行
            if (!r.u2(numPairs)) return true;
            stack.push_back(detail::Task{numPairs, detail::kTaskPair, 0});
        }
        break;
    }
    case AnnotationAttrKind::AnnotationDefault:
        stack.push_back(detail::Task{1, detail::kTaskElement, 0});
        break;
    default:
        return true;   // 不可达；防御性放行
    }

    return detail::walkTasks(r, stack, maxDepth);
}

/// @brief 组合门：该属性槽是否可安全交给 libjbc decodeAttr。
///
/// 非注解族属性恒 true（门只拦注解族）；注解族按原始载荷走查裁决。
/// span 截断（slot.data 越出源字节）按歧义放行——libjbc 解码面对该形态
/// 返回 Truncated，优雅。名字比较与形状分派同源（一次映射两用）。
inline bool attrSafeToDecode(const jbc::core::ClassFile& cf,
                             const jbc::core::AttrSlot&  slot,
                             ca::u32 maxDepth = kMaxAnnotationNesting) noexcept
{
    AnnotationAttrKind kind = AnnotationAttrKind::Annotations;
    if (!annotationAttrKind(cf, slot, kind)) return true;
    return annotationNestingWithinLimit(cf.source().spanOrEmpty(slot.data), kind, maxDepth);
}

}   // namespace mjbase::util
