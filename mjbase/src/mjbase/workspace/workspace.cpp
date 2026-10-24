#include "mjbase/workspace/workspace.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/insn.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_util.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/jasm/resource_jasm_service.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/mapping/identifier_util.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/edit_ledger.hpp"
#include "mjbase/project/edit_ledger_store.hpp"
#include "mjbase/project/project_store.hpp"
#include "mjbase/spi/class_file_sanitize.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"
#include "mjbase/workspace/class_mutation_service.hpp"
#include "mjbase/workspace/classfile_support.hpp"
#include "mjbase/workspace/reference_scan.hpp"
#include "mjbase/workspace/summary_scan.hpp"

namespace mjbase::workspace {

namespace {

using support::utf8Text;

// ── 基础助手（照旧直迁）─────────────────────────────────────────────────

WorkspaceErrorInfo error(WorkspaceError code, const std::string& operation,
                         const std::string& detail)
{
    return WorkspaceErrorInfo{code, operation + ": " + detail};
}

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// @brief UTF-8 窄串 → filesystem path（Windows 上 path(std::string) 按 ACP
///        解码，非 ASCII 必乱；与 model/resource_io.cpp 的 u8path 口径一致）。
std::filesystem::path fsPath(const std::string& text)
{
    return std::filesystem::u8path(text);
}

/// @brief filesystem path → UTF-8 窄串（path::string() 在 Windows 走 ACP，
///        非 ASCII 会抛 code-page 异常；API 边界契约是 UTF-8）。
std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

/// @brief IResource::getLocation() 借用视图 → owning 文本（跨界传递用）。
std::string locationText(const model::IResource& resource)
{
    const auto location = resource.getLocation();
    return std::string(reinterpret_cast<const char*>(location.data()), location.byte_length());
}

/// @brief 资源类型短名（旧栈小写口径："class"/"manifest"/"jar"/"resource"）。
std::string resourceTypeName(model::ResourceType type)
{
    switch (type) {
    case model::ResourceType::Class: return "class";
    case model::ResourceType::Manifest: return "manifest";
    case model::ResourceType::Jar: return "jar";
    default: return "resource";
    }
}

/// @brief model 模块错误 → workspace 错误值。
///
/// 旧栈这些路径以异常携带文本、统一落 DATA_LOSS；mjbase 值语义下按码分流：
/// 文件/条目缺失 → NotFound，zip-slip → InvalidArgument，其余（Io/BadEntry/
/// UnsupportedFormat/TypeMismatch/BadArgument）保持 DataLoss 兜底。
WorkspaceErrorInfo modelFailure(const std::string& operation, model::ModelError modelError)
{
    switch (modelError) {
    case model::ModelError::NotFound:
        return error(WorkspaceError::NotFound, operation, "archive or entry not found");
    case model::ModelError::PathEscape:
        return error(WorkspaceError::InvalidArgument, operation,
                     "archive entry escapes target dir");
    default:
        return error(WorkspaceError::DataLoss, operation, model::to_string(modelError));
    }
}

WorkspaceResult<std::vector<ca::u8>> bytesOf(const model::IResource& resource)
{
    if (const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource))
        return ca::core::Ok(classResource->get());
    if (const auto* manifestResource =
            ca::core::tag_cast::dyn_cast<const model::ManifestResource>(&resource))
        return ca::core::Ok(manifestResource->get().toBytes());
    if (const auto* unknownResource =
            ca::core::tag_cast::dyn_cast<const model::UnknownResource>(&resource))
        return ca::core::Ok(unknownResource->get());
    return ca::core::Err(error(WorkspaceError::Unimplemented, "read resource",
                               "unsupported resource type at " + locationText(resource)));
}

std::unique_ptr<model::IResource> cloneResource(const model::IResource& resource)
{
    if (const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource))
        return std::make_unique<model::ClassResource>(locationText(*classResource),
                                                      classResource->get());
    if (const auto* manifestResource =
            ca::core::tag_cast::dyn_cast<const model::ManifestResource>(&resource))
        return std::make_unique<model::ManifestResource>(manifestResource->get());
    if (const auto* unknownResource =
            ca::core::tag_cast::dyn_cast<const model::UnknownResource>(&resource))
        return std::make_unique<model::UnknownResource>(locationText(*unknownResource),
                                                        unknownResource->get());
    return nullptr;
}

WorkspaceResult<std::unique_ptr<model::ResourceContainer>> cloneContainer(
    const model::ResourceContainer& source)
{
    auto               result = std::make_unique<model::ResourceContainer>();
    WorkspaceErrorInfo err;
    source.visitResources([&](std::string_view uri, const model::IResource& resource) {
        if (err.is_err()) return;
        auto copied = cloneResource(resource);
        if (!copied) {
            err = error(WorkspaceError::Unimplemented, "clone container",
                        "unsupported resource type at " + std::string(uri));
            return;
        }
        result->addResource(uri, std::move(copied));
    });
    if (err.is_err()) return ca::core::Err(std::move(err));
    return ca::core::Ok(std::move(result));
}

// ── class 解码（libjbc 会话；字段口径照旧）──────────────────────────────

WorkspaceResult<std::shared_ptr<jbc::ir::ClassSession>> sessionOf(
    const std::vector<ca::u8>& bytes, const std::string& operation)
{
    auto parsed = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (parsed.is_err())
        return ca::core::Err(
            error(WorkspaceError::DataLoss, operation, "class bytes cannot be parsed"));
    return ca::core::Ok(std::move(parsed).unwrap());
}

/// @brief Code 嵌套 LNT 行 → {line, 代码指令序号}；pc 无对应指令时 index 为 -1。
WorkspaceResult<std::vector<LineNumberInfo>> lineNumbersOf(jbc::ir::ClassSession& session,
                                                           jbc::core::MemberInfo& method,
                                                           ca::usize              methodIndex)
{
    std::vector<LineNumberInfo> result;
    jbc::core::CodeAttr*        code = support::ensureCode(session.backing(), method);
    if (code == nullptr) return ca::core::Ok(std::move(result));

    std::vector<std::pair<ca::u32, ca::i32>> rows;   // {pc, line}
    for (jbc::core::AttrSlot& nested : code->attrs) {
        if (!session.backing().cp.utf8Equals(nested.nameCp, "LineNumberTable")) continue;
        if (!nested.decoded()) {
            if (session.backing().decodeAttr(nested).is_err()) continue;
        }
        const auto* table =
            ca::core::tag_cast::cast<const jbc::core::LineNumberTableAttr>(nested.peek());
        if (table == nullptr) continue;
        for (const auto& row : table->rows)
            rows.emplace_back(row.startPc, static_cast<ca::i32>(row.line));
    }
    if (rows.empty()) return ca::core::Ok(std::move(result));

    auto promoted = jbc::ir::promoteMethod(session, methodIndex);
    if (promoted.is_err())
        return ca::core::Err(
            error(WorkspaceError::DataLoss, "decode method", "method body cannot be promoted"));
    auto raw = promoted.unwrap()->dom.exportRaw();
    if (raw.is_err())
        return ca::core::Err(
            error(WorkspaceError::DataLoss, "decode method", "method body cannot be exported"));
    const auto insns = std::move(raw).unwrap();
    for (const auto& [pc, line] : rows) {
        LineNumberInfo info;
        info.line = line;
        for (ca::usize index = 0; index < insns.size(); ++index) {
            if (insns[index].pc == pc) {
                info.instructionIndex = static_cast<ca::i32>(index);
                break;
            }
        }
        result.push_back(info);
    }
    return ca::core::Ok(std::move(result));
}

FieldInfo makeFieldInfo(jbc::core::ClassFile& cf, jbc::core::MemberInfo& field)
{
    FieldInfo output;
    output.name             = utf8Text(cf, field.nameCp);
    output.descriptor       = utf8Text(cf, field.descCp);
    output.signature        = support::signatureText(cf, field.attrs);
    output.access           = static_cast<ca::i32>(field.access);
    output.constantValue    = support::fieldConstantValue(cf, field);
    support::collectAnnotations(cf, field.attrs, output.annotations);
    return output;
}

WorkspaceResult<MethodInfo> makeMethodInfo(jbc::ir::ClassSession& session,
                                           jbc::core::MemberInfo& method, ca::usize methodIndex)
{
    jbc::core::ClassFile& cf = session.backing();
    MethodInfo output;
    output.name       = utf8Text(cf, method.nameCp);
    output.descriptor = utf8Text(cf, method.descCp);
    output.signature  = support::signatureText(cf, method.attrs);
    output.access     = static_cast<ca::i32>(method.access);
    if (jbc::core::CodeAttr* code = support::ensureCode(cf, method)) {
        output.maxStack      = static_cast<ca::i32>(code->maxStack);
        output.maxLocals     = static_cast<ca::i32>(code->maxLocals);
        output.tryCatchCount = code->excTable.size();
        // instructionCount：代码指令序数（CodeDom 程序顺序；label/line 不是指令）。
        auto promoted = jbc::ir::promoteMethod(session, methodIndex);
        if (promoted.is_err())
            return ca::core::Err(error(
                WorkspaceError::DataLoss, "decode method",
                "method body cannot be promoted: " + utf8Text(cf, method.nameCp) + " (" +
                    std::to_string(static_cast<int>(promoted.unwrap_err())) + ")"));
        output.instructionCount = promoted.unwrap()->dom.size();
        support::collectLocalVariables(cf, *code, output.localVariables);
    }
    output.exceptions = support::exceptionNames(cf, method);
    auto lines        = lineNumbersOf(session, method, methodIndex);
    if (lines.is_err()) return ca::core::Err(std::move(lines).unwrap_err());
    output.lineNumbers = std::move(lines).unwrap();
    support::collectAnnotations(cf, method.attrs, output.annotations);
    return ca::core::Ok(std::move(output));
}

/// @brief InnerClasses 属性的内部类名（去重排序；inner_name_index=0 的匿名形态跳过）。
///
/// typed InnerClassesAttr 体经公开解码入口读出（B3 分派接线前后同形，不触碰
/// 槽状态）；解码失败的槽跳过（查询面对畸形输入宽容）。
std::vector<std::string> innerClassNames(jbc::core::ClassFile& cf)
{
    std::vector<std::string> names;
    for (jbc::core::AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "InnerClasses")) continue;
        auto d = decodeInnerClassesBody(cf, slot);
        if (d.is_err()) continue;
        const auto body = std::move(d).unwrap();
        const auto& ic  = static_cast<const jbc::core::InnerClassesAttr&>(*body);
        for (const jbc::core::InnerClassesAttr::Row& row : ic.rows) {
            if (row.innerNameCp == 0) continue;
            const auto name = utf8Text(cf, row.innerNameCp);
            if (!name.empty()) names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

WorkspaceResult<ClassInfo> decodeClass(const std::string& uri, const model::ClassResource& resource)
{
    auto session = sessionOf(resource.get(), "decode class " + uri);
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                  held = std::move(session).unwrap();
    jbc::core::ClassFile& cf   = held->backing();

    ClassInfo output;
    output.resourceUri  = uri;
    output.internalName = std::string{cf.thisName()};
    output.superName    = std::string{cf.superName()};
    output.signature    = support::signatureText(cf, cf.attrs);
    output.access       = static_cast<ca::i32>(cf.access);
    // 旧栈 ASM 语义：version = major | (minor << 16)。
    output.version      = static_cast<ca::i32>(cf.major) | (static_cast<ca::i32>(cf.minor) << 16);
    for (const auto interfaceCp : cf.interfaces) {
        output.interfaces.push_back(std::string{cf.className(interfaceCp)});
    }
    for (auto& field : cf.fields) output.fields.push_back(makeFieldInfo(cf, field));
    for (ca::usize index = 0; index < cf.methods.size(); ++index) {
        auto info = makeMethodInfo(*held, cf.methods[index], index);
        if (info.is_err()) return ca::core::Err(std::move(info).unwrap_err());
        output.methods.push_back(std::move(info).unwrap());
    }
    output.innerClasses = innerClassNames(cf);
    support::collectAnnotations(cf, cf.attrs, output.annotations);
    return ca::core::Ok(std::move(output));
}

// ── 搜索（照旧直迁）──────────────────────────────────────────────────────

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool matchesText(const std::string& value, const std::string& query,
                 const SearchOptions& options, const std::regex* compiledRegex)
{
    if (options.matchMode == SearchMatchMode::Regex)
        return compiledRegex != nullptr && std::regex_search(value, *compiledRegex);
    const auto comparableValue = options.caseSensitive ? value : lowercase(value);
    const auto comparableQuery = options.caseSensitive ? query : lowercase(query);
    if (options.matchMode == SearchMatchMode::Exact) return comparableValue == comparableQuery;
    return comparableValue.find(comparableQuery) != std::string::npos;
}

WorkspaceErrorInfo validateSearchOptions(const std::string&   query, const SearchOptions& options,
                                         const std::string& operation)
{
    if (query.empty())
        return error(WorkspaceError::InvalidArgument, operation, "query must not be empty");
    if (query.size() > 1024)
        return error(WorkspaceError::InvalidArgument, operation, "query exceeds 1024 bytes");
    if (!options.memberKind.empty() && options.memberKind != "field" &&
        options.memberKind != "method")
        return error(WorkspaceError::InvalidArgument, operation,
                     "member kind must be field or method");
    if (options.opcode < -1 || options.opcode >= 200)
        return error(WorkspaceError::InvalidArgument, operation,
                     "opcode must be between 0 and 199");
    return WorkspaceErrorInfo{};
}

WorkspaceResult<std::optional<std::regex>> compileSearchRegex(const std::string&   query,
                                                              const SearchOptions& options,
                                                              const std::string&   operation)
{
    WorkspaceErrorInfo status = validateSearchOptions(query, options, operation);
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (options.matchMode != SearchMatchMode::Regex)
        return ca::core::Ok(std::optional<std::regex>{});
    try {
        auto flags = std::regex_constants::ECMAScript;
        if (!options.caseSensitive) flags |= std::regex_constants::icase;
        return ca::core::Ok(std::optional<std::regex>(std::in_place, query, flags));
    }
    catch (const std::regex_error& exception) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, operation, exception.what()));
    }
}

bool matchesSearchScope(const std::string& owner, const SearchOptions& options)
{
    if (!options.owner.empty() && owner != options.owner) return false;
    return options.packagePrefix.empty() || owner.rfind(options.packagePrefix, 0) == 0;
}

bool matchesAnyText(const std::vector<std::string>& values, const std::string& query,
                    const SearchOptions& options, const std::regex* compiledRegex)
{
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
        return matchesText(value, query, options, compiledRegex);
    });
}

template <typename T>
std::string numberText(T value)
{
    std::ostringstream output;
    if constexpr (std::is_floating_point_v<T>) {
        output << std::setprecision(std::numeric_limits<T>::max_digits10);
    }
    output << value;
    return output.str();
}

bool matchesNumber(const std::string& value, const std::string& query)
{
    if (value == query) return true;
    try {
        std::size_t valueOffset = 0;
        std::size_t queryOffset = 0;
        const auto  parsedValue = std::stold(value, &valueOffset);
        const auto  parsedQuery = std::stold(query, &queryOffset);
        return valueOffset == value.size() && queryOffset == query.size() &&
               parsedValue == parsedQuery;
    }
    catch (const std::exception&) {
        return false;
    }
}

WorkspaceErrorInfo validateNumberQuery(const std::string& query)
{
    try {
        std::size_t offset = 0;
        const auto  parsed = std::stold(query, &offset);
        (void)parsed;
        if (offset == query.size()) return WorkspaceErrorInfo{};
    }
    catch (const std::exception&) {
    }
    return error(WorkspaceError::InvalidArgument, "search numbers",
                 "query must be a complete numeric value");
}

void sortAndDeduplicateSearchHits(std::vector<SearchHit>& hits)
{
    std::sort(hits.begin(), hits.end(), [](const SearchHit& left, const SearchHit& right) {
        return std::tie(left.resourceUri,
                        left.owner,
                        left.name,
                        left.descriptor,
                        left.instructionIndex,
                        left.kind,
                        left.value) <
               std::tie(right.resourceUri,
                        right.owner,
                        right.name,
                        right.descriptor,
                        right.instructionIndex,
                        right.kind,
                        right.value);
    });
    hits.erase(
        std::unique(hits.begin(), hits.end(), [](const SearchHit& left, const SearchHit& right) {
            return left.resourceUri == right.resourceUri && left.kind == right.kind &&
                   left.owner == right.owner && left.name == right.name &&
                   left.descriptor == right.descriptor &&
                   left.instructionIndex == right.instructionIndex && left.value == right.value;
        }),
        hits.end());
}

// ── 指令渲染（libjbc 形状分派；nodeType 按操作数布局命名）────────────────

/// @brief libjbc 无 ASM 节点类型 → 按操作数布局命名；标签/行号不是指令。
std::string instructionNodeType(jbc::core::Op op)
{
    switch (jbc::core::shapeOf(op)) {
    case jbc::core::Shape::None: return "insn";
    case jbc::core::Shape::Imm1:
    case jbc::core::Shape::Imm2:
    case jbc::core::Shape::Atype1: return "int";
    case jbc::core::Shape::Local1: return "var";
    case jbc::core::Shape::Iinc1: return "iinc";
    case jbc::core::Shape::CP1:
    case jbc::core::Shape::CP2: return "ldc";
    case jbc::core::Shape::CP2Count: return "method";
    case jbc::core::Shape::CP2Zero2: return "invokedynamic";
    case jbc::core::Shape::CP2Dims: return "multianewarray";
    case jbc::core::Shape::Branch2:
    case jbc::core::Shape::Branch4: return "jump";
    case jbc::core::Shape::TableSwitch: return "tableswitch";
    case jbc::core::Shape::LookupSwitch: return "lookupswitch";
    }
    return "unknown";
}

/// @brief 成员引用 CP 索引 → "owner.name:desc"（operandComment 渲染口径）。
std::string memberRefColonText(const jbc::core::ClassFile& cf, ca::u32 cpIndex)
{
    if (!cf.cp.valid(cpIndex)) return {};
    const jbc::core::ConstantPool::Entry& entry = cf.cp.at(cpIndex);
    if (entry.tag != jbc::core::CpTag::Fieldref && entry.tag != jbc::core::CpTag::Methodref &&
        entry.tag != jbc::core::CpTag::InterfaceMethodref)
        return {};
    std::string owner;
    std::string name;
    std::string desc;
    if (cf.cp.valid(entry.a)) {
        const auto& classEntry = cf.cp.at(entry.a);
        if (classEntry.tag == jbc::core::CpTag::Class) owner = utf8Text(cf, classEntry.a);
    }
    if (cf.cp.valid(entry.b)) {
        const auto& nt = cf.cp.at(entry.b);
        if (nt.tag == jbc::core::CpTag::NameAndType) {
            name = utf8Text(cf, nt.a);
            desc = utf8Text(cf, nt.b);
        }
    }
    return owner + "." + name + ":" + desc;
}

/// @brief 成员引用 CP 索引 → 无冒号拼接（指令搜索文本口径，与旧栈 methodTarget
///        一致：workspace/Subject.value()I）。
std::string memberRefFlatText(const jbc::core::ClassFile& cf, ca::u32 cpIndex)
{
    const std::string resolved = memberRefColonText(cf, cpIndex);
    if (resolved.empty()) return {};
    const auto colon = resolved.rfind(':');
    return resolved.substr(0, colon) + resolved.substr(colon + 1);
}

/// @brief LDC 常量与成员引用的可读注释（字符串加引号、数值无后缀、已过
///        IdentifierUtil 可读化——InstructionDetailInfo::operandComment 契约）。
std::string ldcComment(const jbc::core::ClassFile& cf, ca::u32 cpIndex)
{
    if (!cf.cp.valid(cpIndex)) return {};
    const jbc::core::ConstantPool::Entry& entry = cf.cp.at(cpIndex);
    char                                  tmp[64];
    switch (entry.tag) {
    case jbc::core::CpTag::String:
        return "\"" +
               mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, entry.a)) + "\"";
    case jbc::core::CpTag::Class:
        return mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, entry.a));
    case jbc::core::CpTag::Integer:
        std::snprintf(tmp, sizeof(tmp), "%d", entry.ival);
        return tmp;
    case jbc::core::CpTag::Float:
        std::snprintf(tmp, sizeof(tmp), "%g",
                      static_cast<double>(support::bitsToFloat(static_cast<ca::u32>(entry.ival))));
        return tmp;
    case jbc::core::CpTag::Long:
        std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(entry.lval));
        return tmp;
    case jbc::core::CpTag::Double:
        std::snprintf(tmp, sizeof(tmp), "%g",
                      support::bitsToDouble(static_cast<ca::u64>(entry.lval)));
        return tmp;
    case jbc::core::CpTag::Fieldref:
    case jbc::core::CpTag::Methodref:
    case jbc::core::CpTag::InterfaceMethodref:
    {
        // owner.name:desc 各段过 IdentifierUtil 可读化。
        std::string owner;
        std::string name;
        std::string desc;
        if (cf.cp.valid(entry.a)) {
            const auto& classEntry = cf.cp.at(entry.a);
            if (classEntry.tag == jbc::core::CpTag::Class)
                owner = mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, classEntry.a));
        }
        if (cf.cp.valid(entry.b)) {
            const auto& nt = cf.cp.at(entry.b);
            if (nt.tag == jbc::core::CpTag::NameAndType) {
                name = mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, nt.a));
                desc = mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, nt.b));
            }
        }
        return owner + "." + name + ":" + desc;
    }
    default: return {};
    }
}

