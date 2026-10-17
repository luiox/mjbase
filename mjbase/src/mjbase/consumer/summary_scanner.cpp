/// @file        summary_scanner.cpp
/// @brief       SummaryScanner 实现：libjbc core 骨架 → ClassSummary。
#include "mjbase/consumer/summary_scanner.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/error.hpp"
#include "jbc/core/reader.hpp"

namespace mjbase::consumer {
namespace {

/// @brief 纯扫描读选项：不保留源字节副本。
jbc::core::ReadOptions scanReadOptions()
{
    jbc::core::ReadOptions options;
    options.preserveSourceBytes = false;
    return options;
}

/// @brief 成员属性表中是否存在名为 name 的槽（hasCode 判定；不解码属性体）。
bool hasAttrNamed(const jbc::core::ClassFile& cf, const std::vector<jbc::core::AttrSlot>& attrs,
                  std::string_view name)
{
    for (const jbc::core::AttrSlot& slot : attrs) {
        if (cf.utf8(slot.nameCp) == name) return true;
    }
    return false;
}

}   // namespace

ConsumerResult<ClassSummary> SummaryScanner::scan(jbc::core::ByteView bytes, KeyPool& pool) const
{
    auto read = jbc::core::readClass(bytes, scanReadOptions());
    if (read.is_err()) {
        return ca::core::Err(ConsumerError{
            ErrorCode::BadClassFile,
            "summary scan: " + std::string{jbc::core::to_string(std::move(read).unwrap_err())}});
    }
    jbc::core::ReadResult       parsed = std::move(read).unwrap();
    const jbc::core::ClassFile& cf     = *parsed.file;

    ClassSummary summary;
    summary.access  = cf.access;
    summary.version = (static_cast<ca::u32>(cf.minor) << 16) | static_cast<ca::u32>(cf.major);
    summary.self    = classKeyOf(pool, cf.thisName());

    // 无父类（Object/module-info）：读侧物化空串，映射回 invalid 键。
    const std::string_view superName = cf.superName();
    summary.super = superName.empty() ? ClassKey{} : classKeyOf(pool, superName);

    summary.interfaces.reserve(cf.interfaces.size());
    for (const ca::u16 iface : cf.interfaces) {
        summary.interfaces.push_back(classKeyOf(pool, cf.className(iface)));
    }

    summary.methods.reserve(cf.methods.size());
    for (const jbc::core::MemberInfo& m : cf.methods) {
        SummaryMethod row;
        row.name    = pool.intern(cf.nameOf(m));
        row.desc    = pool.intern(cf.descOf(m));
        row.access  = m.access;
        row.hasCode = hasAttrNamed(cf, m.attrs, "Code");
        summary.methods.push_back(row);
    }

    summary.fields.reserve(cf.fields.size());
    for (const jbc::core::MemberInfo& f : cf.fields) {
        SummaryField row;
        row.name   = pool.intern(cf.nameOf(f));
        row.desc   = pool.intern(cf.descOf(f));
        row.access = f.access;
        summary.fields.push_back(row);
    }
    return ca::core::Ok(std::move(summary));
}

ConsumerResult<std::vector<ClassSummary>> SummaryScanner::scanSource(const IClassSource& source,
                                                                     KeyPool& pool) const
{
    std::vector<std::string_view> names;
    source.listClasses(names);
    std::vector<ClassSummary> summaries;
    summaries.reserve(names.size());
    for (const std::string_view name : names) {
        auto bytes = source.bytes(name);
        if (bytes.is_err()) {
            return ca::core::Err(std::move(bytes).unwrap_err());
        }
        auto scanned = scan(bytes.unwrap(), pool);
        if (scanned.is_err()) {
            ConsumerError failure = std::move(scanned).unwrap_err();
            failure.message += " (class " + std::string{name} + ")";
            return ca::core::Err(std::move(failure));
        }
        summaries.push_back(std::move(scanned).unwrap());
    }
    return ca::core::Ok(std::move(summaries));
}

}   // namespace mjbase::consumer
