#include "relaxed_verifier.hpp"

#include "jbytecode/analysis/analyzer.hpp"
#include "jbytecode/analysis/basic_interpreter.hpp"
#include "jbytecode/analysis/simple_verifier.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/readers.hpp"
#include "jbytecode/core/type.hpp"
#include "jbytecode/tree/nodes.hpp"

#include <exception>
#include <list>
#include <string>
#include <utility>

namespace mjbase::jasm {
namespace {

using jbytecode::analysis::Analyzer;
using jbytecode::analysis::AnalyzerException;
using jbytecode::analysis::BasicInterpreter;
using jbytecode::analysis::BasicValue;
using jbytecode::analysis::SimpleVerifier;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// @brief 层次解析视图：容器内已登记类走层次图（含接口实现）；未登记（外部/
///        JDK）类型一律不可解析——赋值判定由调用方按 fail-open 处置，
///        层次不可解析时不得判非法。
class HierarchyView final
{
public:
    explicit HierarchyView(const mjbase::hierarchy::ClassHierarchy* hierarchy)
        : hierarchy_(hierarchy)
    {}

    /// @brief 类型是否可解析（层次图已登记）；无图为恒 false。
    bool resolvable(const std::string& internalName) const
    {
        if (hierarchy_ == nullptr) return false;
        return hierarchy_->findClass(internalName).valid();
    }

    /// @brief 子类型判定（含接口实现）；两侧必须已登记（调用方先 resolvable）。
    bool isSubtype(const std::string& child, const std::string& parent) const
    {
        const mjbase::ClassKey childKey  = hierarchy_->findClass(child);
        const mjbase::ClassKey parentKey = hierarchy_->findClass(parent);
        if (!childKey.valid() || !parentKey.valid()) return false;
        return hierarchy_->isSubtype(childKey, parentKey);
    }

    /// @brief 公共超类（合并用）；任一侧不可解析回落 java/lang/Object。
    std::string commonSuperclass(const std::string& first, const std::string& second) const
    {
        if (first == second) return first;
        if (hierarchy_ != nullptr) {
            const mjbase::ClassKey firstKey  = hierarchy_->findClass(first);
            const mjbase::ClassKey secondKey = hierarchy_->findClass(second);
            if (firstKey.valid() && secondKey.valid()) {
                const mjbase::ClassKey commonKey = hierarchy_->commonAncestor(firstKey, secondKey);
                if (commonKey.valid()) return std::string{hierarchy_->nameOf(commonKey)};
            }
        }
        return "java/lang/Object";
    }

private:
    const mjbase::hierarchy::ClassHierarchy* hierarchy_;
};

/// @brief relaxed verifier：SimpleVerifier 的赋值/合并判定换 JVM 赋值兼容语义。
///
/// 形状照 libjhasm HierarchyVerifier（含 verifier 私有 type arena——分析期合成
/// Type 的 intern 落点，对象析构即回收），差异：
/// - isSubTypeOf 走 assignableNames 的 fail-open 子类型判定 + 数组协变；
/// - 不覆写 unary/binary/ternary/naryOperation——known-null 检查不产生。
class RelaxedVerifier final : public SimpleVerifier
{
public:
    RelaxedVerifier(const jbytecode::core::Type&              currentClass,
                    const jbytecode::core::Type&              currentSuperClass,
                    const std::vector<jbytecode::core::Type>& interfaces, bool isInterface,
                    const HierarchyView& hierarchy)
        : SimpleVerifier(currentClass, currentSuperClass, interfaces, isInterface)
        , hierarchy_(hierarchy)
    {}

protected:
    bool isSubTypeOf(const BasicValue& value, const BasicValue& expected) override
    {
        const auto* actualType   = value.getType();
        const auto* expectedType = expected.getType();
        if (actualType == nullptr || expectedType == nullptr)
            return actualType == nullptr && expectedType == nullptr;
        if (*actualType == *expectedType) return true;

        const int actualSort   = actualType->getSort();
        const int expectedSort = expectedType->getSort();

        // known-null 对任意引用目标合法（null 是万型子值）。
        if (*actualType == BasicInterpreter::NULL_TYPE)
            return expectedSort == jbytecode::core::Type::OBJECT ||
                   expectedSort == jbytecode::core::Type::ARRAY;

        // 引用↔原始混合：JVM 拒绝，保留。
        if (expectedSort != jbytecode::core::Type::OBJECT &&
            expectedSort != jbytecode::core::Type::ARRAY)
            return false;
        if (actualSort != jbytecode::core::Type::OBJECT &&
            actualSort != jbytecode::core::Type::ARRAY)
            return false;

        // 维度规约（照 SimpleVerifier 口径）：primitive 基座数组按标量 Object
        // 参与判定（int[] 是 Object，不是 Object[]）；引用元素数组保留元素名。
        int         actualDim       = 0;
        std::string actualElem;
        bool        actualScalarObject = false;   ///< primitive 基座提升形态
        if (actualSort == jbytecode::core::Type::ARRAY) {
            actualDim                            = actualType->getDimensions();
            const jbytecode::core::Type element = actualType->getElementType();
            if (element.getSort() == jbytecode::core::Type::OBJECT) {
                actualElem = internalName(element);
            }
            else {
                actualDim -= 1;
                actualScalarObject = true;
            }
        }
        else {
            actualElem = internalName(*actualType);
        }

        int         expectedDim  = 0;
        std::string expectedElem;
        if (expectedSort == jbytecode::core::Type::ARRAY) {
            expectedDim                        = expectedType->getDimensions();
            const jbytecode::core::Type element = expectedType->getElementType();
            // 引用值存入 primitive 元素数组：JVM 拒绝，保留精确拒绝。
            if (element.getSort() != jbytecode::core::Type::OBJECT) return false;
            expectedElem = internalName(element);
        }
        else {
            expectedElem = internalName(*expectedType);
        }

        if (actualDim < expectedDim) return false;
        if (actualDim > expectedDim) {
            // 实际维度更深：元素提升为 Object（String[] → Object 合法）。
            actualElem         = "java/lang/Object";
            actualScalarObject = false;
        }
        if (actualScalarObject) return assignableNames(expectedElem, "java/lang/Object");
        return assignableNames(expectedElem, actualElem);
    }