/// @brief 一条指令的完整渲染（保留 CP 索引与字节 PC；switch 给绝对目标 PC）。
InstructionDetailInfo renderInstruction(const jbc::core::ClassFile& cf,
                                        const jbc::core::RawInsn&   insn)
{
    InstructionDetailInfo info;
    info.pc     = static_cast<ca::i32>(insn.pc);
    info.opcode = static_cast<ca::i32>(insn.op);
    const char* mnemonic = jbc::core::mnemonicOf(insn.op);
    info.mnemonic        = mnemonic != nullptr ? mnemonic : "?";
    char tmp[64];
    switch (jbc::core::shapeOf(insn.op)) {
    case jbc::core::Shape::CP1:
    case jbc::core::Shape::CP2:
        std::snprintf(tmp, sizeof(tmp), "#%u", insn.cp);
        info.operandText = tmp;
        info.cpIndex     = static_cast<ca::i32>(insn.cp);
        break;
    case jbc::core::Shape::Imm1:
    case jbc::core::Shape::Imm2:
    case jbc::core::Shape::Atype1:
    case jbc::core::Shape::Local1:
        std::snprintf(tmp, sizeof(tmp), "%d", insn.a);
        info.operandText = tmp;
        break;
    case jbc::core::Shape::Iinc1:
        std::snprintf(tmp, sizeof(tmp), "%d, %d", insn.a, insn.b);
        info.operandText = tmp;
        break;
    case jbc::core::Shape::CP2Dims:
        std::snprintf(tmp, sizeof(tmp), "#%u dims=%d", insn.cp, insn.a);
        info.operandText = tmp;
        info.cpIndex     = static_cast<ca::i32>(insn.cp);
        break;
    case jbc::core::Shape::CP2Count:
    case jbc::core::Shape::CP2Zero2:
        std::snprintf(tmp, sizeof(tmp), "#%u", insn.cp);
        info.operandText = tmp;
        info.cpIndex     = static_cast<ca::i32>(insn.cp);
        break;
    case jbc::core::Shape::TableSwitch:
    {
        info.isSwitch        = true;
        info.switchDefaultPc = static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(insn.t));
        for (ca::usize k = 0; k < insn.offs.size(); ++k) {
            info.switchCases.push_back(
                {insn.a + static_cast<ca::i32>(k),
                 static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(insn.offs[k]))});
        }
        break;
    }
    case jbc::core::Shape::LookupSwitch:
    {
        info.isSwitch        = true;
        info.switchDefaultPc = static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(insn.t));
        for (const auto& kase : insn.cases) {
            info.switchCases.push_back(
                {kase.key, static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(kase.off))});
        }
        break;
    }
    case jbc::core::Shape::None:
    case jbc::core::Shape::Branch2:
    case jbc::core::Shape::Branch4: break;
    }
    return info;
}

// ── 字节 diff / 修复 / 指纹（照旧直迁）──────────────────────────────────

std::vector<ByteDiff> diffContainers(const model::ResourceContainer& before,
                                     const model::ResourceContainer& after)
{
    std::set<std::string> uris;
    before.visitResources(
        [&](std::string_view uri, const model::IResource&) { uris.insert(std::string(uri)); });
    after.visitResources(
        [&](std::string_view uri, const model::IResource&) { uris.insert(std::string(uri)); });

    std::vector<ByteDiff> diffs;
    for (const auto& uri : uris) {
        const model::IResource* beforeResource = before.getResource(uri);
        const model::IResource* afterResource  = after.getResource(uri);
        std::vector<ca::u8>     beforeBytes;
        std::vector<ca::u8>     afterBytes;
        if (beforeResource != nullptr) {
            auto bytes = bytesOf(*beforeResource);
            if (bytes.is_err()) continue;
            beforeBytes = std::move(bytes).unwrap();
        }
        if (afterResource != nullptr) {
            auto bytes = bytesOf(*afterResource);
            if (bytes.is_err()) continue;
            afterBytes = std::move(bytes).unwrap();
        }
        if (beforeBytes == afterBytes) continue;

        // 单一公共前后缀收缩：第一个差异字节到最后一个差异字节（照旧口径）。
        ca::usize start = 0;
        while (start < beforeBytes.size() && start < afterBytes.size() &&
               beforeBytes[start] == afterBytes[start]) {
            ++start;
        }
        ca::usize beforeEnd = beforeBytes.size();
        ca::usize afterEnd  = afterBytes.size();
        while (beforeEnd > start && afterEnd > start &&
               beforeBytes[beforeEnd - 1] == afterBytes[afterEnd - 1]) {
            --beforeEnd;
            --afterEnd;
        }
        ByteDiff diff;
        diff.uri        = uri;
        diff.beforeSize = beforeBytes.size();
        diff.afterSize  = afterBytes.size();
        diff.ranges.push_back({start, beforeEnd - start, afterEnd - start});
        diffs.push_back(std::move(diff));
    }
    return diffs;
}

/// @brief 编辑校验门的作用域：前后差异条目的 URI 集——操作受影响面。
///        容器基线未编辑类不参与门禁（它们本来就是这个样子，编辑前就存在）。
std::vector<std::string> changedUris(const std::vector<ByteDiff>& diffs)
{
    std::vector<std::string> uris;
    uris.reserve(diffs.size());
    for (const auto& diff : diffs) uris.push_back(diff.uri);
    return uris;
}

bool hasClassExtension(const std::string& uri)
{
    constexpr char kExtension[] = ".class";
    if (uri.size() < sizeof(kExtension) - 1) return false;
    const auto offset = uri.size() - (sizeof(kExtension) - 1);
    for (ca::usize index = 0; index < sizeof(kExtension) - 1; ++index) {
        const auto character = static_cast<unsigned char>(uri[offset + index]);
        if (std::tolower(character) != kExtension[index]) return false;
    }
    return true;
}

std::string internalNameFromClassUri(const std::string& uri)
{
    constexpr char kMultiReleasePrefix[] = "META-INF/versions/";
    std::string    path                  = uri;
    if (path.rfind(kMultiReleasePrefix, 0) == 0) {
        const auto versionEnd = path.find('/', sizeof(kMultiReleasePrefix) - 1);
        if (versionEnd == std::string::npos || versionEnd + 1 >= path.size()) return {};
        const auto version = path.substr(sizeof(kMultiReleasePrefix) - 1,
                                         versionEnd - (sizeof(kMultiReleasePrefix) - 1));
        if (version.empty() ||
            !std::all_of(version.begin(), version.end(), [](unsigned char character) {
                return std::isdigit(character) != 0;
            }))
            return {};
        path = path.substr(versionEnd + 1);
    }
    if (!hasClassExtension(path)) return {};
    path.resize(path.size() - (sizeof(".class") - 1));
    return path;
}

/// @brief class 字节 → 内部名（骨架解析；失败为空）。
std::string internalNameOfBytes(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return {};
    auto parsed = std::move(read).unwrap();
    return std::string{parsed.file->thisName()};
}

std::vector<JarRepairIssue> scanJarRepairIssues(const model::ResourceContainer& container,
                                                const JarRepairRequest& request,
                                                ca::usize&              classCount)
{
    jasm::JvmJasmService        validator;
    std::vector<JarRepairIssue> issues;
    container.visitResources([&](std::string_view uriView, const model::IResource& resource) {
        const std::string uri{uriView};
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource != nullptr) {
            ++classCount;
            const auto validated = validator.validate(classResource->get());
            if (!validated.isOk()) {
                issues.push_back({uri,
                                  "invalid_class",
                                  "class bytes did not pass Jasm validation",
                                  classResource->get().size(),
                                  request.removeInvalidClassEntries});
            }
        }
        else if (hasClassExtension(uri)) {
            auto bytes = bytesOf(resource);
            issues.push_back({uri,
                              "invalid_class_entry",
                              "a .class entry was not recognized as valid class bytes",
                              bytes.is_ok() ? bytes.unwrap().size() : 0,
                              request.removeInvalidClassEntries});
        }
        if (uri.find("corrupt_output_stream") != std::string::npos) {
            auto bytes = bytesOf(resource);
            issues.push_back({uri,
                              "corrupt_output_marker",
                              "resource name matches a known corrupt output marker",
                              bytes.is_ok() ? bytes.unwrap().size() : 0,
                              request.removeCorruptOutputMarkers});
        }
    });
    std::sort(issues.begin(), issues.end(),
              [](const JarRepairIssue& left, const JarRepairIssue& right) {
                  if (left.uri != right.uri) return left.uri < right.uri;
                  return left.kind < right.kind;
              });
    return issues;
}

void removeSelectedRepairIssues(model::ResourceContainer&         candidate,
                                const std::vector<JarRepairIssue>& issues)
{
    // 容器以 string_view 键视图指向资源自持文本：先拷贝待删键再删除。
    auto&                    resources = candidate.getResources();
    std::vector<std::string> doomed;
    for (const auto& issue : issues) {
        if (issue.selectedForRemoval) doomed.push_back(issue.uri);
    }
    for (const std::string& uri : doomed) resources.erase(std::string_view(uri));
}

bool hasSelectedRepairIssues(const std::vector<JarRepairIssue>& issues)
{
    return std::any_of(issues.begin(), issues.end(), [](const JarRepairIssue& issue) {
        return issue.selectedForRemoval;
    });
}

std::vector<std::string> changedClassNames(const model::ResourceContainer& container,
                                           const std::vector<ByteDiff>&    diffs)
{
    std::vector<std::string> result;
    for (const auto& diff : diffs) {
        const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
            container.getResource(diff.uri));
        if (resource == nullptr) continue;
        const auto name = internalNameOfBytes(resource->get());
        if (!name.empty()) result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void appendUniqueImpact(std::vector<ReferenceImpact>&    impacts,
                        std::unordered_set<std::string>& seen, const std::string& kind,
                        const std::string& target)
{
    const std::string key = kind + "\n" + target;
    if (!seen.insert(key).second) return;
    impacts.push_back({kind, target});
}

/// @brief 受影响引用事实（mjbase ReferenceScan 直连面：direct 方法/字段/类型引用）。
std::vector<ReferenceImpact> collectImpacts(const model::ResourceContainer& container,
                                            const std::vector<std::string>& classes)
{
    try {
        ReferenceScan                   index;
        index.build(container);
        std::vector<ReferenceImpact>    impacts;
        std::unordered_set<std::string> seen;
        for (const auto& owner : classes) {
            for (const auto& reference : index.methodReferencesFromClass(owner)) {
                appendUniqueImpact(impacts, seen, "method",
                                   reference.target.owner + "." + reference.target.name +
                                       reference.target.descriptor);
            }
            for (const auto& reference : index.fieldReferencesFromClass(owner)) {
                appendUniqueImpact(impacts, seen, "field",
                                   reference.target.owner + "." + reference.target.name +
                                       reference.target.descriptor);
            }
            for (const auto& reference : index.typeReferencesFromClass(owner)) {
                appendUniqueImpact(impacts, seen, "type", reference.targetType);
            }
        }
        std::sort(impacts.begin(), impacts.end(),
                  [](const ReferenceImpact& left, const ReferenceImpact& right) {
                      if (left.kind != right.kind) return left.kind < right.kind;
                      return left.target < right.target;
                  });
        return impacts;
    }
    catch (const std::exception&) {
        return {};
    }
}

// ── 指纹（fnv1a64:16hex；长度按小端逐字节入串——照旧逐字节直迁）──────────

void hashFingerprintBytes(ca::u64& state, const ca::u8* data, ca::usize size)
{
    constexpr ca::u64 kPrime = 1099511628211ULL;
    for (ca::usize index = 0; index < size; ++index) {
        state ^= data[index];
        state *= kPrime;
    }
}

void hashFingerprintLength(ca::u64& state, ca::usize value)
{
    const auto stableValue = static_cast<ca::u64>(value);
    for (ca::usize index = 0; index < sizeof(stableValue); ++index) {
        const auto byte = static_cast<ca::u8>((stableValue >> (index * 8U)) & 0xffU);
        hashFingerprintBytes(state, &byte, 1);
    }
}

std::string fingerprintBytes(const std::vector<ca::u8>& bytes)
{
    constexpr ca::u64 kOffsetBasis = 14695981039346656037ULL;
    ca::u64           state        = kOffsetBasis;
    hashFingerprintLength(state, bytes.size());
    hashFingerprintBytes(state, bytes.data(), bytes.size());
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << state;
    return output.str();
}

WorkspaceResult<std::string> fingerprintContainer(const model::ResourceContainer& container)
{
    constexpr ca::u64        kOffsetBasis = 14695981039346656037ULL;
    ca::u64                  state        = kOffsetBasis;
    std::vector<std::string> uris;
    container.visitResources(
        [&](std::string_view uri, const model::IResource&) { uris.push_back(std::string(uri)); });
    std::sort(uris.begin(), uris.end());
    for (const auto& uri : uris) {
        const model::IResource* resource = container.getResource(uri);
        if (resource == nullptr) continue;
        auto bytes = bytesOf(*resource);
        if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());
        const auto& content = bytes.unwrap();
        hashFingerprintLength(state, uri.size());
        hashFingerprintBytes(state, reinterpret_cast<const ca::u8*>(uri.data()), uri.size());
        hashFingerprintLength(state, content.size());
        hashFingerprintBytes(state, content.data(), content.size());
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << state;
    return ca::core::Ok(output.str());
}

// ── 导出目的地与原子导出（照旧直迁）─────────────────────────────────────

WorkspaceResult<std::string> normalizedRegularFile(const std::string& path,
                                                   const std::string& operation)
{
    namespace fs = std::filesystem;
    if (path.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, operation, "path is empty"));
    std::error_code filesystemError;
    const auto      absolute = fs::absolute(fsPath(path), filesystemError).lexically_normal();
    if (filesystemError)
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, operation, filesystemError.message()));
    const auto status = fs::status(absolute, filesystemError);
    if (filesystemError || !fs::exists(status))
        return ca::core::Err(error(WorkspaceError::NotFound, operation, pathToUtf8(absolute)));
    if (!fs::is_regular_file(status))
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, operation, "path is not a regular file"));
    return ca::core::Ok(pathToUtf8(absolute));
}

WorkspaceResult<std::filesystem::path> validateExportDestination(
    const std::string& sourcePath, const std::string& outputPath,
    const std::string& operation = "export JAR",
    const std::string& sourceDescription = "input JAR")
{
    namespace fs = std::filesystem;
    if (outputPath.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, operation, "output path is empty"));

    std::error_code filesystemError;
    const auto      output = fs::absolute(fsPath(outputPath), filesystemError);
    if (filesystemError)
        return ca::core::Err(error(WorkspaceError::InvalidArgument, operation, outputPath));
    const auto source = fs::absolute(fsPath(sourcePath), filesystemError);
    if (!filesystemError && source == output) {
        return ca::core::Err(error(WorkspaceError::PermissionDenied, operation,
                                   "output must not overwrite the " + sourceDescription));
    }
    const auto parent = output.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        return ca::core::Err(
            error(WorkspaceError::NotFound, operation, "output parent does not exist"));
    }
    if (fs::exists(output))
        return ca::core::Err(error(WorkspaceError::AlreadyExists, operation, pathToUtf8(output)));
    return ca::core::Ok(output);
}

WorkspaceErrorInfo exportContainerToJar(model::ResourceContainer& container,
                                        const std::string& sourcePath,
                                        const std::string& outputPath, ca::u64 revision)
{
    namespace fs = std::filesystem;
    auto output = validateExportDestination(sourcePath, outputPath);
    if (output.is_err()) return std::move(output).unwrap_err();
    const auto destination = std::move(output).unwrap();

    std::error_code filesystemError;
    auto            temporary = destination;
    temporary += ".mjt-tmp-" + std::to_string(revision);
    if (fs::exists(temporary))
        return error(WorkspaceError::AlreadyExists, "export JAR", pathToUtf8(temporary));
    try {
        auto written = model::ResourceHelper::exportToJar(container, pathToUtf8(temporary));
        if (written.is_err()) {
            fs::remove(temporary, filesystemError);
            return modelFailure("export JAR", std::move(written).unwrap_err());
        }
        if (!fs::exists(temporary) || fs::file_size(temporary) == 0) {
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export JAR",
                         "temporary archive was not written");
        }
        fs::rename(temporary, destination, filesystemError);
        if (filesystemError) {
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export JAR", "atomic rename failed");
        }
        return WorkspaceErrorInfo{};
    }
    catch (const std::exception& exception) {
        fs::remove(temporary, filesystemError);
        return error(WorkspaceError::Internal, "export JAR", exception.what());
    }
}

WorkspaceErrorInfo exportContainerToClass(model::ResourceContainer& container,
                                          const std::string& classUri,
                                          const std::string& sourcePath,
                                          const std::string& outputPath, ca::u64 revision)
{
    namespace fs = std::filesystem;
    auto output =
        validateExportDestination(sourcePath, outputPath, "export class", "input class");
    if (output.is_err()) return std::move(output).unwrap_err();
    const auto destination = std::move(output).unwrap();

    std::error_code filesystemError;
    auto            temporary = destination;
    temporary += ".mjbase-class-tmp-" + std::to_string(revision);
    if (fs::exists(temporary))
        return error(WorkspaceError::AlreadyExists, "export class", pathToUtf8(temporary));
    try {
        auto written =
            model::ResourceHelper::exportToClassFile(container, pathToUtf8(temporary), classUri);
        if (written.is_err()) {
            fs::remove(temporary, filesystemError);
            return modelFailure("export class", std::move(written).unwrap_err());
        }
        if (!fs::exists(temporary) || fs::file_size(temporary) == 0) {
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export class",
                         "temporary classfile was not written");
        }
        fs::rename(temporary, destination, filesystemError);
        if (filesystemError) {
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export class", "atomic rename failed");
        }
        return WorkspaceErrorInfo{};
    }
    catch (const std::exception& exception) {
        fs::remove(temporary, filesystemError);
        return error(WorkspaceError::Internal, "export class", exception.what());
    }
}

template <typename T>
jasm::Result<T> jasmFailure(const std::string& message)
{
    jasm::Result<T> result;
    result.diagnostics.push_back(
        {jasm::DiagnosticSeverity::Error, jasm::DiagnosticStage::Resource, utf16(message),
         std::nullopt});
    return result;
}

void sortUnique(std::vector<std::string>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

}   // namespace

// ── 错误码短名 ──────────────────────────────────────────────────────────

const char* to_string(WorkspaceError errorCode) noexcept
{
    switch (errorCode) {
    case WorkspaceError::Ok: return "Ok";
    case WorkspaceError::InvalidArgument: return "InvalidArgument";
    case WorkspaceError::NotFound: return "NotFound";
    case WorkspaceError::AlreadyExists: return "AlreadyExists";
    case WorkspaceError::FailedPrecondition: return "FailedPrecondition";
    case WorkspaceError::PermissionDenied: return "PermissionDenied";
    case WorkspaceError::Aborted: return "Aborted";
    case WorkspaceError::DataLoss: return "DataLoss";
    case WorkspaceError::Internal: return "Internal";
    case WorkspaceError::Unimplemented: return "Unimplemented";
    }
    return "?";
}

// ── WorkspaceBase 内部结构 ──────────────────────────────────────────────

struct WorkspaceBase::SupportingResource
{
    std::string                               id;
    std::string                               path;
    std::string                               fingerprint;
    std::string                               role = "support";   ///< 角色标签
    std::unique_ptr<model::ResourceContainer> container;
};

/// @brief 层次/覆盖分析图：KeyPool + ClassHierarchy + 每类声明摘要。
///
/// 未登记的超类型（java/lang/Object 等）物化为合成节点（无成员、access 0），
/// 使 superclass 链/接口闭包与旧栈 ClassHierarchy 的 findOrCreateEntry 行为一致。
struct WorkspaceBase::AnalysisGraph
{
    std::shared_ptr<mjbase::KeyPool>           pool      = std::make_shared<mjbase::KeyPool>();
    std::unique_ptr<hierarchy::ClassHierarchy> hierarchy = nullptr;
    std::unordered_map<ca::u32, hierarchy::ClassSummary> summaries;   ///< key: ClassKey.id

    AnalysisGraph()
        : hierarchy(std::make_unique<hierarchy::ClassHierarchy>(*pool))
    {}

    bool declaresMethod(ca::u32 classKeyId, const std::string& name,
                        const std::string& descriptor) const
    {
        const auto found = summaries.find(classKeyId);
        if (found == summaries.end()) return false;
        for (const auto& method : found->second.methods) {
            if (pool->str(method.name) == name && pool->str(method.desc) == descriptor) return true;
        }
        return false;
    }
};

WorkspaceBase::WorkspaceBase() = default;
WorkspaceBase::~WorkspaceBase() = default;

// ── 打开 / supporting ───────────────────────────────────────────────────

WorkspaceResult<void> WorkspaceBase::openJar(const std::string& path)
{
    // 缺省空 decoder = 原始字节直通（#1110 族缺省零漂移，见 decoder 重载）。
    return openJar(path, {});
}

WorkspaceResult<void> WorkspaceBase::openJar(
    const std::string& path, const mjbase::jar::IJarReader::EntryNameDecoder& decoder)
{
    if (path.empty())
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "open JAR", "path is empty"));
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    const auto      status = fs::status(fsPath(path), filesystemError);
    if (filesystemError || !fs::exists(status))
        return ca::core::Err(error(WorkspaceError::NotFound, "open JAR", path));
    if (!fs::is_regular_file(status))
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "open JAR", "path is not a regular file"));

    try {
        model::ResourceContainer loaded;
        auto imported = model::ResourceHelper::importFromJar(loaded, path, decoder);
        if (imported.is_err())
            return ca::core::Err(modelFailure("open JAR", std::move(imported).unwrap_err()));
        auto original = cloneContainer(loaded);
        if (original.is_err()) return ca::core::Err(std::move(original).unwrap_err());
        auto working = cloneContainer(loaded);
        if (working.is_err()) return ca::core::Err(std::move(working).unwrap_err());

        original_         = std::move(original).unwrap();
        working_          = std::move(working).unwrap();
        artifactKind_     = ArtifactKind::Jar;
        sourcePath_       = path;
        primaryClassUri_.clear();
        supporting_.clear();
        // 会话史清零（旧语义：打开新主资源丢弃 undo/redo/savepoint）。绑定态
        // 先落盘旧容器台账再重置链（新容器台账随下次 flush 落盘；旧容器已落盘
        // 历史入待释放清单随之清理——台账作用域 = 容器）。
        auto digest = ledgerBaseDigest();
        if (digest.is_err()) return ca::core::Err(std::move(digest).unwrap_err());
        if (ledger_.bound()) {
            if (ledger_.hasPending()) {
                auto flushed = ledger_.flush(project::rfc3339UtcNow());
                if (flushed.is_err())
                    return ca::core::Err(ledgerFailure("open JAR", std::move(flushed).unwrap_err()));
            }
            ledger_.resetSession(project::copyLedgerText(digest.unwrap()));
        }
        else {
            auto fresh = project::EditLedgerStore::createInMemory(project::copyLedgerText(digest.unwrap()));
            if (fresh.is_err())
                return ca::core::Err(ledgerFailure("open JAR", std::move(fresh).unwrap_err()));
            ledger_ = std::move(fresh).unwrap();
        }
        redoHint_.clear();
        revision_         = 0;
        nextSupportingId_ = 1;
        return ca::core::Ok();
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::DataLoss, "open JAR", exception.what()));
    }
}

