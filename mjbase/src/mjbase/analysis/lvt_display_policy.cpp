#include "mjbase/analysis/lvt_display_policy.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace mjbase::analysis {
namespace {

/// @brief ASCII 合法标识符字符（JLS 窄化口径：字母/数字/下划线/$，仅 ASCII）。
///
/// 控制字符（<0x20、0x7F）、非 ASCII（≥0x80）、反斜杠等一律不在集内，
/// 逐字符扫描即可拒绝全部非法形态。
bool legalNameChar(char character) noexcept
{
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_' || byte == '$';
}

/// @brief 基本类型描述符字符 → 类型后缀文本；非基本类型返回空。
std::string_view primitiveSuffix(char descriptor) noexcept
{
    switch (descriptor) {
    case 'B': return "byte";
    case 'C': return "char";
    case 'D': return "double";
    case 'F': return "float";
    case 'I': return "int";
    case 'J': return "long";
    case 'S': return "short";
    case 'Z': return "boolean";
    case 'V': return "void";
    default: return {};
    }
}

/// @brief 类型描述符 → 呈现后缀；畸形描述符返回空（调用方落 t<序号> 兜底）。
///
/// 对象类型取最后一段 '/' 后的简单名，并在 '<' 与 ';' 处截断——泛型签名
/// （LVTT 行 desc=签名文本）与 LVT 行同变量的描述符因此收敛同一后缀。
/// 数组递归元素后缀再接 "Array"。
std::string descriptorSuffix(std::string_view descriptor)
{
    if (descriptor.empty()) return {};
    if (descriptor.front() == '[') {
        auto element = descriptorSuffix(descriptor.substr(1));
        if (element.empty()) return {};
        element += "Array";
        return element;
    }
    if (descriptor.front() == 'L') {
        const auto body =
            descriptor.substr(1, descriptor.size() - (descriptor.back() == ';' ? 2 : 1));
        // 泛型实参（LVTT 签名文本）先在 '<' 处截断，再取最后一段 '/' 后的
        // 简单名——实参内类型不参与简单名判定（否则 rfind('/') 会落进实参）。
        const auto bracket = body.find('<');
        const auto head    = bracket == std::string_view::npos ? body : body.substr(0, bracket);
        const auto slash   = head.rfind('/');
        auto       simple  = slash == std::string_view::npos ? head : head.substr(slash + 1);
        const auto cut     = simple.find(';');
        if (cut != std::string_view::npos) simple = simple.substr(0, cut);
        if (simple.empty()) return {};
        return std::string(simple);
    }
    const auto primitive = primitiveSuffix(descriptor.front());
    if (!primitive.empty() && descriptor.size() == 1) return std::string(primitive);
    return {};
}

/// @brief 单条 LVT 行的 var 形态呈现名。
///
/// 同槽 ≥2 个不同描述符（多类型）加类型后缀；畸形描述符按 @p descs 首现序
/// 落 t<序号>（1 起）兜底。
std::string varDisplayName(const LvtDisplayRow& row, const std::vector<std::string_view>& descs)
{
    std::string text = "var" + std::to_string(row.slot);
    if (descs.size() < 2) return text;
    std::string suffix = descriptorSuffix(row.descriptor);
    if (suffix.empty()) {
        ca::usize ordinal = 1;
        for (const auto seen : descs) {
            if (seen == row.descriptor) break;
            ++ordinal;
        }
        suffix = "t" + std::to_string(ordinal);
    }
    text += '_';
    text += suffix;
    return text;
}

/// @brief 槽位 → 该槽去重描述符（首现序）的查找键。
struct SlotDescs final
{
    ca::u32                       slot = 0;
    std::vector<std::string_view> descs;
};

}   // namespace

bool lvtNameTrustworthy(std::string_view name) noexcept
{
    if (name.empty() || name.size() > kLvtMaxTrustedNameBytes) return false;
    const auto first = static_cast<unsigned char>(name.front());
    if (first >= '0' && first <= '9') return false;   // 首字符非数字（JLS 口径）
    for (const char character : name)
        if (!legalNameChar(character)) return false;
    return true;
}

bool lvtJarFallback(const LvtShapeStats& stats) noexcept
{
    return stats.total > 0 && stats.invalid * 2 >= stats.total;
}

std::vector<ca::str::Utf8String> lvtDisplayNames(const std::vector<LvtDisplayRow>& rows,
                                                 bool                              jarFallback)
{
    // 同槽去重描述符表（首现序）：多类型判定与畸形描述符序号兜底共用。
    // 槽数量与方法内局部变量同阶，线性查找足够；表在呈现前构建完成，
    // 呈现阶段只读（无再分配失效风险）。
    std::vector<SlotDescs> slots;
    for (const auto& row : rows) {
        SlotDescs* entry = nullptr;
        for (auto& candidate : slots) {
            if (candidate.slot == row.slot) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            slots.push_back(SlotDescs{row.slot, {}});
            entry = &slots.back();
        }
        if (std::find(entry->descs.begin(), entry->descs.end(), row.descriptor) ==
            entry->descs.end())
            entry->descs.push_back(row.descriptor);
    }
    auto descsOf = [&slots](ca::u32 slot) -> const std::vector<std::string_view>& {
        for (const auto& entry : slots)
            if (entry.slot == slot) return entry.descs;
        return slots.front().descs;   // 不可达：全部行的槽位已在构建阶段入表
    };

    std::vector<ca::str::Utf8String> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        if (!jarFallback && lvtNameTrustworthy(row.name)) {
            out.push_back(ca::str::Utf8String::from_data(
                reinterpret_cast<const ca::u8*>(row.name.data()), row.name.size()));
            continue;
        }
        const std::string text = varDisplayName(row, descsOf(row.slot));
        out.push_back(ca::str::Utf8String::from_data(
            reinterpret_cast<const ca::u8*>(text.data()), text.size()));
    }
    return out;
}

}   // namespace mjbase::analysis
