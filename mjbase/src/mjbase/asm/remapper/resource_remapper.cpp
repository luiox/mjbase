#include "mjbase/asm/remapper/resource_remapper.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/bytes.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/analysis/alias_analysis.hpp"
#include "mjbase/jar/manifest.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjbase::asm_remapper {
namespace {

using jbc::core::ByteView;
using jbc::core::ClassFile;
using mjbase::model::ClassResource;
using mjbase::model::IResource;
using mjbase::model::ManifestResource;
using mjbase::model::ResourceType;
using mjbase::model::UnknownResource;

std::string classResourceUri(const std::string& internalName)
{
    return internalName + ".class";
}

constexpr std::string_view kServicePrefix = "META-INF/services/";

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

std::string toLowerAscii(std::string value)
{
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

std::string toInternalNameToken(std::string value)
{
    for (auto& c : value) {
        if (c == '.') c = '/';
    }
    return value;
}

std::string restoreNameTokenShape(std::string_view original, std::string_view mapped)
{
    std::string value(mapped);
    if (original.find('.') != std::string_view::npos &&
        original.find('/') == std::string_view::npos) {
        for (auto& c : value) {
            if (c == '/') c = '.';
        }
    }
    return value;
}

bool isServiceResourceUri(std::string_view uri)
{
    return startsWith(uri, kServicePrefix) && uri.size() > kServicePrefix.size();
}

std::string remapServiceUri(const MemberRemapPlan& plan, std::string_view uri)
{
    if (!isServiceResourceUri(uri)) return std::string(uri);
    const std::string_view name    = uri.substr(kServicePrefix.size());
    const std::string      oldName = toInternalNameToken(std::string(name));
    const std::string_view mapped  = plan.mapInternalName(oldName);
    if (mapped == oldName) return std::string(uri);
    return std::string(kServicePrefix) + restoreNameTokenShape(name, mapped);
}

bool isProviderNameChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$' || c == '.' || c == '/';
}

std::string remapServiceProviderLine(const MemberRemapPlan& plan, std::string_view line,
                                     bool& changed)
{
    const auto        comment = line.find('#');
    const ca::usize   limit   = comment == std::string_view::npos ? line.size() : comment;
    ca::usize         tokenBegin = 0;
    while (tokenBegin < limit && !isProviderNameChar(line[tokenBegin])) tokenBegin++;
    ca::usize         tokenEnd = tokenBegin;
    while (tokenEnd < limit && isProviderNameChar(line[tokenEnd])) tokenEnd++;
    if (tokenBegin == tokenEnd) return std::string(line);

    const std::string      token    = std::string(line.substr(tokenBegin, tokenEnd - tokenBegin));
    const std::string      internal = toInternalNameToken(token);
    const std::string_view mapped   = plan.mapInternalName(internal);
    if (mapped == internal) return std::string(line);
    changed = true;
    return std::string(line.substr(0, tokenBegin)) +
           restoreNameTokenShape(token, mapped) + std::string(line.substr(tokenEnd));
}

std::vector<ca::u8> remapServiceProviderBytes(const MemberRemapPlan&      plan,
                                              const std::vector<ca::u8>& bytes, bool& changed)
{
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string       out;
    out.reserve(text.size());

    ca::usize lineBegin = 0;
    while (lineBegin < text.size()) {
        ca::usize lineEnd = lineBegin;
        while (lineEnd < text.size() && text[lineEnd] != '\n' && text[lineEnd] != '\r') lineEnd++;

        out += remapServiceProviderLine(plan,
                                        std::string_view(text).substr(lineBegin,
                                                                      lineEnd - lineBegin),
                                        changed);

        if (lineEnd < text.size()) {
            if (text[lineEnd] == '\r' && lineEnd + 1 < text.size() &&
                text[lineEnd + 1] == '\n') {
                out += "\r\n";
                lineEnd += 2;
            }
            else {
                out.push_back(text[lineEnd]);
                lineEnd++;
            }
        }
        lineBegin = lineEnd;
    }

    return std::vector<ca::u8>(out.begin(), out.end());
}

struct ServiceResourceUpdate
{
    std::string         oldUri;
    std::string         newUri;
    std::vector<ca::u8> bytes;
};

std::vector<ServiceResourceUpdate> collectServiceResourceUpdates(
    model::ResourceContainer& container, const MemberRemapPlan& plan, bool enabled)
{
    if (!enabled) return {};
    std::vector<ServiceResourceUpdate> updates;
    for (const auto& [uri, resource] : container.getResources()) {
        if (!resource || resource->getType() != ResourceType::Unknown ||
            !isServiceResourceUri(uri)) {
            continue;
        }
        auto* service = ca::core::tag_cast::cast<UnknownResource>(resource.get());
        if (service == nullptr) continue;

        bool                changed = false;
        auto                bytes   = remapServiceProviderBytes(plan, service->get(), changed);
        auto                newUri  = remapServiceUri(plan, uri);
        if (newUri != std::string(uri)) changed = true;
        if (!changed) continue;

        updates.push_back(ServiceResourceUpdate{std::string(uri), std::move(newUri),
                                                std::move(bytes)});
    }
    return updates;
}

void applyServiceResourceUpdates(model::ResourceContainer&                 container,
                                 const std::vector<ServiceResourceUpdate>& updates,
                                 bool&                                     changed)
{
    auto& resources = container.getResources();
    for (const auto& update : updates) {
        auto node = resources.find(update.oldUri);
        if (node == resources.end()) continue;

        if (update.oldUri != update.newUri &&
            resources.find(std::string_view(update.newUri)) == resources.end()) {
            // URI 换名 = 旧条目删除 + 新条目落入（与 renameClassResources 同款）。
            resources.erase(node);
            container.replaceResource(
                update.newUri,
                std::make_unique<UnknownResource>(update.newUri, std::move(update.bytes)));
        }
        else {
            auto* service = ca::core::tag_cast::cast<UnknownResource>(node->second.get());
            if (service != nullptr) service->set(std::move(update.bytes));
        }
        changed = true;
    }
}

bool remapManifestClassAttribute(const MemberRemapPlan& plan, mjbase::jar::Manifest& manifest,
                                 std::string_view key)
{
    const auto keyRef =
        ca::str::Utf8StringRef(reinterpret_cast<const ca::u8*>(key.data()), key.size(), key.size());
    const auto value = manifest.getMainAttribute(keyRef);
    const std::string_view text(reinterpret_cast<const char*>(value.data()),
                                value.byte_length());
    if (text.empty()) return false;

    const std::string      oldName = toInternalNameToken(std::string(text));
    const std::string_view mapped  = plan.mapInternalName(oldName);
    if (mapped == oldName) return false;

    const std::string restored = restoreNameTokenShape(text, mapped);
    manifest.setMainAttribute(keyRef,
                              ca::str::Utf8StringRef(reinterpret_cast<const ca::u8*>(
                                                         restored.data()),
                                                     restored.size(), restored.size()));
    return true;
}

bool remapManifestResource(const MemberRemapPlan& plan, ManifestResource& resource)
{
    mjbase::jar::Manifest manifest = resource.get();
    bool                  changed  = false;

    changed |= remapManifestClassAttribute(plan, manifest, "Main-Class");
    changed |= remapManifestClassAttribute(plan, manifest, "Launcher-Agent-Class");
    changed |= remapManifestClassAttribute(plan, manifest, "Premain-Class");
    changed |= remapManifestClassAttribute(plan, manifest, "Agent-Class");
    changed |= remapManifestClassAttribute(plan, manifest, "JavaFX-Application-Class");

    if (changed) resource.set(std::move(manifest));
    return changed;
}

bool isMappingTextResourceUri(std::string_view uri)
{
    const std::string lower = toLowerAscii(std::string(uri));
    return endsWith(lower, ".map") || endsWith(lower, ".mapping") || endsWith(lower, ".srg") ||
           endsWith(lower, ".tsrg") || endsWith(lower, ".tiny") || endsWith(lower, ".enigma") ||
           endsWith(lower, ".jadx") || endsWith(lower, ".pro") || endsWith(lower, ".proguard") ||
           endsWith(lower, ".r8");
}

bool looksLikeTextBytes(const std::vector<ca::u8>& bytes)
{
    for (const auto byte : bytes) {
        if (byte == 0) return false;
        if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') return false;
    }
    return true;
}

bool isMappingTokenChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$' || c == '/' || c == '.' || c == '[' || c == ']' ||
           c == '(' || c == ')' || c == ';';
}