WorkspaceResult<void> WorkspaceBase::openClass(const std::string& path)
{
    if (path.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "open class", "path is empty"));
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    const auto      status = fs::status(fsPath(path), filesystemError);
    if (filesystemError || !fs::exists(status))
        return ca::core::Err(error(WorkspaceError::NotFound, "open class", path));
    if (!fs::is_regular_file(status))
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "open class", "path is not a regular file"));

    try {
        model::ResourceContainer imported;
        auto importedStatus = model::ResourceHelper::importFromClassFile(imported, path);
        if (importedStatus.is_err())
            return ca::core::Err(
                modelFailure("open class", std::move(importedStatus).unwrap_err()));
        const auto* importedClass =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(imported.getResource(path));
        if (importedClass == nullptr) {
            return ca::core::Err(
                error(WorkspaceError::DataLoss, "open class", "class resource was not imported"));
        }
        auto info = decodeClass(path, *importedClass);
        if (info.is_err()) return ca::core::Err(std::move(info).unwrap_err());
        auto decoded = std::move(info).unwrap();
        if (decoded.internalName.empty()) {
            return ca::core::Err(error(WorkspaceError::DataLoss, "open class",
                                       "class has an empty internal name"));
        }

        const auto               uri = decoded.internalName + ".class";
        model::ResourceContainer loaded;
        loaded.addResource(uri,
                           std::make_unique<model::ClassResource>(uri, importedClass->get()));
        std::vector<jasm::Diagnostic> diagnostics;
        // openClass 门禁只覆盖本类（作用域收窄；relaxed 语义下协变捷径
        // 基线类照常打开）。
        const WorkspaceErrorInfo      validation =
            validateCandidate(loaded, diagnostics, jasm::ValidationMode::Relaxed, {uri});
        if (validation.is_err()) {
            return ca::core::Err(error(WorkspaceError::DataLoss, "open class",
                                       "class did not pass validation"));
        }

        auto original = cloneContainer(loaded);
        if (original.is_err()) return ca::core::Err(std::move(original).unwrap_err());
        auto working = cloneContainer(loaded);
        if (working.is_err()) return ca::core::Err(std::move(working).unwrap_err());

        original_         = std::move(original).unwrap();
        working_          = std::move(working).unwrap();
        artifactKind_     = ArtifactKind::Class;
        sourcePath_       = path;
        primaryClassUri_  = uri;
        supporting_.clear();
        auto digest = ledgerBaseDigest();
        if (digest.is_err()) return ca::core::Err(std::move(digest).unwrap_err());
        if (ledger_.bound()) {
            if (ledger_.hasPending()) {
                auto flushed = ledger_.flush(project::rfc3339UtcNow());
                if (flushed.is_err())
                    return ca::core::Err(
                        ledgerFailure("open class", std::move(flushed).unwrap_err()));
            }
            ledger_.resetSession(project::copyLedgerText(digest.unwrap()));
        }
        else {
            auto fresh = project::EditLedgerStore::createInMemory(project::copyLedgerText(digest.unwrap()));
            if (fresh.is_err())
                return ca::core::Err(ledgerFailure("open class", std::move(fresh).unwrap_err()));
            ledger_ = std::move(fresh).unwrap();
        }
        redoHint_.clear();
        revision_         = 0;
        nextSupportingId_ = 1;
        return ca::core::Ok();
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::DataLoss, "open class", exception.what()));
    }
}

WorkspaceResult<SupportingResourceInfo> WorkspaceBase::addSupportingJar(const std::string& path)
{
    return addSupportingRole(path, "support");
}

WorkspaceResult<SupportingResourceInfo> WorkspaceBase::addSupportingContainer(
    const std::string& path, model::ContainerRole role)
{
    // primary 由 project_open 绑定，不接受挂载登记。
    if (role == model::ContainerRole::Primary)
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "add supporting container",
                                   "primary role is bound by project open"));
    return addSupportingRole(path, std::string{model::to_string(role)});
}

WorkspaceResult<SupportingResourceInfo> WorkspaceBase::addSupportingRole(
    const std::string& path, const std::string& roleLabel)
{
    WorkspaceErrorInfo loaded = ensureLoaded("add supporting JAR");
    if (loaded.is_err()) return ca::core::Err(std::move(loaded));
    auto normalized = normalizedRegularFile(path, "add supporting JAR");
    if (normalized.is_err()) return ca::core::Err(std::move(normalized).unwrap_err());
    const auto normalPath = std::move(normalized).unwrap();
    for (const auto& support : supporting_) {
        // 路径唯一约束跨角色生效：同一容器换角色挂载不追加第二条记录。
        if (support.path == normalPath) {
            return ca::core::Err(error(WorkspaceError::AlreadyExists, "add supporting JAR",
                                       "supporting path is already attached"));
        }
    }
    try {
        auto container = std::make_unique<model::ResourceContainer>();
        auto imported  = model::ResourceHelper::importFromJar(*container, normalPath);
        if (imported.is_err())
            return ca::core::Err(
                modelFailure("add supporting JAR", std::move(imported).unwrap_err()));
        auto fingerprint = fingerprintContainer(*container);
        if (fingerprint.is_err()) return ca::core::Err(std::move(fingerprint).unwrap_err());
        const auto fingerprintText = std::move(fingerprint).unwrap();
        for (const auto& support : supporting_) {
            if (support.fingerprint == fingerprintText) {
                return ca::core::Err(error(WorkspaceError::AlreadyExists, "add supporting JAR",
                                           "supporting content is already attached"));
            }
        }

        SupportingResourceInfo info;
        info.id            = "support-" + std::to_string(nextSupportingId_++);
        info.path          = normalPath;
        info.fingerprint   = fingerprintText;
        info.resourceCount = container->size();
        info.role          = roleLabel;
        container->visitResources([&](std::string_view, const model::IResource& resource) {
            if (resource.getType() == model::ResourceType::Class) ++info.classCount;
        });
        supporting_.push_back(
            {info.id, info.path, info.fingerprint, roleLabel, std::move(container)});
        return ca::core::Ok(std::move(info));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::DataLoss, "add supporting JAR", exception.what()));
    }
}

WorkspaceResult<SupportingResourceInfo> WorkspaceBase::removeSupportingJar(const std::string& id)
{
    WorkspaceErrorInfo loaded = ensureLoaded("remove supporting JAR");
    if (loaded.is_err()) return ca::core::Err(std::move(loaded));
    if (id.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "remove supporting JAR",
                                   "support ID is empty"));
    }
    const auto found =
        std::find_if(supporting_.begin(), supporting_.end(),
                     [&](const SupportingResource& support) { return support.id == id; });
    if (found == supporting_.end())
        return ca::core::Err(error(WorkspaceError::NotFound, "remove supporting JAR", id));

    // 一容器一记录：摘除即清理该容器的全部角色注册，无残留半边。
    SupportingResourceInfo info;
    info.id            = found->id;
    info.path          = found->path;
    info.fingerprint   = found->fingerprint;
    info.resourceCount = found->container->size();
    info.role          = found->role;
    found->container->visitResources([&](std::string_view, const model::IResource& resource) {
        if (resource.getType() == model::ResourceType::Class) ++info.classCount;
    });
    supporting_.erase(found);
    return ca::core::Ok(std::move(info));
}

// ── 状态面 ──────────────────────────────────────────────────────────────

bool WorkspaceBase::isLoaded() const noexcept
{
    return working_ != nullptr && original_ != nullptr;
}

ArtifactKind WorkspaceBase::artifactKind() const noexcept
{
    return artifactKind_;
}

const std::string& WorkspaceBase::sourcePath() const noexcept
{
    return sourcePath_;
}

const std::string& WorkspaceBase::primaryClassUri() const noexcept
{
    return primaryClassUri_;
}

ca::u64 WorkspaceBase::revision() const noexcept
{
    return revision_;
}

bool WorkspaceBase::isDirty() const
{
    if (!isLoaded()) return false;
    return !diffContainers(*original_, *working_).empty();
}

bool WorkspaceBase::canExport() const noexcept
{
    return isLoaded();
}

std::vector<PassDescription> WorkspaceBase::availablePasses()
{
    std::vector<PassDescription> result;
    for (const auto& pass : spi::defaultPassRegistry().list()) {
        // scope 输出小写（mbase 契约照迁）；mjbase 第四 scope Cf 无默认种子，
        // 仅为穷尽 switch 兜底。
        const std::string scope = pass.scope == spi::PassScope::Method   ? "method"
                                  : pass.scope == spi::PassScope::Class  ? "class"
                                  : pass.scope == spi::PassScope::Global ? "global"
                                                                         : "cf";
        result.push_back({std::string(pass.name), std::string(pass.description), scope});
    }
    return result;
}

std::vector<PipelineDescription> WorkspaceBase::availablePipelines()
{
    spi::registerBuiltinPipelines();
    std::vector<PipelineDescription> result;
    for (const auto& pipeline : spi::PassRegistry::instance().listPipelines())
        result.push_back({pipeline.name, pipeline.description});
    return result;
}

WorkspaceErrorInfo WorkspaceBase::ensureLoaded(const std::string& operation) const
{
    if (!isLoaded())
        return error(WorkspaceError::FailedPrecondition, operation, "workspace is not loaded");
    return WorkspaceErrorInfo{};
}

bool WorkspaceBase::accepts(const MutationRequest& request) const
{
    return request.expectedRevision == kAnyRevision || request.expectedRevision == revision_;
}

MutationResult WorkspaceBase::revisionConflict() const
{
    MutationResult result;
    result.status = error(WorkspaceError::Aborted, "mutate workspace",
                          "expected revision does not match current revision");
    result.revision   = revision_;
    result.exportable = canExport();
    return result;
}

MutationResult WorkspaceBase::stateFailure(WorkspaceErrorInfo status) const
{
    MutationResult result;
    result.status     = std::move(status);
    result.revision   = revision_;
    result.exportable = canExport();
    return result;
}

model::ResourceContainer* WorkspaceBase::workingContainer() noexcept
{
    return working_.get();
}

const model::ResourceContainer* WorkspaceBase::workingContainer() const noexcept
{
    return working_.get();
}

std::unique_ptr<model::ResourceContainer> WorkspaceBase::cloneWorking() const
{
    auto clone = cloneContainer(*working_);
    if (clone.is_err()) throw std::runtime_error(std::move(clone).unwrap_err().message);
    return std::move(clone).unwrap();
}

std::unique_ptr<model::ResourceContainer> WorkspaceBase::cloneOriginal() const
{
    auto clone = cloneContainer(*original_);
    if (clone.is_err()) throw std::runtime_error(std::move(clone).unwrap_err().message);
    return std::move(clone).unwrap();
}

std::unique_ptr<model::ResourceContainer> WorkspaceBase::analysisScanContainer() const
{
    auto merged = std::make_unique<model::ResourceContainer>();
    working_->visitResources([&](std::string_view uri, const model::IResource& resource) {
        auto copied = cloneResource(resource);
        if (copied) merged->addResource(uri, std::move(copied));
    });
    for (const auto& support : supporting_) {
        support.container->visitResources(
            [&](std::string_view uri, const model::IResource& resource) {
                if (merged->getResource(uri) != nullptr) return;   // 主容器 URI 优先
                auto copied = cloneResource(resource);
                if (copied) merged->addResource(uri, std::move(copied));
            });
    }
    return merged;
}

std::unordered_map<std::string, std::string> WorkspaceBase::supportingClassIds() const
{
    std::unordered_set<std::string> primary;
    working_->visitResources([&](std::string_view, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        const auto name = internalNameOfBytes(classResource->get());
        if (!name.empty()) primary.insert(name);
    });

    std::unordered_map<std::string, std::string> result;
    for (const auto& support : supporting_) {
        support.container->visitResources([&](std::string_view, const model::IResource& resource) {
            const auto* classResource =
                ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
            if (classResource == nullptr) return;
            const auto name = internalNameOfBytes(classResource->get());
            if (!name.empty() && primary.find(name) == primary.end())
                result.emplace(name, support.id);
        });
    }
    return result;
}

void WorkspaceBase::annotateSupportingSources(std::vector<ReferenceInfo>& references) const
{
    const auto supportingClasses = supportingClassIds();
    for (auto& reference : references) {
        const auto source = supportingClasses.find(reference.sourceClass);
        if (source == supportingClasses.end()) continue;
        reference.supporting = true;
        reference.supportId  = source->second;
    }
}

WorkspaceResult<WorkspaceBase::AnalysisGraph> WorkspaceBase::analysisGraph() const
{
    WorkspaceErrorInfo status = ensureLoaded("build analysis classpath");
    if (status.is_err()) return ca::core::Err(std::move(status));
    try {
        AnalysisGraph                        graph;
        std::vector<hierarchy::ClassSummary> pending;
        std::unordered_set<ca::u32>          registered;

        const auto appendClasses = [&](const model::ResourceContainer& source, bool primaryOwner) {
            source.visitResources([&](std::string_view, const model::IResource& resource) {
                const auto* classResource =
                    ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
                if (classResource == nullptr) return;
                auto session = sessionOf(classResource->get(), "build analysis classpath");
                if (session.is_err()) return;   // 畸形支撑类不供层次事实（照旧容忍）
                auto summary = hierarchy::fillSummary(*std::move(session).unwrap(), *graph.pool);
                if (primaryOwner || registered.insert(summary.self.id).second)
                    pending.push_back(std::move(summary));
            });
        };
        appendClasses(*working_, true);
        for (const auto& support : supporting_) appendClasses(*support.container, false);

        // 未登记超类型物化为合成节点（java/lang/Object 等；无成员、access 0）。
        std::unordered_set<ca::u32> known;
        for (const auto& summary : pending) known.insert(summary.self.id);
        std::vector<hierarchy::ClassSummary> synthesized;
        const auto ensureNode = [&](ca::u32 keyId) {
            if (keyId == 0 || known.count(keyId) != 0) return;
            known.insert(keyId);
            hierarchy::ClassSummary stub;
            stub.self = mjbase::ClassKey{keyId};
            synthesized.push_back(std::move(stub));
        };
        for (const auto& summary : pending) {
            ensureNode(summary.super.id);
            for (const auto& interfaceKey : summary.interfaces) ensureNode(interfaceKey.id);
        }

        for (auto& summary : pending) {
            if (graph.hierarchy->addClass(summary).is_ok())
                graph.summaries.emplace(summary.self.id, std::move(summary));
        }
        for (auto& stub : synthesized) {
            if (graph.hierarchy->addClass(stub).is_ok())
                graph.summaries.emplace(stub.self.id, std::move(stub));
        }
        return ca::core::Ok(std::move(graph));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "build analysis classpath", exception.what()));
    }
}

// ── 资源 / class 读取面 ─────────────────────────────────────────────────

WorkspaceResult<std::vector<ResourceSummary>> WorkspaceBase::listResources(
    bool includeClasses) const
{
    WorkspaceErrorInfo status = ensureLoaded("list resources");
    if (status.is_err()) return ca::core::Err(std::move(status));
    std::vector<ResourceSummary> result;
    working_->visitResources([&](std::string_view uri, const model::IResource& resource) {
        if (!includeClasses && resource.getType() == model::ResourceType::Class) return;
        auto bytes = bytesOf(resource);
        if (bytes.is_err()) return;
        result.push_back({std::string(uri), resourceTypeName(resource.getType()),
                          bytes.unwrap().size()});
    });
    std::sort(result.begin(), result.end(),
              [](const ResourceSummary& left, const ResourceSummary& right) {
                  return left.uri < right.uri;
              });
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<ResourceContent> WorkspaceBase::readResource(const std::string& uri) const
{
    WorkspaceErrorInfo status = ensureLoaded("read resource");
    if (status.is_err()) return ca::core::Err(std::move(status));
    const model::IResource* resource = working_->getResource(uri);
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "read resource", uri));
    auto bytes = bytesOf(*resource);
    if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());
    ResourceContent result;
    result.summary = {uri, resourceTypeName(resource->getType()), bytes.unwrap().size()};
    result.bytes   = std::move(bytes).unwrap();
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<ClassBytesInfo> WorkspaceBase::readClassBytes(
    const std::string& uriOrInternalName) const
{
    WorkspaceErrorInfo status = ensureLoaded("read class bytes");
    if (status.is_err()) return ca::core::Err(std::move(status));
    std::string uri;
    if (hasClassExtension(uriOrInternalName) &&
        working_->getResource(uriOrInternalName) != nullptr) {
        uri = uriOrInternalName;
    }
    else {
        auto resolved = resolveClassUri(*working_, uriOrInternalName);
        if (resolved.is_err()) return ca::core::Err(std::move(resolved).unwrap_err());
        uri = std::move(resolved).unwrap();
    }
    const model::IResource* resource = working_->getResource(uri);
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "read class bytes", uri));
    auto bytes = bytesOf(*resource);
    if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());

    ClassBytesInfo result;
    result.resourceUri  = uri;
    result.bytes        = std::move(bytes).unwrap();
    result.fingerprint  = fingerprintBytes(result.bytes);
    result.internalName = internalNameOfBytes(result.bytes);
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::string> WorkspaceBase::resolveClassUri(
    const model::ResourceContainer& container, const std::string& uriOrInternalName) const
{
    if (uriOrInternalName.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "resolve class", "selector is empty"));
    if (ca::core::tag_cast::isa<model::ClassResource>(container.getResource(uriOrInternalName)))
        return ca::core::Ok(uriOrInternalName);
    try {
        std::string match;
        container.visitResources([&](std::string_view uri, const model::IResource& resource) {
            const auto* classResource =
                ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
            if (classResource == nullptr || !match.empty()) return;
            if (internalNameOfBytes(classResource->get()) == uriOrInternalName)
                match = std::string(uri);
        });
        if (match.empty())
            return ca::core::Err(
                error(WorkspaceError::NotFound, "resolve class", uriOrInternalName));
        return ca::core::Ok(std::move(match));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::DataLoss, "resolve class", exception.what()));
    }
}

WorkspaceResult<std::vector<ClassSummary>> WorkspaceBase::listClassSummaries() const
{
    WorkspaceErrorInfo status = ensureLoaded("list class summaries");
    if (status.is_err()) return ca::core::Err(std::move(status));
    std::vector<ClassSummary> result;
    working_->visitResources([&](std::string_view uri, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        auto decoded = decodeClassSummary(std::string(uri), *classResource);
        if (decoded.is_err()) return;
        result.push_back(std::move(decoded).unwrap());
    });
    std::sort(result.begin(), result.end(),
              [](const ClassSummary& left, const ClassSummary& right) {
                  if (left.internalName != right.internalName)
                      return left.internalName < right.internalName;
                  return left.resourceUri < right.resourceUri;
              });
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<ClassInfo>> WorkspaceBase::listClasses() const
{
    WorkspaceErrorInfo status = ensureLoaded("list classes");
    if (status.is_err()) return ca::core::Err(std::move(status));
    std::vector<ClassInfo> result;
    working_->visitResources([&](std::string_view uri, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        auto decoded = decodeClass(std::string(uri), *classResource);
        if (decoded.is_err()) return;
        result.push_back(std::move(decoded).unwrap());
    });
    std::sort(result.begin(), result.end(), [](const ClassInfo& left, const ClassInfo& right) {
        if (left.internalName != right.internalName) return left.internalName < right.internalName;
        return left.resourceUri < right.resourceUri;
    });
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<ClassInfo> WorkspaceBase::classInfo(const std::string& uriOrInternalName) const
{
    WorkspaceErrorInfo status = ensureLoaded("get class info");
    if (status.is_err()) return ca::core::Err(std::move(status));
    auto uri = resolveClassUri(*working_, uriOrInternalName);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get class info", uri.unwrap()));
    return decodeClass(uri.unwrap(), *resource);
}

WorkspaceResult<MethodInfo> WorkspaceBase::methodInfo(const std::string& owner,
                                                      const std::string& name,
                                                      const std::string& descriptor) const
{
    if (name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get method info",
                                   "name and descriptor are required"));
    }
    auto decoded = classInfo(owner);
    if (decoded.is_err()) return ca::core::Err(std::move(decoded).unwrap_err());
    for (const auto& method : decoded.unwrap().methods) {
        if (method.name == name && method.descriptor == descriptor) return ca::core::Ok(method);
    }
    return ca::core::Err(
        error(WorkspaceError::NotFound, "get method info", owner + "." + name + descriptor));
}

WorkspaceResult<FieldInfo> WorkspaceBase::fieldInfo(const std::string& owner,
                                                    const std::string& name,
                                                    const std::string& descriptor) const
{
    WorkspaceErrorInfo status = ensureLoaded("get field info");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get field info",
                                   "owner, name and descriptor are required"));
    }
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get field info", owner));
    auto session = sessionOf(resource->get(), "get field info");
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                  held = std::move(session).unwrap();
    jbc::core::ClassFile& cf   = held->backing();
    for (auto& field : cf.fields) {
        if (utf8Text(cf, field.nameCp) == name && utf8Text(cf, field.descCp) == descriptor)
            return ca::core::Ok(makeFieldInfo(cf, field));
    }
    return ca::core::Err(
        error(WorkspaceError::NotFound, "get field info", owner + "." + name + descriptor));
}

