#include "mjbase/pass/kotlin_name_restoration.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "mjbase/mapping/identifier_util.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/kotlin_metadata_collection.hpp"

namespace mjbase::pass {
namespace {

using mjbase::mapping::IdentifierUtil;
using mjbase::mapping::IntermediateMapping;

ca::str::Utf8StringRef utf8Ref(std::string_view value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// CP String 条目内容；非 String 条目返回空（收集件同名 helper 的同口径
/// 复制件——该面未跨件导出）。
std::string_view cpStringText(const jbc::core::ClassFile& cf, ca::u32 stringCp)
{
    if (!cf.cp.valid(stringCp) || cf.cp.tagOf(stringCp) != jbc::core::CpTag::String) return {};
    return cf.cp.utf8Of(cf.cp.at(stringCp).a);
}

/// 类级 RuntimeVisibleAnnotations 中 ``Lkotlin/jvm/JvmName;`` 的 name 元素值
/// （kotlin 文件门面回退面）。无注解/形状不符返回空（回退面 fail-open，
/// 不拒类）。
std::string jvmNameAnnotationValue(jbc::core::ClassFile& cf)
{
    jbc::core::AttrSlot* slot = cf.findClassAttr("RuntimeVisibleAnnotations");
    if (slot == nullptr) return {};
    auto decoded = cf.decodeAttr(*slot);
    if (decoded.is_err()) return {};
    const auto* body = ca::core::tag_cast::cast<const jbc::core::AnnotationsAttr*>(slot->peek());
    if (body == nullptr) return {};
    for (const auto& anno : body->annos) {
        if (!cf.cp.utf8Equals(anno.typeCp, "Lkotlin/jvm/JvmName;")) continue;
        for (const auto& [keyCp, value] : anno.values) {
            if (!cf.cp.utf8Equals(keyCp, "name")) continue;
            if (value.tag != 's') return {};
            return std::string(cpStringText(cf, value.cp));
        }
        return {};
    }
    return {};
}

/// 成员名合成形态：名字含 ``$``（access$ 转发 / $default 桥 / $$delegate_0
/// 委托面 / lambda 编号——Kotlin 用户标识符软保留 ``$``）或 ``<`` 开头
/// （init/clinit）。合成成员不进候选集、不收映射（合成名消解面）。
bool isSyntheticShape(std::string_view name)
{
    if (name.empty()) return true;
    if (name.front() == '<') return true;
    return name.find('$') != std::string_view::npos;
}

/// 描述符匹配双形态：先容器真名段反向重写（reverseMapDescriptor——收集件
/// Kotlin 真名替换回本容器二进制名），再 stdlib 替换（kotlinToJvmDescriptor）；
/// 成员描述符命中任一形态即候选（Recaf getCandidates 同口径）。
struct MappedDesc
{
    std::string reverseMapped;
    std::string fullyMapped;
};

MappedDesc mapKtDescriptor(const KotlinMetadataCollection& collection, std::string_view ktDesc)
{
    MappedDesc out;
    out.reverseMapped = reverseMapDescriptor(collection, ktDesc);
    out.fullyMapped   = kotlinToJvmDescriptor(out.reverseMapped);
    return out;
}

/// 首字母 ASCII 大写（多字节首字符原样保留——申报边界）。
std::string upperFirst(std::string name)
{
    if (!name.empty() && name.front() >= 'a' && name.front() <= 'z') {
        name.front() = static_cast<char>(name.front() - 'a' + 'A');
    }
    return name;
}

/// is 前缀属性名（"is" 后随大写字母才构成 kotlinc 约定前缀；island 不算）。
bool hasIsPrefix(const std::string& name)
{
    return name.size() > 2 && name[0] == 'i' && name[1] == 's' && name[2] >= 'A' && name[2] <= 'Z';
}

/// getter 真名：get/is/do 前缀属性名原样（kotlinc 约定，Recaf 同表），其余
/// "get" + 首字母大写。
std::string getterNameOf(std::string_view propertyName)
{
    const std::string name(propertyName);
    if (name.rfind("get", 0) == 0 || name.rfind("is", 0) == 0 || name.rfind("do", 0) == 0) {
        return name;
    }
    return "get" + upperFirst(name);
}

/// setter 真名："set" + 首字母大写；is 前缀属性按 kotlinc 约定去 is
/// （isActive → setActive。Recaf 原件无 setter 面，本件为任务判据面补强）。
std::string setterNameOf(std::string_view propertyName)
{
    const std::string name(propertyName);
    if (hasIsPrefix(name)) return "set" + name.substr(2);
    return "set" + upperFirst(name);
}

/// 包段前缀（含尾 '/'；根包返回空——Recaf 的空包 + '/' 产 "/Name" 的边界
/// 修正）。
std::string packagePrefixOf(std::string_view binaryName)
{
    const ca::usize slash = binaryName.rfind('/');
    if (slash == std::string_view::npos) return {};
    return std::string(binaryName.substr(0, slash + 1));
}

/// 单类分析：类名槽（JvmName 注解 > metadata fqName）+ 成员槽（属性
/// field/getter/setter → 函数；描述符唯一匹配；槽位先写者胜）。
void analyzeKotlinClass(jbc::ir::ClassSession&          session,
                        const KotlinMetadataCollection& collection,
                        IntermediateMapping&            mapping)
{
    jbc::ir::ClassUnit&         unit       = session.unit();
    const std::string_view      binaryName = unit.symbols().str(unit.thisClass);
    const KtClass*              model      = collection.findClass(binaryName);

    // ── 类名槽：注解声明的名字优先于 metadata 推断（Recaf 口径的兜底面
    // 前置——覆盖文件门面场景：k=2/5 模型名 = 定义类二进制名，无真名信息）──
    const std::string jvmName = jvmNameAnnotationValue(session.backing());
    if (IdentifierUtil::isValidJavaIdentifier(utf8Ref(jvmName))) {
        const std::string target = packagePrefixOf(binaryName) + jvmName;
        if (target != binaryName) {
            mapping.addClass(utf8Ref(binaryName), utf8Ref(target));
        }
    }
    else if (model != nullptr && model->hasName
             && static_cast<std::string_view>(model->name) != binaryName) {
        mapping.addClass(utf8Ref(binaryName), model->name);
    }

    if (model == nullptr) return;   ///< 无模型：类名回退槽到此为止（Recaf 同口径）

    // ── 成员槽 ──
    // 唯一描述符命中（metadata 无序，Recaf 口径：只认恰好一个候选）；合成
    // 形态成员先行剔除（合成名消解）。
    const auto soleFieldOf = [&](const MappedDesc& desc) -> const jbc::ir::FieldUnit* {
        const jbc::ir::FieldUnit* found = nullptr;
        for (const jbc::ir::FieldUnit& f : unit.fields) {
            const std::string_view fd = unit.symbols().str(f.desc);
            if (fd != desc.reverseMapped && fd != desc.fullyMapped) continue;
            if (isSyntheticShape(unit.symbols().str(f.name))) continue;
            if (found != nullptr) return nullptr;   ///< 多命中：放弃（唯一匹配口径）
            found = &f;
        }
        return found;
    };
    const auto soleMethodOf = [&](const MappedDesc& desc) -> const jbc::ir::MethodUnit* {
        const jbc::ir::MethodUnit* found = nullptr;
        for (const jbc::ir::MethodUnit& m : unit.methods) {
            const std::string_view md = unit.symbols().str(m.desc);
            if (md != desc.reverseMapped && md != desc.fullyMapped) continue;
            if (isSyntheticShape(unit.symbols().str(m.name))) continue;
            if (found != nullptr) return nullptr;
            found = &m;
        }
        return found;
    };

    // 槽位去重：同 (desc, oldName) 槽以首写者胜（处理序即优先序：属性
    // field/getter/setter > 函数），同名幂等跳过（冲突消解面）。
    std::unordered_set<std::string> fieldSlots;
    std::unordered_set<std::string> methodSlots;
    const auto addFieldSlot = [&](std::string_view desc, std::string_view oldName,
                                  std::string_view newName) {
        if (oldName == newName) return;   ///< 已正名：零条目（映射面最小化）
        if (isSyntheticShape(newName)) return;
        if (!fieldSlots.emplace(std::string(desc) + '\x1f' + std::string(oldName)).second) return;
        mapping.addField(utf8Ref(binaryName), utf8Ref(desc), utf8Ref(oldName), utf8Ref(newName));
    };
    const auto addMethodSlot = [&](std::string_view desc, std::string_view oldName,
                                   std::string_view newName) {
        if (oldName == newName) return;
        if (isSyntheticShape(newName)) return;
        if (!methodSlots.emplace(std::string(desc) + '\x1f' + std::string(oldName)).second) return;
        mapping.addMethod(utf8Ref(binaryName), utf8Ref(desc), utf8Ref(oldName), utf8Ref(newName));
    };

    // 属性面：backing field / getter / setter 三槽。类型面不全的属性不进
    // 匹配（Object 兜底描述符的匹配证据不足——Recaf 无此门，收紧申报）。
    // setter 槽只对 hasSetter 属性开（val 属性上偶然同形的用户方法不得误
    // 恢复）；getter 当前名已等于属性名时跳过（Recaf 同口径保守面）。
    for (const KtVariable& prop : model->properties) {
        const std::string_view propName = prop.name;
        if (propName.empty() || !prop.hasType) continue;
        const MappedDesc typeDesc = mapKtDescriptor(collection, ktTypeDescriptor(prop.type));

        if (const jbc::ir::FieldUnit* field = soleFieldOf(typeDesc)) {
            addFieldSlot(unit.symbols().str(field->desc), unit.symbols().str(field->name),
                         propName);
        }

        const MappedDesc getterDesc = mapKtDescriptor(collection, "()" + ktTypeDescriptor(prop.type));
        if (const jbc::ir::MethodUnit* getter = soleMethodOf(getterDesc)) {
            const std::string_view getterName = unit.symbols().str(getter->name);
            if (getterName != propName) {   ///< Recaf 同口径：已与属性名等名则跳过
                addMethodSlot(unit.symbols().str(getter->desc), getterName,
                              getterNameOf(propName));
            }
        }

        if (prop.hasSetter) {
            const MappedDesc setterDesc =
                mapKtDescriptor(collection, "(" + ktTypeDescriptor(prop.type) + ")V");
            if (const jbc::ir::MethodUnit* setter = soleMethodOf(setterDesc)) {
                const std::string_view setterName = unit.symbols().str(setter->name);
                if (setterName != propName) {   ///< getter 槽同款保守面
                    addMethodSlot(unit.symbols().str(setter->desc), setterName,
                                  setterNameOf(propName));
                }
            }
        }
    }

    // 函数面：扩展接收者已被收集件展开为 JVM 首参（描述符面对齐）。类型面
    // 不全（无返回或任一参数缺类型）不进匹配（同上收紧口径）。
    for (const KtFunction& fn : model->functions) {
        if (fn.name.is_empty() || !fn.hasReturnType) continue;
        bool paramsTyped = true;
        for (const KtVariable& param : fn.parameters) {
            if (!param.hasType) paramsTyped = false;
        }
        if (!paramsTyped) continue;
        const MappedDesc fnDesc = mapKtDescriptor(collection, ktFunctionDescriptor(fn));
        if (const jbc::ir::MethodUnit* method = soleMethodOf(fnDesc)) {
            addMethodSlot(unit.symbols().str(method->desc), unit.symbols().str(method->name),
                          fn.name);
        }
    }
}

}   // namespace

KotlinNameRestorationAnalysis::Result KotlinNameRestorationAnalysis::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    // 依赖收集件：幂等自举（经 pass 壳的常规路径已注册，重复注册不覆盖）。
    am.registerAnalysis<KotlinMetadataCollectionAnalysis>();
    const KotlinMetadataCollection& collection =
        am.getResult<KotlinMetadataCollectionAnalysis>(container);

    IntermediateMapping mapping;
    for (auto& [uri, res] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(res.get());
        if (cls == nullptr) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) continue;   ///< 畸形类资源跳过（class_scan fail-open 同口径）
        analyzeKotlinClass(*std::move(sessionResult).unwrap(), collection, mapping);
    }
    return mapping;
}

mjbase::spi::PreservedAnalyses KotlinNameRestorer::run(mjbase::model::ResourceContainer&     container,
                                                       mjbase::spi::GlobalAnalysisManager& am)
{
    am.registerAnalysis<KotlinMetadataCollectionAnalysis>();   ///< 幂等：重复注册不覆盖
    am.registerAnalysis<KotlinNameRestorationAnalysis>();
    (void)am.getResult<KotlinNameRestorationAnalysis>(container);   ///< 触发计算并缓存
    return mjbase::spi::PreservedAnalyses::all();   ///< 只读：字节零改动
}

}   // namespace mjbase::pass
