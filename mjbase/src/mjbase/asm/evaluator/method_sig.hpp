/// @file        method_sig.hpp
/// @brief       方法描述符的求值器视角：参数类别、槽位布局与返回类别。
///
/// 初始局部变量注入由描述符驱动：实例方法 local0 = this，随后按参数序
/// 入槽，J/D（cat2）占双槽（高槽置 Top）。解析是纯视图函数，不驻留、
/// 不分配字符串。
#pragma once

#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "value.hpp"

namespace mjbase::asm_evaluator {

// 只引入 Result 三件套（与 jbc::ir 的约定一致），避免整体 using。
using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief 描述符侧的类型类别（比栈上值类别多一个 Void，仅出现在返回位）。
enum class SigKind : ca::u8 {
    Void = 0,
    I32,  ///< B/C/I/S/Z
    I64,  ///< J
    F32,  ///< F
    F64,  ///< D
    Ref,  ///< L...; 与 [...
};

/// @brief 描述符解析失败类别。
enum class SigError : ca::u8 {
    Malformed = 0,  ///< 不是合法方法描述符（括号/类型序列残缺）
};

/// @brief 解析后的方法签名（求值器布局视图）。
class MethodSig
{
public:
    /// 解析方法描述符；isStatic 决定 local0 是否留给 this。
    static Result<MethodSig, SigError> parse(std::string_view descriptor, bool isStatic);

    bool isStatic() const noexcept { return isStatic_; }

    /// 参数个数（不含 this）。
    ca::usize paramCount() const noexcept { return params_.size(); }

    /// 第 i 个参数的类别（i < paramCount()）。
    SigKind paramKind(ca::usize i) const noexcept { return params_[i]; }

    /// this + 参数占用的局部变量槽数（cat2 双槽已计入）。
    ca::usize paramSlotCount() const noexcept { return slots_; }

    /// evaluate() 需要的入参数量：实例方法为 paramCount()+1（首元素 this）。
    ca::usize argCount() const noexcept { return params_.size() + (isStatic_ ? 0 : 1); }

    /// 返回值类别。
    SigKind returnKind() const noexcept { return ret_; }

private:
    MethodSig(std::vector<SigKind> params, SigKind ret, bool isStatic, ca::usize slots) noexcept;

    std::vector<SigKind> params_;
    SigKind ret_ = SigKind::Void;
    bool isStatic_ = true;
    ca::usize slots_ = 0;
};

/// @brief 描述符类别是否为 cat2（J/D，双槽）。
bool isCat2(SigKind kind) noexcept;

}   // namespace mjbase::asm_evaluator