WorkspaceResult<std::vector<ConstantPoolEntryInfo>> WorkspaceBase::constantPool(
    const std::string& uriOrInternalName, bool resolve) const
{
    WorkspaceErrorInfo status = ensureLoaded("get constant pool");
    if (status.is_err()) return ca::core::Err(std::move(status));
    auto uri = resolveClassUri(*working_, uriOrInternalName);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get constant pool", uri.unwrap()));
    auto session = sessionOf(resource->get(), "get constant pool");
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                        held = std::move(session).unwrap();
    const jbc::core::ClassFile& cf   = held->backing();
    std::vector<ConstantPoolEntryInfo> result;
    for (ca::u32 index = 1; index < cf.cp.size(); ++index) {
        result.push_back(support::constantPoolEntry(cf, index, resolve));
    }
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<MethodInstructions> WorkspaceBase::methodInstructions(
    const std::string& owner, const std::string& name, const std::string& descriptor,
    bool resolve) const
{
    WorkspaceErrorInfo status = ensureLoaded("get method instructions");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get method instructions",
                                   "owner, name and descriptor are required"));
    }
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get method instructions", owner));
    auto session = sessionOf(resource->get(), "get method instructions");
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                  held = std::move(session).unwrap();
    jbc::core::ClassFile& cf   = held->backing();
    for (ca::usize methodIndex = 0; methodIndex < cf.methods.size(); ++methodIndex) {
        auto& method = cf.methods[methodIndex];
        if (utf8Text(cf, method.nameCp) != name || utf8Text(cf, method.descCp) != descriptor)
            continue;
        jbc::core::CodeAttr* code = support::ensureCode(cf, method);
        if (code == nullptr)
            return ca::core::Err(error(WorkspaceError::NotFound, "get method instructions",
                                       "method has no Code attribute"));
        auto promoted = jbc::ir::promoteMethod(*held, methodIndex);
        if (promoted.is_err())
            return ca::core::Err(error(WorkspaceError::DataLoss, "get method instructions",
                                       "method body cannot be promoted"));
        auto raw = promoted.unwrap()->dom.exportRaw();
        if (raw.is_err())
            return ca::core::Err(error(WorkspaceError::DataLoss, "get method instructions",
                                       "method body cannot be exported"));
        const auto         insns = std::move(raw).unwrap();
        MethodInstructions result;
        result.maxStack  = static_cast<ca::i32>(code->maxStack);
        result.maxLocals = static_cast<ca::i32>(code->maxLocals);
        for (const auto& insn : insns) {
            InstructionDetailInfo rendered = renderInstruction(cf, insn);
            if (resolve) rendered.operandComment = ldcComment(cf, insn.cp);
            result.instructions.push_back(std::move(rendered));
        }
        return ca::core::Ok(std::move(result));
    }
    return ca::core::Err(error(WorkspaceError::NotFound, "get method instructions",
                               owner + "." + name + descriptor));
}

WorkspaceResult<MethodBytecode> WorkspaceBase::methodBytecode(const std::string& owner,
                                                              const std::string& name,
                                                              const std::string& descriptor) const
{
    WorkspaceErrorInfo status = ensureLoaded("get method bytecode");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get method bytecode",
                                   "owner, name and descriptor are required"));
    }
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get method bytecode", owner));
    auto session = sessionOf(resource->get(), "get method bytecode");
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                  held = std::move(session).unwrap();
    jbc::core::ClassFile& cf   = held->backing();
    for (ca::usize methodIndex = 0; methodIndex < cf.methods.size(); ++methodIndex) {
        auto& method = cf.methods[methodIndex];
        if (utf8Text(cf, method.nameCp) != name || utf8Text(cf, method.descCp) != descriptor)
            continue;
        auto info = makeMethodInfo(*held, method, methodIndex);
        if (info.is_err()) return ca::core::Err(std::move(info).unwrap_err());
        MethodBytecode result;
        result.method = std::move(info).unwrap();
        if (support::ensureCode(cf, method) != nullptr) {
            auto promoted = jbc::ir::promoteMethod(*held, methodIndex);
            if (promoted.is_err())
                return ca::core::Err(error(WorkspaceError::DataLoss, "get method bytecode",
                                           "method body cannot be promoted"));
            auto raw = promoted.unwrap()->dom.exportRaw();
            if (raw.is_err())
                return ca::core::Err(error(WorkspaceError::DataLoss, "get method bytecode",
                                       "method body cannot be exported"));
            const auto insns = std::move(raw).unwrap();
            ca::i32    index = 0;
            for (const auto& insn : insns) {
                InstructionInfo item;
                item.index  = index++;
                item.opcode = static_cast<ca::i32>(insn.op);
                const char* mnemonic = jbc::core::mnemonicOf(insn.op);
                item.mnemonic        = mnemonic != nullptr ? mnemonic : "?";
                item.nodeType        = instructionNodeType(insn.op);
                result.instructions.push_back(std::move(item));
            }
        }
        return ca::core::Ok(std::move(result));
    }
    return ca::core::Err(error(WorkspaceError::NotFound, "get method bytecode",
                               owner + "." + name + descriptor));
}

jasm::Result<jasm::JvmJasmMethodDisassembly> WorkspaceBase::getMethodJasm(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    if (!isLoaded()) return jasmFailure<jasm::JvmJasmMethodDisassembly>("workspace is not loaded");
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) {
        return jasmFailure<jasm::JvmJasmMethodDisassembly>(std::move(uri).unwrap_err().message);
    }
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return jasmFailure<jasm::JvmJasmMethodDisassembly>("Class resource not found: " + owner);
    const auto nameRef = ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(name.data()), name.size());
    const auto descriptorRef = ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(descriptor.data()), descriptor.size());
    return jasm::JvmJasmService{}.disassembleMethod(resource->get(), nameRef, descriptorRef);
}

WorkspaceResult<std::vector<ManifestAttribute>> WorkspaceBase::manifestAttributes() const
{
    WorkspaceErrorInfo status = ensureLoaded("read manifest");
    if (status.is_err()) return ca::core::Err(std::move(status));
    const model::ManifestResource* manifest = nullptr;
    working_->visitResources([&](std::string_view, const model::IResource& resource) {
        if (manifest == nullptr)
            manifest = ca::core::tag_cast::dyn_cast<const model::ManifestResource>(&resource);
    });
    if (manifest == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "read manifest", "MANIFEST.MF"));
    std::vector<ManifestAttribute> result;
    result.reserve(manifest->get().getMainAttributes().size());
    for (const auto& [key, value] : manifest->get().getMainAttributes()) {
        result.push_back({key.to_std_string(), value.to_std_string()});
    }
    std::sort(result.begin(), result.end(),
              [](const ManifestAttribute& left, const ManifestAttribute& right) {
                  return left.key < right.key;
              });
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<ResourceStringInfo>> WorkspaceBase::resourceStrings(
    ca::usize minimumLength) const
{
    WorkspaceErrorInfo status = ensureLoaded("extract resource strings");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (minimumLength == 0) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "extract resource strings",
                                   "minimum length must be positive"));
    }
    try {
        std::vector<ResourceStringInfo> result;
        working_->visitResources([&](std::string_view uriView, const model::IResource& resource) {
            if (resource.getType() == model::ResourceType::Class) return;
            auto              bytes = bytesOf(resource);
            if (bytes.is_err()) return;
            const auto&       value = bytes.unwrap();
            const std::string uri{uriView};
            ca::usize         start = 0;
            for (ca::usize index = 0; index <= value.size(); ++index) {
                const bool printable =
                    index < value.size() && value[index] >= 0x20 && value[index] <= 0x7e;
                if (printable) continue;
                if (index - start >= minimumLength) {
                    result.push_back(
                        {uri, start,
                         std::string(reinterpret_cast<const char*>(value.data() + start),
                                     index - start)});
                }
                start = index + 1;
            }
        });
        std::sort(result.begin(), result.end(),
                  [](const ResourceStringInfo& left, const ResourceStringInfo& right) {
                      if (left.uri != right.uri) return left.uri < right.uri;
                      if (left.offset != right.offset) return left.offset < right.offset;
                      return left.value < right.value;
                  });
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "extract resource strings", exception.what()));
    }
}

// ── 搜索面 ──────────────────────────────────────────────────────────────

WorkspaceResult<std::vector<SearchHit>> WorkspaceBase::searchClasses(
    const std::string& query, const SearchOptions& options) const
{
    WorkspaceErrorInfo status = ensureLoaded("search classes");
    if (status.is_err()) return ca::core::Err(std::move(status));
    auto compiled = compileSearchRegex(query, options, "search classes");
    if (compiled.is_err()) return ca::core::Err(std::move(compiled).unwrap_err());
    auto        compiledRegex = std::move(compiled).unwrap();
    const auto* regex         = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;
    working_->visitResources([&](std::string_view uriView, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        const std::string uri{uriView};
        auto              decoded = decodeClass(uri, *classResource);
        if (decoded.is_err()) return;
        const auto& info = decoded.unwrap();
        if (!matchesSearchScope(info.internalName, options)) return;
        if (!matchesAnyText({info.internalName, info.resourceUri}, query, options, regex)) return;
        result.push_back(
            {info.resourceUri, "class", info.internalName, {}, {}, -1, info.internalName});
    });
    sortAndDeduplicateSearchHits(result);
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<SearchHit>> WorkspaceBase::searchMembers(
    const std::string& query, const SearchOptions& options) const
{
    WorkspaceErrorInfo status = ensureLoaded("search members");
    if (status.is_err()) return ca::core::Err(std::move(status));
    auto compiled = compileSearchRegex(query, options, "search members");
    if (compiled.is_err()) return ca::core::Err(std::move(compiled).unwrap_err());
    auto        compiledRegex = std::move(compiled).unwrap();
    const auto* regex         = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;
    working_->visitResources([&](std::string_view uriView, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        const std::string uri{uriView};
        auto              decoded = decodeClass(uri, *classResource);
        if (decoded.is_err()) return;
        const auto& info = decoded.unwrap();
        if (!matchesSearchScope(info.internalName, options)) return;
        for (const auto& field : info.fields) {
            if (!options.memberKind.empty() && options.memberKind != "field") continue;
            const auto searchable = info.internalName + " " + field.name + " " +
                                    field.descriptor + " " + field.signature;
            if (!matchesAnyText(
                    {info.internalName, field.name, field.descriptor, field.signature, searchable},
                    query, options, regex)) {
                continue;
            }
            result.push_back({info.resourceUri, "field", info.internalName, field.name,
                              field.descriptor, -1, field.name + field.descriptor});
        }
        for (const auto& method : info.methods) {
            if (!options.memberKind.empty() && options.memberKind != "method") continue;
            const auto searchable = info.internalName + " " + method.name + " " +
                                    method.descriptor + " " + method.signature;
            if (!matchesAnyText({info.internalName, method.name, method.descriptor,
                                 method.signature, searchable},
                                query, options, regex)) {
                continue;
            }
            result.push_back({info.resourceUri, "method", info.internalName, method.name,
                              method.descriptor, -1, method.name + method.descriptor});
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<SearchHit>> WorkspaceBase::searchStrings(
    const std::string& query, const SearchOptions& options) const
{
    WorkspaceErrorInfo status = ensureLoaded("search strings");
    if (status.is_err()) return ca::core::Err(std::move(status));
    auto compiled = compileSearchRegex(query, options, "search strings");
    if (compiled.is_err()) return ca::core::Err(std::move(compiled).unwrap_err());
    auto        compiledRegex = std::move(compiled).unwrap();
    const auto* regex         = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;

    // 非 class 资源可打印串（无 owner 语义：owner/packagePrefix 过滤时不参与）。
    if (options.owner.empty() && options.packagePrefix.empty()) {
        auto resources = resourceStrings(1);
        if (resources.is_err()) return ca::core::Err(std::move(resources).unwrap_err());
        for (const auto& item : resources.unwrap()) {
            if (!matchesText(item.value, query, options, regex)) continue;
            result.push_back({item.uri, "resource_string", {}, {}, {}, -1, item.value});
        }
    }

    working_->visitResources([&](std::string_view uriView, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        const std::string uri{uriView};
        auto              session = sessionOf(classResource->get(), "search strings");
        if (session.is_err()) return;   // 搜索对畸形 class 宽容（照旧）
        auto                        held = std::move(session).unwrap();
        const jbc::core::ClassFile& cf   = held->backing();
        const auto                  owner = std::string{cf.thisName()};
        if (!matchesSearchScope(owner, options)) return;
        for (ca::u32 index = 1; index < cf.cp.size(); ++index) {
            if (!cf.cp.valid(index)) continue;
            const auto& entry = cf.cp.at(index);
            std::string value;
            std::string kind;
            if (entry.tag == jbc::core::CpTag::Utf8) {
                value = utf8Text(cf, index);
                kind  = "constant_pool_utf8";
            }
            else if (entry.tag == jbc::core::CpTag::String) {
                value = utf8Text(cf, entry.a);
                kind  = "constant_pool_string";
            }
            else {
                continue;
            }
            if (!matchesText(value, query, options, regex)) continue;
            result.push_back({uri, kind, owner, {}, {}, -1, std::move(value)});
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<SearchHit>> WorkspaceBase::searchNumbers(
    const std::string& query, const SearchOptions& options) const
{
    WorkspaceErrorInfo status = ensureLoaded("search numbers");
    if (status.is_err()) return ca::core::Err(std::move(status));
    status = validateSearchOptions(query, options, "search numbers");
    if (status.is_err()) return ca::core::Err(std::move(status));
    status = validateNumberQuery(query);
    if (status.is_err()) return ca::core::Err(std::move(status));

    std::vector<SearchHit> result;
    working_->visitResources([&](std::string_view uriView, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        const std::string uri{uriView};
        auto              session = sessionOf(classResource->get(), "search numbers");
        if (session.is_err()) return;
        auto                  held = std::move(session).unwrap();
        jbc::core::ClassFile& cf   = held->backing();
        const auto            owner = std::string{cf.thisName()};
        if (!matchesSearchScope(owner, options)) return;

        if (options.opcode < 0) {
            for (ca::u32 index = 1; index < cf.cp.size(); ++index) {
                if (!cf.cp.valid(index)) continue;
                const auto& entry = cf.cp.at(index);
                std::string value;
                switch (entry.tag) {
                case jbc::core::CpTag::Integer: value = numberText(entry.ival); break;
                case jbc::core::CpTag::Float:
                    value = numberText(support::bitsToFloat(static_cast<ca::u32>(entry.ival)));
                    break;
                case jbc::core::CpTag::Long: value = numberText(entry.lval); break;
                case jbc::core::CpTag::Double:
                    value = numberText(support::bitsToDouble(static_cast<ca::u64>(entry.lval)));
                    break;
                default: continue;
                }
                if (!matchesNumber(value, query)) continue;
                result.push_back(
                    {uri, "constant_pool_number", owner, {}, {}, -1, std::move(value)});
            }
        }

        // 指令数值字面量：iconst/lconst/fconst/dconst 族 + 立即数/iinc/switch/ldc。
        for (auto& visit : jbc::ir::collectCodeUnits(*held)) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
            auto raw = visit.unit->dom.exportRaw();
            if (raw.is_err()) continue;
            const auto&       insns = std::move(raw).unwrap();
            auto&             method = cf.methods[visit.methodIdx];
            const std::string methodName = utf8Text(cf, method.nameCp);
            const std::string methodDesc = utf8Text(cf, method.descCp);
            for (ca::i32 index = 0; index < static_cast<ca::i32>(insns.size()); ++index) {
                const auto& insn = insns[static_cast<ca::usize>(index)];
                if (options.opcode >= 0 && static_cast<ca::i32>(insn.op) != options.opcode)
                    continue;
                std::vector<std::string> values;
                switch (insn.op) {
                case jbc::core::Op::ICONST_M1: values.push_back("-1"); break;
                case jbc::core::Op::ICONST_0:
                case jbc::core::Op::LCONST_0:
                case jbc::core::Op::FCONST_0:
                case jbc::core::Op::DCONST_0: values.push_back("0"); break;
                case jbc::core::Op::ICONST_1:
                case jbc::core::Op::LCONST_1:
                case jbc::core::Op::FCONST_1:
                case jbc::core::Op::DCONST_1: values.push_back("1"); break;
                case jbc::core::Op::ICONST_2:
                case jbc::core::Op::FCONST_2: values.push_back("2"); break;
                case jbc::core::Op::ICONST_3: values.push_back("3"); break;
                case jbc::core::Op::ICONST_4: values.push_back("4"); break;
                case jbc::core::Op::ICONST_5: values.push_back("5"); break;
                default: break;
                }
                switch (jbc::core::shapeOf(insn.op)) {
                case jbc::core::Shape::Imm1:
                case jbc::core::Shape::Imm2:
                case jbc::core::Shape::Atype1:
                case jbc::core::Shape::Local1:
                    values.push_back(numberText(insn.a));
                    break;
                case jbc::core::Shape::Iinc1: values.push_back(numberText(insn.b)); break;
                case jbc::core::Shape::TableSwitch:
                    values.push_back(numberText(insn.a));
                    values.push_back(numberText(insn.b));
                    break;
                case jbc::core::Shape::LookupSwitch:
                    for (const auto key : insn.cases) values.push_back(numberText(key.key));
                    break;
                case jbc::core::Shape::CP2Dims: values.push_back(numberText(insn.a)); break;
                case jbc::core::Shape::CP1:
                case jbc::core::Shape::CP2:
                {
                    if (cf.cp.valid(insn.cp)) {
                        const auto& entry = cf.cp.at(insn.cp);
                        if (entry.tag == jbc::core::CpTag::Integer)
                            values.push_back(numberText(entry.ival));
                        else if (entry.tag == jbc::core::CpTag::Long)
                            values.push_back(numberText(entry.lval));
                        else if (entry.tag == jbc::core::CpTag::Float)
                            values.push_back(numberText(
                                support::bitsToFloat(static_cast<ca::u32>(entry.ival))));
                        else if (entry.tag == jbc::core::CpTag::Double)
                            values.push_back(numberText(
                                support::bitsToDouble(static_cast<ca::u64>(entry.lval))));
                    }
                    break;
                }
                default: break;
                }
                for (const auto& value : values) {
                    if (!matchesNumber(value, query)) continue;
                    result.push_back({uri, "instruction_number", owner, methodName, methodDesc,
                                      index, value});
                }
            }
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<SearchHit>> WorkspaceBase::searchInstructions(
    const std::string& query, const SearchOptions& options) const
{
    WorkspaceErrorInfo status = ensureLoaded("search instructions");
    if (status.is_err()) return ca::core::Err(std::move(status));
    auto compiled = compileSearchRegex(query, options, "search instructions");
    if (compiled.is_err()) return ca::core::Err(std::move(compiled).unwrap_err());
    auto        compiledRegex = std::move(compiled).unwrap();
    const auto* regex         = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;
    working_->visitResources([&](std::string_view uriView, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        const std::string uri{uriView};
        auto              session = sessionOf(classResource->get(), "search instructions");
        if (session.is_err()) return;
        auto                  held = std::move(session).unwrap();
        jbc::core::ClassFile& cf   = held->backing();
        const auto            owner = std::string{cf.thisName()};
        if (!matchesSearchScope(owner, options)) return;
        for (auto& visit : jbc::ir::collectCodeUnits(*held)) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
            auto raw = visit.unit->dom.exportRaw();
            if (raw.is_err()) continue;
            const auto&       insns = std::move(raw).unwrap();
            auto&             method = cf.methods[visit.methodIdx];
            const std::string methodName = utf8Text(cf, method.nameCp);
            const std::string methodDesc = utf8Text(cf, method.descCp);
            for (ca::i32 index = 0; index < static_cast<ca::i32>(insns.size()); ++index) {
                const auto& insn = insns[static_cast<ca::usize>(index)];
                if (options.opcode >= 0 && static_cast<ca::i32>(insn.op) != options.opcode)
                    continue;
                std::ostringstream output;
                const char*        mnemonic = jbc::core::mnemonicOf(insn.op);
                output << (mnemonic != nullptr ? mnemonic : "?") << ' '
                       << instructionNodeType(insn.op) << ' ' << static_cast<ca::i32>(insn.op);
                switch (jbc::core::shapeOf(insn.op)) {
                case jbc::core::Shape::Imm1:
                case jbc::core::Shape::Imm2:
                case jbc::core::Shape::Atype1:
                case jbc::core::Shape::Local1:
                    output << ' ' << insn.a;
                    break;
                case jbc::core::Shape::Iinc1: output << ' ' << insn.a << ' ' << insn.b; break;
                case jbc::core::Shape::CP2Dims:
                    output << ' ' << support::classNameOf(cf, insn.cp) << ' ' << insn.a;
                    break;
                case jbc::core::Shape::CP2Count:
                case jbc::core::Shape::CP2Zero2:
                    output << ' ' << memberRefFlatText(cf, insn.cp);
                    break;
                case jbc::core::Shape::CP1:
                case jbc::core::Shape::CP2:
                {
                    if (cf.cp.valid(insn.cp)) {
                        const auto& entry = cf.cp.at(insn.cp);
                        if (entry.tag == jbc::core::CpTag::String)
                            output << ' ' << utf8Text(cf, entry.a);
                        else if (entry.tag == jbc::core::CpTag::Class)
                            output << ' ' << support::classNameOf(cf, insn.cp);
                        else if (entry.tag == jbc::core::CpTag::Integer)
                            output << ' ' << entry.ival;
                        else if (entry.tag == jbc::core::CpTag::Long)
                            output << ' ' << entry.lval;
                        else if (entry.tag == jbc::core::CpTag::Float)
                            output << ' '
                                   << support::bitsToFloat(static_cast<ca::u32>(entry.ival));
                        else if (entry.tag == jbc::core::CpTag::Double)
                            output << ' '
                                   << support::bitsToDouble(static_cast<ca::u64>(entry.lval));
                        else
                        {
                            // CP2 形态含 invoke*virtual/static/special 与字段/类访问
                            // 指令——成员引用文本与旧栈 methodTarget 口径一致。
                            const auto reference = memberRefFlatText(cf, insn.cp);
                            if (!reference.empty()) output << ' ' << reference;
                        }
                    }
                    break;
                }
                default: break;
                }
                const auto value = output.str();
                if (!matchesText(value, query, options, regex)) continue;
                result.push_back({uri, "instruction", owner, methodName, methodDesc, index, value});
            }
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::core::Ok(std::move(result));
}

// ── 引用 / 层次分析面 ───────────────────────────────────────────────────

WorkspaceResult<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    WorkspaceErrorInfo status = ensureLoaded("xref method");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "xref method",
                                   "owner, name and descriptor are required"));
    }
    try {
        ReferenceScan scan;
        scan.build(*analysisScanContainer());
        std::vector<ReferenceInfo> result;
        for (const auto& reference : scan.methodReferencesTo(owner, name, descriptor)) {
            result.push_back(referenceInfo(
                reference.site,
                reference.target.owner + "." + reference.target.name +
                    reference.target.descriptor,
                "direct"));
        }
        annotateSupportingSources(result);
        sortReferences(result);
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "xref method", exception.what()));
    }
}

WorkspaceResult<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToField(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    WorkspaceErrorInfo status = ensureLoaded("xref field");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "xref field",
                                   "owner, name and descriptor are required"));
    }
    try {
        ReferenceScan scan;
        scan.build(*analysisScanContainer());
        std::vector<ReferenceInfo> result;
        for (const auto& reference : scan.fieldReferencesTo(owner, name, descriptor)) {
            result.push_back(referenceInfo(
                reference.site,
                reference.target.owner + "." + reference.target.name +
                    reference.target.descriptor,
                "direct"));
        }
        annotateSupportingSources(result);
        sortReferences(result);
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "xref field", exception.what()));
    }
}

WorkspaceResult<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToClass(
    const std::string& internalName) const
{
    WorkspaceErrorInfo status = ensureLoaded("xref class");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (internalName.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "xref class", "class is empty"));
    try {
        ReferenceScan scan;
        scan.build(*analysisScanContainer());
        std::vector<ReferenceInfo> result;
        for (const auto& reference : scan.typeReferencesTo(internalName)) {
            result.push_back(referenceInfo(reference.site, reference.targetType, "type"));
        }
        annotateSupportingSources(result);
        sortReferences(result);
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "xref class", exception.what()));
    }
}