    BasicValue merge(const BasicValue& first, const BasicValue& second) override
    {
        const auto* firstType  = first.getType();
        const auto* secondType = second.getType();
        if (firstType == nullptr || secondType == nullptr || *firstType == *secondType)
            return SimpleVerifier::merge(first, second);
        const bool firstReference = firstType->getSort() == jbytecode::core::Type::OBJECT ||
                                    firstType->getSort() == jbytecode::core::Type::ARRAY;
        const bool secondReference = secondType->getSort() == jbytecode::core::Type::OBJECT ||
                                     secondType->getSort() == jbytecode::core::Type::ARRAY;
        if (firstReference != secondReference)
            throw AnalyzerException("Cannot merge object and non-object values");
        if (!firstReference || firstType->getSort() != jbytecode::core::Type::OBJECT ||
            secondType->getSort() != jbytecode::core::Type::OBJECT)
            return SimpleVerifier::merge(first, second);
        if (isNull(first)) return second;
        if (isNull(second)) return first;
        const std::string common =
            hierarchy_.commonSuperclass(internalName(*firstType), internalName(*secondType));
        types_.push_back(jbytecode::core::Type::getObjectType(
            ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(common.data()),
                                             common.size())));
        return newValue(&types_.back());
    }

private:
    static bool isNull(const BasicValue& value)
    {
        const auto* type = value.getType();
        return type != nullptr && *type == BasicInterpreter::NULL_TYPE;
    }

    std::string internalName(const jbytecode::core::Type& type)
    {
        const auto name = type.getInternalName(arena_);
        return std::string{reinterpret_cast<const char*>(name.data()), name.byte_length()};
    }

    /// @brief JVM 引用赋值兼容判定。
    ///
    /// 同名恒真；任一侧 java/lang/Object 恒真；两侧都可在层次图解析 → 子类型
    /// 判定（含接口实现）；任一侧不可解析（外部/JDK 类型无字节码可查）→ 不得
    /// 判非法（fail-open）。
    bool assignableNames(const std::string& expected, const std::string& actual)
    {
        if (expected == actual) return true;
        if (expected == "java/lang/Object") return true;
        if (!hierarchy_.resolvable(expected) || !hierarchy_.resolvable(actual)) return true;
        return hierarchy_.isSubtype(actual, expected);
    }

    const HierarchyView&            hierarchy_;
    std::list<jbytecode::core::Type> types_;   ///< merge 合成 Type 的稳定落点
    ca::str::Utf8StringArena         arena_;   ///< verifier 私有 intern 落点（析构回收）
};

std::vector<Diagnostic> analyzeRelaxedClass(const std::vector<ca::u8>&              classBytes,
                                            const mjbase::hierarchy::ClassHierarchy* hierarchy)
{
    std::vector<Diagnostic> diagnostics;
    try {
        ca::str::Utf8StringArena     arena;
        jbytecode::core::ClassReader reader(arena, classBytes);
        jbytecode::tree::ClassNode   clazz;
        reader.accept(&clazz, jbytecode::core::ClassReader::SKIP_FRAMES);

        const auto current = jbytecode::core::Type::getObjectType(clazz.name);
        const auto super   = clazz.superName.is_empty()
                                 ? jbytecode::core::Type{}
                                 : jbytecode::core::Type::getObjectType(clazz.superName);
        std::vector<jbytecode::core::Type> interfaces;
        interfaces.reserve(clazz.interfaces.size());
        for (const auto& name : clazz.interfaces)
            interfaces.push_back(jbytecode::core::Type::getObjectType(name));

        const HierarchyView view(hierarchy);
        for (auto* method : clazz.methods) {
            try {
                RelaxedVerifier verifier(current,
                                         super,
                                         interfaces,
                                         (clazz.access & jbytecode::core::Opcodes::ACC_INTERFACE) != 0,
                                         view);
                Analyzer<BasicValue> analyzer(&verifier);
                (void)analyzer.analyze(clazz.name, method);
            }
            catch (const std::exception& exception) {
                // 消息形状与 strict 路径一致（"Method <name><desc>: <what>"）。
                const std::string message = "Method " + method->name.to_std_string() +
                                            method->desc.to_std_string() + ": " +
                                            exception.what();
                diagnostics.push_back(
                    {DiagnosticSeverity::Error, DiagnosticStage::Validate, utf16(message),
                     std::nullopt});
            }
        }
    }
    catch (const std::exception& exception) {
        diagnostics.push_back(
            {DiagnosticSeverity::Error, DiagnosticStage::Validate, utf16(exception.what()),
             std::nullopt});
    }
    return diagnostics;
}

}   // namespace

std::vector<Diagnostic> analyzeRelaxed(const std::vector<ca::u8>&                    classBytes,
                                       const mjbase::hierarchy::ClassHierarchy* hierarchy)
{
    return analyzeRelaxedClass(classBytes, hierarchy);
}

}   // namespace mjbase::jasm
