#include "mjbase/spi/pass_name_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "mjbase/pass/access_modifier_tightener.hpp"
#include "mjbase/pass/char_arr_to_str_simplifier.hpp"
#include "mjbase/pass/class_member_sorter.hpp"
#include "mjbase/pass/class_renamer.hpp"
#include "mjbase/pass/const_char_array_string_folder.hpp"
#include "mjbase/pass/enum_cast_check_remover.hpp"
#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/illegal_class_file_cleaner.hpp"
#include "mjbase/pass/junk_code_inserter.hpp"
#include "mjbase/pass/meta_info_remover.hpp"
#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/pass/remove_method_calls.hpp"
#include "mjbase/pass/stack_operation.hpp"
#include "mjbase/pass/strength_reducer.hpp"
#include "mjbase/pass/tool_class_combiner.hpp"
#include "mjbase/pass/unused_check_cleaner.hpp"
#include "mjbase/pass/unused_member_remover.hpp"

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

// ── 全局默认表 ──
//
// 依赖方向注记：本文件在 spi 域，默认表的种子却是 pass 域的具体 pass——spi 头
// 文件面保持不依赖 pass（pass 依赖 spi 的正向），只在 .cpp 反向包含做种子注册。
// 名字用真实类名，对齐 mbase 旧栈 registerBuiltinPasses 的命名纪律。