WorkspaceResult<std::vector<ReferenceInfo>> WorkspaceBase::calleesOfMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor,
    bool resolved) const
{
    WorkspaceErrorInfo status = ensureLoaded("list callees");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "list callees",
                                   "owner, name and descriptor are required"));
    }
    try {
        std::vector<ReferenceInfo> result;
        if (!resolved) {
            ReferenceScan scan;
            scan.build(*analysisScanContainer());
            for (const auto& reference : scan.methodReferencesFrom(owner, name, descriptor)) {
                result.push_back(referenceInfo(
                    reference.site,
                    reference.target.owner + "." + reference.target.name +
                        reference.target.descriptor,
                    "direct"));
            }
        }
        else {
            // 解析展开（mjbase 面）：虚调用/接口调用沿层次图展开到「静态目标 +
            // 后代中声明同名同描述符方法」的候选集合（虚分派可达面）。旧栈
            // ResolvedCallGraph 的完整闭包设施未随迁，本近似覆盖 direct 与
            // 单继承/接口分派的展开语义——完整闭包由主线 ResolvedCallGraph
            // 设施接管（迁移报告项）。
            auto graph = analysisGraph();
            if (graph.is_err()) return ca::core::Err(std::move(graph).unwrap_err());
            auto           analysis = std::move(graph).unwrap();
            auto           uri      = resolveClassUri(*working_, owner);
            if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
            const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
                working_->getResource(uri.unwrap()));
            if (resource == nullptr)
                return ca::core::Err(error(WorkspaceError::NotFound, "list callees", owner));
            auto session = sessionOf(resource->get(), "list callees");
            if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
            auto                  held = std::move(session).unwrap();
            jbc::core::ClassFile& cf   = held->backing();
            for (ca::usize methodIndex = 0; methodIndex < cf.methods.size(); ++methodIndex) {
                auto& method = cf.methods[methodIndex];
                if (utf8Text(cf, method.nameCp) != name ||
                    utf8Text(cf, method.descCp) != descriptor)
                    continue;
                if (support::ensureCode(cf, method) == nullptr) break;
                auto promoted = jbc::ir::promoteMethod(*held, methodIndex);
                if (promoted.is_err()) break;
                const jbc::ir::CodeUnit& code  = *promoted.unwrap();
                const auto&              order = code.dom.order();
                for (ca::usize index = 0; index < order.size(); ++index) {
                    const jbc::ir::Insn* insn = code.dom.peek(order[index]);
                    if (insn == nullptr) continue;
                    if (insn->op != jbc::core::Op::INVOKEVIRTUAL &&
                        insn->op != jbc::core::Op::INVOKEINTERFACE &&
                        insn->op != jbc::core::Op::INVOKESPECIAL &&
                        insn->op != jbc::core::Op::INVOKESTATIC)
                        continue;
                    jbc::ir::ClassSession::MemberSem sem;
                    if (!held->memberOf(*insn, sem) || sem.owner.empty()) continue;
                    const std::string semOwner{sem.owner};
                    const std::string semName{sem.name};
                    const std::string semDesc{sem.desc};
                    const std::string declaredTarget = semOwner + "." + semName + semDesc;
                    bool              expanded       = false;
                    if (insn->op == jbc::core::Op::INVOKEVIRTUAL ||
                        insn->op == jbc::core::Op::INVOKEINTERFACE) {
                        const mjbase::ClassKey targetKey =
                            analysis.hierarchy->findClass(semOwner);
                        if (targetKey.valid()) {
                            std::vector<mjbase::ClassKey> candidates{targetKey};
                            for (const auto descendant :
                                 analysis.hierarchy->descendants(targetKey))
                                candidates.push_back(descendant);
                            for (const auto candidate : candidates) {
                                if (!analysis.declaresMethod(candidate.id, semName, semDesc))
                                    continue;
                                expanded = true;
                                result.push_back(referenceInfo(
                                    {owner, name, descriptor, static_cast<ca::i32>(index)},
                                    std::string{analysis.hierarchy->nameOf(candidate)} + "." +
                                        semName + semDesc,
                                    "resolved_override"));
                            }
                        }
                    }
                    if (!expanded) {
                        result.push_back(referenceInfo(
                            {owner, name, descriptor, static_cast<ca::i32>(index)},
                            declaredTarget, "direct"));
                    }
                }
                break;
            }
        }
        annotateSupportingSources(result);
        sortReferences(result);
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "list callees", exception.what()));
    }
}

WorkspaceResult<ClassRelations> WorkspaceBase::classRelations(
    const std::string& internalName) const
{
    WorkspaceErrorInfo status = ensureLoaded("get class relations");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (internalName.empty()) {
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "get class relations", "class is empty"));
    }
    try {
        auto graph = analysisGraph();
        if (graph.is_err()) return ca::core::Err(std::move(graph).unwrap_err());
        auto analysis = std::move(graph).unwrap();
        const mjbase::ClassKey index    = analysis.hierarchy->findClass(internalName);
        if (!index.valid())
            return ca::core::Err(
                error(WorkspaceError::NotFound, "get class relations", internalName));

        ClassRelations result;
        // super 链：ancestors 含起点（superChainOf 的公开面），跳过自身；合成节点使链以
        // java/lang/Object 收尾。
        const auto chain = analysis.hierarchy->ancestors(index);
        for (ca::usize at = 1; at < chain.size(); ++at) {
            result.superclasses.push_back(std::string{analysis.hierarchy->nameOf(chain[at])});
        }
        // 接口闭包：super 链各类的直接接口 + 传递超接口（照旧 ancestors∩interface 语义）。
        for (const auto ancestor : analysis.hierarchy->interfaceClosure(index)) {
            if (ancestor == index || !analysis.hierarchy->isInterface(ancestor)) continue;
            result.interfaces.push_back(std::string{analysis.hierarchy->nameOf(ancestor)});
        }
        for (const auto candidate : analysis.hierarchy->registrationOrder()) {
            if (candidate == index) continue;
            if (analysis.hierarchy->superClass(candidate) == index) {
                result.directSubclasses.push_back(
                    std::string{analysis.hierarchy->nameOf(candidate)});
            }
            if (analysis.hierarchy->isSubtype(candidate, index)) {
                if (analysis.hierarchy->isInterface(index) &&
                    !analysis.hierarchy->isInterface(candidate)) {
                    result.implementors.push_back(
                        std::string{analysis.hierarchy->nameOf(candidate)});
                }
                if (!analysis.hierarchy->isInterface(index)) {
                    result.allSubclasses.push_back(
                        std::string{analysis.hierarchy->nameOf(candidate)});
                }
            }
        }
        sortUnique(result.superclasses);
        sortUnique(result.interfaces);
        sortUnique(result.directSubclasses);
        sortUnique(result.allSubclasses);
        sortUnique(result.implementors);

        std::set<std::string> relatedClasses{internalName};
        relatedClasses.insert(result.superclasses.begin(), result.superclasses.end());
        relatedClasses.insert(result.interfaces.begin(), result.interfaces.end());
        relatedClasses.insert(result.directSubclasses.begin(), result.directSubclasses.end());
        relatedClasses.insert(result.allSubclasses.begin(), result.allSubclasses.end());
        relatedClasses.insert(result.implementors.begin(), result.implementors.end());
        const auto supportingClasses = supportingClassIds();
        for (const auto& name : relatedClasses) {
            const auto source = supportingClasses.find(name);
            result.sources.push_back({name,
                                      source != supportingClasses.end(),
                                      source == supportingClasses.end() ? std::string{}
                                                                        : source->second});
        }
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "get class relations", exception.what()));
    }
}

WorkspaceResult<std::vector<MethodOverrideInfo>> WorkspaceBase::overridesOfMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    WorkspaceErrorInfo status = ensureLoaded("get method overrides");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get method overrides",
                                   "owner, name and descriptor are required"));
    }
    try {
        auto graph = analysisGraph();
        if (graph.is_err()) return ca::core::Err(std::move(graph).unwrap_err());
        auto analysis = std::move(graph).unwrap();
        const mjbase::ClassKey ownerKey = analysis.hierarchy->findClass(owner);
        if (!ownerKey.valid())
            return ca::core::Err(error(WorkspaceError::NotFound, "get method overrides", owner));
        if (!analysis.declaresMethod(ownerKey.id, name, descriptor)) {
            return ca::core::Err(error(WorkspaceError::NotFound, "get method overrides",
                                       owner + "." + name + descriptor));
        }

        // 覆盖组件（mjbase 面直算）：候选 = 全部声明同名同描述符方法的登记类；
        // 两候选之间是子类型关系（任一方向）即连通；并查集闭包给出组件。
        // 旧栈经 renamer RenameContext 的组件游走未随迁，此处等价重建；
        // 与引擎共用的组件解析设施由主线 renamer 接管（迁移报告项）。
        std::vector<mjbase::ClassKey> candidates;
        for (const auto& [id, summary] : analysis.summaries) {
            (void)summary;
            if (analysis.declaresMethod(id, name, descriptor))
                candidates.push_back(mjbase::ClassKey{id});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const mjbase::ClassKey& left, const mjbase::ClassKey& right) {
                      return left.id < right.id;
                  });
        std::unordered_map<ca::u32, ca::usize> slotOf;
        for (ca::usize at = 0; at < candidates.size(); ++at)
            slotOf.emplace(candidates[at].id, at);
        std::vector<ca::usize> parent(candidates.size());
        for (ca::usize at = 0; at < parent.size(); ++at) parent[at] = at;
        const std::function<ca::usize(ca::usize)> find = [&](ca::usize node) -> ca::usize {
            return parent[node] == node ? node : parent[node] = find(parent[node]);
        };
        for (ca::usize i = 0; i < candidates.size(); ++i) {
            for (ca::usize j = i + 1; j < candidates.size(); ++j) {
                if (analysis.hierarchy->isSubtype(candidates[i], candidates[j]) ||
                    analysis.hierarchy->isSubtype(candidates[j], candidates[i])) {
                    parent[find(i)] = find(j);
                }
            }
        }
        const ca::usize ownerRoot = find(slotOf.at(ownerKey.id));

        const auto supportingClasses = supportingClassIds();
        std::vector<MethodOverrideInfo> result;
        for (ca::usize at = 0; at < candidates.size(); ++at) {
            if (find(at) != ownerRoot) continue;
            const auto methodOwner = std::string{analysis.hierarchy->nameOf(candidates[at])};
            const auto support     = supportingClasses.find(methodOwner);
            result.push_back({methodOwner,
                              name,
                              descriptor,
                              methodOwner == owner,
                              support != supportingClasses.end(),
                              support == supportingClasses.end() ? std::string{} : support->second});
        }
        std::sort(result.begin(), result.end(), [](const MethodOverrideInfo& left,
                                                   const MethodOverrideInfo& right) {
            if (left.owner != right.owner) return left.owner < right.owner;
            if (left.name != right.name) return left.name < right.name;
            return left.descriptor < right.descriptor;
        });
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "get method overrides", exception.what()));
    }
}

jasm::Result<jasm::JvmJasmDisassembly> WorkspaceBase::getJasm(const std::string& uri) const
{
    if (!isLoaded()) return jasmFailure<jasm::JvmJasmDisassembly>("workspace is not loaded");
    jasm::ResourceJasmService service(*working_);
    return service.disassemble(uri);
}

// ── 变更事务面 ──────────────────────────────────────────────────────────

MutationResult WorkspaceBase::validateJasm(const std::string&      uri,
                                           ca::str::Utf16StringRef sourceName,
                                           ca::str::Utf16StringRef source,
                                           const MutationRequest&  request)
{
    WorkspaceErrorInfo status = ensureLoaded("validate JASM");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    // 选择器与结构化补丁同口径：裸内部名按容器字节内容解析到规范 URI。
    auto resolvedUri = resolveClassUri(*working_, uri);
    if (resolvedUri.is_err()) return stateFailure(std::move(resolvedUri).unwrap_err());
    try {
        auto                 candidate = cloneWorking();
        ClassMutationService service(*candidate);
        auto                 assembled = service.applyJasm(std::move(resolvedUri).unwrap(),
                                                           sourceName, source,
                                                           request.validationMode);

        MutationResult result;
        result.revision    = revision_;
        result.exportable  = canExport();
        result.diagnostics = std::move(assembled.diagnostics);
        if (!assembled.isOk()) {
            result.status = error(WorkspaceError::InvalidArgument, "validate JASM",
                                  "candidate class did not pass validation");
            return result;
        }
        result.status             = WorkspaceErrorInfo{};
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "validate JASM", exception.what()));
    }
}

MutationResult WorkspaceBase::commit(std::unique_ptr<model::ResourceContainer> candidate,
                                     std::vector<jasm::Diagnostic> diagnostics,
                                     const MutationRequest&        request)
{
    if (!accepts(request)) return revisionConflict();
    auto diffs = diffContainers(*working_, *candidate);

    MutationResult result;
    result.status      = WorkspaceErrorInfo{};
    result.diagnostics = std::move(diagnostics);
    result.exportable  = true;
    result.byteDiffs   = std::move(diffs);
    if (result.byteDiffs.empty()) {
        // 空 diff 不产生新 revision、不进历史（照旧）。
        result.revision = revision_;
        return result;
    }

    //-- 台账事务装配（RFC §6.2：uri 字节级 entries，before/after digest 链）----
    std::set<std::string> uris;
    working_->visitResources(
        [&](std::string_view uri, const model::IResource&) { uris.insert(std::string(uri)); });
    candidate->visitResources(
        [&](std::string_view uri, const model::IResource&) { uris.insert(std::string(uri)); });

    try {
        project::LedgerAppendInput input;
        input.label      = request.label;
        input.producedBy = "mjbase-workspace/1";
        input.createdAt  = project::rfc3339UtcNow();
        const ca::u64 txnId = ledger_.nextTxnId();
        ca::u32       seq   = 0;
        for (const auto& uri : uris) {
            const model::IResource* beforeResource = working_->getResource(uri);
            const model::IResource* afterResource  = candidate->getResource(uri);
            std::vector<ca::u8>     beforeBytes;
            std::vector<ca::u8>     afterBytes;
            if (beforeResource != nullptr) {
                auto bytes = bytesOf(*beforeResource);
                if (bytes.is_err())
                    return stateFailure(std::move(bytes).unwrap_err());
                beforeBytes = std::move(bytes).unwrap();
            }
            if (afterResource != nullptr) {
                auto bytes = bytesOf(*afterResource);
                if (bytes.is_err()) return stateFailure(std::move(bytes).unwrap_err());
                afterBytes = std::move(bytes).unwrap();
            }
            if (beforeBytes == afterBytes) continue;

            project::LedgerEntry entry;
            entry.uri = project::copyLedgerText(uri);
            if (afterResource == nullptr) {
                entry.op     = project::LedgerEntryOp::Remove;
                entry.before = project::ledgerDigestOfBytes(beforeBytes.data(),
                                                            beforeBytes.size());
            }
            else {
                entry.after = project::ledgerDigestOfBytes(afterBytes.data(), afterBytes.size());
                entry.op    = beforeResource == nullptr ? project::LedgerEntryOp::Add
                                                        : project::LedgerEntryOp::Replace;
                if (beforeResource != nullptr) {
                    entry.before = project::ledgerDigestOfBytes(beforeBytes.data(),
                                                                beforeBytes.size());
                }
                entry.payloadRef = project::overlaySectionName(txnId, seq++);
                input.payloads.push_back({entry.payloadRef.clone(), afterBytes});
            }
            input.entries.push_back(std::move(entry));
        }
        auto appended = ledger_.append(input);
        if (appended.is_err()) return stateFailure(ledgerFailure("commit", std::move(appended).unwrap_err()));

        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        redoHint_.clear();
        working_ = std::move(candidate);
        ++revision_;
        result.revision  = revision_;
        result.committed = true;
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "commit", exception.what()));
    }
}

