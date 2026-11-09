#include "resource_jasm_service.hpp"

#include "classfile_utf8.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"

#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mjbase::jasm {
namespace {

using ca::core::Err;
using ca::core::Ok;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string_view view(ca::str::Utf8StringRef value)
{
    return std::string_view(reinterpret_cast<const char*>(value.data()), value.byte_length());
}

template<typename T>
Result<T> resourceError(const std::string& message)
{
    Result<T> result;
    result.diagnostics.push_back(
        {DiagnosticSeverity::Error, DiagnosticStage::Resource, utf16(message), std::nullopt});
    return result;
}

// 读取 class internal name（jbc 骨架解析）；字节不可解析返回 Err(ReadError)。
// 与 jvm_jasm_service.cpp 的同名 helper 保持同一语义（mbase 两 TU 也各自持有）。
ca::core::Result<ca::str::Utf8String, jbc::core::ReadError>
readInternalName(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return Err(std::move(read).unwrap_err());
    auto                parsed = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
    const std::string_view name = parsed.file->thisName();
    // cp Utf8 是 modified UTF-8，严格校验会拒绝增补平面类名；重组后物化。
    const std::string converted = classFileUtf8ToString(name);
    return Ok(ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(converted.data()),
                                             converted.size()));
}

// 字节不可解析时的稳定 Error 诊断（落在 Resource 阶段）。
template<typename T>
Result<T> unreadableClassResult(jbc::core::ReadError error)
{
    return resourceError<T>(std::string("invalid class bytes: ") + jbc::core::to_string(error));
}

void appendDiagnostics(std::vector<Diagnostic>& target, std::vector<Diagnostic> source)
{
    target.insert(target.end(),
                  std::make_move_iterator(source.begin()),
                  std::make_move_iterator(source.end()));
}

// ── 保留原帧档（issue #190）──────────────────────────────────────────────

/// @brief 原 class 单方法的帧保留素材。
struct PreservedMethodFrames final
{
    /// @brief 原 code[] 字节——候选 pc 对齐的判等依据（唯一比较面）。
    ///
    /// 刻意只比 code[]：异常表/max_stack/max_locals/嵌套 LNT 的改动不移动
    /// 指令 pc，原帧对指令流的描述仍然成立。
    std::vector<ca::u8> code;
    /// @brief 原 StackMapTable 内容字节（span 原样副本，不解码不规范化——
    /// 结构化解码对坏帧类可能丢失 unnamed slot 等形态）。
    std::vector<ca::u8> stackMapTable;
    bool hasStackMapTable = false;
    /// @brief 原 Code 属性可解码（false 时无法做 pc 对齐判等，fail-closed）。
    bool codeReadable = false;
};

/// @brief 原 class 的逐方法帧素材与版本（回填用）。
struct PreservedClassFrames final
{
    std::map<std::string, PreservedMethodFrames> methods;   ///< 键 "name:desc"。
    ca::u16 classMajor = 0;
    ca::u16 classMinor = 0;
};

/// @brief 从原 class 字节提取逐方法帧素材。
///
/// readClass 默认保留源字节副本：StackMapTable 槽的 span 指向模型内部字节，
/// 立即复制后与原 buffer 生命周期解耦。抽象/native 方法（无 Code）不参与。
ca::core::Result<PreservedClassFrames, std::string>
extractPreservedFrames(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err())
        return Err(std::string("invalid class bytes: ") + jbc::core::to_string(read.unwrap_err()));
    auto                      parsed = std::move(read).unwrap();
    jbc::core::ClassFile&     file   = *parsed.file;
    PreservedClassFrames      out;
    out.classMajor = file.major;
    out.classMinor = file.minor;
    for (auto& method : file.methods) {
        const std::string key = std::string(file.nameOf(method)) + ':' +
                                std::string(file.descOf(method));
        jbc::core::AttrSlot* codeSlot = nullptr;
        for (auto& slot : method.attrs) {
            if (file.utf8(slot.nameCp) == "Code") {
                codeSlot = &slot;
                break;
            }
        }
        if (codeSlot == nullptr) continue;   // 抽象/native：无帧参与
        PreservedMethodFrames entry;
        auto decoded = file.decodeAttr(*codeSlot);
        if (decoded.is_err()) {
            out.methods.emplace(std::move(key), std::move(entry));   // codeReadable=false
            continue;
        }
        auto* code  = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
        entry.codeReadable = true;
        entry.code         = code->code;
        for (const auto& nested : code->attrs) {
            if (file.utf8(nested.nameCp) != "StackMapTable") continue;
            const auto raw = file.source().spanOrEmpty(nested.data);
            entry.stackMapTable.assign(raw.data(), raw.data() + raw.size());
            entry.hasStackMapTable = true;
            break;
        }
        out.methods.emplace(std::move(key), std::move(entry));
    }
    return Ok(std::move(out));
}

