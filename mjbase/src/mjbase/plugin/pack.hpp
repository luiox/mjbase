#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/spi/pass_name_registry.hpp"

namespace mjbase::plugin {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// 本构建所链 mjbase 的提交哈希，编译期由构建脚本注入（实现见
/// build_stamp.cpp）。
const char* buildCommit() noexcept;

/// pack 装配失败码。
enum class PackErrorKind : ca::u8
{
    Ok = 0,
    BuildMismatch,       ///< pack 与宿主链接的 mjbase 不是同一提交构建
    InvalidManifest,     ///< name 为空 / 含非法字符 / 含点，或未注册任何 pass
    NamespaceViolation,  ///< 注册了 "<pack>." 前缀之外的名字
    RegistrationFailed,  ///< 注册面拒绝（DuplicateName 等），detail 携带 pass 名
    ConfigRejected,      ///< pack 拒绝宿主传入的配置文本
};

inline const char* to_string(PackErrorKind kind) noexcept
{
    switch (kind) {
    case PackErrorKind::Ok:                 return "Ok";
    case PackErrorKind::BuildMismatch:      return "BuildMismatch";
    case PackErrorKind::InvalidManifest:    return "InvalidManifest";
    case PackErrorKind::NamespaceViolation: return "NamespaceViolation";
    case PackErrorKind::RegistrationFailed: return "RegistrationFailed";
    case PackErrorKind::ConfigRejected:     return "ConfigRejected";
    }
    return "?";
}

struct PackError
{
    PackErrorKind kind = PackErrorKind::Ok;
    std::string   detail;   ///< 直述文本（日志/CLI 展示）；pack 边界属外部边界，std::string 允许
};

/// pack 自述；name 同时是该 pack 全部 pass 名字的强制前缀（分仓撞名防线）。
/// name/mjbaseCommit/description 均为借用视图，字符串实体由 pack 自持
/// （惯例：静态存储或 pack 成员），寿命覆盖注册表存活期。
struct PackManifest
{
    std::string_view name;          ///< 单段 [a-z0-9_]，如 "dasho"
    std::string_view mjbaseCommit;  ///< pack 所链 mjbase 的构建戳（惯例：buildCommit()）
    std::string_view description = {};
};

/// @brief pass-pack 接口：一个独立仓库的变换族向宿主注册的唯一缝。
///
/// 契约要点（全文见 doc_ai/plugin-api.md）：
/// - pack 实例由宿主以引用持有，宿主不经本接口 delete（无虚析构）；
/// - registerPasses 只在宿主装配阶段调用，非线程安全（对齐注册表纪律）；
/// - config 为 UTF-8 JSON 文本（借用，仅调用期有效），pack 自行解析并闭包进
///   工厂——注册表工厂无参，pack 级配置靠闭包注入，不改注册表；
/// - 注册的 pass 名一律 "<name>.<Pass>"，名字与 description 实体由 pack 自持；
/// - 实现不抛异常跨界，失败一律走 Err 通道。
class IPassPack
{
public:
    virtual const PackManifest& manifest() const = 0;

    /// 把本 pack 全部 pass 注册进 registry。
    virtual Result<void, PackError> registerPasses(spi::PassNameRegistry& registry,
                                                   std::string_view config) = 0;

protected:
    IPassPack() = default;
    ~IPassPack() = default;

private:
    IPassPack(const IPassPack&)            = delete;
    IPassPack& operator=(const IPassPack&) = delete;
};

/// pack 侧便利：构造 "<pack>.<pass>" 限定名。返回值必须转存为 pack 自持
/// 存储（成员/静态）后再注册，不得以临时体借给注册表。
inline std::string qualifiedName(std::string_view pack, std::string_view pass)
{
    std::string out;
    out.reserve(pack.size() + 1 + pass.size());
    out.append(pack.data(), pack.size());
    out.push_back('.');
    out.append(pass.data(), pass.size());
    return out;
}

inline PackError makePackError(PackErrorKind kind, std::string_view detail)
{
    PackError e;
    e.kind   = kind;
    e.detail.assign(detail.data(), detail.size());
    return e;
}

/// @brief 宿主唯一装配入口：同源门 → manifest 校验 → 委托注册 → 非空与命名
///        空间验收。任何一关不过即 Err；注册表可能已留下部分注册条目，宿主
///        应丢弃整表重建，不做单 pass 回滚。
///
/// 同源门：pack 声明的 mjbaseCommit 与宿主自身 buildCommit() 不一致即拒绝
/// ——接口不设人工版本号，注入面的任何变化都体现为 mjbase 提交变化，异源
/// 构建在此处 fail-closed。前缀验收机械执行：装配前后 registry.list() 的
/// 差集里每个新名字必须以 "<name>." 开头，不依赖 pack 自觉。
inline Result<void, PackError> installPack(IPassPack& pack, spi::PassNameRegistry& registry,
                                           std::string_view config = {})
{
    const PackManifest& mf = pack.manifest();
    if (mf.mjbaseCommit != std::string_view(buildCommit())) {
        std::string detail(mf.mjbaseCommit);
        detail += " != ";
        detail += buildCommit();
        return Err(makePackError(PackErrorKind::BuildMismatch, detail));
    }
    if (mf.name.empty()) {
        return Err(makePackError(PackErrorKind::InvalidManifest, "pack name is empty"));
    }
    for (const char c : mf.name) {
        const bool legal = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!legal) {
            return Err(makePackError(PackErrorKind::InvalidManifest,
                                     "pack name must be a single [a-z0-9_] segment"));
        }
    }

    const std::vector<spi::PassNameEntry> before = registry.list();

    if (Result<void, PackError> r = pack.registerPasses(registry, config); r.is_err()) {
        return r;
    }

    const std::vector<spi::PassNameEntry> after = registry.list();
    std::string prefix;
    prefix.reserve(mf.name.size() + 1);
    prefix.append(mf.name.data(), mf.name.size());
    prefix.push_back('.');

    ca::usize fresh  = 0;
    ca::usize bi     = 0;
    for (const spi::PassNameEntry& entry : after) {
        // 两表同名字典序；跳过 before 已有条目，其余即本 pack 新注册的名字。
        while (bi < before.size() && before[bi].name < entry.name) {
            ++bi;
        }
        if (bi < before.size() && before[bi].name == entry.name) {
            ++bi;
            continue;
        }
        if (entry.name.substr(0, prefix.size()) != std::string_view(prefix)) {
            return Err(makePackError(PackErrorKind::NamespaceViolation, entry.name));
        }
        ++fresh;
    }
    if (fresh == 0) {
        return Err(makePackError(PackErrorKind::InvalidManifest, "pack registered no pass"));
    }
    return Ok();
}

}   // namespace mjbase::plugin
