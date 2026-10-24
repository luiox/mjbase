/// @file        manifest.hpp
/// @brief       MANIFEST.MF 模型：字节级保序解析与序列化。
///
/// 字符串存储用 ca::str::Utf8String
/// 家族（条目名/属性值按原始字节经 from_data_unchecked 直通，不校验、不改写），
/// 解析与序列化全部按字节进行，输出字节与 mbase 逐字节一致。
#pragma once

#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::jar {

using ca::str::Utf8String;
using ca::str::Utf8StringRef;

/// MANIFEST.MF 模型。
/// 支持从字节解析和序列化回字节，提供主属性区读写。
///
/// 存储必须是保序结构（vector 而非 unordered_map）：toBytes 的输出字节进入
/// jar 条目并被字节级 golden 测试锁定，序列化行序必须确定且与输入一致。
/// std::unordered_map 的遍历序属实现定义（std::hash 与桶增长策略各标准库
/// 不同），libstdc++ 与 MSVC 对同两组属性给出相反行序 → 同一管线在
/// Windows/linux 产出不同的 MANIFEST.MF 字节（CI 实证：golden 字节比对仅
/// MANIFEST.MF 条目失配，两行互换、总字节数相同）。行序对 JVM 语义无影响，
/// 但产出必须平台无关，故按插入序线性存储；属性个数量级为个位数，线性
/// 查找无性能影响。
class Manifest
{
public:
    Manifest() = default;

    /// @brief Utf8String 不可隐式复制（显式 clone），拷贝用 clone 逐对重建，
    ///        保持 mbase 版 Manifest 的值语义（mbase 消费面存在按值拷贝）。
    Manifest(const Manifest& other);
    Manifest& operator=(const Manifest& other);
    Manifest(Manifest&& other)            = default;
    Manifest& operator=(Manifest&& other) = default;

    /// 从 MANIFEST.MF 格式的字节创建 Manifest 对象。
    /// @param bytes 原始字节
    static Manifest fromBytes(const std::vector<ca::u8>& bytes);

    /// 序列化回 MANIFEST.MF 格式字节。
    std::vector<ca::u8> toBytes() const;

    /// 获取所有主属性（插入序 = 原 MANIFEST.MF 行序；对既有 key 赋值不改变
    /// 位置，仅覆写值；新 key 追加尾部）。
    const std::vector<std::pair<Utf8String, Utf8String>>& getMainAttributes() const
    {
        return mainAttributes_;
    }

    /// 获取指定主属性值，不存在返回空串。
    Utf8String getMainAttribute(const Utf8StringRef& key) const
    {
        for (const auto& [k, v] : mainAttributes_) {
            if (k == key) return v.clone();
        }
        return Utf8String{};
    }

    /// 设置主属性（既有 key 原位覆写；新 key 追加尾部）。
    void setMainAttribute(const Utf8StringRef& key, const Utf8StringRef& value)
    {
        for (auto& [k, v] : mainAttributes_) {
            if (k == key) {
                v = Utf8String::from_data_unchecked(value.data(), value.byte_length());
                return;
            }
        }
        mainAttributes_.emplace_back(
            Utf8String::from_data_unchecked(key.data(), key.byte_length()),
            Utf8String::from_data_unchecked(value.data(), value.byte_length()));
    }

private:
    /// 主属性，按插入序存储（见类注释：序列化行序确定性要求）。
    std::vector<std::pair<Utf8String, Utf8String>> mainAttributes_;
};

}   // namespace mjbase::jar
