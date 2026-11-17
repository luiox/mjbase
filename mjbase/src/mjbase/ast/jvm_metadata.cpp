/// @file jvm_metadata.cpp
/// @brief Preserve exact metadata anchors when a constant fold erases JVM instructions.
#include "jvm.hpp"

#include "jbc/core/attribute.hpp"
#include "mjbase/util/annotation_depth.hpp"

namespace mjbase::ast {
namespace {

/// Code metadata uses the original bytecode coordinates, including range ends.
class ProtectedPcs
{
public:
    explicit ProtectedPcs(ca::usize codeLength) : codeLength_(codeLength) {}

    bool add(ca::u32 pc)
    {
        if (pc >= codeLength_) return false;
        pcs.insert(pc);
        return true;
    }

    bool addRange(ca::u32 start, ca::u32 length)
    {
        const ca::u64 end = static_cast<ca::u64>(start) + length;
        if (end > codeLength_ || !add(start)) return false;
        return end == codeLength_ || add(static_cast<ca::u32>(end));
    }

    std::unordered_set<ca::u32> pcs;

private:
    ca::usize codeLength_;
};

bool collectStackMap(const jbc::core::StackMapTableAttr& table, ProtectedPcs& protectedPcs)
{
    if (!table.trailing.empty()) return false;
    ca::u64 absolute = 0;
    bool first = true;
    for (const auto& frame : table.frames) {
        absolute = first ? frame.offset : absolute + frame.offset + 1;
        first = false;
        if (absolute > 0xFFFFFFFFu || !protectedPcs.add(static_cast<ca::u32>(absolute)))
            return false;
        for (const auto* types : {&frame.locals, &frame.stack}) {
            for (const auto& type : *types) {
                if (type.tag == jbc::core::StackMapTableAttr::VType::kUninitialized
                    && !protectedPcs.add(type.cp))
                    return false;
            }
        }
    }
    return true;
}

bool collectTypeAnnotations(const jbc::core::TypeAnnotationsAttr& table,
                            const jbc::core::CodeAttr& code, ProtectedPcs& protectedPcs)
{
    using TypeAnnotations = jbc::core::TypeAnnotationsAttr;
    if (!table.trailing.empty()) return false;
    for (const auto& annotation : table.annotations) {
        const auto target = annotation.targetType;
        if (target == TypeAnnotations::kLocalVar || target == TypeAnnotations::kLocalVarResource) {
            for (const auto& range : annotation.localVars) {
                if (!protectedPcs.addRange(range.startPc, range.length)) return false;
            }
        } else if (target == TypeAnnotations::kCatch) {
            if (annotation.catchIndex >= code.excTable.size()) return false;
        } else if (target >= TypeAnnotations::kOffsetBase && target <= TypeAnnotations::kTypeArgMax) {
            if (!protectedPcs.add(annotation.offset)) return false;
        } else {
            // Other target_info forms belong to a class, field or method attribute.
            return false;
        }
    }
    return true;
}

}   // namespace

std::optional<std::unordered_set<ca::u32>>
jvmConstantFoldProtectedPcs(jbc::ir::ClassSession& session, jbc::ir::CodeUnit& code)
{
    const auto* method = session.unit().method(code.method);
    auto& file = session.backing();
    if (method == nullptr || method->code.get() != &code || method->backingIndex >= file.methods.size())
        return std::nullopt;

    jbc::core::CodeAttr* backingCode = nullptr;
    for (auto& slot : file.methods[method->backingIndex].attrs) {
        if (!file.cp.utf8Equals(slot.nameCp, "Code")) continue;
        if (backingCode != nullptr) return std::nullopt;
        auto decoded = file.decodeAttr(slot);
        if (decoded.is_err() || decoded.unwrap()->kind() != jbc::core::AttrKind::Code)
            return std::nullopt;
        backingCode = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
    }
    if (backingCode == nullptr || !backingCode->trailing.empty()) return std::nullopt;

    ProtectedPcs protectedPcs(backingCode->code.size());
    for (auto& slot : backingCode->attrs) {
        const bool lineNumbers = file.cp.utf8Equals(slot.nameCp, "LineNumberTable");
        const bool localVariables = file.cp.utf8Equals(slot.nameCp, "LocalVariableTable")
                                    || file.cp.utf8Equals(slot.nameCp, "LocalVariableTypeTable");
        const bool stackMap = file.cp.utf8Equals(slot.nameCp, "StackMapTable");
        const bool characterRanges = file.cp.utf8Equals(slot.nameCp, "CharacterRangeTable");
        const bool typeAnnotations = file.cp.utf8Equals(slot.nameCp, "RuntimeVisibleTypeAnnotations")
                                     || file.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleTypeAnnotations");
        if (!(lineNumbers || localVariables || stackMap || characterRanges || typeAnnotations)
            || !util::attrSafeToDecode(file, slot))
            return std::nullopt;
        auto decoded = file.decodeAttr(slot);
        if (decoded.is_err()) return std::nullopt;
        const auto* body = decoded.unwrap();
        if (lineNumbers && body->kind() == jbc::core::AttrKind::LineNumberTable) {
            const auto& table = *static_cast<const jbc::core::LineNumberTableAttr*>(body);
            if (!table.trailing.empty()) return std::nullopt;
            for (const auto& row : table.rows) {
                if (!protectedPcs.add(row.startPc)) return std::nullopt;
            }
        } else if (localVariables && body->kind() == jbc::core::AttrKind::LocalVariableTable) {
            const auto& table = *static_cast<const jbc::core::LocalVariableTableAttr*>(body);
            if (!table.trailing.empty()) return std::nullopt;
            for (const auto& row : table.rows) {
                if (!protectedPcs.addRange(row.startPc, row.length)) return std::nullopt;
            }
        } else if (stackMap && body->kind() == jbc::core::AttrKind::StackMapTable) {
            if (!collectStackMap(*static_cast<const jbc::core::StackMapTableAttr*>(body), protectedPcs))
                return std::nullopt;
        } else if (characterRanges && body->kind() == jbc::core::AttrKind::CharacterRangeTable) {
            const auto& table = *static_cast<const jbc::core::CharacterRangeTableAttr*>(body);
            if (!table.trailing.empty()) return std::nullopt;
            for (const auto& row : table.rows) {
                if (row.startPc > row.endPc || !protectedPcs.add(row.startPc)
                    || !protectedPcs.add(row.endPc))
                    return std::nullopt;
            }
        } else if (typeAnnotations && body->kind() == jbc::core::AttrKind::TypeAnnotations) {
            if (!collectTypeAnnotations(*static_cast<const jbc::core::TypeAnnotationsAttr*>(body),
                                        *backingCode, protectedPcs))
                return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    return std::move(protectedPcs.pcs);
}

}   // namespace mjbase::ast
