#include "mjbase/workspace/reference_scan.hpp"

#include <algorithm>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/analysis/cp_refs.hpp"
#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_file_sanitize.hpp"
#include "mjbase/workspace/classfile_support.hpp"

namespace mjbase::workspace {

namespace {

using jbc::core::Op;

/// @brief invoke 家族（invokestatic/special/virtual/interface）；indy 由语义面单独分流。
bool isInvokeOp(Op op)
{
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKESPECIAL || op == Op::INVOKESTATIC ||
           op == Op::INVOKEINTERFACE;
}

/// @brief 字段访问家族（getstatic/getfield/putstatic/putfield）。
bool isFieldAccessOp(Op op)
{
    return op == Op::GETSTATIC || op == Op::GETFIELD || op == Op::PUTSTATIC || op == Op::PUTFIELD;
}

/// @brief 显式类型指令（new/anewarray/checkcast/instanceof/multianewarray）。
bool isTypeOp(Op op)
{
    return op == Op::NEW || op == Op::ANEWARRAY || op == Op::CHECKCAST || op == Op::INSTANCEOF ||
           op == Op::MULTIANEWARRAY;
}

}   // namespace

void ReferenceScan::build(const model::ResourceContainer& container)
{
    methods_.clear();
    fields_.clear();
    types_.clear();
    container.visitResources([&](std::string_view uri, const model::IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (!classResource) return;
        // 净化预检与 WorkspaceBase::validateCandidate 同口径：引用语义损坏的
        // class 不进扫描（旧栈索引对畸形 class 的跳过纪律）。
        if (!spi::sanitizeClassFile(classResource->get()).empty()) return;
        auto session = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(classResource->get()));
        if (session.is_err()) return;
        auto held = std::move(session).unwrap();

        const std::string sourceClass{held->backing().thisName()};

        // class 头声明：super 与直接接口是 class 级类型引用（无源方法）。
        const auto super = held->backing().superName();
        if (!super.empty()) types_.push_back({std::string{super}, {sourceClass, {}, {}, -1}});
        for (const auto interfaceCp : held->backing().interfaces) {
            const auto name = held->backing().className(interfaceCp);
            if (!name.empty()) types_.push_back({std::string{name}, {sourceClass, {}, {}, -1}});
        }

        for (auto& visit : jbc::ir::collectCodeUnits(*held)) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
            const jbc::ir::CodeUnit& code = *visit.unit;
            const auto&            unit  = held->unit().methods[visit.methodIdx];
            const std::string      methodName{held->unit().symbols().str(unit.name)};
            const std::string      methodDesc{held->unit().symbols().str(unit.desc)};
            const ReferenceSite    site{sourceClass, methodName, methodDesc, -1};

            const auto& order = code.dom.order();
            for (ca::usize index = 0; index < order.size(); ++index) {
                const auto          insnId = order[index];
                const jbc::ir::Insn* insn  = code.dom.peek(insnId);
                if (!insn) continue;
                ReferenceSite at = site;
                at.insnIndex     = static_cast<ca::i32>(index);

                if (isInvokeOp(insn->op)) {
                    jbc::ir::ClassSession::MemberSem sem;
                    if (held->memberOf(*insn, sem) &&
                        sem.kind != jbc::ir::ClassSession::MemberSem::Kind::InvokeDynamic &&
                        !sem.owner.empty()) {
                        methods_.push_back({{std::string{sem.owner}, std::string{sem.name},
                                             std::string{sem.desc}},
                                            at,
                                            MethodReferenceKind::Direct});
                        types_.push_back({std::string{sem.owner}, at});
                    }
                    continue;
                }
                if (isFieldAccessOp(insn->op)) {
                    jbc::ir::ClassSession::MemberSem sem;
                    if (held->memberOf(*insn, sem) && !sem.owner.empty()) {
                        fields_.push_back({{std::string{sem.owner}, std::string{sem.name},
                                            std::string{sem.desc}},
                                           at,
                                           FieldReferenceKind::Direct});
                        types_.push_back({std::string{sem.owner}, at});
                    }
                    continue;
                }
                if (isTypeOp(insn->op)) {
                    if (insn->op == Op::MULTIANEWARRAY) {
                        // multianewarray 的 cp 指向数组描述符；提取元素对象类型。
                        const auto element = support::objectClassFromArrayDescriptor(
                            analysis::resolveClassName(held->backing().cp, insn->cp));
                        if (!element.empty()) types_.push_back({std::move(element), at});
                        continue;
                    }
                    const auto name = held->classOf(*insn);
                    if (!name.empty()) types_.push_back({std::string{name}, at});
                    continue;
                }
                if (insn->op == Op::LDC || insn->op == Op::LDC_W) {
                    jbc::ir::ClassSession::ConstSem constant;
                    if (!held->constOf(*insn, constant)) continue;
                    if (constant.kind == jbc::ir::ClassSession::ConstSem::Kind::Class &&
                        !constant.str.empty()) {
                        // 数组形态的 class 常量提取元素对象类型（LDC "[Lfoo/Bar;" → foo/Bar）。
                        const auto element = support::objectClassFromArrayDescriptor(constant.str);
                        types_.push_back({element.empty() ? std::string{constant.str}
                                                          : std::move(element),
                                          at});
                        continue;
                    }
                    // class-string 元数据事实：形态合法的字符串常量是反射引用候选
                    //（与 remapper 反射改写候选同一语法，remap 改的 xref 必须可见）。
                    if (constant.kind == jbc::ir::ClassSession::ConstSem::Kind::String &&
                        asm_remapper::looksLikeClassName(constant.str)) {
                        types_.push_back({asm_remapper::toInternalReflectionName(constant.str),
                                          at});
                    }
                    continue;
                }
            }
        }
    });
}

