#include "mjbase/pass/class_member_sorter.hpp"

#include <numeric>
#include <stdexcept>
#include <vector>

#include "mjbase/spi/class_structure_editor.hpp"

namespace mjbase::pass {

mjbase::spi::PreservedAnalyses ClassMemberSorter::run(jbc::ir::ClassSession&            session,
                                         mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    jbc::ir::ClassUnit&   unit = session.unit();
    const jbc::ir::SymbolPool& syms = unit.symbols();

    // ── 字段序：desc 优先、name 次之，稳定（同键保原相对序）。──
    // 视图借用会话符号池，排序期内有效；比较按字节字典序（成员名/描述符
    // 实际为 ASCII，与旧栈序一致）。
    std::vector<ca::usize> fieldOrder(unit.fields.size());
    std::iota(fieldOrder.begin(), fieldOrder.end(), ca::usize{0});
    std::stable_sort(fieldOrder.begin(), fieldOrder.end(),
                     [&unit, &syms](ca::usize lhs, ca::usize rhs) {
                         const std::string_view ld = syms.str(unit.fields[lhs].desc);
                         const std::string_view rd = syms.str(unit.fields[rhs].desc);
                         if (ld != rd) return ld < rd;
                         return syms.str(unit.fields[lhs].name) <
                                syms.str(unit.fields[rhs].name);
                     });

    // ── 方法序：<clinit> 组 → 其余按 name 稳定排序 → <init> 组。──
    // 组内稳定保原相对序（<init> 重载组、畸形多 <clinit> 均不丢不乱）。
    std::vector<ca::usize> clinitOrder;
    std::vector<ca::usize> initOrder;
    std::vector<ca::usize> plainOrder;
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        const std::string_view name = syms.str(unit.methods[i].name);
        if (name == "<clinit>") {
            clinitOrder.push_back(i);
        }
        else if (name == "<init>") {
            initOrder.push_back(i);
        }
        else {
            plainOrder.push_back(i);
        }
    }
    std::stable_sort(plainOrder.begin(), plainOrder.end(),
                     [&unit, &syms](ca::usize lhs, ca::usize rhs) {
                         return syms.str(unit.methods[lhs].name) <
                                syms.str(unit.methods[rhs].name);
                     });
    std::vector<ca::usize> methodOrder;
    methodOrder.reserve(unit.methods.size());
    methodOrder.insert(methodOrder.end(), clinitOrder.begin(), clinitOrder.end());
    methodOrder.insert(methodOrder.end(), plainOrder.begin(), plainOrder.end());
    methodOrder.insert(methodOrder.end(), initOrder.begin(), initOrder.end());

    // ── 变更检测：两个排列均为恒等时无排序发生（对齐声明的条件 PA 语义）。──
    bool changed = false;
    for (ca::usize i = 0; i < fieldOrder.size(); ++i) {
        if (fieldOrder[i] != i) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        for (ca::usize i = 0; i < methodOrder.size(); ++i) {
            if (methodOrder[i] != i) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return mjbase::spi::PreservedAnalyses::all();

    // ── 落盘：双面落盘义务收敛到结构编辑原语（spi/class_structure_editor）。──
    // 字段/方法两面同排列重排 + MethodUnit.backingIndex 改指新物理位，都由原语
    // 承担；本 pass 只构造排列与声明 PA。成员条目整体移动：MemberInfo 连同属性
    // 槽（含 Code 的 lazy span 状态）一起走，内容不被触碰；MethodUnit 携带稳定
    // MethodId 与已 promote 的 CodeUnit（堆上地址不变，既有 CodeUnitRef 保持
    // 有效）。
    // 两个排列均为本函数构造的双射，原语报 Err 即契约破坏——按 pass 域异常
    // 语义稳定失败（对齐「配置错误稳定失败」纪律），不静默吞错。
    const auto fieldsEdit  = mjbase::spi::reorderClassFields(session, fieldOrder);
    const auto methodsEdit = mjbase::spi::reorderClassMethods(session, methodOrder);
    if (fieldsEdit.is_err() || methodsEdit.is_err()) {
        throw std::logic_error("ClassMemberSorter: structure editor rejected a self-built permutation");
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
