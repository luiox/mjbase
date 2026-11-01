#include "mjbase/project/entry_shape.hpp"

namespace mjbase::project {

EntryPathShape classifyEntryPathShape(std::string_view path) noexcept
{
    if (path.empty()) return EntryPathShape::Empty;

    // 字节面 facet 按优先序分两轮扫描：控制字符/DEL 先于反斜杠（同路径
    // 双异常时取优先序最前者，与出现位置无关）。
    for (const char c : path) {
        const auto raw = static_cast<unsigned char>(c);
        if (raw < 0x20 || raw == 0x7f) return EntryPathShape::ControlChar;
    }
    if (path.find('\\') != std::string_view::npos) return EntryPathShape::Backslash;
    if (path.front() == '/') return EntryPathShape::LeadingSlash;

    // 结构面逐段扫描：中段空 = 连续斜杠（EmptySegment 优先于点段）；末段
    // 空 = 尾随 '/'，留到最后再判（TrailingSlash 优先序最低）。
    std::string_view::size_type start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto end   = (slash == std::string_view::npos) ? path.size() : slash;
        const std::string_view segment(path.data() + start, end - start);
        if (segment.empty()) {
            if (slash != std::string_view::npos) return EntryPathShape::EmptySegment;
            break;   // 末段空：尾随 '/' 情形，跳出走收尾判定
        }
        if (segment == "." || segment == "..") return EntryPathShape::DotSegment;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    if (path.back() == '/') return EntryPathShape::TrailingSlash;
    return EntryPathShape::Regular;
}

bool isRegularEntryPath(std::string_view path) noexcept
{
    return classifyEntryPathShape(path) == EntryPathShape::Regular;
}

std::string_view entryPathShapeName(EntryPathShape shape) noexcept
{
    switch (shape) {
    case EntryPathShape::Regular: return "regular";
    case EntryPathShape::Empty: return "empty";
    case EntryPathShape::ControlChar: return "control_char";
    case EntryPathShape::Backslash: return "backslash";
    case EntryPathShape::LeadingSlash: return "leading_slash";
    case EntryPathShape::EmptySegment: return "empty_segment";
    case EntryPathShape::DotSegment: return "dot_segment";
    case EntryPathShape::TrailingSlash: return "trailing_slash";
    }
    return "regular";
}

}   // namespace mjbase::project
