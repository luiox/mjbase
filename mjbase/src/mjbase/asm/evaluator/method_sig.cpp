/// @file        method_sig.cpp
/// @brief       方法描述符解析：参数类别序列 + 槽位布局 + 返回类别。
#include "method_sig.hpp"

namespace mjbase::asm_evaluator {
namespace {

/// @brief 从 pos 起解析一个字段描述符；失败返回 false。
///
/// 数组描述符允许任意维度前缀；类名只要求到 ';' 为止（内容不校验，
/// 求值器的引用面不区分子类型）。
bool parseFieldKind(std::string_view desc, ca::usize& pos, SigKind& out)
{
    if (pos >= desc.size()) return false;
    const char c = desc[pos];
    switch (c) {
    case 'B':
    case 'C':
    case 'I':
    case 'S':
    case 'Z': out = SigKind::I32; ++pos; return true;
    case 'F': out = SigKind::F32; ++pos; return true;
    case 'J': out = SigKind::I64; ++pos; return true;
    case 'D': out = SigKind::F64; ++pos; return true;
    case 'L': {
        const ca::usize semi = desc.find(';', pos + 1);
        if (semi == std::string_view::npos || semi == pos + 1) return false;
        out = SigKind::Ref;
        pos = semi + 1;
        return true;
    }
    case '[': {
        ++pos;
        SigKind element = SigKind::Void;
        if (!parseFieldKind(desc, pos, element)) return false;
        out = SigKind::Ref;
        return true;
    }
    default: return false;
    }
}

}   // namespace

bool isCat2(SigKind kind) noexcept
{
    return kind == SigKind::I64 || kind == SigKind::F64;
}

MethodSig::MethodSig(std::vector<SigKind> params, SigKind ret, bool isStatic,
                     ca::usize slots) noexcept
    : params_(std::move(params)), ret_(ret), isStatic_(isStatic), slots_(slots)
{}

Result<MethodSig, SigError> MethodSig::parse(std::string_view descriptor, bool isStatic)
{
    if (descriptor.empty() || descriptor.front() != '(') {
        return Err(SigError::Malformed);
    }
    ca::usize pos = 1;
    std::vector<SigKind> params;
    while (pos < descriptor.size() && descriptor[pos] != ')') {
        SigKind kind = SigKind::Void;
        if (!parseFieldKind(descriptor, pos, kind)) {
            return Err(SigError::Malformed);
        }
        params.push_back(kind);
    }
    if (pos >= descriptor.size()) {
        return Err(SigError::Malformed);   // 缺少 ')'
    }
    ++pos;   // 跳过 ')'

    SigKind ret = SigKind::Void;
    if (pos < descriptor.size() && descriptor[pos] == 'V') {
        ++pos;
    }
    else if (!parseFieldKind(descriptor, pos, ret)) {
        return Err(SigError::Malformed);
    }
    if (pos != descriptor.size()) {
        return Err(SigError::Malformed);   // 返回类型后还有残余
    }

    ca::usize slots = isStatic ? 0 : 1;   // this
    for (const SigKind kind : params) {
        slots += isCat2(kind) ? 2 : 1;
    }
    return Ok(MethodSig(std::move(params), ret, isStatic, slots));
}

}   // namespace mjbase::asm_evaluator