MutationResult WorkspaceBase::applyJasm(const std::string&      uri,
                                        ca::str::Utf16StringRef sourceName,
                                        ca::str::Utf16StringRef source,
                                        const MutationRequest&  request)
{
    if (request.dryRun) return validateJasm(uri, sourceName, source, request);
    WorkspaceErrorInfo status = ensureLoaded("apply JASM");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    // 选择器与结构化补丁同口径：裸内部名按容器字节内容解析到规范 URI。
    auto resolvedUri = resolveClassUri(*working_, uri);
    if (resolvedUri.is_err()) return stateFailure(std::move(resolvedUri).unwrap_err());
    try {
        auto                 candidate = cloneWorking();
        ClassMutationService service(*candidate);
        auto                 assembled = service.applyJasm(std::move(resolvedUri).unwrap(),
                                                           sourceName, source,
                                                           request.validationMode);
        if (!assembled.isOk()) {
            MutationResult result;
            result.status = error(WorkspaceError::InvalidArgument, "apply JASM",
                                  "candidate class did not pass validation");
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(assembled.diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(assembled.diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "apply JASM", exception.what()));
    }
}

MutationResult WorkspaceBase::validateClassBytes(const std::string&         uri,
                                                 const std::vector<ca::u8>& classBytes,
                                                 const MutationRequest&     request)
{
    WorkspaceErrorInfo status = ensureLoaded("validate class bytes");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    if (classBytes.empty()) {
        return stateFailure(error(WorkspaceError::InvalidArgument, "validate class bytes",
                                  "candidate class is empty"));
    }
    try {
        auto       candidate = cloneWorking();
        const auto current =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(candidate->getResource(uri));
        if (current == nullptr) {
            return stateFailure(error(WorkspaceError::NotFound, "validate class bytes",
                                      "class resource not found: " + uri));
        }
        candidate->replaceResource(uri,
                                   std::make_unique<model::ClassResource>(uri, classBytes));

        MutationResult result;
        result.revision   = revision_;
        result.exportable = canExport();
        // 门禁作用域 = 本次替换的类。
        result.status     = validateCandidate(*candidate, result.diagnostics,
                                              request.validationMode, {uri});
        if (result.status.is_err()) return result;
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(
            error(WorkspaceError::Internal, "validate class bytes", exception.what()));
    }
}

MutationResult WorkspaceBase::applyClassBytes(const std::string&         uri,
                                              const std::vector<ca::u8>& classBytes,
                                              const MutationRequest&     request)
{
    if (request.dryRun) return validateClassBytes(uri, classBytes, request);
    WorkspaceErrorInfo status = ensureLoaded("apply class bytes");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    if (classBytes.empty()) {
        return stateFailure(error(WorkspaceError::InvalidArgument, "apply class bytes",
                                  "candidate class is empty"));
    }
    try {
        auto       candidate = cloneWorking();
        const auto current =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(candidate->getResource(uri));
        if (current == nullptr) {
            return stateFailure(error(WorkspaceError::NotFound, "apply class bytes",
                                      "class resource not found: " + uri));
        }
        candidate->replaceResource(uri,
                                   std::make_unique<model::ClassResource>(uri, classBytes));
        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = 本次替换的类。
        status = validateCandidate(*candidate, diagnostics, request.validationMode, {uri});
        if (status.is_err()) {
            MutationResult result;
            result.status      = std::move(status);
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(
            error(WorkspaceError::Internal, "apply class bytes", exception.what()));
    }
}

MutationResult WorkspaceBase::validateClassMutation(const ClassMutation&   mutation,
                                                    const MutationRequest& request)
{
    WorkspaceErrorInfo status = ensureLoaded("validate class mutation");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    try {
        auto                 candidate = cloneWorking();
        ClassMutationService service(*candidate);
        auto                 outcome = service.apply(mutation, request.validationMode);

        MutationResult result;
        result.revision    = revision_;
        result.exportable  = canExport();
        result.diagnostics = std::move(outcome.diagnostics);
        if (outcome.status.is_err()) {
            result.status = std::move(outcome.status);
            return result;
        }
        result.status             = WorkspaceErrorInfo{};
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(
            error(WorkspaceError::Internal, "validate class mutation", exception.what()));
    }
}

MutationResult WorkspaceBase::applyClassMutation(const ClassMutation&   mutation,
                                                 const MutationRequest& request)
{
    WorkspaceErrorInfo status = ensureLoaded("apply class mutation");
    if (status.is_err()) return stateFailure(std::move(status));
    //选择器契约对齐 replaceClassBytes：精确资源 URI 直接命中，内部名按字节
    //内容解析到规范 URI。
    auto resolvedUri = resolveClassUri(*working_, mutation.classUri);
    if (resolvedUri.is_err()) return stateFailure(std::move(resolvedUri).unwrap_err());
    ClassMutation resolvedMutation = mutation;
    resolvedMutation.classUri = std::move(resolvedUri).unwrap();
    if (request.dryRun) return validateClassMutation(resolvedMutation, request);
    if (!accepts(request)) return revisionConflict();
    try {
        auto                 candidate = cloneWorking();
        ClassMutationService service(*candidate);
        auto                 outcome = service.apply(resolvedMutation, request.validationMode);
        if (outcome.status.is_err()) {
            MutationResult result;
            result.status      = std::move(outcome.status);
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(outcome.diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(outcome.diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(
            error(WorkspaceError::Internal, "apply class mutation", exception.what()));
    }
}

MutationResult WorkspaceBase::replaceClassBytes(const std::string&         uriOrInternalName,
                                                const std::vector<ca::u8>& bytes,
                                                const MutationRequest&     request)
{
    WorkspaceErrorInfo status = ensureLoaded("replace class bytes");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    if (bytes.empty() || bytes.size() > kMaximumClassReplacementSize)
        return stateFailure(error(WorkspaceError::InvalidArgument, "replace class bytes",
                                  "class size must be in 1..16777216 bytes"));
    if (bytes.size() < 4 || bytes[0] != 0xca || bytes[1] != 0xfe || bytes[2] != 0xba ||
        bytes[3] != 0xbe)
        return stateFailure(error(WorkspaceError::InvalidArgument, "replace class bytes",
                                  "candidate does not start with CAFEBABE"));

    auto resolved = resolveClassUri(*working_, uriOrInternalName);
    if (resolved.is_err()) return stateFailure(std::move(resolved).unwrap_err());
    const auto uri                  = std::move(resolved).unwrap();
    const auto expectedInternalName = internalNameFromClassUri(uri);
    if (expectedInternalName.empty())
        return stateFailure(error(WorkspaceError::InvalidArgument, "replace class bytes",
                                  "target URI is not a canonical class or multi-release class "
                                  "URI"));
    const auto* current =
        ca::core::tag_cast::dyn_cast<const model::ClassResource>(working_->getResource(uri));
    if (current == nullptr)
        return stateFailure(error(WorkspaceError::NotFound, "replace class bytes", uri));

    try {
        const auto actualInternalName = internalNameOfBytes(bytes);
        if (actualInternalName != expectedInternalName)
            return stateFailure(error(WorkspaceError::InvalidArgument, "replace class bytes",
                                      "candidate internal name does not match target URI"));

        auto candidate = cloneWorking();
        candidate->replaceResource(
            uri, std::make_unique<model::ClassResource>(locationText(*current), bytes));
        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = 本次替换的类。
        status = validateCandidate(*candidate, diagnostics, request.validationMode, {uri});
        if (status.is_err()) {
            MutationResult result = stateFailure(std::move(status));
            result.diagnostics    = std::move(diagnostics);
            return result;
        }
        if (!request.dryRun)
            return commit(std::move(candidate), std::move(diagnostics), request);

        MutationResult result;
        result.status             = WorkspaceErrorInfo{};
        result.revision           = revision_;
        result.exportable         = canExport();
        result.diagnostics        = std::move(diagnostics);
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(
            error(WorkspaceError::InvalidArgument, "replace class bytes", exception.what()));
    }
}

WorkspaceResult<std::vector<ModifiedClassInfo>> WorkspaceBase::modifiedClasses() const
{
    WorkspaceErrorInfo status = ensureLoaded("list modified classes");
    if (status.is_err()) return ca::core::Err(std::move(status));
    try {
        std::vector<ModifiedClassInfo> result;
        for (const auto& diff : diffContainers(*original_, *working_)) {
            const auto* original = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
                original_->getResource(diff.uri));
            const auto* current = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
                working_->getResource(diff.uri));
            if (original == nullptr && current == nullptr) continue;

            ModifiedClassInfo item;
            item.resourceUri = diff.uri;
            if (original != nullptr) {
                item.originalSize         = original->get().size();
                item.originalFingerprint  = fingerprintBytes(original->get());
                item.originalInternalName = internalNameOfBytes(original->get());
            }
            if (current != nullptr) {
                item.currentSize         = current->get().size();
                item.currentFingerprint  = fingerprintBytes(current->get());
                item.currentInternalName = internalNameOfBytes(current->get());
            }
            result.push_back(std::move(item));
        }
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "list modified classes", exception.what()));
    }
}

/// @brief 候选容器 → 层次图即时重建（校验门用；resource_jasm_service 同款容
///        错口径）：只读 Class 骨架，畸形/重复/环条目跳过。容器内类可解析走
///        子类型判定；外部/JDK 类型不进图——relaxed 语义按不可解析处置。
void buildValidationHierarchy(const model::ResourceContainer&   container,
                              mjbase::hierarchy::ClassHierarchy& hierarchy, mjbase::KeyPool& pool)
{
    container.visitResources([&](std::string_view, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        auto read = jbc::core::readClass(jbc::core::ByteView(classResource->get()));
        if (read.is_err()) return;
        auto        parsed = std::move(read).unwrap();
        const auto& file   = *parsed.file;

        mjbase::hierarchy::ClassSummary summary;
        summary.self = mjbase::classKeyOf(pool, file.thisName());
        const std::string_view superName = file.superName();
        summary.super = superName.empty() ? mjbase::ClassKey{} : mjbase::classKeyOf(pool, superName);
        for (const ca::u16 ifaceCp : file.interfaces) {
            summary.interfaces.push_back(mjbase::classKeyOf(pool, file.className(ifaceCp)));
        }
        (void)hierarchy.addClass(summary);
    });
}

WorkspaceErrorInfo WorkspaceBase::validateCandidate(const model::ResourceContainer&  candidate,
                                                    std::vector<jasm::Diagnostic>&  diagnostics,
                                                    jasm::ValidationMode            mode,
                                                    const std::vector<std::string>& scopeUris) const
{
    // off：跳过门禁（直通零诊断）。
    if (mode == jasm::ValidationMode::Off) return WorkspaceErrorInfo{};
    jasm::JvmJasmService validator;
    // 层次图按候选容器即时重建：relaxed 下容器内类走子类型判定，外部
    // /JDK 类型不可解析不判非法。
    mjbase::KeyPool                   pool;
    mjbase::hierarchy::ClassHierarchy hierarchy{pool};
    buildValidationHierarchy(candidate, hierarchy, pool);
    jasm::JvmJasmContext  context;
    context.hierarchy      = &hierarchy;
    context.validationMode = mode;

    // per-class 聚合缓冲：类 uri → 计数 + 截断明细，不再逐方法刷屏。
    struct ClassAggregate
    {
        std::string                     internalName;
        ca::usize                       errors   = 0;
        ca::usize                       warnings = 0;
        std::vector<ca::str::Utf16String> details;   ///< 截断明细（前 3 条）
    };
    std::map<std::string, ClassAggregate> aggregates;

    WorkspaceErrorInfo status;
    for (const auto& uri : scopeUris) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(candidate.getResource(uri));
        if (classResource == nullptr) continue;   // 作用域内已删除条目（修复移除等）
        // spi sanitize gate：引用语义损坏的 class 在候选入口前置拒绝（协作面，
        // 迁移口径 2：workspace × sanitize gate 契约落点）。
        const auto issues = spi::sanitizeClassFile(classResource->get());
        if (!issues.empty()) {
            for (const auto& issue : issues) {
                diagnostics.push_back({jasm::DiagnosticSeverity::Error,
                                       jasm::DiagnosticStage::EditGate,
                                       utf16("sanitize: " + issue.message.to_std_string()),
                                       std::nullopt});
            }
            return error(WorkspaceError::InvalidArgument, "validate workspace candidate",
                         "class failed sanitize gate: " + uri);
        }
        auto validated = validator.validate(classResource->get(), context);
        if (validated.diagnostics.empty()) continue;
        auto& aggregate = aggregates[uri];
        aggregate.internalName = internalNameOfBytes(classResource->get());
        for (const auto& diagnostic : validated.diagnostics) {
            if (diagnostic.severity == jasm::DiagnosticSeverity::Error) ++aggregate.errors;
            else
                ++aggregate.warnings;
            if (aggregate.details.size() < 3) aggregate.details.push_back(diagnostic.message.clone());
        }
    }
    for (auto& entry : aggregates) {
        auto&       aggregate = entry.second;
        const bool  failing   = aggregate.errors > 0;
        const auto& label =
            aggregate.internalName.empty() ? entry.first : aggregate.internalName;
        std::string message = "class " + label + ": " + std::to_string(aggregate.errors) +
                              " error(s), " + std::to_string(aggregate.warnings) + " warning(s)";
        if (!aggregate.details.empty()) {
            message +=
                "\n  " + aggregate.details[0].to_utf8_string().to_std_string();
            if (aggregate.details.size() > 1)
                message +=
                    "\n  " + aggregate.details[1].to_utf8_string().to_std_string();
            if (aggregate.details.size() > 2)
                message +=
                    "\n  " + aggregate.details[2].to_utf8_string().to_std_string();
            const ca::usize total = aggregate.errors + aggregate.warnings;
            if (total > aggregate.details.size())
                message += "\n  ... and " + std::to_string(total - aggregate.details.size()) +
                           " more";
        }
        diagnostics.push_back({failing ? jasm::DiagnosticSeverity::Error
                                       : jasm::DiagnosticSeverity::Warning,
                               jasm::DiagnosticStage::EditGate, utf16(message), std::nullopt});
        if (failing && status.is_ok())
            status = error(WorkspaceError::InvalidArgument, "validate workspace candidate",
                           "class did not pass validation: " + entry.first);
    }
    return status;
}

// ── workflow 事务助手（mbase 语义照迁，错误通道换 WorkspaceErrorInfo）─────

void removeWorkflowExport(const WorkflowResult& result)
{
    if (!result.exported || result.exportPath.empty()) return;
    std::error_code filesystemError;
    std::filesystem::remove(fsPath(result.exportPath), filesystemError);
}

WorkspaceResult<std::filesystem::path> createWorkflowCandidateJarPath()
{
    namespace fs = std::filesystem;
    static std::atomic<ca::u64> sequence{0};
    std::error_code             filesystemError;
    const auto                  root = fs::temp_directory_path(filesystemError);
    if (filesystemError) {
        return ca::core::Err(error(WorkspaceError::Internal, "workflow candidate artifact",
                                   filesystemError.message()));
    }
    for (ca::usize attempt = 0; attempt < 16; ++attempt) {
        const auto id = std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()) +
                        "-" + std::to_string(sequence.fetch_add(1));
        const auto path = root / ("mjbase-workflow-candidate-" + id + ".jar");
        if (!fs::exists(path)) return ca::core::Ok(path);
    }
    return ca::core::Err(error(WorkspaceError::AlreadyExists, "workflow candidate artifact",
                               "cannot allocate a unique temporary JAR"));
}

/// RAII 守卫：作用域结束删除候选 JAR 快照（一次性消费契约的兜底清理面）。
class ScopedWorkflowCandidateJar final
{
public:
    explicit ScopedWorkflowCandidateJar(std::filesystem::path path)
        : path_(std::move(path))
    {}

    ~ScopedWorkflowCandidateJar()
    {
        std::error_code filesystemError;
        std::filesystem::remove(path_, filesystemError);
    }

    ScopedWorkflowCandidateJar(const ScopedWorkflowCandidateJar&)            = delete;
    ScopedWorkflowCandidateJar& operator=(const ScopedWorkflowCandidateJar&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

WorkspaceErrorInfo validateWorkflowReferenceQuery(const WorkflowReferenceQuery& query)
{
    if (query.owner.empty()) {
        return error(WorkspaceError::InvalidArgument, "workflow xref", "owner is required");
    }
    if (query.kind == WorkflowReferenceKind::Class) return WorkspaceErrorInfo{};
    if (query.name.empty() || query.descriptor.empty()) {
        return error(WorkspaceError::InvalidArgument, "workflow xref",
                     "member name and descriptor are required");
    }
    return WorkspaceErrorInfo{};
}

/// @brief 对任意容器（工作副本或候选）执行精确 xref 查询。
///
/// 渲染口径与 workspace 自有 xref 族一致（"type"/"direct"，owner.name:desc 拼接）；
/// 不走 analysisScanContainer——候选容器是独立的纯容器，无 supporting 叠加面。
WorkspaceResult<std::vector<ReferenceInfo>> workflowReferences(
    const model::ResourceContainer& container, const WorkflowReferenceQuery& query)
{
    const WorkspaceErrorInfo status = validateWorkflowReferenceQuery(query);
    if (status.is_err()) return ca::core::Err(status);
    try {
        ReferenceScan scan;
        scan.build(container);
        std::vector<ReferenceInfo> result;
        switch (query.kind) {
        case WorkflowReferenceKind::Class:
            for (const auto& reference : scan.typeReferencesTo(query.owner)) {
                result.push_back(referenceInfo(reference.site, reference.targetType, "type"));
            }
            break;
        case WorkflowReferenceKind::Field:
            for (const auto& reference :
                 scan.fieldReferencesTo(query.owner, query.name, query.descriptor)) {
                result.push_back(referenceInfo(
                    reference.site,
                    reference.target.owner + "." + reference.target.name +
                        reference.target.descriptor,
                    "direct"));
            }
            break;
        case WorkflowReferenceKind::Method:
            for (const auto& reference :
                 scan.methodReferencesTo(query.owner, query.name, query.descriptor)) {
                result.push_back(referenceInfo(
                    reference.site,
                    reference.target.owner + "." + reference.target.name +
                        reference.target.descriptor,
                    "direct"));
            }
            break;
        }
        sortReferences(result);
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "workflow xref", exception.what()));
    }
}

/// @brief 扫描 JDK 归档（.jar/.zip/.jmod，递归）建层次图。
///
/// mbase CommonSuperClassProvider::scan 的 mjbase 对应物：resolver 的数据获取
/// 是调用方的事，管线只消费已建好的图（pass-contract §9）。逐归档逐类
/// fail-open——解析不了的归档/类不贡献层次事实（mbase 扫描口径一致），层次
/// 缺口由 CommonSuperClassResolver 的回退路径兜住。jimage（lib/modules）不是
/// zip 容器，不在扫描集（与 mbase 行为一致）。
void scanJdkHierarchy(const std::string& jdkPath, mjbase::hierarchy::ClassHierarchy& hierarchy,
                      mjbase::KeyPool& pool)
{
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    fs::recursive_directory_iterator it(fsPath(jdkPath), fs::directory_options::skip_permission_denied,
                                        filesystemError);
    if (filesystemError) return;
    for (const fs::directory_entry& entry = *it; it != fs::end(it); it.increment(filesystemError)) {
        if (filesystemError) break;
        if (!entry.is_regular_file(filesystemError) || filesystemError) {
            filesystemError.clear();
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".jar" && extension != ".zip" && extension != ".jmod") continue;

        model::ResourceContainer archive;
        auto imported = model::ResourceHelper::importFromJar(archive, pathToUtf8(entry.path()));
        if (imported.is_err()) continue;   // 单归档 fail-open
        for (const auto& scanned : analysis::scanClassResources(archive)) {
            auto summary = hierarchy::fillSummary(*scanned.session, pool);
            (void)hierarchy.addClass(std::move(summary));   // 单类 fail-open（环检查拒绝等）
        }
    }
}

// ── remap / rename / pipeline / workflow 方法族 ─────────────────────────

WorkspaceErrorInfo WorkspaceBase::runPipeline(model::ResourceContainer& candidate,
                                              const PipelineRequest& request) const
{
    if (!request.pipeline.empty() && !request.passNames.empty()) {
        return error(WorkspaceError::InvalidArgument, "run pipeline",
                     "pipeline and passNames are mutually exclusive");
    }

    spi::registerBuiltinPipelines();
    spi::PipelineBuilder builder;
    if (!request.pipeline.empty()) {
        if (!spi::PassRegistry::instance().hasPipeline(request.pipeline)) {
            return error(WorkspaceError::NotFound, "run pipeline", request.pipeline);
        }
        builder = spi::PassRegistry::instance().getPipeline(request.pipeline);
    }
    else if (!request.passNames.empty()) {
        for (const auto& name : request.passNames) {
            if (name.empty() || spi::applyTo(spi::defaultPassRegistry(), name, builder).is_err()) {
                return error(WorkspaceError::NotFound, "run pass", name);
            }
        }
    }
    else {
        builder = spi::PassRegistry::instance().getPipeline("default-optimize");
    }

    // jdkPath 非空：扫描 JDK 归档建层次图，Strict resolver 接入帧重算（旧栈
    // COMPUTE_MAXS|COMPUTE_FRAMES 的对应物）。resolver 链的生命周期必须覆盖
    // buildNestedPipeline + runContainerPipeline 全程，故在 job 之前构造。
    mjbase::KeyPool                        jdkPool;
    mjbase::hierarchy::ClassHierarchy      jdkHierarchy{jdkPool};
    mjbase::spi::CommonSuperClassResolver  superResolver{jdkHierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{superResolver};
    spi::PassPipelineOptions               options;
    if (!request.jdkPath.empty()) {
        std::error_code filesystemError;
        if (!std::filesystem::is_directory(fsPath(request.jdkPath), filesystemError) ||
            filesystemError) {
            return error(WorkspaceError::NotFound, "run pipeline", "JDK path does not exist");
        }
        scanJdkHierarchy(request.jdkPath, jdkHierarchy, jdkPool);
        options.typeResolver = &typeResolver;
    }

    spi::PassJobContext  job;
    spi::GlobalPassManager pipeline = builder.buildNestedPipeline(job, nullptr, nullptr, options);
    auto ran = spi::runContainerPipeline(candidate, pipeline, job);
    if (ran.is_err()) {
        auto pipelineError = std::move(ran).unwrap_err();
        return error(WorkspaceError::Internal, "run pipeline",
                     std::string(spi::to_string(pipelineError.stage)) + " " +
                         pipelineError.target + ": " + pipelineError.detail);
    }
    return WorkspaceErrorInfo{};
}

ImpactAnalysis WorkspaceBase::analyzeRemap(const asm_remapper::MemberRemapPlan& plan,
                                           const asm_remapper::RemapPolicy& policy) const
{
    ImpactAnalysis result;
    auto loaded = ensureLoaded("analyze remap");
    if (loaded.is_err()) {
        result.status = std::move(loaded);
        return result;
    }
    try {
        auto candidate = cloneWorking();
        auto remapped  = asm_remapper::applyResourceRemapPlan(*candidate, plan, policy);
        if (remapped.is_err()) {
            result.status = error(WorkspaceError::Internal, "analyze remap",
                                  std::string("remap failed: ") +
                                      asm_remapper::to_string(std::move(remapped).unwrap_err()));
            return result;
        }
        // 门禁作用域 = remap 改动的类；diff 只算一次，scope 与报告共用。
        const auto byteDiffs = diffContainers(*working_, *candidate);
        result.status =
            validateCandidate(*candidate, result.diagnostics, jasm::ValidationMode::Relaxed,
                              changedUris(byteDiffs));
        if (result.status.is_err()) return result;
        result.byteDiffs         = std::move(byteDiffs);
        const auto beforeClasses = changedClassNames(*working_, result.byteDiffs);
        const auto afterClasses  = changedClassNames(*candidate, result.byteDiffs);
        result.beforeReferences  = collectImpacts(*working_, beforeClasses);
        result.afterReferences   = collectImpacts(*candidate, afterClasses);
        for (const auto& diff : result.byteDiffs) {
            if (diff.uri.find(".class") == std::string::npos)
                result.risks.push_back("resource changed: " + diff.uri);
        }
        result.exportable = true;
        return result;
    }
    catch (const std::exception& exception) {
        result.status = error(WorkspaceError::Internal, "analyze remap", exception.what());
        return result;
    }
}

MutationResult WorkspaceBase::applyRemap(const asm_remapper::MemberRemapPlan& plan,
                                         const asm_remapper::RemapPolicy&    policy,
                                         const MutationRequest&              request)
{
    auto loaded = ensureLoaded("apply remap");
    if (loaded.is_err()) return stateFailure(std::move(loaded));
    if (!accepts(request)) return revisionConflict();
    if (request.dryRun) {
        auto analysis = analyzeRemap(plan, policy);
        MutationResult result;
        result.status             = std::move(analysis.status);
        result.revision           = revision_;
        result.exportable         = analysis.exportable;
        result.diagnostics        = std::move(analysis.diagnostics);
        result.byteDiffs          = std::move(analysis.byteDiffs);
        result.affectedReferences = std::move(analysis.afterReferences);
        result.affectedClasses    = changedClassNames(*working_, result.byteDiffs);
        return result;
    }
    try {
        auto candidate = cloneWorking();
        auto remapped  = asm_remapper::applyResourceRemapPlan(*candidate, plan, policy);
        if (remapped.is_err()) {
            return stateFailure(error(WorkspaceError::Internal, "apply remap",
                                      std::string("remap failed: ") +
                                          asm_remapper::to_string(std::move(remapped).unwrap_err())));
        }
        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = remap 改动的类。
        const auto byteDiffs = diffContainers(*working_, *candidate);
        auto status = validateCandidate(*candidate, diagnostics, request.validationMode,
                                        changedUris(byteDiffs));
        if (status.is_err()) {
            MutationResult result;
            result.status      = std::move(status);
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "apply remap", exception.what()));
    }
}

MutationResult WorkspaceBase::validatePipeline(const PipelineRequest& pipeline,
                                               const MutationRequest& request)
{
    auto loaded = ensureLoaded("validate pipeline");
    if (loaded.is_err()) return stateFailure(std::move(loaded));
    if (!accepts(request)) return revisionConflict();
    try {
        auto candidate = cloneWorking();
        auto status    = runPipeline(*candidate, pipeline);
        if (status.is_err()) return stateFailure(std::move(status));

        MutationResult result;
        result.revision   = revision_;
        result.exportable = canExport();
        // 门禁作用域 = pipeline 改动的类；diff 只算一次，scope 与报告共用。
        const auto byteDiffs = diffContainers(*working_, *candidate);
        result.status =
            validateCandidate(*candidate, result.diagnostics, request.validationMode,
                              changedUris(byteDiffs));
        if (result.status.is_err()) return result;
        result.byteDiffs          = std::move(byteDiffs);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "validate pipeline", exception.what()));
    }
}

MutationResult WorkspaceBase::applyPipeline(const PipelineRequest& pipeline,
                                            const MutationRequest& request)
{
    if (request.dryRun) return validatePipeline(pipeline, request);
    auto loaded = ensureLoaded("apply pipeline");
    if (loaded.is_err()) return stateFailure(std::move(loaded));
    if (!accepts(request)) return revisionConflict();
    try {
        auto candidate = cloneWorking();
        auto status    = runPipeline(*candidate, pipeline);
        if (status.is_err()) return stateFailure(std::move(status));

        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = pipeline 改动的类。
        const auto byteDiffs = diffContainers(*working_, *candidate);
        status = validateCandidate(*candidate, diagnostics, request.validationMode,
                                   changedUris(byteDiffs));
        if (status.is_err()) {
            MutationResult result;
            result.status      = std::move(status);
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "apply pipeline", exception.what()));
    }
}

