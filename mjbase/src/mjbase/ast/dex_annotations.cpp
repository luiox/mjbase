#include "dex_annotations.hpp"

#include <string_view>
#include <unordered_set>

#include "dex/dex_file.hpp"
#include "jbc/type/descriptor.hpp"
#include "jstr/conversion.hpp"

namespace mjbase::ast {
namespace {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using dex::core::AnnotationItem;
using dex::core::DexFile;
using dex::core::EncodedAnnotation;
using dex::core::EncodedValue;
using dex::core::EncodedValueType;

std::string_view className(std::string_view descriptor)
{
    using jbc::type::TypeDesc;
    auto parsed = jbc::type::parseFieldType(descriptor);
    if (parsed.is_err()) return {};
    const auto type = parsed.unwrap();
    if (type.kind != TypeDesc::Kind::Class || type.array_dims != 0) return {};
    return type.class_name;
}

Result<AnnotationValue, AnnotationError>
readValue(DexFile& file, const EncodedValue& value, KeyPool& keys, ca::u32 depth);

Result<Annotation, AnnotationError>
readAnnotation(DexFile& file, const EncodedAnnotation& annotation, KeyPool& keys, ca::u32 depth)
{
    if (depth >= dex::core::kMaxValueDepth) return Err(AnnotationError::Malformed);
    auto type = file.typeDescriptor(annotation.typeIdx);
    if (type.is_err()) return Err(AnnotationError::Malformed);
    const auto descriptor = type.unwrap().as_std_view();
    if (className(descriptor).empty()) return Err(AnnotationError::Malformed);
    Annotation result{keys.intern(descriptor), {}};
    result.elements.reserve(annotation.elements.size());
    std::unordered_set<ca::u32> names;
    for (const auto& element : annotation.elements) {
        auto name = file.string(element.nameIdx);
        if (name.is_err() || name.unwrap().as_std_view().empty()) {
            return Err(AnnotationError::Malformed);
        }
        const auto key = keys.intern(name.unwrap().as_std_view());
        if (!names.insert(key).second) return Err(AnnotationError::Malformed);
        auto value = readValue(file, element.value, keys, depth);
        if (value.is_err()) return Err(value.unwrap_err());
        result.elements.emplace_back(key, std::move(value).unwrap());
    }
    return Ok(std::move(result));
}

Result<AnnotationValue, AnnotationError>
readEnum(DexFile& file, ca::u32 index, KeyPool& keys)
{
    auto owner = file.fieldClassDescriptor(index);
    auto name = file.fieldName(index);
    auto type = file.fieldTypeDescriptor(index);
    if (owner.is_err() || name.is_err() || type.is_err()) return Err(AnnotationError::Malformed);
    const auto descriptor = owner.unwrap().as_std_view();
    const auto internalName = className(descriptor);
    const auto memberName = name.unwrap().as_std_view();
    if (internalName.empty() || memberName.empty() || type.unwrap().as_std_view() != descriptor) {
        return Err(AnnotationError::Malformed);
    }
    const auto key = memberKeyOf(keys, classKeyOf(keys, internalName), memberName, descriptor);
    return Ok(AnnotationValue{AnnotationEnum{key}});
}

Result<AnnotationValue, AnnotationError>
readValue(DexFile& file, const EncodedValue& value, KeyPool& keys, ca::u32 depth)
{
    if (depth > dex::core::kMaxValueDepth) return Err(AnnotationError::Malformed);
    switch (value.type) {
    case EncodedValueType::Byte: return Ok(AnnotationValue{static_cast<ca::i8>(value.integer)});
    case EncodedValueType::Short: return Ok(AnnotationValue{static_cast<ca::i16>(value.integer)});
    case EncodedValueType::Char: return Ok(AnnotationValue{static_cast<char16_t>(value.bits)});
    case EncodedValueType::Boolean: return Ok(AnnotationValue{value.boolean});
    case EncodedValueType::Int: return Ok(AnnotationValue{static_cast<ca::i32>(value.integer)});
    case EncodedValueType::Long: return Ok(AnnotationValue{value.integer});
    case EncodedValueType::Float:
        return Ok(AnnotationValue{AnnotationFloat{static_cast<ca::u32>(value.bits)}});
    case EncodedValueType::Double: return Ok(AnnotationValue{AnnotationDouble{value.bits}});
    case EncodedValueType::String: {
        auto string = file.string(value.index);
        if (string.is_err()) return Err(AnnotationError::Malformed);
        auto decoded = jstr::mutf8_to_utf16(string.unwrap());
        if (decoded.is_err()) return Err(AnnotationError::Malformed);
        return Ok(AnnotationValue{std::move(decoded).unwrap()});
    }
    case EncodedValueType::Type: {
        auto type = file.typeDescriptor(value.index);
        if (type.is_err()) return Err(AnnotationError::Malformed);
        const auto descriptor = type.unwrap().as_std_view();
        if (descriptor != "V" && jbc::type::parseFieldType(descriptor).is_err()) {
            return Err(AnnotationError::Malformed);
        }
        return Ok(AnnotationValue{AnnotationClass{keys.intern(descriptor)}});
    }
    case EncodedValueType::Enum: return readEnum(file, value.index, keys);
    case EncodedValueType::Annotation: {
        if (!value.annotation) return Err(AnnotationError::Malformed);
        auto annotation = readAnnotation(file, *value.annotation, keys, depth + 1);
        if (annotation.is_err()) return Err(annotation.unwrap_err());
        return Ok(AnnotationValue{std::make_unique<Annotation>(std::move(annotation).unwrap())});
    }
    case EncodedValueType::Array: {
        if (depth >= dex::core::kMaxValueDepth) return Err(AnnotationError::Malformed);
        AnnotationValue::Array array;
        array.reserve(value.array.size());
        for (const auto& entry : value.array) {
            auto decoded = readValue(file, entry, keys, depth + 1);
            if (decoded.is_err()) return Err(decoded.unwrap_err());
            array.push_back(std::move(decoded).unwrap());
        }
        return Ok(AnnotationValue{std::move(array)});
    }
    default: return Err(AnnotationError::Unsupported);
    }
}

Result<void, AnnotationError>
appendDeclaration(DexFile& file, const AnnotationItem* item,
                  KeyPool& keys, std::vector<DeclarationAnnotation>& result)
{
    if (!item) return Err(AnnotationError::Malformed);
    using DexVisibility = dex::core::AnnotationVisibility;
    AnnotationVisibility visibility;
    switch (static_cast<DexVisibility>(item->visibility)) {
    case DexVisibility::System: return Ok();
    case DexVisibility::Build: visibility = AnnotationVisibility::Binary; break;
    case DexVisibility::Runtime: visibility = AnnotationVisibility::Runtime; break;
    default: return Err(AnnotationError::Malformed);
    }
    auto annotation = readAnnotation(file, item->annotation, keys, 0);
    if (annotation.is_err()) return Err(annotation.unwrap_err());
    result.push_back({visibility, std::move(annotation).unwrap()});
    return Ok();
}

}   // namespace

Result<std::vector<DeclarationAnnotation>, AnnotationError>
readDexClassAnnotations(DexFile& file, ca::u32 classIndex, KeyPool& keys)
{
    if (classIndex >= file.classDefs.size()) return Err(AnnotationError::InvalidTarget);
    std::vector<DeclarationAnnotation> result;
    const auto overlay = file.annotationAttachments().find(classIndex);
    if (overlay != file.annotationAttachments().end() && overlay->second.hasClass) {
        for (const auto index : overlay->second.classItems) {
            auto added = appendDeclaration(file, file.ownedAnnotationItem(index), keys, result);
            if (added.is_err()) return Err(added.unwrap_err());
        }
        return Ok(std::move(result));
    }
    auto directory = file.classAnnotations(classIndex);
    if (directory.is_err()) return Err(AnnotationError::Read);
    if (!directory.unwrap()) return Ok(std::move(result));
    auto set = file.annotationSet(directory.unwrap()->classAnnotationsOff);
    if (set.is_err()) return Err(AnnotationError::Read);
    if (!set.unwrap()) return Ok(std::move(result));
    for (const auto offset : set.unwrap()->annotationOffs) {
        auto item = file.annotationItem(offset);
        if (item.is_err()) return Err(AnnotationError::Read);
        auto added = appendDeclaration(file, item.unwrap(), keys, result);
        if (added.is_err()) return Err(added.unwrap_err());
    }
    return Ok(std::move(result));
}

}   // namespace mjbase::ast