namespace {

/// 造默认表：新表种子注册不可能重名，Err 分支即注册面契约破坏，fail loud
/// （static 初始化内异常以 terminate 收场，符合不静默吞错纪律）。
PassNameRegistry makeDefaultRegistry()
{
    PassNameRegistry reg;
    const auto       seed = [&reg](Result<void, RegistryError> r) {
        if (r.is_err()) throw std::logic_error("defaultPassRegistry: seed registration failed");
    };
    seed(reg.registerClassPass(
        "ClassMemberSorter", [] { return std::make_unique<mjbase::pass::ClassMemberSorter>(); },
        "Sort class members into a deterministic order."));
    seed(reg.registerGlobalPass(
        "IllegalClassFileCleaner", [] { return std::make_unique<mjbase::pass::IllegalClassFileCleaner>(); },
        "Drop class entries that cannot be parsed as class files."));
    seed(reg.registerGlobalPass(
        "AccessModifierTightener", [] { return std::make_unique<mjbase::pass::AccessModifierTightener>(); },
        "Tighten access modifiers to the minimum visibility that keeps references valid."));
    // A 批（方法体改写族，纯指令变换）：默认链收尾清理件 + 冗余装载消除 +
    // 字符串族化简 + 冗余空检查清除。
    seed(reg.registerMethodPass(
        "NopRemover", [] { return std::make_unique<mjbase::pass::NopRemover>(); },
        "Remove nop instructions from method bodies."));
    seed(reg.registerMethodPass(
        "RedundantLoadValEliminator",
        [] { return std::make_unique<mjbase::pass::RedundantLoadValEliminator>(); },
        "Remove redundant load-then-store pairs on locals."));
    seed(reg.registerMethodPass(
        "CharArrToStrSimplifier", [] { return std::make_unique<mjbase::pass::CharArrToStrSimplifier>(); },
        "Rewrite char-array String construction into string constants where safe."));
    seed(reg.registerMethodPass(
        "UnusedCheckCleaner", [] { return std::make_unique<mjbase::pass::UnusedCheckCleaner>(); },
        "Remove redundant null/bounds checks whose results are unused."));
    // B 批（加控制流，A1 帧链首批消费者）：确定性垃圾分支块插入。
    seed(reg.registerMethodPass(
        "JunkCodeInserter", [] { return std::make_unique<mjbase::pass::JunkCodeInserter>(); },
        "Insert deterministic dead branch blocks (control-flow noise)."));
    // opt pass 波次（常量折叠族）：操作数栈常量链折叠，i/l/f/d 四型算术 +
    // 移位/按位 + 除零/Min/-1 红线保守停止。标签：scope:method /
    // action:fold / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "ConstantFolder", [] { return std::make_unique<mjbase::pass::ConstantFolder>(); },
        "Fold constant operand-stack chains (arithmetic, shifts, bitwise; conservative stop rules)."));
    // opt pass 波次（强度削减族）：常量 2^k 乘/除改写为移位与移位-加法
    // 修正序列（int/long；float/double 形态不同未做）。标签：
    // scope:method / action:simplify / stability:opt-in。
    seed(reg.registerMethodPass(
        "StrengthReducer", [] { return std::make_unique<mjbase::pass::StrengthReducer>(); },
        "Rewrite multiplication/division by powers of two into shifts (int/long)."));
    // opt pass 波次（栈操作家族首件，W0 样板批）：冗余栈操作窗口折叠 25 条
    // 规则迭代到不动点；ldc 载荷不可判别的宽匹配语义与 Err 保守停收敛声明
    // 见 pass 头注。标签：scope:method / action:simplify / stability:default。
    seed(reg.registerMethodPass(
        "StackOperationSimplifier",
        [] { return std::make_unique<mjbase::pass::StackOperationSimplifier>(); },
        "Fold redundant stack-operation windows to a fixpoint (dup/pop family)."));
    // opt pass 波次（C 方法级指令变换组第一件）：常量 char 数组构造的
    // `new String(char[])` 整窗折叠为字符串 ldc。标签：scope:method /
    // action:fold / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "ConstCharArrayStringFolder",
        [] { return std::make_unique<mjbase::pass::ConstCharArrayStringFolder>(); },
        "Fold whole `new String(constant char[])` construction windows into string ldc."));
    // opt pass 扩段：C 组方法级指令变换组第二件——移除枚举 values() 中
    // 可证明冗余的 INVOKEVIRTUAL/CHECKCAST 相邻对（签名：scope:method /
    // action:eliminate / stability:opt-in，默认不进链，CLI -p 显式点名）。
    seed(reg.registerMethodPass(
        "EnumCastCheckRemover",
        [] { return std::make_unique<mjbase::pass::EnumCastCheckRemover>(); },
        "Remove provably redundant enum values() INVOKEVIRTUAL/CHECKCAST adjacent pairs."));
    // 批 A（容器级直写组首件）：全容器确定性类重命名。
    seed(reg.registerGlobalPass(
        "ClassRenamer", [] { return std::make_unique<mjbase::pass::ClassRenamer>(); },
        "Rename classes across the whole container deterministically."));
    // 批 A（未使用成员清理家族，容器级四件）：空类删除 + 可达闭包类删除 +
    // 可达类内不可达 private 字段/方法删除。
    seed(reg.registerGlobalPass(
        "EmptyClassRemover", [] { return std::make_unique<mjbase::pass::EmptyClassRemover>(); },
        "Remove empty classes from the container."));
    seed(reg.registerGlobalPass(
        "UnusedClassRemover", [] { return std::make_unique<mjbase::pass::UnusedClassRemover>(); },
        "Remove classes unreachable from container entry points."));
    seed(reg.registerGlobalPass(
        "UnusedFieldRemover", [] { return std::make_unique<mjbase::pass::UnusedFieldRemover>(); },
        "Remove private fields that are never read or written."));
    seed(reg.registerGlobalPass(
        "GlobalUnusedMethodRemover",
        [] { return std::make_unique<mjbase::pass::GlobalUnusedMethodRemover>(); },
        "Remove private methods that are never invoked."));
    // opt pass 波次（调用点收尾件，删除族前置）：按名单点名删除成员调用点 +
    // 无用结果白名单面。默认无配置 = 完全 no-op（opt-in）；配置为程序化注入
    // （MethodCallRemovalConfig），CLI/配置解析供给面属 cli 批。标签：
    // scope:global / action:eliminate / stability:opt-in。
    seed(reg.registerGlobalPass(
        "MethodCallRemover", [] { return std::make_unique<mjbase::pass::MethodCallRemover>(); },
        "Remove call sites of spec-named members and discardable-result whitelist calls."));
    // 批 B（类级骨架组，meta 元数据清理家族 13 件）：catch-all debug/framework
    // 元数据清除 + 单面件（final / synthetic+bridge / kotlin metadata /
    // source 信息 / 签名 / 非法注解 / 非法签名 / 非法 type 注解 / 非法 source
    // 信息 / 未知属性 / 非法 access 位 / 黑名单注解）。
    seed(reg.registerClassPass(
        "MetaInfoRemover", [] { return std::make_unique<mjbase::pass::MetaInfoRemover>(); },
        "Remove debug/framework metadata (source, signatures, annotations, unknown attributes)."));
    seed(reg.registerClassPass(
        "FinalRemover", [] { return std::make_unique<mjbase::pass::FinalRemover>(); },
        "Remove final modifiers from classes, fields and methods."));
    seed(reg.registerClassPass(
        "SyntheticBridgeRemover", [] { return std::make_unique<mjbase::pass::SyntheticBridgeRemover>(); },
        "Remove synthetic and bridge markers."));
    seed(reg.registerClassPass(
        "KotlinMetadataRemover", [] { return std::make_unique<mjbase::pass::KotlinMetadataRemover>(); },
        "Remove Kotlin @Metadata annotations."));
    seed(reg.registerClassPass(
        "SourceInfoRemover", [] { return std::make_unique<mjbase::pass::SourceInfoRemover>(); },
        "Remove SourceFile attributes and line number tables."));
    seed(reg.registerClassPass(
        "SignatureRemover", [] { return std::make_unique<mjbase::pass::SignatureRemover>(); },
        "Remove generic Signature attributes."));
    seed(reg.registerClassPass(
        "IllegalAnnotationRemover",
        [] { return std::make_unique<mjbase::pass::IllegalAnnotationRemover>(); },
        "Remove annotations with malformed structure or illegal targets."));
    seed(reg.registerClassPass(
        "IllegalSignatureRemover", [] { return std::make_unique<mjbase::pass::IllegalSignatureRemover>(); },
        "Remove malformed Signature attributes."));
    seed(reg.registerClassPass(
        "IllegalTypeAnnotationRemover",
        [] { return std::make_unique<mjbase::pass::IllegalTypeAnnotationRemover>(); },
        "Remove malformed type annotations."));
    seed(reg.registerClassPass(
        "IllegalSourceInfoRemover",
        [] { return std::make_unique<mjbase::pass::IllegalSourceInfoRemover>(); },
        "Remove malformed SourceFile/LineNumberTable attributes."));
    seed(reg.registerClassPass(
        "UnknownAttributeCleaner", [] { return std::make_unique<mjbase::pass::UnknownAttributeCleaner>(); },
        "Remove unrecognized class-file attributes."));
    seed(reg.registerClassPass(
        "IllegalAccessRemover", [] { return std::make_unique<mjbase::pass::IllegalAccessRemover>(); },
        "Remove illegal access flag combinations."));
    seed(reg.registerClassPass(
        "SpecificAnnotationRemover",
        [] { return std::make_unique<mjbase::pass::SpecificAnnotationRemover>(); },
        "Remove annotations matching the configured blacklist."));
    // 批 A（容器级直写组收口件）：static 工具类保守合并 + 全量 owner 重映射。
    seed(reg.registerGlobalPass(
        "ToolClassCombiner", [] { return std::make_unique<mjbase::pass::ToolClassCombiner>(); },
        "Merge static utility classes and remap their owners."));
    return reg;
}

}   // namespace

PassNameRegistry& defaultPassRegistry()
{
    static PassNameRegistry registry = makeDefaultRegistry();
    return registry;
}

}   // namespace mjbase::spi