std::string remapClassNameToken(const MemberRemapPlan& plan, std::string_view token)
{
    const std::string      internal = toInternalNameToken(std::string(token));
    const std::string_view mapped   = plan.mapInternalName(internal);
    if (mapped == internal) return std::string(token);
    return restoreNameTokenShape(token, mapped);
}

std::string remapMemberOwnerPrefix(const MemberRemapPlan& plan, std::string_view token,
                                   char separator)
{
    const auto pos = token.rfind(separator);
    if (pos == std::string_view::npos || pos == 0 || pos + 1 >= token.size()) {
        return std::string(token);
    }
    const std::string owner  = std::string(token.substr(0, pos));
    const std::string mapped = remapClassNameToken(plan, owner);
    if (mapped == owner) return std::string(token);
    return mapped + std::string(token.substr(pos));
}

std::string remapMappingToken(const MemberRemapPlan& plan, std::string_view token)
{
    if (token.empty()) return std::string(token);

    if (token.find('(') != std::string_view::npos) {
        const std::string_view mapped = plan.mapMethodDescriptor(token);
        if (mapped != token) return std::string(mapped);
    }
    if (token.front() == '[' ||
        (token.front() == 'L' && token.find(';') != std::string_view::npos)) {
        const std::string_view mapped = plan.mapFieldDescriptor(token);
        if (mapped != token) return std::string(mapped);
    }

    const std::string classMapped = remapClassNameToken(plan, token);
    if (classMapped != token) return classMapped;

    const std::string slashMemberMapped = remapMemberOwnerPrefix(plan, token, '/');
    if (slashMemberMapped != token) return slashMemberMapped;

    return remapMemberOwnerPrefix(plan, token, '.');
}

