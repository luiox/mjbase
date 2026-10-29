#include "mjbase/asm/remapper/member_remapper.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/cp_resolve.hpp"
#include "jbc/core/opcode.hpp"

#include "mjbase/asm/analyzer/alias_analysis.hpp"
#include "mjbase/jasm/classfile_utf8.hpp"
#include "mjbase/reference/reflection_interpreter.hpp"

namespace mjbase::asm_remapper {
namespace {

using jbc::core::AnnotationsAttr;
using jbc::core::AnnotationDefaultAttr;
using jbc::core::AttrBody;
using jbc::core::AttrKind;
using jbc::core::AttrSlot;
using jbc::core::BootstrapMethodsAttr;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::ConstantPool;
using jbc::core::CpTag;
using jbc::core::EnclosingMethodAttr;
using jbc::core::ExceptionsAttr;
using jbc::core::HandleSem;
using jbc::core::InnerClassesAttr;
using jbc::core::LocalVariableTableAttr;
using jbc::core::MemberInfo;
using jbc::core::ModuleAttr;
using jbc::core::ModuleMainClassAttr;
using jbc::core::NestHostAttr;
using jbc::core::NestMembersAttr;
using jbc::core::ParameterAnnotationsAttr;
using jbc::core::PermittedSubclassesAttr;
using jbc::core::RecordAttr;
using jbc::core::StackMapTableAttr;
using jbc::core::TypeAnnotationsAttr;
using jbc::ir::ClassSession;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using Op = jbc::core::Op;

constexpr ca::u16 kAccStatic = 0x0008;

// ── cp 字符串边界 ───────────────────────────────────────────────────────────
//
// 计划键值、容器 URI 与文本资源都是标准 UTF-8 规范形；常量池 Utf8 条目是
// modified UTF-8。读侧（plan 查表入参）一律经 cpToUtf8 解码，写侧（cp
// 驻留）经 utf8ToCp 编码——编码对 modified UTF-8 形态输入幂等，混入旧
// 形态字节不会二次变形。增补平面类名（混淆器常用）在两种形态字节不同，
// 不做边界转换时计划查表必然 miss。

std::string cpToUtf8(std::string_view value)
{
    return mjbase::jasm::classFileUtf8ToString(value);
}

std::string utf8ToCp(std::string_view value)
{
    return mjbase::jasm::utf8StringToClassFile(value);
}

// ── 常量池基础助手 ──────────────────────────────────────────────────────────

/// @brief CP Class 条目 → 内部名；非 Class/越界返回空。
std::string_view classNameOfCp(const ClassFile& cf, ca::u32 classCp) noexcept
{
    if (!cf.cp.valid(classCp) || cf.cp.tagOf(classCp) != CpTag::Class) return {};
    return cf.cp.utf8Of(cf.cp.at(classCp).a);
}

/// @brief 扫描式 Utf8 驻留：命中既有条目返回原索引，miss 才追加。
///
/// CP 只追加不去重；等值替换语义（描述符/签名/成员名改写）必须扫描式，
/// 盲加会让等值文本分裂成多个条目。入参是标准 UTF-8 规范形，内部编码为
/// modified UTF-8 再比对/落池（编码幂等，传入 cp 原始视图亦安全）。
ca::u32 findOrAddUtf8(ClassFile& cf, std::string_view text)
{
    const std::string encoded = utf8ToCp(text);
    const ConstantPool& cp    = cf.cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (cp.isUtf8(i) && cp.utf8Equals(i, encoded)) return i;
    }
    return cf.cp.addUtf8(encoded);
}

/// @brief 类名 cp 驻留：规范形入参编码后走 session.internClass。
ca::u32 internClassName(ClassSession& session, std::string_view utf8Name)
{
    return session.internClass(utf8ToCp(utf8Name));
}

// 句柄/成员引用的 CP 解析自 B8 8.4 起直用 libjbc cp_resolve 原语
//（jbc::core::resolveHandleCp / resolveMemberRef：严格口径，reference_kind
// ∉1..9 及 kind-tag 不匹配即解析失败；旧本地宽松版「非字段 kind 一律走方
// 法分支」随收编退役——对合法类零影响，JVM 解析与 libjbc 读闸本就即拒）。
// libjbc 版返回零拷贝 view，本文件的消费边界见 remapHandle / remapInvoke
// dynamic 里的落 string 纪律。

inline bool isLdcFamily(Op op) noexcept
{
    return op == Op::LDC || op == Op::LDC_W || op == Op::LDC2_W;
}

inline bool isInvokeFamily(Op op) noexcept
{
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKESPECIAL || op == Op::INVOKESTATIC ||
           op == Op::INVOKEINTERFACE || op == Op::INVOKEDYNAMIC;
}

/// @brief 字段访问指令族（GETSTATIC/PUTSTATIC/GETFIELD/PUTFIELD）。
inline bool isFieldAccessFamily(Op op) noexcept
{
    return op == Op::GETSTATIC || op == Op::PUTSTATIC || op == Op::GETFIELD ||
           op == Op::PUTFIELD;
}

/// @brief 类型操作数指令族（NEW/ANEWARRAY/CHECKCAST/INSTANCEOF）。
inline bool isTypeInstruction(Op op) noexcept
{
    return op == Op::NEW || op == Op::ANEWARRAY || op == Op::CHECKCAST ||
           op == Op::INSTANCEOF;
}

/// @brief LDC 族单字节索引超域时升格 LDC → LDC_W（宽度陷阱就地挡掉）。
void upgradeLdcWidth(Insn& in) noexcept
{
    if (in.cp > 0xFF && in.op == Op::LDC) in.op = Op::LDC_W;
}

/// @brief 按名字在属性槽表中定位（逐槽比较名字字节；CP 只追加，索引等值不可靠）。
AttrSlot* findAttrByName(const ClassFile& cf, std::vector<AttrSlot>& attrs,
                         std::string_view name)
{
    for (AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// @brief 解码属性槽并取指定形态的已解码体（可变）；失败/形态不符返回 nullptr。
template <typename AttrT>
AttrT* decodedAttrOf(ClassFile& cf, AttrSlot& slot)
{
    if (cf.decodeAttr(slot).is_err()) return nullptr;
    return ca::core::tag_cast::cast<AttrT>(slot.body.get());
}

inline bool annotationsSlotName(const ClassFile& cf, const AttrSlot& slot) noexcept
{
    return cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleAnnotations") ||
           cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleAnnotations");
}

inline bool typeAnnotationsSlotName(const ClassFile& cf, const AttrSlot& slot) noexcept
{
    return cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleTypeAnnotations") ||
           cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleTypeAnnotations");
}

inline bool parameterAnnotationsSlotName(const ClassFile& cf, const AttrSlot& slot) noexcept
{
    return cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleParameterAnnotations") ||
           cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleParameterAnnotations");
}

// ── 改写上下文（CP 常量改写 + 会话 intern 收口）────────────────────────────

struct RemapCtx
{
    ClassFile&             cf;
    ClassSession&          session;
    const MemberRemapPlan& plan;

    mutable BootstrapMethodsAttr* bsm     = nullptr;   ///< 惰性解码的 BootstrapMethods 表
    mutable AttrSlot*             bsmSlot = nullptr;

    std::string_view ruleName(ca::u32 id) const { return plan.pool().str(id); }
    std::string_view ruleDesc(ca::u32 id) const { return plan.pool().str(id); }
    std::string_view ruleOwner(ca::u32 id) const { return plan.pool().str(id); }

    /// @brief 惰性取 BootstrapMethods 表；无该属性返回 false。
    bool ensureBsm() const
    {
        if (bsm != nullptr) return true;
        AttrSlot* slot = findAttrByName(cf, cf.attrs, "BootstrapMethods");
        if (slot == nullptr) return false;
        if (cf.decodeAttr(*slot).is_err()) return false;
        auto* body = ca::core::tag_cast::cast<BootstrapMethodsAttr>(slot->body.get());
        if (body == nullptr) return false;
        bsm     = body;
        bsmSlot = slot;
        return true;
    }

    /// @brief 重映射一个方法句柄条目（owner/name/desc 就地改，引用条目新 intern）。
    ///
    /// 数组型 owner（数组 clone 等句柄可指向 `[LX;`）与直接类名同口径映射；
    /// 条目就地改对全部共享方一致（同一内容在同一计划下映射结果逐字节相同）。
    /// 解析走 libjbc cp_resolve 严格口径（reference_kind ∉1..9 / kind-tag
    /// 不匹配即失败跳过），成功时 kind 必在 1..9、(kind, isInterface) 与引用
    /// 条目 tag 严格一致。
    bool remapHandle(ca::u32 handleIdx) const
    {
        HandleSem sem;
        if (!jbc::core::resolveHandleCp(cf.cp, handleIdx, sem)) return false;
        // 三段先落规范形 string（plan 键是标准 UTF-8，cp view 是 modified
        // UTF-8，直接比较/查表对增补平面名必 miss——见文件头「cp 字符串边
        // 界」）；同时满足 view 生命周期纪律：下方 internRef 会向池追加，
        // cp 视图不得跨扩池使用（remapCondy 同款教训）。
        const std::string oldOwner = cpToUtf8(sem.owner);
        const std::string oldName  = cpToUtf8(sem.name);
        const std::string oldDesc  = cpToUtf8(sem.desc);
        std::string_view  newOwner = plan.mapOwnerRef(oldOwner);
        std::string_view  newName  = oldName;
        std::string_view  newDesc  = oldDesc;
        if (sem.kind >= 1 && sem.kind <= 4) {
            if (const auto* rule = plan.findField(oldOwner, oldName, oldDesc)) {
                newOwner = ruleOwner(rule->newKey.cls);
                newName  = ruleName(rule->newKey.name);
                // 描述符空规则（映射文件形态）：命中后描述符保持现状。
                newDesc  = rule->newKey.desc != 0 ? ruleDesc(rule->newKey.desc) : oldDesc;
            }
            else {
                newDesc = plan.mapFieldDescriptor(oldDesc);
            }
        }
        else {
            if (const auto* rule = plan.findMethod(oldOwner, oldName, oldDesc)) {
                newOwner = ruleOwner(rule->newKey.cls);
                newName  = ruleName(rule->newKey.name);
                newDesc  = ruleDesc(rule->newKey.desc);
            }
            else {
                newDesc = plan.mapMethodDescriptor(oldDesc);
            }
        }
        if (newOwner == oldOwner && newName == oldName && newDesc == oldDesc) return false;

        // re-intern 保原引用条目 tag：严格口径下 tag 由 (kind, isInterface)
        // 唯一决定——1..4 必为 Fieldref，9 必为 InterfaceMethodref，5..8 按
        // isInterface 分叉。newDesc 绑定的 oldDesc 存活到函数尾，internRef
        // 内先拷贝后扩池，视图安全。
        const CpTag refTag = sem.kind <= 4 ? CpTag::Fieldref
                                           : (sem.isInterface ? CpTag::InterfaceMethodref
                                                              : CpTag::Methodref);
        const ca::u32        newRef = internRef(refTag, newOwner, newName, newDesc);
        ConstantPool::Entry& handle = cf.cp.at(handleIdx);
        handle.b                    = newRef;
        handle.dirty                = true;
        return true;
    }

    /// @brief 重映射一个 condy 条目（描述符字段描述符口径 + BSM 链改写）。
    bool remapCondy(ca::u32 condyIdx) const
    {
        const ConstantPool& cp = cf.cp;
        if (!cp.valid(condyIdx) || cp.tagOf(condyIdx) != CpTag::Dynamic) return false;
        const auto& entry = cp.at(condyIdx);
        if (!cp.valid(entry.b)) return false;
        const auto& nat = cp.at(entry.b);
        if (nat.tag != CpTag::NameAndType || !cp.isUtf8(nat.a) || !cp.isUtf8(nat.b)) return false;
        const std::string name    = cpToUtf8(cp.utf8Of(nat.a));
        const std::string oldDesc = cpToUtf8(cp.utf8Of(nat.b));
        const std::string_view newDesc = plan.mapFieldDescriptor(oldDesc);
        // BSM 索引在改写前拷出：后续链路会向常量池追加条目，entries_ 扩容使
        // entry/nat 引用失效，悬空读取会拿到垃圾序号。
        const ca::u32 bsmIndex = entry.a;

        bool changed = false;
        if (ensureBsm() && bsmIndex < bsm->entries.size()) {
            // bootstrap 方法句柄本体随类映射（entry.methodRef 是 CP 句柄索引，
            // 与 BSM 属性索引是两个编号空间）；实参表随后。
            changed = remapHandle(bsm->entries[bsmIndex].methodRef);
            changed |= remapBsmArgsOf(bsmIndex);
        }
        if (newDesc == oldDesc && !changed) return false;

        const ca::u32 newNat = cf.cp.addNameAndType(findOrAddUtf8(cf, name),
                                                    findOrAddUtf8(cf, newDesc));
        ConstantPool::Entry& mutableEntry = cf.cp.at(condyIdx);
        mutableEntry.b                    = newNat;
        mutableEntry.dirty                = true;
        return true;
    }

    /// @brief 重映射一个 BSM 条目的实参表（Class 实参重指向；其余条目级改）。
    ///
    /// 条目级改写幂等：同一实参被多条 BSM 共享时结果逐字节相同，不产生跨面误伤。
    bool remapBsmArgsOf(ca::u32 bsmIndex) const
    {
        if (!ensureBsm() || bsmIndex >= bsm->entries.size()) return false;
        auto& args        = bsm->entries[bsmIndex].args;
        bool  argsChanged = false;
        for (ca::u32& arg : args) {
            if (!cf.cp.valid(arg)) continue;
            const ConstantPool::Entry& entry = cf.cp.at(arg);
            switch (entry.tag) {
            case CpTag::Class: {
                const std::string oldName = cpToUtf8(cf.cp.utf8Of(entry.a));
                if (oldName.empty()) break;
                const std::string_view mapped = oldName.front() == '['
                                                    ? plan.mapFieldDescriptor(oldName)
                                                    : plan.mapInternalName(oldName);
                if (mapped != oldName) {
                    arg         = internClassName(session, mapped);
                    argsChanged = true;
                }
                break;
            }
            case CpTag::MethodType: {
                const std::string oldDesc     = cpToUtf8(cf.cp.utf8Of(entry.a));
                const std::string_view mapped = plan.mapMethodDescriptor(oldDesc);
                if (mapped != oldDesc) {
                    // 先 intern 再绑定条目引用：findOrAddUtf8 可能追加条目使引用失效。
                    const ca::u32 descIdx = findOrAddUtf8(cf, mapped);
                    ConstantPool::Entry& mutableEntry = cf.cp.at(arg);
                    mutableEntry.a                    = descIdx;
                    mutableEntry.dirty                = true;
                    argsChanged                       = true;
                }
                break;
            }
            case CpTag::MethodHandle: argsChanged |= remapHandle(arg); break;
            case CpTag::Dynamic: argsChanged |= remapCondy(arg); break;
            default: break;
            }
        }
        if (argsChanged && bsmSlot != nullptr) (void)bsmSlot->mutate();
        return argsChanged;
    }

private:
    ca::u32 internRef(CpTag tag, std::string_view owner, std::string_view name,
                      std::string_view desc) const
    {
        // 三段均为规范形入参；session intern 直写 cp，编码在此收口。
        const std::string cpOwner = utf8ToCp(owner);
        const std::string cpName  = utf8ToCp(name);
        const std::string cpDesc  = utf8ToCp(desc);
        switch (tag) {
        case CpTag::Fieldref: return session.internFieldRef(cpOwner, cpName, cpDesc);
        case CpTag::InterfaceMethodref:
            return session.internInterfaceMethodRef(cpOwner, cpName, cpDesc);
        default: return session.internMethodRef(cpOwner, cpName, cpDesc);
        }
    }
};

// ── 注解改写（建模形态 AnnotationsAttr；remapAnnotation 互相递归）──────────
bool remapAnnotation(const RemapCtx& ctx, AnnotationsAttr::Annotation& annotation);

/// @brief 重映射一个注解元素值；返回是否变化。
bool remapElemValue(const RemapCtx& ctx, AnnotationsAttr::ElemValue& value)
{
    bool changed = false;
    switch (value.tag) {
    case 'e': {
        const std::string oldText     = cpToUtf8(ctx.cf.cp.utf8Of(value.typeNameCp));
        const std::string_view mapped = ctx.plan.mapFieldDescriptor(oldText);
        if (mapped != oldText) {
            value.typeNameCp = findOrAddUtf8(ctx.cf, mapped);
            changed          = true;
        }
        break;
    }
    case 'c': {
        // 'c' 载荷是返回描述符形态的 Utf8（JVMS 4.7.16.1）。
        const std::string oldText     = cpToUtf8(ctx.cf.cp.utf8Of(value.cp));
        const std::string_view mapped = ctx.plan.mapFieldDescriptor(oldText);
        if (mapped != oldText) {
            value.cp = findOrAddUtf8(ctx.cf, mapped);
            changed  = true;
        }
        break;
    }
    case '@':
        if (value.nested) changed |= remapAnnotation(ctx, *value.nested);
        break;
    case '[':
        for (auto& item : value.items) changed |= remapElemValue(ctx, item);
        break;
    default: break;
    }
    return changed;
}

/// @brief 重映射一个注解（类型描述符 + 元素值递归）。
bool remapAnnotation(const RemapCtx& ctx, AnnotationsAttr::Annotation& annotation)
{
    bool changed = false;
    if (ctx.cf.cp.isUtf8(annotation.typeCp)) {
        const std::string oldText     = cpToUtf8(ctx.cf.cp.utf8Of(annotation.typeCp));
        const std::string_view mapped = ctx.plan.mapFieldDescriptor(oldText);
        if (mapped != oldText) {
            annotation.typeCp = findOrAddUtf8(ctx.cf, mapped);
            changed           = true;
        }
    }
    for (auto& kv : annotation.values) {
        changed |= remapElemValue(ctx, kv.second);
    }
    return changed;
}

/// @brief 重映射属性槽表中全部 Runtime{Visible,Invisible}Annotations。
bool remapAnnotationSlots(const RemapCtx& ctx, std::vector<AttrSlot>& attrs)
{
    bool changed = false;
    for (AttrSlot& slot : attrs) {
        if (!annotationsSlotName(ctx.cf, slot)) continue;
        auto* body = decodedAttrOf<AnnotationsAttr>(ctx.cf, slot);
        if (body == nullptr) continue;
        bool attrChanged = false;
        for (auto& annotation : body->annos) {
            attrChanged |= remapAnnotation(ctx, annotation);
        }
        if (attrChanged) {
            (void)slot.mutate();
            changed = true;
        }
    }
    return changed;
}

/// @brief 重映射一个 TypeAnnotationsAttr 体（Runtime{Visible,Invisible}-
///        TypeAnnotations，typed 全结构模型——B8 8.3 收口，此前整族 copy-through）。
///
/// 类名承载位与 libjbc jbc::ir::type_annotation_attr（design §32.7）同口径：
/// 注解类型描述符（typeIndexCp，字段描述符形态）与元素值（'c' 类字面量、
/// 'e' 枚举类型名、'@' 嵌套、'[' 数组）走计划映射面；**type_path 与
/// target_info 不承载类名**（type_path 是泛型结构定位 (kind,index) 序列、
/// supertype/throws 下标指向类级 interfaces/Exceptions 表的 CP Class 条目），
/// 随 typed 体往返原样保留，后者随类头/Exceptions 的条目级重指向自动跟随，
/// 不在此改写。解码失败的槽保守保真（fail-closed，不产半截改写）。
bool remapTypeAnnotationBody(const RemapCtx& ctx, TypeAnnotationsAttr& body)
{
    bool changed = false;
    for (auto& ta : body.annotations) {
        if (ctx.cf.cp.isUtf8(ta.typeIndexCp)) {
            const std::string oldText     = cpToUtf8(ctx.cf.cp.utf8Of(ta.typeIndexCp));
            const std::string_view mapped = ctx.plan.mapFieldDescriptor(oldText);
            if (mapped != oldText) {
                ta.typeIndexCp = findOrAddUtf8(ctx.cf, mapped);
                changed        = true;
            }
        }
        for (auto& kv : ta.values) {
            changed |= remapElemValue(ctx, kv.second);
        }
    }
    return changed;
}

/// @brief 重映射属性槽表中全部 Runtime{Visible,Invisible}TypeAnnotations。
bool remapTypeAnnotationSlots(const RemapCtx& ctx, std::vector<AttrSlot>& attrs)
{
    bool changed = false;
    for (AttrSlot& slot : attrs) {
        if (!typeAnnotationsSlotName(ctx.cf, slot)) continue;
        auto* body = decodedAttrOf<TypeAnnotationsAttr>(ctx.cf, slot);
        if (body == nullptr) continue;
        if (remapTypeAnnotationBody(ctx, *body)) {
            (void)slot.mutate();
            changed = true;
        }
    }
    return changed;
}

/// @brief 重映射方法级 Runtime{Visible,Invisible}ParameterAnnotations
///        （typed ParameterAnnotationsAttr 行模型——B8 8.3 收口，此前整族
///        copy-through）。逐参数行的注解走与成员级同一条结构化手术路径
///        （remapAnnotation）；行数与行序是方法描述符参数数的结构事实，
///        原样保留。解码失败的槽保守保真。
bool remapParameterAnnotationSlots(const RemapCtx& ctx, std::vector<AttrSlot>& attrs)
{
    bool changed = false;
    for (AttrSlot& slot : attrs) {
        if (!parameterAnnotationsSlotName(ctx.cf, slot)) continue;
        auto* body = decodedAttrOf<ParameterAnnotationsAttr>(ctx.cf, slot);
        if (body == nullptr) continue;
        bool attrChanged = false;
        for (auto& row : body->parameters) {
            for (auto& annotation : row) {
                attrChanged |= remapAnnotation(ctx, annotation);
            }
        }
        if (attrChanged) {
            (void)slot.mutate();
            changed = true;
        }
    }
    return changed;
}

// ── Signature 与元数据属性（typed 体手术）────────────────────────────────────

/// @brief Signature 属性（typed SignatureAttr，u2 utf8）：泛型签名重映射。
///
/// 签名文本经计划的结构化签名面改写（mapSignature → libjbc
/// jbc::type::signature 解析树 span 手术，B8 8.3 收口；扫描式字符串替换
/// 退役），类/字段/方法三级挂载点共用本入口（形态无关：结构化重写按签名
/// 自身语法分派）。解析失败的签名原样保真（清洗归 IllegalSignatureRemover）。
bool remapSignatureAttr(const RemapCtx& ctx, std::vector<AttrSlot>& attrs)
{
    AttrSlot* slot = findAttrByName(ctx.cf, attrs, "Signature");
    if (slot == nullptr) return false;
    auto* attr = decodedAttrOf<jbc::core::SignatureAttr>(ctx.cf, *slot);
    if (attr == nullptr) return false;
    const std::string text         = cpToUtf8(ctx.cf.cp.utf8Of(attr->signatureCp));
    const std::string_view mapped  = ctx.plan.mapSignature(text);
    if (mapped == text) return false;
    attr->signatureCp = findOrAddUtf8(ctx.cf, mapped);
    (void)slot->mutate();
    return true;
}

/// @brief Exceptions 属性（typed ExceptionsAttr，u2 count + u2 Class 表）：
///        捕获类重映射。typed 体经公开解码入口读出（B3 分派接线前后同形），
///        命中改槽、miss 原样；makeNew 置 New 走重编码。
bool remapExceptionsAttr(const RemapCtx& ctx, MemberInfo& method)
{
    AttrSlot* slot = findAttrByName(ctx.cf, method.attrs, "Exceptions");
    if (slot == nullptr) return false;
    auto d = decodeExceptionsBody(ctx.cf, *slot);
    if (d.is_err()) return false;
    auto  body = std::move(d).unwrap();
    auto& exc  = static_cast<ExceptionsAttr&>(*body);
    bool  changed = false;
    for (ca::u32& classCp : exc.exceptionCps) {
        const std::string name        = cpToUtf8(classNameOfCp(ctx.cf, classCp));
        const std::string_view mapped = ctx.plan.mapInternalName(name);
        if (mapped != name) {
            classCp = static_cast<ca::u16>(internClassName(ctx.session, mapped));
            changed = true;
        }
    }
    if (!changed) return false;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return true;
}

/// @brief EnclosingMethod 属性（typed EnclosingMethodAttr，u2 Class + u2 NAT）：
///        外围类/方法重映射。
///
/// 规则命中改方法名与描述符
///（新描述符空则按描述符映射面），无规则时描述符照描述符映射面改写；
/// method_index==0（外围为初始化器）不指名方法，NAT 槽原样保留。typed 体经
/// 公开解码入口读出。
bool remapEnclosingMethodAttr(const RemapCtx& ctx)
{
    AttrSlot* slot = findAttrByName(ctx.cf, ctx.cf.attrs, "EnclosingMethod");
    if (slot == nullptr) return false;
    auto d = decodeEnclosingMethodBody(ctx.cf, *slot);
    if (d.is_err()) return false;
    auto body = std::move(d).unwrap();
    auto& em  = static_cast<EnclosingMethodAttr&>(*body);

    const std::string oldClassName = cpToUtf8(classNameOfCp(ctx.cf, em.classCp));
    const std::string newClassName{ctx.plan.mapInternalName(oldClassName)};

    std::string oldName;
    std::string oldDesc;
    bool        hasMethod = false;
    if (em.methodNatCp != 0 && ctx.cf.cp.valid(em.methodNatCp)) {
        const auto& nat = ctx.cf.cp.at(em.methodNatCp);
        if (nat.tag == CpTag::NameAndType && ctx.cf.cp.isUtf8(nat.a) && ctx.cf.cp.isUtf8(nat.b)) {
            oldName   = cpToUtf8(ctx.cf.cp.utf8Of(nat.a));
            oldDesc   = cpToUtf8(ctx.cf.cp.utf8Of(nat.b));
            hasMethod = true;
        }
    }
    std::string_view newName = oldName;
    std::string_view newDesc = oldDesc;
    const auto*      rule =
        (!hasMethod || oldName.empty()) ? nullptr
                                        : ctx.plan.findMethod(oldClassName, oldName, oldDesc);
    if (rule != nullptr) {
        newName = ctx.ruleName(rule->newKey.name);
        newDesc = rule->newKey.desc != 0 ? ctx.ruleDesc(rule->newKey.desc) : oldDesc;
    }
    else if (hasMethod && !oldDesc.empty()) {
        newDesc = ctx.plan.mapMethodDescriptor(oldDesc);
    }
    if (newClassName == oldClassName && newName == oldName && newDesc == oldDesc) return false;

    em.classCp = static_cast<ca::u16>(internClassName(ctx.session, newClassName));
    if (hasMethod) {
        em.methodNatCp = ctx.cf.cp.addNameAndType(findOrAddUtf8(ctx.cf, newName),
                                                  findOrAddUtf8(ctx.cf, newDesc));
    }
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return true;
}

/// @brief ModuleMainClass 属性（typed ModuleMainClassAttr，单 u2 Class 槽）：
///        模块主类重映射。typed 体经公开解码入口读出（分派接线前后同形），
///        类名映射命中才 intern 新条目；makeNew 置 New 走重编码。与
///        NestHost 曾共用的单 Class 条目字节手术已随 typed 分派翻面退役。
bool remapModuleMainClassAttr(const RemapCtx& ctx)
{
    AttrSlot* slot = findAttrByName(ctx.cf, ctx.cf.attrs, "ModuleMainClass");
    if (slot == nullptr) return false;
    auto d = jbc::core::decodeModuleMainClassBody(ctx.cf, *slot);
    if (d.is_err()) return false;
    auto body = std::move(d).unwrap();
    auto& mc  = static_cast<ModuleMainClassAttr&>(*body);
    const std::string name         = cpToUtf8(classNameOfCp(ctx.cf, mc.mainClassCp));
    const std::string_view mapped  = ctx.plan.mapInternalName(name);
    if (mapped == name) return false;
    mc.mainClassCp = static_cast<ca::u16>(internClassName(ctx.session, mapped));
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return true;
}

/// @brief NestHost 属性（typed NestHostAttr，单 u2 Class 槽）：nest 宿主
///        重映射。typed 体经公开解码入口读出（分派接线前后同形），命中才动
///        （类名映射命中才 intern 新条目）；makeNew 置 New 走重编码。
bool remapNestHostAttr(const RemapCtx& ctx)
{
    AttrSlot* slot = findAttrByName(ctx.cf, ctx.cf.attrs, "NestHost");
    if (slot == nullptr) return false;
    auto d = decodeNestHostBody(ctx.cf, *slot);
    if (d.is_err()) return false;
    auto body = std::move(d).unwrap();
    auto& nh  = static_cast<NestHostAttr&>(*body);
    const std::string name         = cpToUtf8(classNameOfCp(ctx.cf, nh.hostClassCp));
    const std::string_view mapped  = ctx.plan.mapInternalName(name);
    if (mapped == name) return false;
    nh.hostClassCp = static_cast<ca::u16>(internClassName(ctx.session, mapped));
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return true;
}

/// @brief u2 Class 列表属性（typed，NestMembers / PermittedSubclasses 共形）：
///        条目逐个类名重映射，命中才 intern 新条目、miss 原样；makeNew 置
///        New 走重编码。typed 体经公开解码入口读出（分派接线前后同形）。
///        AttrT 区分体类型，cpsMember 指向各自的条目表成员。
template <typename AttrT, typename DecodeFn>
bool remapTypedClassListAttr(const RemapCtx& ctx, std::string_view attrName,
                             DecodeFn decode, std::vector<ca::u32> AttrT::* cpsMember)
{
    AttrSlot* slot = findAttrByName(ctx.cf, ctx.cf.attrs, attrName);
    if (slot == nullptr) return false;
    auto d = decode(ctx.cf, *slot);
    if (d.is_err()) return false;
    auto  body    = std::move(d).unwrap();
    auto& typed   = static_cast<AttrT&>(*body);
    bool  changed = false;
    for (ca::u32& classCp : typed.*cpsMember) {
        const std::string name        = cpToUtf8(classNameOfCp(ctx.cf, classCp));
        const std::string_view mapped = ctx.plan.mapInternalName(name);
        if (mapped != name) {
            classCp = internClassName(ctx.session, mapped);
            changed = true;
        }
    }
    if (!changed) return false;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return true;
}

/// @brief NestMembers 属性（typed NestMembersAttr）：nest 成员表逐条目重映射。
bool remapClassListAttr(const RemapCtx& ctx, std::string_view attrName)
{
    return remapTypedClassListAttr<jbc::core::NestMembersAttr>(
        ctx, attrName, jbc::core::decodeNestMembersBody, &NestMembersAttr::memberCps);
}

/// @brief PermittedSubclasses 属性（typed PermittedSubclassesAttr）：允许子类
///        表逐条目重映射（体与 NestMembers 同构，typed 面各归各）。
bool remapPermittedSubclassesAttr(const RemapCtx& ctx)
{
    return remapTypedClassListAttr<jbc::core::PermittedSubclassesAttr>(
        ctx, "PermittedSubclasses", jbc::core::decodePermittedSubclassesBody,
        &PermittedSubclassesAttr::permittedCps);
}

/// @brief 内部类简单名（最后一个 '$' 之后的尾段；无 '$' 为空）。
std::string_view innerSimpleName(std::string_view internalName) noexcept
{
    const auto dollar = internalName.rfind('$');
    if (dollar == std::string_view::npos || dollar + 1 >= internalName.size()) return {};
    return internalName.substr(dollar + 1);
}

/// @brief InnerClasses 属性（typed InnerClassesAttr）：内部/外围类名重映射
///        + 简单名随类映射同步。行三 CP 槽语义：类名映射命中才动、
///        outer==0/name==0 保 0、简单名仅在全名命中旧简单名时改写。typed 体
///        经公开解码入口读出（B3 分派接线前后同形）。
bool remapInnerClassesAttr(const RemapCtx& ctx)
{
    AttrSlot* slot = findAttrByName(ctx.cf, ctx.cf.attrs, "InnerClasses");
    if (slot == nullptr) return false;
    auto d = decodeInnerClassesBody(ctx.cf, *slot);
    if (d.is_err()) return false;
    auto body = std::move(d).unwrap();
    auto& ic  = static_cast<InnerClassesAttr&>(*body);
    bool  changed = false;
    for (InnerClassesAttr::Row& row : ic.rows) {
        const std::string innerName    = cpToUtf8(classNameOfCp(ctx.cf, row.innerClassInfoCp));
        const std::string_view mapped  = ctx.plan.mapInternalName(innerName);
        if (mapped != innerName) {
            row.innerClassInfoCp = internClassName(ctx.session, mapped);
            changed = true;
        }

        if (row.outerClassInfoCp != 0) {
            const std::string outerName   = cpToUtf8(classNameOfCp(ctx.cf, row.outerClassInfoCp));
            const std::string_view outerMap = ctx.plan.mapInternalName(outerName);
            if (outerMap != outerName) {
                row.outerClassInfoCp = internClassName(ctx.session, outerMap);
                changed = true;
            }
        }

        if (row.innerNameCp != 0 && mapped != innerName) {
            const std::string_view oldSimple = innerSimpleName(innerName);
            const std::string_view newSimple = innerSimpleName(mapped);
            if (!oldSimple.empty() && !newSimple.empty() &&
                ctx.cf.cp.utf8Equals(row.innerNameCp, utf8ToCp(oldSimple))) {
                row.innerNameCp = findOrAddUtf8(ctx.cf, newSimple);
                changed = true;
            }
        }
    }
    if (!changed) return false;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return true;
}

/// @brief AnnotationDefault 属性（typed AnnotationDefaultAttr，单 element_value）：
///        注解类型方法默认值重映射。B8 8.3 收口：Raw 字节手术（ByteReader 重拼）
///        退役，typed 体经公开解码入口读出后复用 remapElemValue 同一条结构化
///        路径（'c'/'e'/'@'/'[' 全 tag 递归）；typed 解码拒绝的形状保守保真。
bool remapAnnotationDefaultAttr(const RemapCtx& ctx, MemberInfo& method)
{
    AttrSlot* slot = findAttrByName(ctx.cf, method.attrs, "AnnotationDefault");
    if (slot == nullptr) return false;
    auto* attr = decodedAttrOf<AnnotationDefaultAttr>(ctx.cf, *slot);
    if (attr == nullptr) return false;
    if (!remapElemValue(ctx, attr->defaultValue)) return false;
    (void)slot->mutate();
    return true;
}

/// @brief Record 组件名映射：字段/访问器规则择一，双规则改名冲突时保持原名。
std::string_view recordComponentMappedName(const RemapCtx& ctx, std::string_view owner,
                                           std::string_view name, std::string_view desc)
{
    const auto* fieldRule = ctx.plan.findField(owner, name, desc);
    const std::string accessorDesc = std::string("()").append(desc);
    const auto*       methodRule   = ctx.plan.findMethod(owner, name, accessorDesc);
    if (fieldRule != nullptr && methodRule != nullptr &&
        ctx.plan.pool().str(fieldRule->newKey.name) !=
            ctx.plan.pool().str(methodRule->newKey.name)) {
        return name;
    }
    if (fieldRule != nullptr) return ctx.plan.pool().str(fieldRule->newKey.name);
    if (methodRule != nullptr) return ctx.plan.pool().str(methodRule->newKey.name);
    return name;
}

/// @brief Record 属性（typed RecordAttr，JVMS 4.7.30）：组件名/描述符 +
///        嵌套 Signature/注解按门重写。
///
/// 组件名映射走字段/访问器规则择一，描述符走字段描述符映射面；组件嵌套
/// 属性表逐槽手术——Signature 槽走 typed 签名手术、Runtime{Visible,Invisible}
/// Annotations 与 Runtime{Visible,Invisible}TypeAnnotations 槽走 typed 注解
/// 手术（与成员级同一条路径），其余嵌套属性槽不触碰走 copy-through。typed
/// 体经公开解码入口读出；组件或嵌套命中才置脏（makeNew 置 New / 嵌套槽
/// mutate），全 miss 原样。
bool remapRecordAttr(const RemapCtx& ctx, std::string_view owner, bool signatures,
                     bool annotations)
{
    AttrSlot* slot = findAttrByName(ctx.cf, ctx.cf.attrs, "Record");
    if (slot == nullptr) return false;
    auto d = decodeRecordBody(ctx.cf, *slot);
    if (d.is_err()) return false;
    auto  body    = std::move(d).unwrap();
    auto& rec     = static_cast<RecordAttr&>(*body);
    bool  changed = false;
    for (RecordAttr::Component& component : rec.components) {
        const std::string nameText = cpToUtf8(ctx.cf.cp.utf8Of(component.nameCp));
        const std::string descText = cpToUtf8(ctx.cf.cp.utf8Of(component.descriptorCp));
        const std::string_view mappedName =
            recordComponentMappedName(ctx, owner, nameText, descText);
        const std::string_view mappedDesc = ctx.plan.mapFieldDescriptor(descText);
        if (mappedName != nameText) {
            component.nameCp = findOrAddUtf8(ctx.cf, mappedName);
            changed          = true;
        }
        if (mappedDesc != descText) {
            component.descriptorCp = findOrAddUtf8(ctx.cf, mappedDesc);
            changed                = true;
        }
        if (signatures) changed |= remapSignatureAttr(ctx, component.attrs);
        if (annotations) {
            changed |= remapAnnotationSlots(ctx, component.attrs);
            changed |= remapTypeAnnotationSlots(ctx, component.attrs);
        }
    }
    if (!changed) return false;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return true;
}

/// @brief Module 属性族：ModuleMainClass + Module 的 uses/provides 段。
///
/// typed 体经公开解码入口读出；requires/exports/opens
/// 段不触碰（typed 体上原样携带）；uses/provides 的 Class
/// 槽逐条目类名重映射，命中才 intern 新条目、全 miss 原样；makeNew 置 New
/// 走重编码。
bool remapModuleAttrs(const RemapCtx& ctx)
{
    bool changed = remapModuleMainClassAttr(ctx);

    AttrSlot* slot = findAttrByName(ctx.cf, ctx.cf.attrs, "Module");
    if (slot == nullptr) return changed;
    auto d = jbc::core::decodeModuleBody(ctx.cf, *slot);
    if (d.is_err()) return changed;
    auto  body = std::move(d).unwrap();
    auto& mod  = static_cast<ModuleAttr&>(*body);
    // 单个 CP Class 槽的类名重映射（命中才 intern），供 uses/provides 共用。
    const auto remapClassSlot = [&ctx](ca::u32& classCp) {
        const std::string name        = cpToUtf8(classNameOfCp(ctx.cf, classCp));
        const std::string_view mapped = ctx.plan.mapInternalName(name);
        if (mapped != name) {
            classCp = internClassName(ctx.session, mapped);
            return true;
        }
        return false;
    };
    bool moduleChanged = false;
    for (ca::u32& useCp : mod.usesCps) {
        moduleChanged |= remapClassSlot(useCp);
    }
    for (ModuleAttr::Provide& p : mod.provides) {
        moduleChanged |= remapClassSlot(p.serviceCp);
        for (ca::u32& withCp : p.withCps) {
            moduleChanged |= remapClassSlot(withCp);
        }
    }
    if (!moduleChanged) return changed;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return true;
}

// ── 指令改写 ────────────────────────────────────────────────────────────────

/// @brief 字段访问指令：精确规则优先，未命中 owner/描述符随映射面改写。
bool remapFieldInsn(const RemapCtx& ctx, Insn& in)
{
    ClassSession::MemberSem sem;
    if (!ctx.session.memberOf(in, sem)) return false;
    const std::string owner = cpToUtf8(sem.owner);
    const std::string name  = cpToUtf8(sem.name);
    const std::string desc  = cpToUtf8(sem.desc);
    std::string_view newOwner;
    std::string_view newName;
    std::string_view newDesc;
    if (const auto* rule = ctx.plan.findField(owner, name, desc)) {
        newOwner = ctx.ruleOwner(rule->newKey.cls);
        newName  = ctx.ruleName(rule->newKey.name);
        // 描述符空规则（映射文件形态）：命中后描述符保持现状。
        newDesc  = rule->newKey.desc != 0 ? ctx.ruleDesc(rule->newKey.desc) : desc;
    }
    else {
        newOwner = ctx.plan.mapOwnerRef(owner);
        newName  = name;
        newDesc  = ctx.plan.mapFieldDescriptor(desc);
    }
    if (newOwner == owner && newName == name && newDesc == desc) return false;
    return ctx.session.setMember(in, ClassSession::MemberSem::Kind::Field, utf8ToCp(newOwner),
                                 utf8ToCp(newName), utf8ToCp(newDesc));
}

/// @brief invoke 指令（非 indy）：精确规则优先，未命中 owner/描述符随映射面改写。
bool remapMethodInsn(const RemapCtx& ctx, Insn& in)
{
    ClassSession::MemberSem sem;
    if (!ctx.session.memberOf(in, sem)) return false;
    const std::string owner = cpToUtf8(sem.owner);
    const std::string name  = cpToUtf8(sem.name);
    const std::string desc  = cpToUtf8(sem.desc);
    std::string_view newOwner;
    std::string_view newName;
    std::string_view newDesc;
    if (const auto* rule = ctx.plan.findMethod(owner, name, desc)) {
        newOwner = ctx.ruleOwner(rule->newKey.cls);
        newName  = ctx.ruleName(rule->newKey.name);
        newDesc  = ctx.ruleDesc(rule->newKey.desc);
    }
    else {
        newOwner = ctx.plan.mapOwnerRef(owner);
        newName  = name;
        newDesc  = ctx.plan.mapMethodDescriptor(desc);
    }
    if (newOwner == owner && newName == name && newDesc == desc) return false;
    const auto kind = in.op == Op::INVOKEINTERFACE
                          ? ClassSession::MemberSem::Kind::InterfaceMethod
                          : ClassSession::MemberSem::Kind::Method;
    return ctx.session.setMember(in, kind, utf8ToCp(newOwner), utf8ToCp(newName),
                                 utf8ToCp(newDesc));
}

/// @brief invokedynamic：描述符/合成方法名 + BSM 链。
bool remapInvokedynamic(const RemapCtx& ctx, Insn& in)
{
    const ConstantPool& cp = ctx.cf.cp;
    if (!cp.valid(in.cp) || cp.tagOf(in.cp) != CpTag::InvokeDynamic) return false;
    const auto& entry = cp.at(in.cp);
    if (!cp.valid(entry.b)) return false;
    const auto& nat = cp.at(entry.b);
    if (nat.tag != CpTag::NameAndType || !cp.isUtf8(nat.a) || !cp.isUtf8(nat.b)) return false;
    const std::string oldName = cpToUtf8(cp.utf8Of(nat.a));
    const std::string oldDesc = cpToUtf8(cp.utf8Of(nat.b));
    std::string_view  newName = oldName;
    std::string_view  newDesc = ctx.plan.mapMethodDescriptor(oldDesc);
    // BSM 索引在改写前拷出：后续链路会向常量池追加条目，entries_ 扩容使
    // entry/nat 引用失效，悬空读取会拿到垃圾序号。
    const ca::u32 bsmIndex = entry.a;

    bool changed = false;
    if (ctx.ensureBsm() && bsmIndex < ctx.bsm->entries.size()) {
        auto& bsmEntry = ctx.bsm->entries[bsmIndex];
        // LambdaMetafactory 合成方法按 SAM 规则改名：BSM 方法句柄为 metafactory/
        // altMetafactory 且调用点描述符返回类型为对象类型时，
        // findMethod(返回类型, indy 名, SAM 描述符) 命中才改。SAM 描述符取自
        // 改写前的 BSM 首实参（先判名后改链）。识别用名字先落 string：libjbc
        // resolveHandleCp 返回 cp 视图，下方 remapHandle/remapBsmArgsOf 链路
        // 会向池追加，视图不得跨扩池使用。
        HandleSem   bsmHandle;
        std::string bsmOwner;
        std::string bsmName;
        if (jbc::core::resolveHandleCp(cp, bsmEntry.methodRef, bsmHandle)) {
            bsmOwner = cpToUtf8(bsmHandle.owner);
            bsmName  = cpToUtf8(bsmHandle.name);
        }
        if (bsmOwner == "java/lang/invoke/LambdaMetafactory" &&
            (bsmName == "metafactory" || bsmName == "altMetafactory") &&
            !bsmEntry.args.empty()) {
            const ca::u32 samIdx = bsmEntry.args.front();
            if (cp.valid(samIdx) && cp.tagOf(samIdx) == CpTag::MethodType &&
                cp.isUtf8(cp.at(samIdx).a)) {
                const std::string samDesc = cpToUtf8(cp.utf8Of(cp.at(samIdx).a));
                const ca::usize        ret     = oldDesc.rfind(')');
                if (ret != std::string_view::npos) {
                    // substr 产物必须持有：string_view 绑临时 string 会悬空。
                    const std::string retDesc = oldDesc.substr(ret + 1);
                    if (retDesc.size() >= 3 && retDesc.front() == 'L' && retDesc.back() == ';') {
                        const std::string retOwner = retDesc.substr(1, retDesc.size() - 2);
                        if (const auto* rule = ctx.plan.findMethod(retOwner, oldName, samDesc)) {
                            newName = ctx.ruleName(rule->newKey.name);
                        }
                    }
                }
            }
        }
        // BSM 链改写：bootstrap 方法句柄本体随类映射（entry.methodRef 是 CP 句柄
        // 索引，与 BSM 属性索引是两个编号空间），实参表随后。
        changed = ctx.remapHandle(bsmEntry.methodRef);
        changed |= ctx.remapBsmArgsOf(bsmIndex);
    }

    if (newName == oldName && newDesc == oldDesc && !changed) return false;
    const ca::u32 newNat = ctx.cf.cp.addNameAndType(findOrAddUtf8(ctx.cf, newName),
                                                    findOrAddUtf8(ctx.cf, newDesc));
    ConstantPool::Entry& mutableEntry = ctx.cf.cp.at(in.cp);
    mutableEntry.b                    = newNat;
    mutableEntry.dirty                = true;
    return true;
}

/// @brief 单条指令的字节码面改写（policy.bytecode 门控由调用方保证）。
bool remapInsn(const RemapCtx& ctx, Insn& in)
{
    const ConstantPool& cp = ctx.cf.cp;
    if (isFieldAccessFamily(in.op)) return remapFieldInsn(ctx, in);
    if (in.op == Op::INVOKEDYNAMIC) return remapInvokedynamic(ctx, in);
    if (isInvokeFamily(in.op)) return remapMethodInsn(ctx, in);
    if (isTypeInstruction(in.op)) {
        const std::string oldName = cpToUtf8(ctx.session.classOf(in));
        if (oldName.empty()) return false;
        // 数组形态 owner（anewarray 数组类）按字段描述符口径整体重映射。
        const std::string_view mapped = oldName.front() == '['
                                            ? ctx.plan.mapFieldDescriptor(oldName)
                                            : ctx.plan.mapInternalName(oldName);
        if (mapped == oldName) return false;
        return ctx.session.setClass(in, utf8ToCp(mapped));
    }
    if (in.op == Op::MULTIANEWARRAY) {
        if (!cp.valid(in.cp) || cp.tagOf(in.cp) != CpTag::Class || !cp.isUtf8(cp.at(in.cp).a)) {
            return false;
        }
        const std::string oldDesc      = cpToUtf8(cp.utf8Of(cp.at(in.cp).a));
        const std::string_view mapped  = ctx.plan.mapFieldDescriptor(oldDesc);
        if (mapped == oldDesc) return false;
        in.cp = internClassName(ctx.session, mapped);
        return true;
    }
    if (isLdcFamily(in.op)) {
        ClassSession::ConstSem c;
        if (!ctx.session.constOf(in, c)) return false;
        switch (c.kind) {
        case ClassSession::ConstSem::Kind::Class: {
            const std::string oldName     = cpToUtf8(c.str);
            const std::string_view mapped = !oldName.empty() && oldName.front() == '['
                                                ? ctx.plan.mapFieldDescriptor(oldName)
                                                : ctx.plan.mapInternalName(oldName);
            if (mapped == oldName) return false;
            in.cp = internClassName(ctx.session, mapped);
            upgradeLdcWidth(in);
            return true;
        }
        case ClassSession::ConstSem::Kind::MethodType: {
            const std::string_view mapped = ctx.plan.mapMethodDescriptor(c.str);
            if (mapped == c.str) return false;
            // 先 intern 再绑定条目引用：findOrAddUtf8 可能追加条目使引用失效。
            const ca::u32 descIdx = findOrAddUtf8(ctx.cf, mapped);
            ConstantPool::Entry& entry = ctx.cf.cp.at(in.cp);
            entry.a                    = descIdx;
            entry.dirty                = true;
            return true;
        }
        case ClassSession::ConstSem::Kind::MethodHandle: return ctx.remapHandle(in.cp);
        case ClassSession::ConstSem::Kind::Dynamic: return ctx.remapCondy(in.cp);
        default: return false;   // 字符串/数值常量不经字节码面（反射面另管）
        }
    }
    return false;
}

}   // namespace

// ── 反射类名形态（导出面：xref 元数据事实与 remap 改写候选共用同一语法）──────

/// @brief 反射类名形态判定。
bool looksLikeClassName(std::string_view value) noexcept
{
    if (value.empty()) return false;
    if (value.find('/') == std::string_view::npos &&
        value.find('.') == std::string_view::npos) {
        return false;
    }
    if (value.find(' ') != std::string_view::npos ||
        value.find('(') != std::string_view::npos ||
        value.find(')') != std::string_view::npos) {
        return false;
    }
    return true;
}

/// @brief 反射类名 → 内部名：剥 L..; 外壳、点分换斜杠。
std::string toInternalReflectionName(std::string_view value)
{
    std::string internal(value);
    if (internal.size() >= 2 && internal.front() == 'L' && internal.back() == ';') {
        internal = internal.substr(1, internal.size() - 2);
    }
    for (auto& c : internal) {
        if (c == '.') c = '/';
    }
    return internal;
}

namespace
{

/// @brief 按原始串形态恢复改写结果（点分/描述符外壳）。
std::string restoreReflectionNameShape(std::string_view original, std::string_view mapped)
{
    std::string value(mapped);
    if (original.find('.') != std::string_view::npos &&
        original.find('/') == std::string_view::npos) {
        for (auto& c : value) {
            if (c == '/') c = '.';
        }
    }
    if (original.size() >= 2 && original.front() == 'L' && original.back() == ';') {
        return "L" + value + ";";
    }
    return value;
}

/// @brief 反射类名重映射：不合法形态或未命中映射原样返回。
std::string remapReflectionClassName(const MemberRemapPlan& plan, std::string_view value)
{
    if (!looksLikeClassName(value)) return std::string(value);
    const std::string      internal = toInternalReflectionName(value);
    const std::string_view mapped   = plan.mapInternalName(internal);
    if (mapped == internal) return std::string(value);
    return restoreReflectionNameShape(value, mapped);
}

/// @brief 反射查找调用形态（isReflectionMethodLookup/isReflectionFieldLookup）。
bool isReflectionMethodLookup(const ClassSession::MemberSem& sem) noexcept
{
    return sem.owner == "java/lang/Class" &&
           (sem.name == "getDeclaredMethod" || sem.name == "getMethod") &&
           sem.desc == "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;";
}

bool isReflectionFieldLookup(const ClassSession::MemberSem& sem) noexcept
{
    return sem.owner == "java/lang/Class" &&
           (sem.name == "getDeclaredField" || sem.name == "getField") &&
           sem.desc == "(Ljava/lang/String;)Ljava/lang/reflect/Field;";
}

/// @brief 改写一条 LDC 字符串常量；指令句柄失效/改写失败返回 false。
bool setLdcString(ClassSession& session, CodeUnit& code, InsnId id, std::string_view text)
{
    auto got = code.dom.get(id);
    if (got.is_err()) return false;
    return session.setString(*got.unwrap(), utf8ToCp(text));
}

/// @brief 反射字符串改写：线性状态机轨道 + alias 数据流轨道。
///
/// 线性轨道：类字面量 LDC / Class.forName 实参提供 owner，其后最近字符串
/// LDC 是成员名；查找调用命中即改写。字面量对在任意无关方法调用处失效
///（跨调用传播属 AliasAnalysis，不在状态机里猜）。
/// 数据流轨道：域引擎快照给接收者，alias "class:X" 身份覆盖；名字实参取
/// 直线回溯最近的字符串 LDC（CodeDom 无栈帧视图，最近串即实参产点近似）。
bool remapReflectionNames(const RemapCtx& ctx, CodeUnit& code, std::string_view className,
                          std::string_view methodName, std::string_view methodDesc,
                          bool isStatic, const asm_analyzer::AliasAnalysis* aliases)
{
    bool changed = false;

    // —— 线性状态机轨道 ——
    std::string reflectionOwner;
    std::string lastString;
    InsnId      lastStringId{};
    bool        hasOwner       = false;
    bool        hasLastString  = false;
    const auto& order          = code.dom.order();
    const auto  reset = [&] {
        hasOwner      = false;
        hasLastString = false;
        lastStringId  = InsnId{};
        reflectionOwner.clear();
        lastString.clear();
    };

    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const Insn* in = code.dom.peek(order[pos]);
        if (in == nullptr) continue;
        if (isLdcFamily(in->op)) {
            ClassSession::ConstSem c;
            if (ctx.session.constOf(*in, c)) {
                if (c.kind == ClassSession::ConstSem::Kind::String) {
                    hasLastString = true;
                    lastString.assign(cpToUtf8(c.str));
                    lastStringId = order[pos];
                }
                else if (c.kind == ClassSession::ConstSem::Kind::Class) {
                    const std::string decoded          = cpToUtf8(c.str);
                    const std::string_view tracking    = reference::classLiteralTrackingName(decoded);
                    if (!tracking.empty() && (!hasOwner || !hasLastString)) {
                        reflectionOwner.assign(tracking);
                        hasOwner      = true;
                        hasLastString = false;
                        lastStringId  = InsnId{};
                        lastString.clear();
                    }
                }
            }
            continue;
        }
        if (!isInvokeFamily(in->op)) continue;
        ClassSession::MemberSem sem;
        if (!ctx.session.memberOf(*in, sem)) continue;

        if (sem.owner == "java/lang/Class" && sem.name == "forName" &&
            sem.desc == "(Ljava/lang/String;)Ljava/lang/Class;") {
            if (hasLastString && looksLikeClassName(lastString)) {
                reflectionOwner = toInternalReflectionName(lastString);
                hasOwner        = true;
                const std::string mapped = remapReflectionClassName(ctx.plan, lastString);
                if (mapped != lastString) {
                    if (setLdcString(ctx.session, code, lastStringId, mapped)) changed = true;
                }
            }
            // forName 消费的是名字串：owner 保持给紧随的成员查找用。
            hasLastString = false;
            lastStringId  = InsnId{};
            lastString.clear();
            continue;
        }

        if (hasOwner && hasLastString && isReflectionMethodLookup(sem)) {
            if (const auto* rule = ctx.plan.findMethodByName(reflectionOwner, lastString)) {
                const std::string_view newName = ctx.plan.pool().str(rule->newKey.name);
                if (newName != lastString &&
                    setLdcString(ctx.session, code, lastStringId, newName)) {
                    changed = true;
                }
            }
            reset();
            continue;
        }
        if (hasOwner && hasLastString && isReflectionFieldLookup(sem)) {
            if (const auto* rule = ctx.plan.findFieldByName(reflectionOwner, lastString)) {
                const std::string_view newName = ctx.plan.pool().str(rule->newKey.name);
                if (newName != lastString &&
                    setLdcString(ctx.session, code, lastStringId, newName)) {
                    changed = true;
                }
            }
            reset();
            continue;
        }
        reset();
    }

    // —— 数据流轨道（alias 快照；fail-open：引擎拒斥时保留线性轨道成果）——
    if (aliases == nullptr) return changed;
    reference::ReflectionQueryContext query;
    query.code     = &code;
    query.cp       = &ctx.cf.cp;
    query.owner    = className;
    query.name     = methodName;
    query.desc     = methodDesc;
    query.isStatic = isStatic;
    query.aliases  = aliases;
    const reference::ReflectionSnapshot snapshot = reference::snapshotReflectionFacts(query);
    if (!snapshot.ok) return changed;

    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const Insn* in = code.dom.peek(order[pos]);
        if (in == nullptr) continue;
        ClassSession::MemberSem sem;
        if (!ctx.session.memberOf(*in, sem)) continue;
        const bool methodLookup = isReflectionMethodLookup(sem);
        const bool fieldLookup  = isReflectionFieldLookup(sem);
        if (!methodLookup && !fieldLookup) continue;
        const auto it = snapshot.receivers.find(static_cast<ca::u32>(pos));
        if (it == snapshot.receivers.end()) continue;
        reference::ReflectionValue receiver = it->second;
        if (const auto* alias = aliases->reflectionReceiverOf(
                className, methodName, methodDesc, static_cast<ca::i32>(pos))) {
            if (alias->isKnown() && alias->identity.rfind("class:", 0) == 0) {
                receiver.kind  = reference::ReflectionValue::Kind::Class;
                receiver.owner = alias->identity.substr(6);
            }
        }
        if (receiver.kind != reference::ReflectionValue::Kind::Class) continue;

        for (ca::usize cursor = pos; cursor-- > 0;) {
            const Insn* cand = code.dom.peek(order[cursor]);
            if (cand == nullptr || !isLdcFamily(cand->op)) continue;
            ClassSession::ConstSem c;
            if (!ctx.session.constOf(*cand, c) ||
                c.kind != ClassSession::ConstSem::Kind::String) {
                continue;
            }
            const std::string text = cpToUtf8(c.str);
            std::string_view mapped;
            if (methodLookup) {
                if (const auto* rule = ctx.plan.findMethodByName(receiver.owner, text)) {
                    mapped = ctx.plan.pool().str(rule->newKey.name);
                }
            }
            else {
                if (const auto* rule = ctx.plan.findFieldByName(receiver.owner, text)) {
                    mapped = ctx.plan.pool().str(rule->newKey.name);
                }
            }
            if (!mapped.empty() && mapped != text) {
                if (setLdcString(ctx.session, code, order[cursor], mapped)) changed = true;
            }
            break;
        }
    }
    return changed;
}

// ── 方法体与嵌套代码元数据 ──────────────────────────────────────────────────

/// @brief Code 嵌套元数据：StackMapTable Object 校验类型 + LVT/LVTT 描述符 +
///        Runtime{Visible,Invisible}TypeAnnotations（B8 8.3 收口，此前嵌套
///        TypeAnnotation 槽 copy-through）。
///
/// LVTT 行的 descriptor 列承载字段形态签名，走计划的结构化签名面
/// （mapSignature）；LVT 行走描述符映射面。lower 布局未变走 copy-through 会
/// 把嵌套直改吞掉——有改即显式把 Code 槽置脏，强制 writeClass 重编码
///（嵌套槽状态随行）。
bool remapCodeMetadata(const RemapCtx& ctx, MemberInfo& method)
{
    AttrSlot* codeSlot = findAttrByName(ctx.cf, method.attrs, "Code");
    if (codeSlot == nullptr) return false;
    auto* code = decodedAttrOf<CodeAttr>(ctx.cf, *codeSlot);
    if (code == nullptr) return false;
    bool changed = false;
    for (AttrSlot& nested : code->attrs) {
        if (ctx.cf.decodeAttr(nested).is_err()) continue;   // 解不开的嵌套属性原样保真
        const AttrBody* body = nested.peek();
        if (body == nullptr) continue;
        if (body->kind() == AttrKind::LocalVariableTable) {
            const auto* lvt = ca::core::tag_cast::cast<const LocalVariableTableAttr>(body);
            if (lvt == nullptr) continue;
            const bool isLvtt = ctx.cf.cp.utf8Equals(nested.nameCp, "LocalVariableTypeTable");
            bool rowsChanged = false;
            for (const auto& row : lvt->rows) {
                const std::string text         = cpToUtf8(ctx.cf.cp.utf8Of(row.descCp));
                const std::string_view mapped  = isLvtt ? ctx.plan.mapSignature(text)
                                                        : ctx.plan.mapFieldDescriptor(text);
                if (mapped != text) rowsChanged = true;
            }
            if (rowsChanged) {
                auto* mutableLvt = ca::core::tag_cast::cast<LocalVariableTableAttr>(nested.mutate());
                if (mutableLvt == nullptr) continue;
                for (auto& row : mutableLvt->rows) {
                    const std::string text        = cpToUtf8(ctx.cf.cp.utf8Of(row.descCp));
                    const std::string_view mapped = isLvtt ? ctx.plan.mapSignature(text)
                                                           : ctx.plan.mapFieldDescriptor(text);
                    if (mapped != text) row.descCp = findOrAddUtf8(ctx.cf, mapped);
                }
                changed = true;
            }
        }
        else if (typeAnnotationsSlotName(ctx.cf, nested)) {
            auto* ta = decodedAttrOf<TypeAnnotationsAttr>(ctx.cf, nested);
            if (ta != nullptr && remapTypeAnnotationBody(ctx, *ta)) {
                (void)nested.mutate();
                changed = true;
            }
        }
        else if (body->kind() == AttrKind::StackMapTable) {
            const auto* smt = ca::core::tag_cast::cast<const StackMapTableAttr>(body);
            if (smt == nullptr) continue;
            bool framesChanged = false;
            for (const auto& frame : smt->frames) {
                for (const auto* group : {&frame.locals, &frame.stack}) {
                    for (const auto& vtype : *group) {
                        if (vtype.tag != 7) continue;   // Object 校验类型
                        const std::string name = cpToUtf8(classNameOfCp(ctx.cf, vtype.cp));
                        if (name.empty()) continue;
                        const std::string_view mapped = name.front() == '['
                                                            ? ctx.plan.mapFieldDescriptor(name)
                                                            : ctx.plan.mapInternalName(name);
                        if (mapped != name) framesChanged = true;
                    }
                }
            }
            if (framesChanged) {
                auto* mutableSmt = ca::core::tag_cast::cast<StackMapTableAttr>(nested.mutate());
                if (mutableSmt == nullptr) continue;
                for (auto& frame : mutableSmt->frames) {
                    for (auto* group : {&frame.locals, &frame.stack}) {
                        for (auto& vtype : *group) {
                            if (vtype.tag != 7) continue;
                            const std::string name = cpToUtf8(classNameOfCp(ctx.cf, vtype.cp));
                            if (name.empty()) continue;
                            const std::string_view mapped = name.front() == '['
                                                                ? ctx.plan.mapFieldDescriptor(name)
                                                                : ctx.plan.mapInternalName(name);
                            if (mapped != name) {
                                vtype.cp = internClassName(ctx.session, mapped);
                            }
                        }
                    }
                }
                changed = true;
            }
        }
    }
    if (changed) (void)codeSlot->mutate();
    return changed;
}

/// @brief 一个方法体的完整改写：指令 + 异常表 + 反射串 + lower + 嵌套元数据。
Result<bool, RemapError> remapMethodBody(const RemapCtx& ctx, ca::usize methodIdx,
                                         std::string_view className,
                                         std::string_view methodName,
                                         std::string_view methodDesc, bool isStatic,
                                         const RemapPolicy&             policy,
                                         const asm_analyzer::AliasAnalysis* aliases)
{
    const bool needBytecode = policy.bytecode;
    const bool needReflect  = policy.reflectionStrings;
    if (!needBytecode && !needReflect) return Ok(false);
    if (!jbc::ir::hasCode(ctx.session, methodIdx)) return Ok(false);
    auto promoted = jbc::ir::promoteMethod(ctx.session, methodIdx);
    if (promoted.is_err()) return Err(RemapError::PromoteFailed);
    jbc::ir::CodeUnitRef unit = std::move(promoted).unwrap();

    bool changed = false;
    // 反射字符串道先于字节码道：类字面量 LDC 的
    // owner 要以映射前的旧名参与 findMethodByName/findFieldByName 查表，
    // 字节码道改过的类名会让规则查询必然 miss。
    if (needReflect) {
        changed |= remapReflectionNames(ctx, *unit, className, methodName, methodDesc,
                                        isStatic, aliases);
    }
    if (needBytecode) {
        for (InsnId id : unit->dom.order()) {
            auto got = unit->dom.get(id);
            if (got.is_err()) continue;
            changed |= remapInsn(ctx, *got.unwrap());
        }
        for (auto& exc : unit->exc) {
            if (exc.catchCp == 0) continue;
            const std::string name        = cpToUtf8(classNameOfCp(ctx.cf, exc.catchCp));
            const std::string_view mapped = ctx.plan.mapInternalName(name);
            if (mapped != name) {
                exc.catchCp = internClassName(ctx.session, mapped);
                changed     = true;
            }
        }
    }
    auto lowered = jbc::ir::lowerMethod(ctx.session, methodIdx);
    if (lowered.is_err()) return Err(RemapError::LowerFailed);
    if (needBytecode) changed |= remapCodeMetadata(ctx, ctx.cf.methods[methodIdx]);
    return Ok(changed);
}

/// @brief 方法改名同步：lower 的 commitMethodMetadata 以 ClassUnit 符号为准
///        回写 backing name/desc——只改 backing 会被 promote/lower 往返回滚。
void syncMethodSymbols(ClassSession& session, ca::usize methodIdx, std::string_view name,
                       std::string_view desc)
{
    // 符号池字节会被 lower 原样写回 cp：入参是规范形，编码后入池。
    const std::string cpName = utf8ToCp(name);
    const std::string cpDesc = utf8ToCp(desc);
    for (auto& mu : session.unit().methods) {
        if (mu.backingIndex == methodIdx) {
            mu.name = session.unit().symbols().intern(cpName);
            mu.desc = session.unit().symbols().intern(cpDesc);
            return;
        }
    }
}

}   // namespace