WorkflowResult WorkspaceBase::runWorkflow(const WorkflowRequest& workflow,
                                          const MutationRequest& request)
{
    WorkflowResult result;
    const auto     loaded = ensureLoaded("run workflow");
    if (loaded.is_err()) {
        result.mutation = stateFailure(std::move(loaded));
        return result;
    }
    if (!accepts(request)) {
        result.mutation = revisionConflict();
        return result;
    }

    const auto fail = [&](const std::string& stage, WorkspaceErrorInfo status) {
        result.stages.push_back({stage, status});
        result.mutation = stateFailure(std::move(status));
        return std::move(result);
    };

    try {
        auto inputFingerprint = fingerprintContainer(*working_);
        if (inputFingerprint.is_err())
            return fail("fingerprint", std::move(inputFingerprint).unwrap_err());
        result.inputFingerprint = std::move(inputFingerprint).unwrap();

        auto candidate = cloneWorking();
        if (workflow.remapPlan != nullptr) {
            auto remapped = asm_remapper::applyResourceRemapPlan(*candidate, *workflow.remapPlan,
                                                                 workflow.remapPolicy);
            if (remapped.is_err()) {
                return fail("mapping",
                            error(WorkspaceError::Internal, "run workflow mapping",
                                  std::string("remap failed: ") +
                                      asm_remapper::to_string(std::move(remapped).unwrap_err())));
            }
            result.stages.push_back({"mapping", WorkspaceErrorInfo{}});
        }
        if (workflow.runPipeline) {
            const auto status = runPipeline(*candidate, workflow.pipeline);
            if (status.is_err()) return fail("pipeline", status);
            result.stages.push_back({"pipeline", WorkspaceErrorInfo{}});
        }

        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = workflow 改动的类。
        const auto byteDiffs = diffContainers(*working_, *candidate);
        const auto validation =
            validateCandidate(*candidate, diagnostics, jasm::ValidationMode::Relaxed,
                              changedUris(byteDiffs));
        if (validation.is_err()) {
            result.stages.push_back({"validation", validation});
            result.mutation.status      = validation;
            result.mutation.revision    = revision_;
            result.mutation.exportable  = canExport();
            result.mutation.diagnostics = std::move(diagnostics);
            return result;
        }
        result.stages.push_back({"validation", WorkspaceErrorInfo{}});

        for (const auto& query : workflow.referenceQueries) {
            auto before = workflowReferences(*working_, query);
            if (before.is_err()) return fail("xref", std::move(before).unwrap_err());
            auto after = workflowReferences(*candidate, query);
            if (after.is_err()) return fail("xref", std::move(after).unwrap_err());
            result.references.push_back(
                {query, std::move(before).unwrap(), std::move(after).unwrap()});
        }
        if (!workflow.referenceQueries.empty())
            result.stages.push_back({"xref", WorkspaceErrorInfo{}});

        auto candidateFingerprint = fingerprintContainer(*candidate);
        if (candidateFingerprint.is_err())
            return fail("fingerprint", std::move(candidateFingerprint).unwrap_err());
        result.candidateFingerprint = std::move(candidateFingerprint).unwrap();

        if (workflow.candidateJarConsumer) {
            auto artifactPath = createWorkflowCandidateJarPath();
            if (artifactPath.is_err())
                return fail("candidate_artifact", std::move(artifactPath).unwrap_err());
            ScopedWorkflowCandidateJar artifact(std::move(artifactPath).unwrap());
            const auto exportStatus =
                exportContainerToJar(*candidate, sourcePath_, pathToUtf8(artifact.path()), revision_);
            if (exportStatus.is_err()) return fail("candidate_artifact", exportStatus);
            const auto stageStatus = workflow.candidateJarConsumer(pathToUtf8(artifact.path()));
            if (stageStatus.is_err()) {
                return fail(workflow.candidateJarStageName.empty() ? "candidate_artifact"
                                                                   : workflow.candidateJarStageName,
                            stageStatus);
            }
            result.stages.push_back({workflow.candidateJarStageName.empty()
                                         ? "candidate_artifact"
                                         : workflow.candidateJarStageName,
                                     WorkspaceErrorInfo{}});
        }

        if (!workflow.exportPath.empty()) {
            auto destination = validateExportDestination(sourcePath_, workflow.exportPath);
            if (destination.is_err()) return fail("export", std::move(destination).unwrap_err());
            result.exportPath = pathToUtf8(destination.unwrap());
            if (!request.dryRun) {
                const auto exportStatus =
                    exportContainerToJar(*candidate, sourcePath_, result.exportPath, revision_);
                if (exportStatus.is_err()) return fail("export", exportStatus);
                result.exported = true;
            }
            result.stages.push_back(
                {request.dryRun ? "export_preview" : "export", WorkspaceErrorInfo{}});
        }

        if (request.dryRun) {
            result.mutation.status             = WorkspaceErrorInfo{};
            result.mutation.revision           = revision_;
            result.mutation.exportable         = canExport();
            result.mutation.diagnostics        = std::move(diagnostics);
            result.mutation.byteDiffs          = diffContainers(*working_, *candidate);
            result.mutation.affectedClasses    = changedClassNames(*candidate, result.mutation.byteDiffs);
            result.mutation.affectedReferences =
                collectImpacts(*candidate, result.mutation.affectedClasses);
            return result;
        }
        result.mutation = commit(std::move(candidate), std::move(diagnostics), request);
        if (result.mutation.status.is_err()) {
            removeWorkflowExport(result);
            result.exported = false;
        }
        return result;
    }
    catch (const std::exception& exception) {
        removeWorkflowExport(result);
        result.exported = false;
        return fail("workflow", error(WorkspaceError::Internal, "run workflow", exception.what()));
    }
}

WorkspaceErrorInfo WorkspaceBase::buildRenamePlan(const RenameRequest&   request,
                                                  asm_remapper::MemberRemapPlan&   plan,
                                                  std::vector<RenameMapping>&      mappings,
                                                  std::vector<MethodOverrideInfo>& overrides,
                                                  std::vector<std::string>&        risks) const
{
    if (request.owner.empty() || request.newName.empty())
        return error(WorkspaceError::InvalidArgument, "build rename plan",
                     "owner and new name are required");
    switch (request.kind) {
    case RenameKind::Class:
    {
        auto source = classInfo(request.owner);
        if (source.is_err()) return source.unwrap_err();
        auto existing = classInfo(request.newName);
        if (existing.is_ok() && request.newName != request.owner)
            return error(WorkspaceError::AlreadyExists, "rename class", request.newName);
        plan.mapClass(request.owner, request.newName);
        mappings.push_back({"class", request.owner, request.newName});
        return WorkspaceErrorInfo{};
    }
    case RenameKind::Package:
    {
        auto classes = listClasses();
        if (classes.is_err()) return classes.unwrap_err();
        std::string oldPrefix = request.owner;
        std::string newPrefix = request.newName;
        while (!oldPrefix.empty() && oldPrefix.back() == '/') oldPrefix.pop_back();
        while (!newPrefix.empty() && newPrefix.back() == '/') newPrefix.pop_back();
        for (const auto& clazz : classes.unwrap()) {
            if (clazz.internalName != oldPrefix &&
                clazz.internalName.rfind(oldPrefix + "/", 0) != 0)
                continue;
            const auto suffix = clazz.internalName.substr(oldPrefix.size());
            const auto mapped = newPrefix + suffix;
            plan.mapClass(clazz.internalName, mapped);
            mappings.push_back({"class", clazz.internalName, mapped});
        }
        if (mappings.empty()) return error(WorkspaceError::NotFound, "rename package", request.owner);
        return WorkspaceErrorInfo{};
    }
    case RenameKind::Field:
    {
        if (request.name.empty() || request.descriptor.empty())
            return error(WorkspaceError::InvalidArgument, "rename field",
                         "name and descriptor are required");
        auto field = fieldInfo(request.owner, request.name, request.descriptor);
        if (field.is_err()) return field.unwrap_err();
        plan.mapField(request.owner, request.name, request.descriptor, request.owner,
                      request.newName, request.descriptor);
        mappings.push_back({"field",
                            request.owner + "." + request.name + request.descriptor,
                            request.owner + "." + request.newName + request.descriptor});
        return WorkspaceErrorInfo{};
    }
    case RenameKind::Method:
    {
        if (request.name.empty() || request.descriptor.empty())
            return error(WorkspaceError::InvalidArgument, "rename method",
                         "name and descriptor are required");
        auto method = methodInfo(request.owner, request.name, request.descriptor);
        if (method.is_err()) return method.unwrap_err();
        auto component = overridesOfMethod(request.owner, request.name, request.descriptor);
        if (component.is_err()) return component.unwrap_err();
        overrides = std::move(component).unwrap();
        const bool hasSupporting =
            std::any_of(overrides.begin(), overrides.end(),
                        [](const auto& item) { return item.supporting; });
        if (overrides.size() > 1 && !request.cascadeOverrides)
            risks.push_back("method belongs to an override component; set cascadeOverrides "
                            "after reviewing mappings");
        if (hasSupporting)
            risks.push_back("override component contains supporting-library declarations that "
                            "cannot be renamed in this workspace");
        for (const auto& item : overrides) {
            if (item.supporting) continue;
            plan.mapMethod(item.owner, item.name, item.descriptor, item.owner, request.newName,
                           item.descriptor);
            mappings.push_back({"method",
                                item.owner + "." + item.name + item.descriptor,
                                item.owner + "." + request.newName + item.descriptor});
        }
        return WorkspaceErrorInfo{};
    }
    case RenameKind::Local:
        if (request.methodName.empty() || request.name.empty() || request.descriptor.empty())
            return error(WorkspaceError::InvalidArgument, "rename local",
                         "method name, local name and descriptor are required");
        {
            auto method = methodInfo(request.owner, request.methodName, request.descriptor);
            if (method.is_err()) return method.unwrap_err();
        }
        mappings.push_back({"local", request.name, request.newName});
        return WorkspaceErrorInfo{};
    }
    return error(WorkspaceError::InvalidArgument, "build rename plan", "unknown rename kind");
}

RenameAnalysis WorkspaceBase::analyzeRename(const RenameRequest& request) const
{
    RenameAnalysis result;
    asm_remapper::MemberRemapPlan plan;
    result.status = buildRenamePlan(request, plan, result.mappings, result.overrideComponent,
                                    result.impact.risks);
    if (result.status.is_err()) {
        result.impact.status = result.status;
        return result;
    }
    if (request.kind == RenameKind::Local) {
        result.impact.status     = WorkspaceErrorInfo{};
        result.impact.exportable = canExport();
        result.readyToApply      = true;
        return result;
    }
    auto renameRisks = std::move(result.impact.risks);
    result.impact    = analyzeRemap(plan, request.policy);
    result.impact.risks.insert(result.impact.risks.end(),
                               std::make_move_iterator(renameRisks.begin()),
                               std::make_move_iterator(renameRisks.end()));
    result.status                 = result.impact.status;
    result.requiresOverrideReview = !result.impact.risks.empty();
    result.readyToApply = result.status.is_ok() && !result.requiresOverrideReview;
    return result;
}

MutationResult WorkspaceBase::applyRename(const RenameRequest& request,
                                          const MutationRequest& requestOptions)
{
    auto preview = analyzeRename(request);
    if (preview.status.is_err()) return stateFailure(std::move(preview.status));
    if (!preview.readyToApply) {
        return stateFailure(error(WorkspaceError::FailedPrecondition, "apply rename",
                                  "rename requires override review: " +
                                      (preview.impact.risks.empty()
                                           ? std::string("unknown risk")
                                           : preview.impact.risks.front())));
    }
    if (request.kind == RenameKind::Local) {
        ClassMutation mutation;
        mutation.classUri             = request.owner;
        mutation.kind                 = ClassMutationKind::RenameLocal;
        mutation.member               = {request.methodName, request.descriptor};
        mutation.localName            = request.name;
        mutation.replacementLocalName = request.newName;
        mutation.localIndex           = request.localIndex;
        return applyClassMutation(mutation, requestOptions);
    }
    asm_remapper::MemberRemapPlan plan;
    std::vector<RenameMapping>    mappings;
    std::vector<MethodOverrideInfo> overrides;
    std::vector<std::string>      risks;
    auto status = buildRenamePlan(request, plan, mappings, overrides, risks);
    if (status.is_err()) return stateFailure(std::move(status));
    return applyRemap(plan, request.policy, requestOptions);
}

WorkflowCandidateCommitResult WorkspaceBase::commitWorkflowCandidateJar(
    const WorkflowCandidateCommitRequest& candidateRequest, const MutationRequest& request)
{
    WorkflowCandidateCommitResult result;
    WorkspaceErrorInfo            loaded = ensureLoaded("commit workflow candidate JAR");
    if (loaded.is_err()) {
        result.mutation = stateFailure(std::move(loaded));
        return result;
    }
    if (!accepts(request)) {
        result.mutation = revisionConflict();
        return result;
    }
    if (candidateRequest.candidateJarPath.empty()) {
        result.mutation =
            stateFailure(error(WorkspaceError::InvalidArgument, "commit workflow candidate JAR",
                               "candidate JAR path is empty"));
        return result;
    }

    const auto removeExport = [&result] {
        if (!result.exported || result.exportPath.empty()) return;
        std::error_code filesystemError;
        std::filesystem::remove(result.exportPath, filesystemError);
        result.exported = false;
    };

    try {
        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(candidateRequest.candidateJarPath, filesystemError) ||
            filesystemError) {
            result.mutation =
                stateFailure(error(WorkspaceError::NotFound, "commit workflow candidate JAR",
                                   candidateRequest.candidateJarPath));
            return result;
        }

        auto candidate = std::make_unique<model::ResourceContainer>();
        auto imported =
            model::ResourceHelper::importFromJar(*candidate, candidateRequest.candidateJarPath);
        if (imported.is_err()) {
            result.mutation = stateFailure(
                modelFailure("commit workflow candidate JAR", std::move(imported).unwrap_err()));
            return result;
        }

        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = 候选 JAR 相对工作副本的改动面。
        const auto byteDiffs = diffContainers(*working_, *candidate);
        const WorkspaceErrorInfo validation =
            validateCandidate(*candidate, diagnostics, jasm::ValidationMode::Relaxed,
                              changedUris(byteDiffs));
        if (validation.is_err()) {
            result.mutation.status      = validation;
            result.mutation.revision    = revision_;
            result.mutation.exportable  = canExport();
            result.mutation.diagnostics = std::move(diagnostics);
            return result;
        }

        if (!candidateRequest.exportPath.empty()) {
            auto destination =
                validateExportDestination(sourcePath_, candidateRequest.exportPath);
            if (destination.is_err()) {
                result.mutation = stateFailure(std::move(destination).unwrap_err());
                return result;
            }
            result.exportPath = pathToUtf8(destination.unwrap());
            if (!request.dryRun) {
                const WorkspaceErrorInfo exportStatus =
                    exportContainerToJar(*candidate, sourcePath_, result.exportPath, revision_);
                if (exportStatus.is_err()) {
                    result.mutation = stateFailure(exportStatus);
                    return result;
                }
                result.exported = true;
            }
        }

        if (request.dryRun) {
            result.mutation.status      = WorkspaceErrorInfo{};
            result.mutation.revision    = revision_;
            result.mutation.exportable  = canExport();
            result.mutation.diagnostics = std::move(diagnostics);
            result.mutation.byteDiffs   = diffContainers(*working_, *candidate);
            result.mutation.affectedClasses =
                changedClassNames(*candidate, result.mutation.byteDiffs);
            result.mutation.affectedReferences =
                collectImpacts(*candidate, result.mutation.affectedClasses);
            return result;
        }

        result.mutation = commit(std::move(candidate), std::move(diagnostics), request);
        if (result.mutation.status.is_err()) removeExport();
        return result;
    }
    catch (const std::exception& exception) {
        removeExport();
        result.mutation = stateFailure(error(WorkspaceError::Internal,
                                             "commit workflow candidate JAR", exception.what()));
        return result;
    }
}

JarDiffResult WorkspaceBase::diffJar(const std::string& referencePath) const
{
    JarDiffResult result;
    result.referencePath = referencePath;
    WorkspaceErrorInfo loaded = ensureLoaded("diff JAR");
    if (loaded.is_err()) {
        result.status = std::move(loaded);
        return result;
    }
    if (referencePath.empty()) {
        result.status =
            error(WorkspaceError::InvalidArgument, "diff JAR", "reference path is empty");
        return result;
    }

    try {
        model::ResourceContainer reference;
        auto imported = model::ResourceHelper::importFromJar(reference, referencePath);
        if (imported.is_err()) {
            result.status = modelFailure("diff JAR", std::move(imported).unwrap_err());
            return result;
        }
        result.byteDiffs = diffContainers(*working_, reference);
        for (const auto& diff : result.byteDiffs) {
            const model::IResource* workspaceResource = working_->getResource(diff.uri);
            const model::IResource* referenceResource = reference.getResource(diff.uri);
            if (workspaceResource == nullptr) {
                ++result.referenceOnly;
            }
            else if (referenceResource == nullptr) {
                ++result.workspaceOnly;
            }
            else {
                ++result.modified;
            }
        }
        result.status = WorkspaceErrorInfo{};
        return result;
    }
    catch (const std::exception& exception) {
        result.status = error(WorkspaceError::DataLoss, "diff JAR", exception.what());
        return result;
    }
}

JarRepairResult WorkspaceBase::analyzeJarRepair(const JarRepairRequest& repair,
                                                const MutationRequest&  request) const
{
    JarRepairResult    result;
    WorkspaceErrorInfo loaded = ensureLoaded("analyze JAR repair");
    if (loaded.is_err()) {
        result.mutation = stateFailure(std::move(loaded));
        return result;
    }
    if (!accepts(request)) {
        result.mutation = revisionConflict();
        return result;
    }

    try {
        auto candidate                    = cloneWorking();
        result.resourceCount              = candidate->size();
        result.issues                     = scanJarRepairIssues(*candidate, repair, result.classCount);
        result.mutation.revision          = revision_;
        result.mutation.exportable        = canExport();
        if (!hasSelectedRepairIssues(result.issues)) return result;
        removeSelectedRepairIssues(*candidate, result.issues);

        // 门禁作用域 = 修复改动面：删除型修复的改动条目已不在候选中，
        // 其余条目未动不参与门禁（基线类编辑前就存在）。
        const auto byteDiffs              = diffContainers(*working_, *candidate);
        result.mutation.status =
            validateCandidate(*candidate, result.mutation.diagnostics,
                              jasm::ValidationMode::Relaxed, changedUris(byteDiffs));
        if (result.mutation.status.is_err()) return result;
        result.mutation.byteDiffs = std::move(byteDiffs);
        result.mutation.affectedClasses =
            changedClassNames(*candidate, result.mutation.byteDiffs);
        result.mutation.affectedReferences =
            collectImpacts(*candidate, result.mutation.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        result.mutation =
            stateFailure(error(WorkspaceError::Internal, "analyze JAR repair", exception.what()));
        return result;
    }
}

JarRepairResult WorkspaceBase::applyJarRepair(const JarRepairRequest& repair,
                                              const MutationRequest&  request)
{
    if (request.dryRun) return analyzeJarRepair(repair, request);
    JarRepairResult    result;
    WorkspaceErrorInfo loaded = ensureLoaded("apply JAR repair");
    if (loaded.is_err()) {
        result.mutation = stateFailure(std::move(loaded));
        return result;
    }
    if (!accepts(request)) {
        result.mutation = revisionConflict();
        return result;
    }

    try {
        auto candidate       = cloneWorking();
        result.resourceCount = candidate->size();
        result.issues        = scanJarRepairIssues(*candidate, repair, result.classCount);
        if (!hasSelectedRepairIssues(result.issues)) {
            result.mutation.revision   = revision_;
            result.mutation.exportable = canExport();
            return result;
        }
        removeSelectedRepairIssues(*candidate, result.issues);

        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = 修复改动面：删除型修复的改动条目已不在候选中，
        // 其余条目未动不参与门禁。
        const auto byteDiffs                 = diffContainers(*working_, *candidate);
        const WorkspaceErrorInfo      status = validateCandidate(
            *candidate, diagnostics, request.validationMode, changedUris(byteDiffs));
        if (status.is_err()) {
            result.mutation.status      = status;
            result.mutation.revision    = revision_;
            result.mutation.exportable  = canExport();
            result.mutation.diagnostics = std::move(diagnostics);
            return result;
        }
        result.mutation = commit(std::move(candidate), std::move(diagnostics), request);
        return result;
    }
    catch (const std::exception& exception) {
        result.mutation =
            stateFailure(error(WorkspaceError::Internal, "apply JAR repair", exception.what()));
        return result;
    }
}

MutationResult WorkspaceBase::revertClass(const std::string& uri, const MutationRequest& request)
{
    WorkspaceErrorInfo status = ensureLoaded("revert class");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    const auto* original =
        ca::core::tag_cast::dyn_cast<const model::ClassResource>(original_->getResource(uri));
    const auto* current =
        ca::core::tag_cast::dyn_cast<const model::ClassResource>(working_->getResource(uri));
    if (original == nullptr || current == nullptr) {
        return stateFailure(error(WorkspaceError::NotFound, "revert class", uri));
    }
    try {
        auto candidate = cloneWorking();
        candidate->replaceResource(uri,
                                   std::make_unique<model::ClassResource>(
                                       locationText(*original), original->get()));
        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = 本次还原的类——undo/revert 自救路径不再被容器
        // 基线类的既存形态卡死。
        status = validateCandidate(*candidate, diagnostics, request.validationMode, {uri});
        if (status.is_err()) {
            MutationResult result = stateFailure(std::move(status));
            result.diagnostics    = std::move(diagnostics);
            return result;
        }
        if (!request.dryRun)
            return commit(std::move(candidate), std::move(diagnostics), request);

        MutationResult result;
        result.status             = WorkspaceErrorInfo{};
        result.revision           = revision_;
        result.exportable         = canExport();
        result.diagnostics        = std::move(diagnostics);
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "revert class", exception.what()));
    }
}

WorkspaceResult<void> WorkspaceBase::createSavepoint(const std::string& name)
{
    WorkspaceErrorInfo status = ensureLoaded("create savepoint");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (name.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "create savepoint", "name is empty"));
    // 台账换底：savepoint = 书签（零字节命名指针，§4）。savepoint 时机查自动
    // 压缩阈值（尽力而为：失败不影响 savepoint 本身）。
    auto bookmarked = ledger_.setBookmark(name, project::rfc3339UtcNow());
    if (bookmarked.is_err())
        return ca::core::Err(ledgerFailure("create savepoint", std::move(bookmarked).unwrap_err()));
    autoCompactLedger();
    return ca::core::Ok();
}

