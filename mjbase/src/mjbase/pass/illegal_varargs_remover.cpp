#include "mjbase/pass/illegal_varargs_remover.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

#include "jbc/core/class_file.hpp"
#include "libca/core/datatype.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::ClassFile;
using jbc::core::MemberInfo;

// ── JVM 访问标志位（JVMS 4.6；meta 家族同款本地常量纪律）。──
constexpr ca::u16 kAccVarargs = 0x0080;

/// 方法描述符尾参是否阵列类型（JVMS 4.3 参数段解析：`(` 起逐 FieldType
/// 消费至 `)`，FieldType = `[`* 基类型；尾参含数组维度即阵列尾参；无参
/// 描述符返回 false）。形状非法（无 `(`/`)`、参数段残缺、基类型字符不在
/// JVMS 集内）返回 nullopt——调用方 fail-closed 跳过该方法，两向都不改。
std::optional<bool> descTailParamIsArray(std::string_view desc)
{
    if (desc.empty() || desc.front() != '(') return std::nullopt;
    ca::usize pos         = 1;
    bool      tailIsArray = false;
    while (pos < desc.size() && desc[pos] != ')') {
        ca::usize dims = 0;
        while (pos < desc.size() && desc[pos] == '[') {
            ++dims;
            ++pos;
        }
        if (pos >= desc.size()) return std::nullopt;
        const char base = desc[pos];
        if (base == 'L') {
            ++pos;
            const ca::usize semi = desc.find(';', pos);
            if (semi == std::string_view::npos) return std::nullopt;
            pos = semi + 1;
        }
        else if (base == 'B' || base == 'C' || base == 'D' || base == 'F' || base == 'I'
                 || base == 'J' || base == 'S' || base == 'Z') {
            ++pos;
        }
        else {
            return std::nullopt;
        }
        tailIsArray = dims > 0;
    }
    if (pos >= desc.size()) return std::nullopt;   // `)` 未闭合
    return tailIsArray;
}

}   // namespace

// ── IllegalVarargsRemover ──

mjbase::spi::PreservedAnalyses IllegalVarargsRemover::run(
    jbc::ir::ClassSession& session, mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile&          cf   = session.backing();
    jbc::ir::ClassUnit& unit = session.unit();
    bool                changed = false;

    for (std::size_t i = 0; i < cf.methods.size(); ++i) {
        MemberInfo& m = cf.methods[i];
        const std::optional<bool> tailIsArray = descTailParamIsArray(cf.descOf(m));
        if (!tailIsArray.has_value()) continue;   // 描述符形状非法：fail-closed
        const bool hasBit = (m.access & kAccVarargs) != 0;
        if (*tailIsArray && !hasBit) {
            // 位无 + 阵列尾参：置位（修复方向）。
            m.access    = static_cast<ca::u16>(m.access | kAccVarargs);
            unit.methods[i].access |= static_cast<ca::u32>(kAccVarargs);
            changed = true;
        }
        else if (!*tailIsArray && hasBit) {
            // 位有 + 无阵列尾参：摘位（Recaf 对齐方向）。
            m.access    = static_cast<ca::u16>(m.access & ~kAccVarargs);
            unit.methods[i].access &= ~static_cast<ca::u32>(kAccVarargs);
            changed = true;
        }
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
