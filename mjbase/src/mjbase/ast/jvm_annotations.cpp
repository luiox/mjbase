#include "jvm_annotations.hpp"

#include <limits>
#include <type_traits>
#include <unordered_set>

#include "jbc/core/writer.hpp"
#include "jbc/type/descriptor.hpp"
#include "jstr/conversion.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/util.hpp"
#include "mjbase/util/annotation_depth.hpp"

namespace mjbase::ast {
namespace {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using jbc::core::AnnotationsAttr;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::ConstantPool;
using jbc::core::CpTag;
using jbc::ir::ClassSession;
using NativeAnnotation = AnnotationsAttr::Annotation;
using NativeValue = AnnotationsAttr::ElemValue;

constexpr ca::usize kU2Max = std::numeric_limits<ca::u16>::max();

bool isObjectDescriptor(std::string_view descriptor)
{
    return !util::descriptorToInternalName(descriptor).empty()
        && jbc::type::parseFieldType(descriptor).is_ok();
}

bool isClassLiteral(std::string_view descriptor)
{
    return descriptor == "V" || jbc::type::parseFieldType(descriptor).is_ok();
}

bool validName(std::string_view name)
{
    return !name.empty() && name.find_first_of(".;[/<>") == std::string_view::npos;
}

bool writableSymbol(std::string_view text)
{
    return text.size() <= kU2Max && jstr::mutf8_to_utf16(jstr::MUtf8StringRef(text)).is_ok();
}

std::vector<AttrSlot>* attributesOf(ClassSession& session, JvmAnnotationTarget target)
{
    auto& unit = session.unit();
    auto& file = session.backing();
    switch (target.kind) {
    case JvmAnnotationTarget::Kind::Class:
        return target.name.empty() && target.descriptor.empty() ? &file.attrs : nullptr;
    case JvmAnnotationTarget::Kind::Field: {
        const auto* field = unit.field(unit.findFieldId(target.name, target.descriptor));
        return field && field->backingIndex < file.fields.size()
            ? &file.fields[field->backingIndex].attrs : nullptr;
    }
    case JvmAnnotationTarget::Kind::Method: {
        const auto* method = unit.method(unit.findMethodId(target.name, target.descriptor));
        return method && method->backingIndex < file.methods.size()
            ? &file.methods[method->backingIndex].attrs : nullptr;
    }
    }
    return nullptr;
}

std::string_view attributeName(AnnotationVisibility visibility)
{
    return visibility == AnnotationVisibility::Runtime
        ? "RuntimeVisibleAnnotations" : "RuntimeInvisibleAnnotations";
}

struct AnnotationSlot
{
    std::vector<AttrSlot>& attributes;
    ca::usize index;
    const AnnotationsAttr* annotation;
};

Result<AnnotationSlot, AnnotationError>
loadSlot(ClassSession& session, JvmAnnotationTarget target, AnnotationVisibility visibility)
{
    auto* attributes = attributesOf(session, target);
    if (!attributes) return Err(AnnotationError::InvalidTarget);

    auto& file = session.backing();
    const auto name = attributeName(visibility);
    ca::usize index = attributes->size();
    for (ca::usize i = 0; i < attributes->size(); ++i) {
        if (!file.cp.utf8Equals((*attributes)[i].nameCp, name)) continue;
        if (index != attributes->size()) return Err(AnnotationError::Malformed);
        index = i;
    }
    if (index == attributes->size()) return Ok(AnnotationSlot{*attributes, index, nullptr});
    auto& slot = (*attributes)[index];
    if (!util::attrSafeToDecode(file, slot)) return Err(AnnotationError::Unsupported);
    const auto result = file.decodeAttr(slot);
    if (result.is_err()) return Err(AnnotationError::Malformed);
    const auto* ann = ca::core::tag_cast::cast<const AnnotationsAttr*>(result.unwrap());
    if (!ann) return Err(AnnotationError::Malformed);
    return Ok(AnnotationSlot{*attributes, index, ann});
}

bool hasConstant(const ConstantPool& cp, ca::u32 index, CpTag type)
{
    return cp.valid(index) && cp.tagOf(index) == type;
}

template <typename T>
Result<AnnotationValue, AnnotationError> readInteger(const ConstantPool& cp, ca::u32 index)
{
    if (!hasConstant(cp, index, CpTag::Integer)) return Err(AnnotationError::Malformed);
    const auto value = cp.at(index).ival;
    if (value < static_cast<ca::i32>(std::numeric_limits<T>::min())
        || value > static_cast<ca::i32>(std::numeric_limits<T>::max())) {
        return Err(AnnotationError::Malformed);
    }
    return Ok(AnnotationValue{static_cast<T>(value)});
}

Result<Annotation, AnnotationError> decodeAnnotation(const ConstantPool& cp,
    const NativeAnnotation& annotation, KeyPool& keys, ca::u32 depth);

Result<AnnotationValue, AnnotationError>
decodeValue(const ConstantPool& cp, const NativeValue& input, KeyPool& keys, ca::u32 depth)
{
    if (depth > util::kMaxAnnotationNesting) return Err(AnnotationError::Unsupported);
    switch (input.tag) {
    case 'B': return readInteger<ca::i8>(cp, input.cp);
    case 'C': return readInteger<char16_t>(cp, input.cp);
    case 'I': return readInteger<ca::i32>(cp, input.cp);
    case 'S': return readInteger<ca::i16>(cp, input.cp);
    case 'Z': return readInteger<bool>(cp, input.cp);
    case 'J':
        if (!hasConstant(cp, input.cp, CpTag::Long)) return Err(AnnotationError::Malformed);
        return Ok(AnnotationValue{cp.at(input.cp).lval});
    case 'F':
        if (!hasConstant(cp, input.cp, CpTag::Float)) return Err(AnnotationError::Malformed);
        return Ok(AnnotationValue{AnnotationFloat{static_cast<ca::u32>(cp.at(input.cp).ival)}});
    case 'D':
        if (!hasConstant(cp, input.cp, CpTag::Double)) return Err(AnnotationError::Malformed);
        return Ok(AnnotationValue{AnnotationDouble{static_cast<ca::u64>(cp.at(input.cp).lval)}});
    case 's': {
        if (!cp.isUtf8(input.cp)) return Err(AnnotationError::Malformed);
        auto text = jstr::mutf8_to_utf16(jstr::MUtf8StringRef(cp.utf8Of(input.cp)));
        if (text.is_err()) return Err(AnnotationError::Malformed);
        return Ok(AnnotationValue{std::move(text).unwrap()});
    }
    case 'c': {
        const auto descriptor = cp.utf8Of(input.cp);
        if (!isClassLiteral(descriptor)) return Err(AnnotationError::Malformed);
        return Ok(AnnotationValue{AnnotationClass{keys.intern(descriptor)}});
    }
    case 'e': {
        const auto descriptor = cp.utf8Of(input.typeNameCp);
        const auto name = cp.utf8Of(input.constNameCp);
        if (!isObjectDescriptor(descriptor) || !validName(name)) {
            return Err(AnnotationError::Malformed);
        }
        const auto owner = classKeyOf(keys, util::descriptorToInternalName(descriptor));
        return Ok(AnnotationValue{AnnotationEnum{memberKeyOf(keys, owner, name, descriptor)}});
    }
    case '@': {
        if (!input.nested) return Err(AnnotationError::Malformed);
        auto nested = decodeAnnotation(cp, *input.nested, keys, depth + 1);
        if (nested.is_err()) return Err(nested.unwrap_err());
        return Ok(AnnotationValue{std::make_unique<Annotation>(std::move(nested).unwrap())});
    }
    case '[': {
        AnnotationValue::Array values;
        values.reserve(input.items.size());
        for (const auto& item : input.items) {
            auto value = decodeValue(cp, item, keys, depth + 1);
            if (value.is_err()) return Err(value.unwrap_err());
            values.push_back(std::move(value).unwrap());
        }
        return Ok(AnnotationValue{std::move(values)});
    }
    }
    return Err(AnnotationError::Malformed);
}

Result<Annotation, AnnotationError> decodeAnnotation(const ConstantPool& cp,
    const NativeAnnotation& annotation, KeyPool& keys, ca::u32 depth)
{
    if (depth > util::kMaxAnnotationNesting) return Err(AnnotationError::Unsupported);
    const auto descriptor = cp.utf8Of(annotation.typeCp);
    if (!isObjectDescriptor(descriptor)) return Err(AnnotationError::Malformed);
    Annotation result{keys.intern(descriptor), {}};
    result.elements.reserve(annotation.values.size());
    for (const auto& [nameCp, input] : annotation.values) {
        const auto name = cp.utf8Of(nameCp);
        if (!validName(name)) return Err(AnnotationError::Malformed);
        auto value = decodeValue(cp, input, keys, depth);
        if (value.is_err()) return Err(value.unwrap_err());
        result.elements.emplace_back(keys.intern(name), std::move(value).unwrap());
    }
    return Ok(std::move(result));
}

Result<std::vector<Annotation>, AnnotationError>
decodeAnnotations(const ConstantPool& cp, const AnnotationsAttr* attr, KeyPool& keys)
{
    std::vector<Annotation> result;
    if (attr) {
        result.reserve(attr->annos.size());
        for (const auto& entry : attr->annos) {
            auto annotation = decodeAnnotation(cp, entry, keys, 0);
            if (annotation.is_err()) return Err(annotation.unwrap_err());
            result.push_back(std::move(annotation).unwrap());
        }
    }
    return Ok(std::move(result));
}

Result<NativeAnnotation, AnnotationError> encodeAnnotation(ClassSession& session,
    const Annotation& annotation, const KeyPool& keys, ca::u32 depth);

template <typename T>
constexpr ca::u8 integerTag()
{
    if constexpr (std::is_same_v<T, ca::i8>) return 'B';
    if constexpr (std::is_same_v<T, ca::i16>) return 'S';
    if constexpr (std::is_same_v<T, char16_t>) return 'C';
    if constexpr (std::is_same_v<T, bool>) return 'Z';
    return 'I';
}

Result<NativeValue, AnnotationError> encodeValue(ClassSession& session,
    const AnnotationValue& value, const KeyPool& keys, ca::u32 depth)
{
    if (depth > util::kMaxAnnotationNesting) return Err(AnnotationError::InvalidEdit);
    return std::visit([&](const auto& input) -> Result<NativeValue, AnnotationError> {
        using T = std::decay_t<decltype(input)>;
        NativeValue encoded;
        if constexpr (std::is_same_v<T, ca::i64>) {
            encoded.tag = 'J';
            encoded.cp = session.internLong(input);
        } else if constexpr (std::is_integral_v<T>) {
            encoded.tag = integerTag<T>();
            encoded.cp = session.internInteger(static_cast<ca::i32>(input));
        } else if constexpr (std::is_same_v<T, AnnotationFloat>) {
            encoded.tag = 'F';
            encoded.cp = session.internFloatBits(input.bits);
        } else if constexpr (std::is_same_v<T, AnnotationDouble>) {
            encoded.tag = 'D';
            encoded.cp = session.internDoubleBits(input.bits);
        } else if constexpr (std::is_same_v<T, std::u16string>) {
            auto result = jstr::utf16_to_mutf8(input);
            if (result.is_err()) return Err(AnnotationError::InvalidEdit);
            const auto text = std::move(result).unwrap();
            if (text.size() > kU2Max) return Err(AnnotationError::InvalidEdit);
            encoded.tag = 's';
            encoded.cp = session.internUtf8(text.view().as_std_view());
        } else if constexpr (std::is_same_v<T, AnnotationClass>) {
            const auto descriptor = keys.str(input.descriptor);
            if (!isClassLiteral(descriptor) || !writableSymbol(descriptor)) {
                return Err(AnnotationError::InvalidEdit);
            }
            encoded.tag = 'c';
            encoded.cp = session.internUtf8(descriptor);
        } else if constexpr (std::is_same_v<T, AnnotationEnum>) {
            const auto descriptor = keys.str(input.constant.desc);
            const auto owner = keys.str(input.constant.cls);
            const auto name = keys.str(input.constant.name);
            if (!isObjectDescriptor(descriptor) || !writableSymbol(descriptor)
                || util::descriptorToInternalName(descriptor) != owner
                || !validName(name) || !writableSymbol(name)) {
                return Err(AnnotationError::InvalidEdit);
            }
            encoded.tag = 'e';
            encoded.typeNameCp = session.internUtf8(descriptor);
            encoded.constNameCp = session.internUtf8(name);
        } else if constexpr (std::is_same_v<T, AnnotationValue::Nested>) {
            if (!input) return Err(AnnotationError::InvalidEdit);
            auto nested = encodeAnnotation(session, *input, keys, depth + 1);
            if (nested.is_err()) return Err(nested.unwrap_err());
            encoded.tag = '@';
            encoded.nested = std::make_unique<NativeAnnotation>(std::move(nested).unwrap());
        } else {
            static_assert(std::is_same_v<T, AnnotationValue::Array>);
            if (input.size() > kU2Max) return Err(AnnotationError::InvalidEdit);
            encoded.tag = '[';
            encoded.items.reserve(input.size());
            for (const auto& item : input) {
                auto element = encodeValue(session, item, keys, depth + 1);
                if (element.is_err()) return Err(element.unwrap_err());
                encoded.items.push_back(std::move(element).unwrap());
            }
        }
        return Ok(std::move(encoded));
    }, value.value);
}

Result<NativeAnnotation, AnnotationError> encodeAnnotation(ClassSession& session,
    const Annotation& annotation, const KeyPool& keys, ca::u32 depth)
{
    const auto descriptor = keys.str(annotation.descriptor);
    if (depth > util::kMaxAnnotationNesting || !writableSymbol(descriptor)
        || !isObjectDescriptor(descriptor) || annotation.elements.size() > kU2Max) {
        return Err(AnnotationError::InvalidEdit);
    }
    NativeAnnotation result;
    result.typeCp = session.internUtf8(descriptor);
    result.values.reserve(annotation.elements.size());
    std::unordered_set<ca::u32> names;
    for (const auto& [key, value] : annotation.elements) {
        const auto name = keys.str(key);
        if (!validName(name) || !writableSymbol(name) || !names.insert(key).second) {
            return Err(AnnotationError::InvalidEdit);
        }
        auto element = encodeValue(session, value, keys, depth);
        if (element.is_err()) return Err(element.unwrap_err());
        result.values.emplace_back(session.internUtf8(name), std::move(element).unwrap());
    }
    return Ok(std::move(result));
}

}   // namespace

Result<std::vector<Annotation>, AnnotationError>
readJvmAnnotations(ClassSession& session, JvmAnnotationTarget target,
                   AnnotationVisibility visibility, KeyPool& keys)
{
    const auto loaded = loadSlot(session, target, visibility);
    if (loaded.is_err()) return Err(loaded.unwrap_err());
    return decodeAnnotations(session.backing().cp, loaded.unwrap().annotation, keys);
}

Result<std::vector<ca::u8>, AnnotationError>
rewriteJvmAnnotations(jbc::core::ByteView bytes, JvmAnnotationTarget target,
                      AnnotationVisibility visibility, const JvmAnnotationEditor& edit)
{
    if (!edit) return Err(AnnotationError::InvalidEdit);
    auto read = jbc::ir::makeSessionFromBytes(bytes);
    if (read.is_err()) return Err(AnnotationError::Read);
    auto session = std::move(read).unwrap();
    const auto loaded = loadSlot(*session, target, visibility);
    if (loaded.is_err()) return Err(loaded.unwrap_err());
    const auto slot = loaded.unwrap();
    KeyPool keys;
    auto decoded = decodeAnnotations(session->backing().cp, slot.annotation, keys);
    if (decoded.is_err()) return Err(decoded.unwrap_err());
    auto annotations = std::move(decoded).unwrap();
    if (!edit(annotations, keys) || (!slot.annotation && annotations.empty())) {
        return Ok(std::vector<ca::u8>(bytes.data(), bytes.data() + bytes.size()));
    }

    if (annotations.size() > kU2Max) return Err(AnnotationError::InvalidEdit);
    std::vector<NativeAnnotation> encoded;
    encoded.reserve(annotations.size());
    std::unordered_set<ca::u32> descriptors;
    for (const auto& annotation : annotations) {
        if (!descriptors.insert(annotation.descriptor).second) return Err(AnnotationError::InvalidEdit);
        auto item = encodeAnnotation(*session, annotation, keys, 0);
        if (item.is_err()) return Err(item.unwrap_err());
        encoded.push_back(std::move(item).unwrap());
    }
    if (slot.annotation) {
        auto& attribute = slot.attributes[slot.index];
        if (encoded.empty() && slot.annotation->trailing.empty()) {
            slot.attributes.erase(slot.attributes.begin() + slot.index);
        } else {
            auto* typed = ca::core::tag_cast::cast<AnnotationsAttr*>(attribute.mutate());
            if (!typed) return Err(AnnotationError::Write);
            typed->annos = std::move(encoded);
        }
    } else {
        if (slot.attributes.size() >= kU2Max || session->backing().major < 49) {
            return Err(AnnotationError::InvalidEdit);
        }
        auto attr = std::make_unique<AnnotationsAttr>();
        attr->annos = std::move(encoded);
        slot.attributes.push_back(AttrSlot::makeNew(session->internUtf8(attributeName(visibility)),
                                                   std::move(attr)));
    }
    if (session->backing().cp.size() > kU2Max) return Err(AnnotationError::Write);
    auto bytesOut = jbc::core::writeClass(session->backing());
    if (bytesOut.is_err()) return Err(AnnotationError::Write);
    return Ok(std::move(bytesOut).unwrap());
}

}   // namespace mjbase::ast
