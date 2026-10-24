#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "mapping_file_format.hpp"
#include "libca/str/utf8_string_arena.hpp"

namespace mjbase::mapping {

using MappingFormatFactory = std::function<std::unique_ptr<MappingFileFormat>()>;

/// @brief In-process registry for mapping file formats.
///
/// This mirrors Recaf's explicit format manager shape without introducing a
/// plugin ABI. Register formats during initialization, then create parser/writer
/// instances by stable implementation name.
class MappingFormatRegistry
{
public:
    /// @brief Registry preloaded with built-in formats.
    static MappingFormatRegistry withStandardFormats();

    /// @brief Register or replace one mapping file format factory.
    ///
    /// @return false when name is empty or factory is null.
    bool registerFormat(ca::str::Utf8StringRef name, MappingFormatFactory factory);

    /// @brief Return true when a format name is registered.
    bool hasFormat(ca::str::Utf8StringRef name) const;

    /// @brief Create a fresh format instance, or nullptr when not registered.
    std::unique_ptr<MappingFileFormat> createFormat(ca::str::Utf8StringRef name) const;

    /// @brief List known format names in deterministic order.
    std::vector<ca::str::Utf8StringRef> formatNames() const;

    /// @brief Parse text with a registered format.
    ///
    /// 未注册的格式名返回 Err(UnknownFormat)（原 mbase 静默返回空产物，改走
    /// 错误通道）；已注册格式的行级宽容语义。
    ca::Result<IntermediateMapping, MappingError> parse(
        ca::str::Utf8StringRef name, ca::str::Utf8StringRef mappingText) const;

    /// @brief Format mappings with a registered writer.
    ///
    /// 未修改的格式原文快照原样透传；未注册的格式名返回 Err(UnknownFormat)。
    ca::Result<ca::str::Utf8String, MappingError> format(
        ca::str::Utf8StringRef name, const IntermediateMapping& mappings) const;

private:
    // -- arena 死亡点声明（memory-governance-plan §5-P1③）--
    // 本成员持有本注册表私有的 Utf8StringArena：registerFormat() 把格式名
    // arena_.intern 后作为 factories_ 的 key，formatNames() 返回的 Utf8StringRef
    // 即这些 key 的视图。hasFormat/createFormat 不返回视图；parse() 返回的
    // IntermediateMapping 自带独立 arena，与本成员无关。
    // 视图寿命 = 本对象寿命；死亡点：随持有者析构（注册表由调用方按作业/命令
    // 作用域构造，无全局单例）。
    // 消费者不得把 formatNames() 的视图带过注册表的死亡点；需跨期持有则 intern
    // 进自己的 arena 或深拷贝。
    ca::str::Utf8StringArena                                         arena_;
    std::unordered_map<ca::str::Utf8StringRef, MappingFormatFactory> factories_;
};

/// @brief Register Recaf-parity and mjbase-specific mapping formats.
void registerStandardMappingFormats(MappingFormatRegistry& registry);

}   // namespace mjbase::mapping