std::vector<MethodReference> ReferenceScan::methodReferencesTo(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    std::vector<MethodReference> result;
    for (const auto& reference : methods_) {
        if (reference.target.owner == owner && reference.target.name == name &&
            reference.target.descriptor == descriptor) {
            result.push_back(reference);
        }
    }
    return result;
}

std::vector<MethodReference> ReferenceScan::methodReferencesFrom(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    std::vector<MethodReference> result;
    for (const auto& reference : methods_) {
        if (reference.site.sourceClass == owner && reference.site.sourceMethod == name &&
            reference.site.sourceDescriptor == descriptor) {
            result.push_back(reference);
        }
    }
    return result;
}

std::vector<MethodReference> ReferenceScan::methodReferencesFromClass(const std::string& owner) const
{
    std::vector<MethodReference> result;
    for (const auto& reference : methods_) {
        if (reference.site.sourceClass == owner) result.push_back(reference);
    }
    return result;
}

std::vector<FieldReference> ReferenceScan::fieldReferencesTo(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    std::vector<FieldReference> result;
    for (const auto& reference : fields_) {
        if (reference.target.owner == owner && reference.target.name == name &&
            reference.target.descriptor == descriptor) {
            result.push_back(reference);
        }
    }
    return result;
}

std::vector<FieldReference> ReferenceScan::fieldReferencesFromClass(const std::string& owner) const
{
    std::vector<FieldReference> result;
    for (const auto& reference : fields_) {
        if (reference.site.sourceClass == owner) result.push_back(reference);
    }
    return result;
}

std::vector<TypeReference> ReferenceScan::typeReferencesTo(const std::string& targetType) const
{
    std::vector<TypeReference> result;
    for (const auto& reference : types_) {
        if (reference.targetType == targetType) result.push_back(reference);
    }
    return result;
}

std::vector<TypeReference> ReferenceScan::typeReferencesFromClass(const std::string& owner) const
{
    std::vector<TypeReference> result;
    for (const auto& reference : types_) {
        if (reference.site.sourceClass == owner) result.push_back(reference);
    }
    return result;
}

std::string methodReferenceKind(MethodReferenceKind)
{
    return "direct";
}

std::string fieldReferenceKind(FieldReferenceKind)
{
    return "direct";
}

ReferenceInfo referenceInfo(const ReferenceSite& site, const std::string& target, const std::string& kind)
{
    return {site.sourceClass, site.sourceMethod, site.sourceDescriptor, site.insnIndex, target, kind};
}

void sortReferences(std::vector<ReferenceInfo>& references)
{
    std::sort(references.begin(), references.end(), [](const ReferenceInfo& left,
                                                       const ReferenceInfo& right) {
        if (left.sourceClass != right.sourceClass) return left.sourceClass < right.sourceClass;
        if (left.sourceMethod != right.sourceMethod) return left.sourceMethod < right.sourceMethod;
        if (left.sourceDescriptor != right.sourceDescriptor)
            return left.sourceDescriptor < right.sourceDescriptor;
        if (left.instructionIndex != right.instructionIndex)
            return left.instructionIndex < right.instructionIndex;
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.target < right.target;
    });
}

}   // namespace mjbase::workspace
