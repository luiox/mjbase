#include "intermediate_mapping.hpp"

namespace mjbase::mapping {

void IntermediateMapping::markModified()
{
    ++revision_;
}

void IntermediateMapping::addPackage(ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName)
{
    auto       oldKey = arena_.intern(oldName);
    const auto inserted =
        packages_.emplace(oldKey, PackageMapping{oldKey, arena_.intern(newName)}).second;
    if (inserted) markModified();
}

void IntermediateMapping::addClass(ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName,
                                   ca::str::Utf8StringRef token)
{
    auto       oldKey = arena_.intern(oldName);
    const auto inserted =
        classes_.emplace(oldKey, ClassMapping{oldKey, arena_.intern(newName), arena_.intern(token)})
            .second;
    if (inserted) markModified();
}

void IntermediateMapping::addField(ca::str::Utf8StringRef ownerName, ca::str::Utf8StringRef desc,
                                   ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName,
                                   ca::str::Utf8StringRef token)
{
    auto ownerKey = arena_.intern(ownerName);
    fields_[ownerKey].push_back(FieldMapping{
        ownerKey, arena_.intern(desc), arena_.intern(oldName), arena_.intern(newName),
        arena_.intern(token)});
    markModified();
}

void IntermediateMapping::addMethod(ca::str::Utf8StringRef ownerName, ca::str::Utf8StringRef desc,
                                    ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName,
                                    ca::str::Utf8StringRef token)
{
    auto ownerKey = arena_.intern(ownerName);
    methods_[ownerKey].push_back(MethodMapping{
        ownerKey, arena_.intern(desc), arena_.intern(oldName), arena_.intern(newName),
        arena_.intern(token)});
    markModified();
}

const PackageMapping* IntermediateMapping::getPackageMapping(ca::str::Utf8StringRef name) const
{
    auto it = packages_.find(name);
    return it != packages_.end() ? &it->second : nullptr;
}

const ClassMapping* IntermediateMapping::getClassMapping(ca::str::Utf8StringRef name) const
{
    auto it = classes_.find(name);
    return it != classes_.end() ? &it->second : nullptr;
}

const std::vector<FieldMapping>* IntermediateMapping::getClassFieldMappings(
    ca::str::Utf8StringRef name) const
{
    auto it = fields_.find(name);
    return it != fields_.end() ? &it->second : nullptr;
}

const std::vector<MethodMapping>* IntermediateMapping::getClassMethodMappings(
    ca::str::Utf8StringRef name) const
{
    auto it = methods_.find(name);
    return it != methods_.end() ? &it->second : nullptr;
}

ca::str::Utf8StringRef IntermediateMapping::getMappedClassName(
    ca::str::Utf8StringRef internalName) const
{
    auto* cm = getClassMapping(internalName);
    if (cm) return cm->newName;

    for (const auto& [oldPackage, pm] : packages_) {
        if (!internalName.starts_with(oldPackage)) continue;

        ca::str::Utf8StringBuilder mapped;
        mapped.append(pm.newName);
        mapped.append(internalName.slice(oldPackage.byte_length(), internalName.byte_length()));
        auto mappedName = mapped.build();
        return arena_.intern(mappedName.ref());
    }
    return {};
}

ca::str::Utf8StringRef IntermediateMapping::getMappedFieldName(
    ca::str::Utf8StringRef ownerName, ca::str::Utf8StringRef fieldName,
    ca::str::Utf8StringRef fieldDesc) const
{
    auto* fieldsInClass = getClassFieldMappings(ownerName);
    if (!fieldsInClass) return {};
    for (const auto& field : *fieldsInClass) {
        if ((field.desc.is_empty() || field.desc.equals(fieldDesc)) &&
            field.oldName.equals(fieldName))
            return field.newName;
    }
    return {};
}

ca::str::Utf8StringRef IntermediateMapping::getMappedMethodName(
    ca::str::Utf8StringRef ownerName, ca::str::Utf8StringRef methodName,
    ca::str::Utf8StringRef methodDesc) const
{
    auto* methodsInClass = getClassMethodMappings(ownerName);
    if (!methodsInClass) return {};
    for (const auto& method : *methodsInClass) {
        if (method.desc.equals(methodDesc) && method.oldName.equals(methodName))
            return method.newName;
    }
    return {};
}

void IntermediateMapping::updateClass()
{
    if (!classes_.empty() || !packages_.empty()) markModified();
    for (auto& [oldName, cm] : classes_) {
        auto newOwner = arena_.intern(cm.newName);
        auto itM      = methods_.find(oldName);
        if (itM != methods_.end()) {
            for (auto& mm : itM->second) mm.ownerName = newOwner;
            auto node  = methods_.extract(itM);
            node.key() = newOwner;
            methods_.insert(std::move(node));
        }
        auto itF = fields_.find(oldName);
        if (itF != fields_.end()) {
            for (auto& fm : itF->second) fm.ownerName = newOwner;
            auto node  = fields_.extract(itF);
            node.key() = newOwner;
            fields_.insert(std::move(node));
        }
    }
    classes_.clear();
    packages_.clear();
}

void IntermediateMapping::updateField()
{
    if (!fields_.empty()) markModified();
    fields_.clear();
}

void IntermediateMapping::updateMethod()
{
    if (!methods_.empty()) markModified();
    methods_.clear();
}

void IntermediateMapping::putAll(const IntermediateMapping& other)
{
    for (const auto& [key, pm] : other.getPackages()) {
        addPackage(pm.oldName, pm.newName);
    }
    for (const auto& [key, cm] : other.getClasses()) {
        addClass(cm.oldName, cm.newName, cm.token);
    }
    for (const auto& [key, fields] : other.getFields()) {
        for (const auto& fm : fields) {
            addField(fm.ownerName, fm.desc, fm.oldName, fm.newName, fm.token);
        }
    }
    for (const auto& [key, methods] : other.getMethods()) {
        for (const auto& mm : methods) {
            addMethod(mm.ownerName, mm.desc, mm.oldName, mm.newName, mm.token);
        }
    }
}

std::unordered_map<ca::str::Utf8StringRef, ca::str::Utf8StringRef>
IntermediateMapping::exportClassMap() const
{
    std::unordered_map<ca::str::Utf8StringRef, ca::str::Utf8StringRef> out;
    out.reserve(classes_.size());
    for (const auto& [k, cm] : classes_) out.emplace(k, cm.newName);
    return out;
}

bool IntermediateMapping::hasClassMapping(ca::str::Utf8StringRef name) const
{
    return classes_.find(name) != classes_.end();
}

bool IntermediateMapping::isEmpty() const
{
    return packages_.empty() && classes_.empty() && fields_.empty() && methods_.empty();
}

void IntermediateMapping::rememberSource(ca::str::Utf8StringRef format, ca::str::Utf8StringRef text)
{
    sourceSnapshots_[format.to_std_string()] = text.to_std_string();
    sourceRevision_                          = revision_;
}

const std::string* IntermediateMapping::unmodifiedSource(ca::str::Utf8StringRef format) const
{
    if (sourceRevision_ != revision_) return nullptr;
    const auto it = sourceSnapshots_.find(format.to_std_string());
    return it == sourceSnapshots_.end() ? nullptr : &it->second;
}

}   // namespace mjbase::mapping