/// @brief 候选字节回填原类版本并逐方法挂回原 StackMapTable。
///
/// 成功返回落盘字节；Err 携带逐条边界申报（每方法一条，点名方法）。
/// 挂回面：新 AttrSlot（RawAttr 原样副本，State::New）入 Code 嵌套属性表，
/// 槽置 Modified 触发重编码——Decoded 态写回走源 span copy，改动会静默丢失。
ca::core::Result<std::vector<ca::u8>, std::vector<std::string>>
applyPreservedFrames(const std::vector<ca::u8>& candidate, const PreservedClassFrames& original)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(candidate));
    if (read.is_err()) {
        return Err(std::vector<std::string>{
            std::string("assembled candidate bytes unreadable: ") +
            jbc::core::to_string(read.unwrap_err())});
    }
    auto                  parsed = std::move(read).unwrap();
    jbc::core::ClassFile& file   = *parsed.file;

    // 汇编面为跳过帧生成以 v49 装配（见 assemble 内注释），此处回填原类版本。
    file.major = original.classMajor;
    file.minor = original.classMinor;

    std::vector<std::string> errors;
    for (auto& method : file.methods) {
        const std::string key = std::string(file.nameOf(method)) + ':' +
                                std::string(file.descOf(method));
        jbc::core::AttrSlot* codeSlot = nullptr;
        for (auto& slot : method.attrs) {
            if (file.utf8(slot.nameCp) == "Code") {
                codeSlot = &slot;
                break;
            }
        }
        if (codeSlot == nullptr) continue;
        auto decoded = file.decodeAttr(*codeSlot);
        if (decoded.is_err()) {
            errors.push_back("method " + key + ": assembled code attribute unreadable");
            continue;
        }
        auto* code    = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
        const auto it = original.methods.find(key);
        if (it == original.methods.end()) {
            // 候选新方法体：无原帧可保留；v50+ 缺帧对分支方法是不可加载产物，
            // fail-closed 拒绝（线性方法同理拒收——不做逐方法分支分析，保守申报）。
            if (original.classMajor >= 50)
                errors.push_back("method " + key +
                                 ": new method body has no original StackMapTable to preserve"
                                 " (use FrameMode::Regenerate)");
            continue;
        }
        const PreservedMethodFrames& src = it->second;
        if (!src.codeReadable) {
            errors.push_back("method " + key +
                             ": original code attribute unreadable, cannot verify pc alignment");
            continue;
        }
        if (code->code != src.code) {
            // 指令面改动：原帧的 pc 描述失效，保留即产坏类——如实申报边界。
            if (src.hasStackMapTable)
                errors.push_back("method " + key +
                                 ": instructions changed, the original StackMapTable no longer"
                                 " applies (use FrameMode::Regenerate)");
            continue;   // 原本无帧：无可失效面，放行
        }
        if (!src.hasStackMapTable) continue;
        auto nameCp = file.cp.findUtf8("StackMapTable");
        if (nameCp == 0) nameCp = file.cp.addUtf8("StackMapTable");
        // 槽推进 Modified：嵌入原帧后重编码是唯一出路（Decoded 态写回走源
        // span copy，嵌入会静默丢失）。
        static_cast<void>(codeSlot->mutate());
        code->attrs.push_back(jbc::core::AttrSlot::makeNew(
            nameCp, std::make_unique<jbc::core::RawAttr>(src.stackMapTable)));
    }
    if (!errors.empty()) return Err(std::move(errors));

    auto written = jbc::core::writeClass(file);
    if (written.is_err())
        return Err(std::vector<std::string>{
            std::string("preserved-frames write-back failed: ") +
            jbc::core::to_string(written.unwrap_err())});
    return Ok(std::move(written).unwrap());
}

