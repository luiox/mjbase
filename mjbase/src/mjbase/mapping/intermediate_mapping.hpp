#pragma once
#include <vector>
#include <unordered_map>
#include <string>
#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

namespace mjbase::mapping {

struct ClassMapping
{
    ca::str::Utf8StringRef oldName;
    ca::str::Utf8StringRef newName;
    /// shadow sidecar v2 专用载荷（#1156 token8）：仅 Shadow-Alias 格式读写，
    /// 其余映射格式与真实 remap 路径恒空（不序列化、不消费）。
    ca::str::Utf8StringRef token = {};
};

struct PackageMapping
{
    ca::str::Utf8StringRef oldName;
    ca::str::Utf8StringRef newName;
};

struct FieldMapping
{
    ca::str::Utf8StringRef ownerName;
    ca::str::Utf8StringRef desc;
    ca::str::Utf8StringRef oldName;
    ca::str::Utf8StringRef newName;
    /// shadow sidecar v2 专用载荷（#1156 token8）：仅 Shadow-Alias 格式读写。
    ca::str::Utf8StringRef token = {};

    bool hasType() const { return !desc.is_empty(); }
};

struct MethodMapping
{
    ca::str::Utf8StringRef ownerName;
    ca::str::Utf8StringRef desc;
    ca::str::Utf8StringRef oldName;
    ca::str::Utf8StringRef newName;
    /// shadow sidecar v2 专用载荷（#1156 token8）：仅 Shadow-Alias 格式读写。
    ca::str::Utf8StringRef token = {};
};

class IntermediateMapping
{
public:
    void addPackage(ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName);
    void addClass(ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName,
                  ca::str::Utf8StringRef token = {});
    void addField(ca::str::Utf8StringRef ownerName, ca::str::Utf8StringRef desc,
                  ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName,
                  ca::str::Utf8StringRef token = {});
    void addMethod(ca::str::Utf8StringRef ownerName, ca::str::Utf8StringRef desc,
                   ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName,
                   ca::str::Utf8StringRef token = {});

    const auto& getPackages() const { return packages_; }
    const auto& getClasses() const { return classes_; }
    const auto& getFields() const { return fields_; }
    const auto& getMethods() const { return methods_; }

    const PackageMapping*             getPackageMapping(ca::str::Utf8StringRef name) const;
    const ClassMapping*               getClassMapping(ca::str::Utf8StringRef name) const;
    const std::vector<FieldMapping>*  getClassFieldMappings(ca::str::Utf8StringRef name) const;
    const std::vector<MethodMapping>* getClassMethodMappings(ca::str::Utf8StringRef name) const;

    ca::str::Utf8StringRef getMappedClassName(ca::str::Utf8StringRef internalName) const;
    ca::str::Utf8StringRef getMappedFieldName(ca::str::Utf8StringRef ownerName,
                                              ca::str::Utf8StringRef fieldName,
                                              ca::str::Utf8StringRef fieldDesc) const;
    ca::str::Utf8StringRef getMappedMethodName(ca::str::Utf8StringRef ownerName,
                                               ca::str::Utf8StringRef methodName,
                                               ca::str::Utf8StringRef methodDesc) const;

    void updateClass();
    void updateField();
    void updateMethod();

    void putAll(const IntermediateMapping& other);

    std::unordered_map<ca::str::Utf8StringRef, ca::str::Utf8StringRef> exportClassMap() const;

    ca::str::Utf8StringArena&       arena() { return arena_; }
    const ca::str::Utf8StringArena& arena() const { return arena_; }

    bool hasClassMapping(ca::str::Utf8StringRef name) const;
    bool isEmpty() const;

    /// Preserve exact source text while the parsed model remains unmodified.
    void rememberSource(ca::str::Utf8StringRef format, ca::str::Utf8StringRef text);

    /// Return exact source text for the same format, or nullptr after any mutation.
    const std::string* unmodifiedSource(ca::str::Utf8StringRef format) const;

    /// @brief Source snapshots carried through line-oriented format conversion.
    const auto& sourceSnapshots() const { return sourceSnapshots_; }

private:
    void markModified();

    // -- arena 死亡点声明（memory-governance-plan §5-P1③）--
    // 本成员持有本映射私有的 Utf8StringArena（mutable：const 查询也会按需
    // intern）。指向本 arena 的视图包括：addPackage/addClass/addField/addMethod
    // intern 进去的 key/value，getPackages/getClasses/getFields/getMethods 返回的
    // 条目字段，getMappedClassName/FieldName/MethodName 的返回值，
    // exportClassMap() 的键值；arena() 访问器还把 arena 本身借给外部格式解析器
    // 复用（如 shadow-alias 严格解析）。
    // 视图寿命 = 本对象寿命；死亡点：随持有者析构（长期持有如调用方持有的
    // unique_ptr 成员，短期如 MappingFormatRegistry::parse 的按值返回与
    // remapper 的 const& 入参作用域）。
    // 消费者不得把视图带过本对象的死亡点；需跨期持有则 intern 进自己的 arena 或深拷贝。
    mutable ca::str::Utf8StringArena                                       arena_;
    std::unordered_map<ca::str::Utf8StringRef, PackageMapping>             packages_;
    std::unordered_map<ca::str::Utf8StringRef, ClassMapping>               classes_;
    std::unordered_map<ca::str::Utf8StringRef, std::vector<FieldMapping>>  fields_;
    std::unordered_map<ca::str::Utf8StringRef, std::vector<MethodMapping>> methods_;
    std::unordered_map<std::string, std::string>                           sourceSnapshots_;
    ca::usize                                                              revision_       = 0;
    ca::usize                                                              sourceRevision_ = 0;
};

}   // namespace mjbase::mapping
