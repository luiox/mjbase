#include "mjbase/pass/source_name_restoration.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "mjbase/mapping/identifier_util.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjbase::pass {

namespace {

using mjbase::mapping::IdentifierUtil;
using mjbase::mapping::IntermediateMapping;

/// 源名 stem 长度上限（头注判据 2 的形状门上界，Recaf 同数）。
constexpr ca::usize kMaxSourceNameLength = 50;

ca::str::Utf8StringRef utf8Ref(std::string_view value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// ASCII word 字符（[A-Za-z0-9_]）判定。
bool isWordChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
           || c == '_';
}

/// SourceFile 形状判定（头注判据 2）：值整体 = 1..50 个 word 字符的 stem
/// + ".java"/".kt" 后缀。命中给出剥离后缀的源名，否则 nullopt。
std::optional<std::string> sourceStemOf(std::string_view value)
{
    std::string_view stem;
    if (value.size() > 5 && value.compare(value.size() - 5, 5, ".java") == 0) {
        stem = value.substr(0, value.size() - 5);
    }
    else if (value.size() > 3 && value.compare(value.size() - 3, 3, ".kt") == 0) {
        stem = value.substr(0, value.size() - 3);
    }
    else {
        return std::nullopt;
    }
    if (stem.empty() || stem.size() > kMaxSourceNameLength) return std::nullopt;
    for (const char c : stem) {
        if (!isWordChar(c)) return std::nullopt;
    }
    return std::string{stem};
}

/// 具名内部类行：InnerClasses 中「innerClassInfo == 本类 且 outerClassInfo
/// 非 0」的行存在。解码失败的属性不判内（该门不触发，交后续判据）。
bool hasNamedOuterRow(jbc::core::ClassFile& cf)
{
    jbc::core::AttrSlot* slot = cf.findClassAttr("InnerClasses");
    if (slot == nullptr) return false;
    if (cf.decodeAttr(*slot).is_err()) return false;
    const auto* body = ca::core::tag_cast::cast<const jbc::core::InnerClassesAttr*>(slot->peek());
    if (body == nullptr) return false;
    const std::string_view self = cf.thisName();
    for (const jbc::core::InnerClassesAttr::Row& row : body->rows) {
        if (row.outerClassInfoCp == 0) continue;
        if (!cf.cp.valid(row.innerClassInfoCp)) continue;
        if (cf.cp.tagOf(row.innerClassInfoCp) != jbc::core::CpTag::Class) continue;
        if (cf.className(row.innerClassInfoCp) != self) continue;   ///< 只看本类行
        return true;
    }
    return false;
}

/// 单类分析（头注判据 1-4）。
void analyzeClass(jbc::core::ClassFile& backing, IntermediateMapping& mapping)
{
    // 门 1：内部类跳过（匿名/局部形态带 EnclosingMethod；具名形态带本类
    // outer 行）——内部类沿用外围类 SourceFile，不跳过会与外围类映射相撞。
    if (backing.findClassAttr("EnclosingMethod") != nullptr) return;
    if (hasNamedOuterRow(backing)) return;

    // 门 2：SourceFile 形状。
    jbc::core::AttrSlot* slot = backing.findClassAttr("SourceFile");
    if (slot == nullptr) return;
    if (backing.decodeAttr(*slot).is_err()) return;
    const auto* body = ca::core::tag_cast::cast<const jbc::core::SourceFileAttr*>(slot->peek());
    if (body == nullptr) return;
    if (!backing.cp.valid(body->sourceFileCp)) return;
    const auto stem = sourceStemOf(backing.cp.utf8Of(body->sourceFileCp));
    if (!stem.has_value()) return;

    // 门 3：标识符合法（word 面允许数字开头名，非法标识符不可信不恢复）。
    if (!IdentifierUtil::isValidJavaIdentifier(utf8Ref(*stem))) return;

    // 门 4：产出（包前缀 + 源名；已正名零条目）。
    const std::string_view binary = backing.thisName();
    const ca::usize        slash  = binary.rfind('/');
    const std::string prefix =
        slash == std::string_view::npos ? std::string{} : std::string(binary.substr(0, slash + 1));
    const std::string restored = prefix + *stem;
    if (restored == binary) return;
    mapping.addClass(utf8Ref(binary), utf8Ref(restored));
}

}   // namespace

IntermediateMapping SourceNameRestorationAnalysis::run(mjbase::model::ResourceContainer&    container,
                                                       mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    IntermediateMapping mapping;
    for (auto& [uri, res] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(res.get());
        if (cls == nullptr) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) continue;   // 畸形类跳过（class_scan fail-open 同口径）
        analyzeClass(std::move(sessionResult).unwrap()->backing(), mapping);
    }
    return mapping;
}

mjbase::spi::PreservedAnalyses SourceNameRestorer::run(mjbase::model::ResourceContainer&    container,
                                                       mjbase::spi::GlobalAnalysisManager& am)
{
    am.registerAnalysis<SourceNameRestorationAnalysis>();     // 幂等：重复注册不覆盖
    (void)am.getResult<SourceNameRestorationAnalysis>(container);   // 触发计算并缓存
    return mjbase::spi::PreservedAnalyses::all();   // 只读：字节零改动
}

}   // namespace mjbase::pass
