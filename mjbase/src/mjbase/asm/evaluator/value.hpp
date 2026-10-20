/// @file        value.hpp
/// @brief       asm/evaluator 具体值模型：数值四类 + 只读引用面。
///
/// 求值器是具体栈机：每个值都是精确已知的，不存在 unknown/部分已知状态
/// （旧 旧栈 evaluator 的 unknown 降级语义由 analyzer 域承接，本模块
/// 遇到不可求值的形状直接返回结构化错误，不猜、不静默降级）。
/// 引用面是只读子集：null、具长度的数组、不透明对象；仅支撑
/// null 判定、arraylength 读长度与按身份的引用相等，不建模对象内容。
#pragma once

#include "libca/core/datatype.hpp"

namespace mjbase::asm_evaluator {

/// @brief 栈/局部变量上的具体值。
///
/// 值语义、可自由拷贝；数值载荷按 kind 读取对应 union 成员。
/// `Top` 是局部变量占位标记（cat2 高槽、未初始化槽），不进操作数栈。
struct Value
{
    enum class Kind : ca::u8 {
        Top = 0,  ///< 占位：cat2 第二槽 / 未初始化局部变量
        I32,      ///< int/boolean/byte/char/short 计算类别
        I64,      ///< long（cat2）
        F32,      ///< float
        F64,      ///< double（cat2）
        Ref,      ///< 引用（null / 具长数组 / 不透明对象）
    };

    Kind kind = Kind::Top;

    /// 数值载荷：按 kind 读取活跃成员（其余成员不参与相等与语义）。
    union {
        ca::i32 i32;
        ca::i64 i64;
        ca::f32 f32;
        ca::f64 f64;
    };

    /// 引用载荷（kind == Ref 时有效）。
    bool refNull = false;       ///< true 表示 null 引用
    bool refIsArray = false;    ///< 非空且为数组时 true（arraylength 可读）
    ca::i32 refLength = 0;      ///< refIsArray 时的数组长度
    ca::u32 refId = 0;          ///< 非空引用的身份；if_acmp* 按此判定相等

    static Value top();
    static Value ofI32(ca::i32 v);
    static Value ofI64(ca::i64 v);
    static Value ofF32(ca::f32 v);
    static Value ofF64(ca::f64 v);
    /// null 引用：所有 null 互相相等，无需身份。
    static Value ofNull();

    /// JVM 计算类别 2（long/double，占双槽）。
    bool isCat2() const noexcept { return kind == Kind::I64 || kind == Kind::F64; }
    bool isRef() const noexcept { return kind == Kind::Ref; }
    bool isNull() const noexcept { return kind == Kind::Ref && refNull; }

    /// 引用相等（if_acmp* 语义）：null 只与 null 相等；非空按身份（refId）。
    bool sameRef(const Value& other) const noexcept;

    /// 内容相等（活跃字段逐一比较；测试断言用）。
    bool operator==(const Value& other) const noexcept;
    bool operator!=(const Value& other) const noexcept { return !(*this == other); }
};

/// @brief 非空引用的构造入口。
///
/// 引用需要身份：两个独立构造的引用即使形状（数组长度等）相同也不相等，
/// 与 JVM 引用语义一致。同一次求值的入参请用同一个 RefFactory 构造，
/// 需要表达「两个参数是同一个数组」时复用同一个 Value 拷贝即可
/// （拷贝保留身份）。
class RefFactory
{
public:
    /// 具长度的数组引用：仅支撑 arraylength / null 判定 / 引用相等。
    Value array(ca::i32 length);

    /// 不透明非空对象引用：除 null 判定与引用相等外不可观察。
    Value object();

private:
    ca::u32 nextId_ = 1;
};

}   // namespace mjbase::asm_evaluator