// 复制容器中全部 Class 资源，目标 URI 的字节换成候选字节——事务预览的工作区。
model::ResourceContainer candidateContainer(const model::ResourceContainer& container,
                                            std::string_view                targetUri,
                                            const std::vector<ca::u8>&      candidateBytes)
{
    model::ResourceContainer candidate;
    container.visitResources([&](std::string_view uri, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (!classResource) return;
        candidate.addResource(uri,
                              std::make_unique<model::ClassResource>(
                                  view(classResource->getLocation()),
                                  targetUri == uri ? candidateBytes : classResource->get()));
    });
    return candidate;
}

// 容器 → 层次图即时重建：只读 Class 资源的 this/super/接口骨架（jbc 骨架解析）。
// 畸形条目跳过，不进图。帧计算（InheritanceChecker）只依赖类图，成员摘要不参与；
// 层次错误（DuplicateClass/Cycle）按跳过处理——服务这里只取「可用的类图」，
// 与 mbase ClassHierarchyAnalysis 对畸形输入的容错口径一致。
mjbase::hierarchy::ClassHierarchy buildHierarchy(const model::ResourceContainer& container,
                                                 mjbase::KeyPool&                pool)
{
    mjbase::hierarchy::ClassHierarchy hierarchy(pool);
    container.visitResources([&](std::string_view, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (!classResource) return;
        const auto& bytes = classResource->get();
        auto        read  = jbc::core::readClass(jbc::core::ByteView(bytes));
        if (read.is_err()) return;
        auto        parsed = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
        const auto& file   = *parsed.file;

        mjbase::hierarchy::ClassSummary summary;
        summary.self = mjbase::classKeyOf(pool, classFileUtf8ToString(file.thisName()));
        const std::string_view superName = file.superName();
        summary.super = superName.empty()
                            ? mjbase::ClassKey{}
                            : mjbase::classKeyOf(pool, classFileUtf8ToString(superName));
        for (const ca::u16 ifaceCp : file.interfaces) {
            summary.interfaces.push_back(
                mjbase::classKeyOf(pool, classFileUtf8ToString(file.className(ifaceCp))));
        }
        (void)hierarchy.addClass(summary);
    });
    return hierarchy;
}

}   // namespace

Result<JvmJasmDisassembly> ResourceJasmService::disassemble(std::string_view uri) const
{
    const auto* resource =
        ca::core::tag_cast::dyn_cast<const model::ClassResource>(container_->getResource(uri));
    if (!resource)
        return resourceError<JvmJasmDisassembly>("Class resource not found: " + std::string(uri));
    return jasm_.disassemble(resource->get());
}