std::vector<ca::u8> remapMappingTextBytes(const MemberRemapPlan&      plan,
                                          const std::vector<ca::u8>& bytes, bool& changed)
{
    if (!looksLikeTextBytes(bytes)) return bytes;

    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string       out;
    out.reserve(text.size());

    for (ca::usize i = 0; i < text.size();) {
        if (!isMappingTokenChar(text[i])) {
            out.push_back(text[i]);
            i++;
            continue;
        }
        ca::usize end = i + 1;
        while (end < text.size() && isMappingTokenChar(text[end])) end++;

        const std::string token  = text.substr(i, end - i);
        const std::string mapped = remapMappingToken(plan, token);
        if (mapped != token) changed = true;
        out += mapped;
        i = end;
    }

    return std::vector<ca::u8>(out.begin(), out.end());
}

template <typename UriGate>
bool remapTextResourcesOfKind(model::ResourceContainer& container, const MemberRemapPlan& plan,
                              UriGate&& uriGate)
{
    bool changed = false;
    for (auto& [uri, resource] : container.getResources()) {
        if (!resource || resource->getType() != ResourceType::Unknown || !uriGate(uri)) {
            continue;
        }
        auto* textResource = ca::core::tag_cast::cast<UnknownResource>(resource.get());
        if (textResource == nullptr) continue;
        bool resourceChanged = false;
        auto bytes = remapMappingTextBytes(plan, textResource->get(), resourceChanged);
        if (!resourceChanged) continue;
        textResource->set(std::move(bytes));
        changed = true;
    }
    return changed;
}

bool isAuthorizedTextResourceUri(const RemapPolicy& policy, std::string_view uri)
{
    return std::find(policy.authorizedTextResourceUris.begin(),
                     policy.authorizedTextResourceUris.end(),
                     uri) != policy.authorizedTextResourceUris.end();
}