MutationResult WorkspaceBase::restoreSavepoint(const std::string&     name,
                                               const MutationRequest& request)
{
    WorkspaceErrorInfo status = ensureLoaded("restore savepoint");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    const auto* bookmark = ledger_.bookmark(name);
    if (bookmark == nullptr)
        return stateFailure(error(WorkspaceError::NotFound, "restore savepoint", name));
    try {
        // 台账换底：restore = 物化书签事务位状态再走一次普通提交（记一条反向
        // 事务，undo 窗不受损；书签可位于头之前或之后——undo/redo 皆可越过）。
        auto candidate = cloneWorking();
        const auto restored = materializeState(candidate, bookmark->txn, "restore savepoint");
        if (restored.is_err()) return stateFailure(restored);
        return commit(std::move(candidate), {}, request);
    }
    catch (const std::exception& exception) {
        return stateFailure(
            error(WorkspaceError::Internal, "restore savepoint", exception.what()));
    }
}

MutationResult WorkspaceBase::undo(const MutationRequest& request)
{
    WorkspaceErrorInfo status = ensureLoaded("undo");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    if (!ledger_.canUndo())
        return stateFailure(
            error(WorkspaceError::FailedPrecondition, "undo", "history is empty"));
    try {
        auto before = cloneWorking();
        const ca::u64       head    = ledger_.headTxn();
        const auto*         txn     = ledger_.transaction(head);
        const WorkspaceErrorInfo applied =
            inverseApplyLedgerTransaction(*before, *txn, "undo");
        if (applied.is_err()) return stateFailure(applied);
        auto moved = ledger_.moveHead(txn->parent);
        if (moved.is_err()) return stateFailure(ledgerFailure("undo", std::move(moved).unwrap_err()));
        redoHint_.push_back(head);
        auto diffs = diffContainers(*working_, *before);
        working_ = std::move(before);
        ++revision_;

        MutationResult result;
        result.status             = WorkspaceErrorInfo{};
        result.revision           = revision_;
        result.committed          = true;
        result.exportable         = true;
        result.byteDiffs          = std::move(diffs);
        result.affectedClasses    = changedClassNames(*working_, result.byteDiffs);
        result.affectedReferences = collectImpacts(*working_, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "undo", exception.what()));
    }
}

MutationResult WorkspaceBase::redo(const MutationRequest& request)
{
    WorkspaceErrorInfo status = ensureLoaded("redo");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    if (redoHint_.empty())
        return stateFailure(
            error(WorkspaceError::FailedPrecondition, "redo", "history is empty"));
    try {
        const ca::u64 target = redoHint_.back();
        const auto*   txn    = ledger_.transaction(target);
        if (txn == nullptr || txn->parent != ledger_.headTxn())
            return stateFailure(
                error(WorkspaceError::Internal, "redo", "redo target is no longer reachable"));
        auto before = cloneWorking();
        const auto  applied = applyLedgerTransaction(*before, *txn, "redo");
        if (applied.is_err()) return stateFailure(applied);
        auto moved = ledger_.moveHead(target);
        if (moved.is_err()) return stateFailure(ledgerFailure("redo", std::move(moved).unwrap_err()));
        redoHint_.pop_back();
        auto diffs = diffContainers(*working_, *before);
        working_ = std::move(before);
        ++revision_;

        MutationResult result;
        result.status             = WorkspaceErrorInfo{};
        result.revision           = revision_;
        result.committed          = true;
        result.exportable         = true;
        result.byteDiffs          = std::move(diffs);
        result.affectedClasses    = changedClassNames(*working_, result.byteDiffs);
        result.affectedReferences = collectImpacts(*working_, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "redo", exception.what()));
    }
}

// ── 导出面 ──────────────────────────────────────────────────────────────

WorkspaceResult<void> WorkspaceBase::exportJar(const std::string& outputPath) const
{
    WorkspaceErrorInfo status = ensureLoaded("export JAR");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (artifactKind_ != ArtifactKind::Jar) {
        return ca::core::Err(error(WorkspaceError::FailedPrecondition, "export JAR",
                                   "workspace artifact is not a JAR"));
    }
    // close/export 前 flush 挂点（§3.2）：导出物离开内存前台账定稿。未绑定 =
    // no-op；flush 失败 fail-closed（不产出一个后台无账的导出物）。
    if (ledger_.bound() && ledger_.hasPending()) {
        auto flushed = ledger_.flush(project::rfc3339UtcNow());
        if (flushed.is_err())
            return ca::core::Err(ledgerFailure("export JAR", std::move(flushed).unwrap_err()));
    }
    const WorkspaceErrorInfo exported =
        exportContainerToJar(*working_, sourcePath_, outputPath, revision_);
    if (exported.is_err()) return ca::core::Err(std::move(exported));
    return ca::core::Ok();
}

WorkspaceResult<void> WorkspaceBase::exportClass(const std::string& outputPath) const
{
    WorkspaceErrorInfo status = ensureLoaded("export class");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (artifactKind_ != ArtifactKind::Class || primaryClassUri_.empty()) {
        return ca::core::Err(error(WorkspaceError::FailedPrecondition, "export class",
                                   "workspace artifact is not a class"));
    }
    if (ledger_.bound() && ledger_.hasPending()) {
        auto flushed = ledger_.flush(project::rfc3339UtcNow());
        if (flushed.is_err())
            return ca::core::Err(ledgerFailure("export class", std::move(flushed).unwrap_err()));
    }
    const WorkspaceErrorInfo exported =
        exportContainerToClass(*working_, primaryClassUri_, sourcePath_, outputPath, revision_);
    if (exported.is_err()) return ca::core::Err(std::move(exported));
    return ca::core::Ok();
}

std::vector<SupportingResourceInfo> WorkspaceBase::supportingResources() const
{
    std::vector<SupportingResourceInfo> result;
    for (const auto& support : supporting_) {
        SupportingResourceInfo info;
        info.id            = support.id;
        info.path          = support.path;
        info.fingerprint   = support.fingerprint;
        info.resourceCount = support.container->size();
        info.role          = support.role;
        support.container->visitResources([&](std::string_view, const model::IResource& resource) {
            if (resource.getType() == model::ResourceType::Class) ++info.classCount;
        });
        result.push_back(std::move(info));
    }
    return result;
}

// ── 台账换底：引擎接线与重放辅助 ──────────────────────────────────

WorkspaceErrorInfo WorkspaceBase::ledgerFailure(const std::string& operation,
                                                const project::ProjectStoreError& cause) const
{
    WorkspaceErrorInfo failure;
    switch (cause.kind) {
    case project::ProjectStoreErrorKind::Open: failure.code = WorkspaceError::NotFound; break;
    case project::ProjectStoreErrorKind::Lock:
        failure.code = WorkspaceError::PermissionDenied;
        break;
    case project::ProjectStoreErrorKind::Section:
    case project::ProjectStoreErrorKind::Schema:
        failure.code = WorkspaceError::DataLoss;
        break;
    case project::ProjectStoreErrorKind::Io: failure.code = WorkspaceError::Internal; break;
    }
    failure.message = operation + ": " + cause.message.to_std_string();
    return failure;
}

WorkspaceResult<std::string> WorkspaceBase::ledgerBaseDigest() const
{
    if (original_ == nullptr) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "ledger digest", "no container loaded"));
    }
    try {
        std::vector<std::string> uris;
        original_->visitResources(
            [&](std::string_view uri, const model::IResource&) { uris.push_back(std::string(uri)); });
        std::sort(uris.begin(), uris.end());
        project::ContainerDigestBuilder builder;
        for (const auto& uri : uris) {
            const auto* resource = original_->getResource(uri);
            if (resource == nullptr) continue;
            auto bytes = bytesOf(*resource);
            if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());
            const auto& content = bytes.unwrap();
            builder.add(uri, content.data(), content.size());
        }
        return ca::core::Ok(builder.finish().to_std_string());
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "ledger digest", exception.what()));
    }
}

std::unique_ptr<model::IResource> WorkspaceBase::resourceFromBytes(const std::string& uri,
                                                                   std::vector<ca::u8> bytes)
{
    if (uri == std::string(mjbase::jar::JarUtil::kManifestFileName)) {
        return std::make_unique<model::ManifestResource>(model::ManifestResource::fromBytes(bytes));
    }
    if (hasClassExtension(uri)) {
        return std::make_unique<model::ClassResource>(uri, std::move(bytes));
    }
    return std::make_unique<model::UnknownResource>(uri, std::move(bytes));
}

WorkspaceResult<std::unique_ptr<model::ResourceContainer>> WorkspaceBase::replayBaseline() const
{
    if (!ledger_.hasBaselineSection()) return cloneContainer(*original_);
    auto bytes = ledger_.baselineBytes();
    if (bytes.is_err())
        return ca::core::Err(ledgerFailure("replay baseline", std::move(bytes).unwrap_err()));
    auto baseline = std::move(bytes).unwrap();
    auto container = std::make_unique<model::ResourceContainer>();
    WorkspaceErrorInfo failure;
    auto scanned = project::forEachContainerEntry(
        reinterpret_cast<const ca::u8*>(baseline.data()), baseline.size(),
        [&](std::string_view uri, const ca::u8* payload, ca::usize payloadSize) {
            container->addResource(uri, resourceFromBytes(
                                            std::string(uri),
                                            std::vector<ca::u8>(payload, payload + payloadSize)));
            return true;
        });
    if (scanned.is_err()) {
        failure = ledgerFailure("replay baseline", std::move(scanned).unwrap_err());
        return ca::core::Err(std::move(failure));
    }
    return ca::core::Ok(std::move(container));
}

WorkspaceResult<std::vector<ca::u8>> WorkspaceBase::resolveBeforeBytes(
    const project::LedgerTransaction& txn, const project::LedgerEntry& entry) const
{
    const std::string uri = entry.uri.to_std_string();
    std::vector<ca::u8> bytes;
    const auto          last = ledger_.lastTouchOf(uri, txn.parent);
    if (last != 0) {
        const auto* touch = ledger_.transaction(last);
        for (const auto& prior : touch->entries) {
            if (!prior.uri.ref().equals(entry.uri.ref())) continue;
            auto payload = ledger_.payloadBytes(prior.payloadRef);
            if (payload.is_err())
                return ca::core::Err(ledgerFailure("resolve before bytes",
                                                   std::move(payload).unwrap_err()));
            bytes = std::move(payload).unwrap();
            break;
        }
    }
    else {
        // 基线侧：压缩基线 section 或原始快照。
        if (ledger_.hasBaselineSection()) {
            auto baseline = ledger_.baselineBytes();
            if (baseline.is_err())
                return ca::core::Err(
                    ledgerFailure("resolve before bytes", std::move(baseline).unwrap_err()));
            auto baselineBytes = std::move(baseline).unwrap();
            std::vector<ca::u8> found;
            auto hit = project::findContainerEntry(
                reinterpret_cast<const ca::u8*>(baselineBytes.data()), baselineBytes.size(), uri,
                found);
            if (hit.is_err())
                return ca::core::Err(ledgerFailure("resolve before bytes",
                                                   std::move(hit).unwrap_err()));
            if (hit.unwrap()) bytes = std::move(found);
        }
        else {
            const auto* resource = original_ != nullptr ? original_->getResource(uri) : nullptr;
            if (resource != nullptr) {
                auto content = bytesOf(*resource);
                if (content.is_err()) return ca::core::Err(std::move(content).unwrap_err());
                bytes = std::move(content).unwrap();
            }
        }
    }
    // digest 自检：before 侧字节必须与台账记录一致（载荷/基线损毁当场暴露）。
    const auto digest =
        project::ledgerDigestOfBytes(bytes.data(), bytes.size());
    if (!digest.ref().equals(entry.before.ref())) {
        return ca::core::Err(error(WorkspaceError::DataLoss, "resolve before bytes",
                                   "digest mismatch for '" + uri + "': ledger says " +
                                       entry.before.to_std_string() + ", resolved " +
                                       digest.to_std_string()));
    }
    return ca::core::Ok(std::move(bytes));
}

WorkspaceErrorInfo WorkspaceBase::applyLedgerTransaction(
    model::ResourceContainer& container, const project::LedgerTransaction& txn,
    const std::string& operation) const
{
    for (const auto& entry : txn.entries) {
        const std::string uri = entry.uri.to_std_string();
        const bool        exists = container.hasResource(uri);
        if (entry.op == project::LedgerEntryOp::Add && exists) {
            return error(WorkspaceError::DataLoss, operation,
                         "add entry but uri already exists: " + uri);
        }
        if (entry.op == project::LedgerEntryOp::Replace && !exists) {
            return error(WorkspaceError::DataLoss, operation,
                         "replace entry but uri is missing: " + uri);
        }
        if (entry.op == project::LedgerEntryOp::Remove) {
            if (!exists)
                return error(WorkspaceError::DataLoss, operation,
                             "remove entry but uri is missing: " + uri);
            auto& resources = container.getResources();
            resources.erase(std::string_view(uri));
            continue;
        }
        auto payload = ledger_.payloadBytes(entry.payloadRef);
        if (payload.is_err()) return ledgerFailure(operation, std::move(payload).unwrap_err());
        auto bytes = std::move(payload).unwrap();
        const auto digest = project::ledgerDigestOfBytes(bytes.data(), bytes.size());
        if (!digest.ref().equals(entry.after.ref())) {
            return error(WorkspaceError::DataLoss, operation,
                         "overlay digest mismatch for '" + uri + "': ledger says " +
                             entry.after.to_std_string() + ", payload is " +
                             digest.to_std_string());
        }
        container.replaceResource(uri, resourceFromBytes(uri, std::move(bytes)));
    }
    return WorkspaceErrorInfo{};
}

WorkspaceErrorInfo WorkspaceBase::inverseApplyLedgerTransaction(
    model::ResourceContainer& container, const project::LedgerTransaction& txn,
    const std::string& operation) const
{
    for (const auto& entry : txn.entries) {
        const std::string uri    = entry.uri.to_std_string();
        const bool        exists = container.hasResource(uri);
        if (entry.op == project::LedgerEntryOp::Add) {
            if (!exists)
                return error(WorkspaceError::DataLoss, operation,
                             "undo add but uri is missing: " + uri);
            auto& resources = container.getResources();
            resources.erase(std::string_view(uri));
            continue;
        }
        if (entry.op == project::LedgerEntryOp::Remove && exists) {
            return error(WorkspaceError::DataLoss, operation,
                         "undo remove but uri already exists: " + uri);
        }
        if (entry.op == project::LedgerEntryOp::Replace && !exists) {
            return error(WorkspaceError::DataLoss, operation,
                         "undo replace but uri is missing: " + uri);
        }
        auto before = resolveBeforeBytes(txn, entry);
        if (before.is_err()) return std::move(before).unwrap_err();
        container.replaceResource(uri,
                                  resourceFromBytes(uri, std::move(before).unwrap()));
    }
    return WorkspaceErrorInfo{};
}

WorkspaceErrorInfo WorkspaceBase::materializeState(
    std::unique_ptr<model::ResourceContainer>& candidate, ca::u64 txn,
    const std::string& operation) const
{
    const ca::u64 head = ledger_.headTxn();
    if (txn == head) return WorkspaceErrorInfo{};
    if (txn > head) {
        for (const ca::u64 step : ledger_.chainTo(txn)) {
            if (step <= head) continue;
            const auto* row = ledger_.transaction(step);
            const auto  applied = applyLedgerTransaction(*candidate, *row, operation);
            if (applied.is_err()) return applied;
        }
        return WorkspaceErrorInfo{};
    }
    auto chain = ledger_.chainTo(head);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (*it <= txn) continue;
        const auto* row     = ledger_.transaction(*it);
        const auto  applied = inverseApplyLedgerTransaction(*candidate, *row, operation);
        if (applied.is_err()) return applied;
    }
    return WorkspaceErrorInfo{};
}

void WorkspaceBase::autoCompactLedger()
{
    if (!ledger_.bound() || !ledger_.shouldAutoCompact()) return;
    // 自动压缩（尽力而为）：先落 pending（压缩前置），再按保留参数阈值推进；
    // 任何失败留给下一次触发，不打断调用方主路径。
    if (ledger_.hasPending()) {
        auto flushed = ledger_.flush(project::rfc3339UtcNow());
        if (flushed.is_err()) return;
    }
    const auto point = ledger_.retentionPoint(project::LedgerCompactRequest{}, true, nullptr);
    if (point <= ledger_.baseTxn()) return;
    try {
        auto candidate = cloneWorking();
        const auto materialized = materializeState(candidate, point, "ledger compaction");
        if (materialized.is_err()) return;
        std::vector<std::pair<ca::str::Utf8String, std::vector<ca::u8>>> entries;
        std::vector<std::string>                                         uris;
        candidate->visitResources(
            [&](std::string_view uri, const model::IResource&) { uris.push_back(std::string(uri)); });
        std::sort(uris.begin(), uris.end());
        for (const auto& uri : uris) {
            const auto* resource = candidate->getResource(uri);
            if (resource == nullptr) continue;
            auto bytes = bytesOf(*resource);
            if (bytes.is_err()) return;
            entries.emplace_back(project::copyLedgerText(uri), std::move(bytes).unwrap());
        }
        (void)ledger_.compact(project::LedgerCompactRequest{},
                              project::serializeContainerEntries(entries),
                              project::rfc3339UtcNow(), true);
    }
    catch (const std::exception&) {
        return;
    }
}

WorkspaceResult<void> WorkspaceBase::bindLedgerStore(const std::string& bundleDir)
{
    const WorkspaceErrorInfo loaded = ensureLoaded("bind ledger store");
    if (loaded.is_err()) return ca::core::Err(std::move(loaded));
    if (bundleDir.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "bind ledger store", "path is empty"));
    auto digest = ledgerBaseDigest();
    if (digest.is_err()) return ca::core::Err(std::move(digest).unwrap_err());

    // 采纳失败不破坏当前会话：先在旁路物化，成功才换底。
    const std::string previousDigest = ledger_.baseContainerDigest().to_std_string();
    auto opened = project::EditLedgerStore::openBundle(
        project::copyLedgerText(bundleDir), project::copyLedgerText(digest.unwrap()));
    if (opened.is_err())
        return ca::core::Err(ledgerFailure("bind ledger store", std::move(opened).unwrap_err()));
    ledger_ = std::move(opened).unwrap();
    if (ledger_.headTxn() != ledger_.baseTxn()) {
        // 候选处于基线态（刚打开的主资源）：从基点向头做前向重放——与
        // materializeState（候选在头位，向书签位正/反向走）语义不同。
        auto candidate = cloneWorking();
        WorkspaceErrorInfo replayFailure;
        for (const ca::u64 step : ledger_.chainTo(ledger_.headTxn())) {
            const auto* row = ledger_.transaction(step);
            if (row == nullptr) {
                replayFailure = error(WorkspaceError::DataLoss, "bind ledger store",
                                      "chain step " + std::to_string(step) +
                                          " is not on the active chain");
                break;
            }
            replayFailure = applyLedgerTransaction(*candidate, *row, "bind ledger store");
            if (replayFailure.is_err()) break;
        }
        if (replayFailure.is_err()) {
            // 回落纯内存引擎（持有锁已随 move 释放），保留旧基线 digest。
            auto fallback = project::EditLedgerStore::createInMemory(
                project::copyLedgerText(previousDigest));
            if (fallback.is_err()) {
                return ca::core::Err(ledgerFailure("bind ledger store",
                                                   std::move(fallback).unwrap_err()));
            }
            ledger_ = std::move(fallback).unwrap();
            return ca::core::Err(std::move(replayFailure));
        }
        working_ = std::move(candidate);
    }
    revision_ = 0;
    redoHint_.clear();
    return ca::core::Ok();
}

WorkspaceResult<void> WorkspaceBase::flushLedger()
{
    auto flushed = ledger_.flush(project::rfc3339UtcNow());
    if (flushed.is_err())
        return ca::core::Err(ledgerFailure("flush ledger", std::move(flushed).unwrap_err()));
    autoCompactLedger();
    return ca::core::Ok();
}

WorkspaceResult<project::LedgerCompactReport> WorkspaceBase::compactLedger(
    const project::LedgerCompactRequest& request)
{
    const WorkspaceErrorInfo loaded = ensureLoaded("compact ledger");
    if (loaded.is_err()) return ca::core::Err(std::move(loaded));
    // 手动压缩前置无 pending（有则先 flush——数据安全序，见 §5.4）。
    if (ledger_.hasPending()) {
        auto flushed = ledger_.flush(project::rfc3339UtcNow());
        if (flushed.is_err())
            return ca::core::Err(ledgerFailure("compact ledger", std::move(flushed).unwrap_err()));
    }
    const auto point = ledger_.retentionPoint(request, false, nullptr);
    if (point <= ledger_.baseTxn()) {
        return ca::core::Err(error(WorkspaceError::FailedPrecondition, "compact ledger",
                                   "retention point does not advance past anchor"));
    }
    try {
        auto candidate = cloneWorking();
        const auto materialized = materializeState(candidate, point, "compact ledger");
        if (materialized.is_err()) return ca::core::Err(materialized);
        std::vector<std::pair<ca::str::Utf8String, std::vector<ca::u8>>> entries;
        std::vector<std::string>                                         uris;
        candidate->visitResources(
            [&](std::string_view uri, const model::IResource&) { uris.push_back(std::string(uri)); });
        std::sort(uris.begin(), uris.end());
        for (const auto& uri : uris) {
            const auto* resource = candidate->getResource(uri);
            if (resource == nullptr) continue;
            auto bytes = bytesOf(*resource);
            if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());
            entries.emplace_back(project::copyLedgerText(uri), std::move(bytes).unwrap());
        }
        auto report = ledger_.compact(request, project::serializeContainerEntries(entries),
                                      project::rfc3339UtcNow(), false);
        if (report.is_err())
            return ca::core::Err(ledgerFailure("compact ledger", std::move(report).unwrap_err()));
        return ca::core::Ok(std::move(report).unwrap());
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "compact ledger", exception.what()));
    }
}

}   // namespace mjbase::workspace
