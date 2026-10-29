#include "mjbase/spi/pass_name_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;

const char* to_string(PassScope scope) noexcept
{
    switch (scope) {
    case PassScope::Method: return "Method";
    case PassScope::Class: return "Class";
    case PassScope::Global: return "Global";
    case PassScope::Cf: return "Cf";
    }
    return "?";
}

const char* to_string(RegistryError e) noexcept
{
    switch (e) {
    case RegistryError::Ok: return "Ok";
    case RegistryError::UnknownPass: return "UnknownPass";
    case RegistryError::DuplicateName: return "DuplicateName";
    case RegistryError::ScopeMismatch: return "ScopeMismatch";
    }
    return "?";
}

// ── 注册 ──

Result<void, RegistryError> PassNameRegistry::registerMethodPass(std::string_view name,
                                                                 MethodFactory    factory,
                                                                 std::string_view description)
{
    if (entries_.find(name) != entries_.end()) return Err(RegistryError::DuplicateName);
    Entry& e     = entries_[name];
    e.scope      = PassScope::Method;
    e.description  = description;
    e.method     = std::move(factory);
    e.clazz      = nullptr;
    e.global     = nullptr;
    e.cf         = nullptr;
    return Ok();
}

Result<void, RegistryError> PassNameRegistry::registerClassPass(std::string_view name,
                                                                ClassFactory     factory,
                                                                 std::string_view description)
{
    if (entries_.find(name) != entries_.end()) return Err(RegistryError::DuplicateName);
    Entry& e     = entries_[name];
    e.scope      = PassScope::Class;
    e.description  = description;
    e.method     = nullptr;
    e.clazz      = std::move(factory);
    e.global     = nullptr;
    e.cf         = nullptr;
    return Ok();
}

Result<void, RegistryError> PassNameRegistry::registerGlobalPass(std::string_view name,
                                                                 GlobalFactory    factory,
                                                                 std::string_view description)
{
    if (entries_.find(name) != entries_.end()) return Err(RegistryError::DuplicateName);
    Entry& e     = entries_[name];
    e.scope      = PassScope::Global;
    e.description  = description;
    e.method     = nullptr;
    e.clazz      = nullptr;
    e.global     = std::move(factory);
    e.cf         = nullptr;
    return Ok();
}

Result<void, RegistryError> PassNameRegistry::registerCfClassPass(std::string_view name,
                                                                  CfClassFactory   factory,
                                                                 std::string_view description)
{
    if (entries_.find(name) != entries_.end()) return Err(RegistryError::DuplicateName);
    Entry& e     = entries_[name];
    e.scope      = PassScope::Cf;
    e.description  = description;
    e.method     = nullptr;
    e.clazz      = nullptr;
    e.global     = nullptr;
    e.cf         = std::move(factory);
    return Ok();
}

// ── 查询 ──

const PassNameRegistry::Entry* PassNameRegistry::findEntry(std::string_view name) const
{
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

bool PassNameRegistry::has(std::string_view name) const { return findEntry(name) != nullptr; }

Result<PassNameEntry, RegistryError> PassNameRegistry::find(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    return Ok(PassNameEntry{name, e->scope, e->description});
}

std::vector<PassNameEntry> PassNameRegistry::list() const
{
    std::vector<PassNameEntry> out;
    out.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        out.push_back(PassNameEntry{name, entry.scope, entry.description});
    }
    // 存储是 unordered_map，list 输出面保持按名稳定序（CLI list 纪律）。
    std::sort(out.begin(), out.end(),
              [](const PassNameEntry& lhs, const PassNameEntry& rhs) { return lhs.name < rhs.name; });
    return out;
}

// ── 构造 ──

Result<std::unique_ptr<MethodPass>, RegistryError>
    PassNameRegistry::createMethodPass(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    if (e->scope != PassScope::Method || !e->method) return Err(RegistryError::ScopeMismatch);
    return Ok(e->method());
}

Result<std::unique_ptr<ClassPass>, RegistryError>
    PassNameRegistry::createClassPass(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    if (e->scope != PassScope::Class || !e->clazz) return Err(RegistryError::ScopeMismatch);
    return Ok(e->clazz());
}

Result<std::unique_ptr<GlobalPass>, RegistryError>
    PassNameRegistry::createGlobalPass(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    if (e->scope != PassScope::Global || !e->global) return Err(RegistryError::ScopeMismatch);
    return Ok(e->global());
}

Result<std::unique_ptr<CfClassPass>, RegistryError>
    PassNameRegistry::createCfClassPass(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    if (e->scope != PassScope::Cf || !e->cf) return Err(RegistryError::ScopeMismatch);
    return Ok(e->cf());
}

}   // namespace mjbase::spi