void renameClassResources(model::ResourceContainer&                                 container,
                          const std::vector<std::pair<std::string, std::string>>&   renames,
                          bool&                                                     changed)
{
    auto& resources = container.getResources();
    for (const auto& [oldUri, newUri] : renames) {
        if (oldUri == newUri) continue;
        auto node = resources.find(oldUri);
        if (node == resources.end()) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(node->second.get());
        if (cls == nullptr) continue;

        std::vector<ca::u8> bytes = cls->get();
        resources.erase(node);
        container.replaceResource(newUri,
                                  std::make_unique<ClassResource>(newUri, std::move(bytes)));
        changed = true;
    }
}

}   // namespace

Result<bool, RemapError> applyResourceRemapPlan(model::ResourceContainer& container,
                                                const MemberRemapPlan&      plan,
                                                const RemapPolicy&          policy)
{
    if (plan.empty()) return Ok(false);

    bool changed = false;

    // 反射别名闭包只在反射字符串面开启时构建（旧栈无条件构建；消费面只有
    // 反射轨道，此处惰性化是纯性能口径，结果语义不变）。
    mjbase::analysis::AliasAnalysis aliases;
    const analysis::AliasAnalysis*  aliasesPtr = nullptr;
    if (policy.reflectionStrings) {
        aliases.buildReflectionClosure(container);
        aliasesPtr = &aliases;
    }

    std::vector<std::pair<std::string, std::string>> resourceRenames;

    for (auto& [uri, resource] : container.getResources()) {
        if (!resource || resource->getType() != ResourceType::Class) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (cls == nullptr) continue;
        const auto& bytes = cls->get();
        if (bytes.empty()) continue;

        auto sessionResult = jbc::ir::makeSessionFromBytes(
            ByteView(bytes.data(), bytes.size()));
        if (sessionResult.is_err()) continue;   // 畸形类跳过
        auto session = std::move(sessionResult).unwrap();

        const std::string oldName{session->backing().thisName()};
        auto applied = applyMemberRemapPlan(*session, plan, policy, aliasesPtr);
        if (applied.is_err()) continue;   // 写回面真失败：该类保持原样（fail-open）
        if (applied.unwrap()) {
            auto written = jbc::core::writeClass(session->backing());
            if (written.is_err()) continue;
            cls->set(std::move(written).unwrap());
            changed = true;
        }

        if (policy.bytecode && policy.resources &&
            uri == std::string_view(classResourceUri(oldName))) {
            const std::string newUri =
                classResourceUri(std::string(plan.mapInternalName(oldName)));
            if (newUri != std::string(uri)) resourceRenames.emplace_back(std::string(uri), newUri);
        }
    }

    renameClassResources(container, resourceRenames, changed);
    if (policy.resources) {
        // META-INF/services 与 MANIFEST 主属性是 resources 面下的细分门
        //，默认开——与既有 resources=true 行为一致。
        applyServiceResourceUpdates(
            container, collectServiceResourceUpdates(container, plan, policy.serviceLoader),
            changed);
        changed |= remapTextResourcesOfKind(container, plan, isMappingTextResourceUri);
        changed |= remapTextResourcesOfKind(
            container, plan,
            [&policy](std::string_view uri) {
                return !isServiceResourceUri(uri) && !isMappingTextResourceUri(uri) &&
                       isAuthorizedTextResourceUri(policy, uri);
            });
        if (policy.manifest) {
            for (auto& [uri, resource] : container.getResources()) {
                (void)uri;
                if (!resource || resource->getType() != ResourceType::Manifest) continue;
                auto* manifestResource =
                    ca::core::tag_cast::cast<ManifestResource>(resource.get());
                if (manifestResource != nullptr) {
                    changed |= remapManifestResource(plan, *manifestResource);
                }
            }
        }
    }
    return Ok(changed);
}

Result<bool, RemapError> applyResourceRemapPlan(model::ResourceContainer& container,
                                                const MemberRemapPlan&    plan)
{
    return applyResourceRemapPlan(container, plan, RemapPolicy::bytecodeOnly());
}

}   // namespace mjbase::asm_remapper