// ── 公开入口 ────────────────────────────────────────────────────────────────

const char* to_string(RemapError error) noexcept
{
    switch (error) {
    case RemapError::PromoteFailed: return "promote-failed";
    case RemapError::LowerFailed: return "lower-failed";
    case RemapError::WriteFailed: return "write-failed";
    case RemapError::UnreadableClass: return "unreadable-class";
    }
    return "?";
}

RemapPolicy RemapPolicy::bytecodeOnly()
{
    return RemapPolicy{};
}

RemapPolicy RemapPolicy::bytecodeAndReflectionStrings()
{
    RemapPolicy policy;
    policy.reflectionStrings = true;
    return policy;
}

RemapPolicy RemapPolicy::bytecodeAndMetadata()
{
    RemapPolicy policy;
    policy.annotations = true;
    policy.signatures  = true;
    return policy;
}

RemapPolicy RemapPolicy::all()
{
    RemapPolicy policy;
    policy.reflectionStrings = true;
    policy.annotations       = true;
    policy.signatures        = true;
    policy.resources         = true;
    return policy;
}

Result<bool, RemapError> applyMemberRemapPlan(jbc::ir::ClassSession&      session,
                                              const MemberRemapPlan&      plan,
                                              const RemapPolicy&          policy,
                                              const asm_analyzer::AliasAnalysis* aliases)
{
    if (plan.empty()) return Ok(false);
    ClassFile&   cf = session.backing();
    const RemapCtx ctx{cf, session, plan, nullptr, nullptr};

    bool changed = false;
    const std::string oldOwner = cpToUtf8(cf.thisName());

    if (policy.bytecode) {
        const auto remapHeaderRef = [&](ca::u16& slot) {
            const std::string name        = cpToUtf8(classNameOfCp(cf, slot));
            const std::string_view mapped = plan.mapInternalName(name);
            if (mapped != name) {
                slot    = static_cast<ca::u16>(internClassName(session, mapped));
                changed = true;
            }
        };
        remapHeaderRef(cf.thisClass);
        if (cf.superClass != 0) remapHeaderRef(cf.superClass);
        for (auto& iface : cf.interfaces) remapHeaderRef(iface);

        changed |= remapEnclosingMethodAttr(ctx);
        changed |= remapNestHostAttr(ctx);
        changed |= remapClassListAttr(ctx, "NestMembers");
        changed |= remapPermittedSubclassesAttr(ctx);
        changed |= remapInnerClassesAttr(ctx);
        changed |= remapModuleAttrs(ctx);
    }
    if (policy.signatures) changed |= remapSignatureAttr(ctx, cf.attrs);
    if (policy.annotations) {
        changed |= remapAnnotationSlots(ctx, cf.attrs);
        changed |= remapTypeAnnotationSlots(ctx, cf.attrs);
    }
    if (policy.bytecode || policy.signatures || policy.annotations) {
        changed |= remapRecordAttr(ctx, oldOwner, policy.signatures, policy.annotations);
    }

    for (auto& field : cf.fields) {
        const std::string oldName = cpToUtf8(cf.nameOf(field));
        const std::string oldDesc = cpToUtf8(cf.descOf(field));
        if (policy.bytecode) {
            if (const auto* rule = plan.findField(oldOwner, oldName, oldDesc)) {
                const std::string_view newName = plan.pool().str(rule->newKey.name);
                // 描述符空规则（映射文件形态）：命中后描述符保持现状。
                const std::string_view newDesc = rule->newKey.desc != 0
                                                     ? plan.pool().str(rule->newKey.desc)
                                                     : oldDesc;
                if (newName != oldName) {
                    field.nameCp = findOrAddUtf8(cf, newName);
                    changed      = true;
                }
                if (newDesc != oldDesc) {
                    field.descCp = findOrAddUtf8(cf, newDesc);
                    changed      = true;
                }
            }
            else {
                const std::string_view newDesc = plan.mapFieldDescriptor(oldDesc);
                if (newDesc != oldDesc) {
                    field.descCp = findOrAddUtf8(cf, newDesc);
                    changed      = true;
                }
            }
        }
        if (policy.signatures) changed |= remapSignatureAttr(ctx, field.attrs);
        if (policy.annotations) {
            changed |= remapAnnotationSlots(ctx, field.attrs);
            changed |= remapTypeAnnotationSlots(ctx, field.attrs);
        }
    }

    for (ca::usize methodIdx = 0; methodIdx < cf.methods.size(); ++methodIdx) {
        MemberInfo&  method = cf.methods[methodIdx];
        const std::string oldName = cpToUtf8(cf.nameOf(method));
        const std::string oldDesc = cpToUtf8(cf.descOf(method));
        if (policy.bytecode) {
            std::string_view newName = oldName;
            std::string_view newDesc = oldDesc;
            if (const auto* rule = plan.findMethod(oldOwner, oldName, oldDesc)) {
                newName = plan.pool().str(rule->newKey.name);
                newDesc = plan.pool().str(rule->newKey.desc);
            }
            else {
                newDesc = plan.mapMethodDescriptor(oldDesc);
            }
            if (newName != oldName || newDesc != oldDesc) {
                method.nameCp = findOrAddUtf8(cf, newName);
                method.descCp = findOrAddUtf8(cf, newDesc);
                syncMethodSymbols(session, methodIdx, newName, newDesc);
                changed = true;
            }
            changed |= remapExceptionsAttr(ctx, method);
        }
        if (policy.signatures) changed |= remapSignatureAttr(ctx, method.attrs);
        if (policy.annotations) {
            changed |= remapAnnotationSlots(ctx, method.attrs);
            changed |= remapTypeAnnotationSlots(ctx, method.attrs);
            changed |= remapParameterAnnotationSlots(ctx, method.attrs);
            changed |= remapAnnotationDefaultAttr(ctx, method);
        }
        auto bodyResult =
            remapMethodBody(ctx, methodIdx, oldOwner, oldName, oldDesc,
                            (method.access & kAccStatic) != 0, policy, aliases);
        if (bodyResult.is_err()) return bodyResult;
        changed |= bodyResult.unwrap();
    }

    return Ok(changed);
}

Result<bool, RemapError> applyMemberRemapPlan(jbc::ir::ClassSession& session,
                                              const MemberRemapPlan& plan)
{
    return applyMemberRemapPlan(session, plan, RemapPolicy::bytecodeOnly(), nullptr);
}

Result<bool, RemapError> applyMemberRemapPlan(jbc::ir::ClassSession& session,
                                              const MemberRemapPlan& plan,
                                              const RemapPolicy&     policy)
{
    return applyMemberRemapPlan(session, plan, policy, nullptr);
}

}   // namespace mjbase::asm_remapper