Result<JvmJasmClass> ResourceJasmService::assemble(std::string_view        uri,
                                                   ca::str::Utf16StringRef sourceName,
                                                   ca::str::Utf16StringRef source,
                                                   JvmJasmContext          context,
                                                   FrameMode               frameMode)
{
    model::IResource* rawResource = container_->getResource(uri);
    if (!ca::core::tag_cast::isa<model::ClassResource>(rawResource))
        return resourceError<JvmJasmClass>("Class resource not found: " + std::string(uri));
    auto* resource = ca::core::tag_cast::cast<model::ClassResource>(rawResource);

    try {
        auto expected = readInternalName(resource->get());
        if (expected.is_err()) return unreadableClassResult<JvmJasmClass>(expected.unwrap_err());
        const ca::str::Utf8String expectedName = std::move(expected).unwrap();

        // 重写保持原类版本：Jasm 文本无版本指令，context 是唯一版本通道，
        // 缺省 61 会把旧版本类静默抬高、改变产物最低 JVM 要求。原字节不可
        // 解析时保留缺省——jasm 是坏类修复通道，不因读不出版本而拒绝。
        auto original          = jbc::core::readClass(jbc::core::ByteView(resource->get()));
        const bool originalReadable = original.is_ok();

        if (originalReadable) {
            auto originalClass        = std::move(original).unwrap();
            context.classMajorVersion = static_cast<ca::i32>(originalClass.file->major);
            context.classMinorVersion = static_cast<ca::i32>(originalClass.file->minor);
        }

        // 保留原帧档素材提取。原字节不可解析时走不到这里——readInternalName
        // 用同一 readClass 已先行拒绝；此处 re-read 只为素材提取自足（原字节
        // 副本随版本通道的 ReadResult 消亡）。
        std::optional<PreservedClassFrames> preserved;
        if (frameMode == FrameMode::PreserveOriginal) {
            auto extracted = extractPreservedFrames(resource->get());
            if (extracted.is_err()) return resourceError<JvmJasmClass>(extracted.unwrap_err());
            preserved = std::move(extracted).unwrap();
            // jhasm 0.3.3 汇编面唯一版本杠杆：cf.major >= 50 是 StackMapTable
            // 生成的唯一闸（engine.compute 数据流不读版本、仍运行——数据流本身
            // 失败的类两档同败，属申报边界）。以 v49 装配跳过帧生成，产物版本
            // 在 applyPreservedFrames 回填原类版本。文本汇编面（JvmJasmService）
            // 不消费 FrameMode：本 lever 只在持有原字节的本服务落地。
            context.classMajorVersion = 49;
        }

        // 当前容器的层次图（每次调用即时重建，无跨调用缓存）。
        mjbase::KeyPool               pool;
        mjbase::hierarchy::ClassHierarchy currentHierarchy = buildHierarchy(*container_, pool);

        context.hierarchy = &currentHierarchy;
        auto preliminary  = jasm_.assemble(sourceName, source, context);
        if (!preliminary.isOk()) return preliminary;
        if (preliminary.value->internalName != expectedName) {
            auto result = resourceError<JvmJasmClass>(
                "Class name mismatch for " + std::string(uri) + ": expected " +
                expectedName.to_std_string() + ", got " +
                preliminary.value->internalName.to_std_string());
            appendDiagnostics(result.diagnostics, std::move(preliminary.diagnostics));
            return result;
        }

        // 候选工作区：候选容器的层次图全部用新字节重建后复验。
        auto candidate                   = candidateContainer(*container_, uri,
                                                               preliminary.value->bytes);
        mjbase::hierarchy::ClassHierarchy candidateHierarchy = buildHierarchy(candidate, pool);

        context.hierarchy = &candidateHierarchy;
        auto compiled     = jasm_.assemble(sourceName, source, context);
        if (!compiled.isOk()) return compiled;
        if (compiled.value->internalName != expectedName)
            return resourceError<JvmJasmClass>("Class name changed between compilation passes");

        // 保留原帧档：候选字节回填原类版本 + 逐方法挂回原 StackMapTable。
        // 边界申报（指令面改动/新方法体）逐条落 Error 诊断，事务不落改动。
        if (preserved) {
            auto grafted = applyPreservedFrames(compiled.value->bytes, *preserved);
            if (grafted.is_err()) {
                Result<JvmJasmClass> result;
                for (const auto& error : grafted.unwrap_err()) {
                    result.diagnostics.push_back(
                        {DiagnosticSeverity::Error, DiagnosticStage::Assemble, utf16(error),
                         std::nullopt});
                }
                appendDiagnostics(result.diagnostics, std::move(compiled.diagnostics));
                return result;
            }
            compiled.value->bytes = std::move(grafted).unwrap();
        }

        auto validated = jasm_.validate(compiled.value->bytes, context);
        if (!validated.isOk()) {
            appendDiagnostics(validated.diagnostics, std::move(compiled.diagnostics));
            return validated;
        }

        JvmJasmClass            output            = std::move(*compiled.value);
        std::vector<Diagnostic> outputDiagnostics = std::move(compiled.diagnostics);
        appendDiagnostics(outputDiagnostics, std::move(validated.diagnostics));
        resource->set(output.bytes);
        return {std::move(output), std::move(outputDiagnostics)};
    }
    catch (const std::exception& exception) {
        return resourceError<JvmJasmClass>(exception.what());
    }
}

}   // namespace mjbase::jasm
