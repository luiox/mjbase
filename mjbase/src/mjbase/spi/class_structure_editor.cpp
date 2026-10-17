#include "mjbase/spi/class_structure_editor.hpp"

#include <utility>

#include "jbc/core/class_file.hpp"

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;

const char* to_string(StructureEditError e) noexcept
{
    switch (e) {
    case StructureEditError::Ok: return "Ok";
    case StructureEditError::Unsupported: return "Unsupported";
    case StructureEditError::BadPermutation: return "BadPermutation";
    case StructureEditError::BadSeed: return "BadSeed";
    case StructureEditError::BadHandle: return "BadHandle";
    case StructureEditError::PoolFull: return "PoolFull";
    }
    return "?";
}

namespace {

/// @brief LowerError → StructureEditError 收口映射。
///
/// 增删通道的可达错误族按库契约只有三个（InvalidSeed / InvalidHandle /
/// CpAppendFailed，libjbc design §34.2）；NotModeled 是搬移（adopt）面的
/// 形态，其余是 lower 指令面的形态——都不在本原语通道上，统一落
/// Unsupported（fail-closed，出现即库契约破坏）。
StructureEditError mapLower(jbc::ir::LowerError e) noexcept
{
    switch (e) {
    case jbc::ir::LowerError::InvalidSeed: return StructureEditError::BadSeed;
    case jbc::ir::LowerError::InvalidHandle: return StructureEditError::BadHandle;
    case jbc::ir::LowerError::CpAppendFailed: return StructureEditError::PoolFull;
    default: return StructureEditError::Unsupported;
    }
}

/// @brief 下标解析出的命名哨兵拦下面：哨兵位形 {0,0,0} 可能与真实身份巧合
///        同形（owner 编号从 0 起时首会话 slot0/gen0 即同形），越界解析出
///        哨兵后不得再喂给库通道——先在本面收 Err(BadHandle)，不触碰会话。
bool isSentinel(const jbc::ir::MethodId& id) noexcept { return id == jbc::ir::kNoMethodId; }
bool isSentinel(const jbc::ir::FieldId& id) noexcept { return id == jbc::ir::kNoFieldId; }

}   // namespace

namespace {

/// @brief 校验 order 是 [0, count) 上的双射排列（长度相等 + 不越界 + 不重复）。
bool isPermutation(const std::vector<ca::usize>& order, ca::usize count)
{
    if (order.size() != count) return false;
    std::vector<bool> seen(count, false);
    for (const ca::usize oldPos : order) {
        if (oldPos >= count || seen[oldPos]) return false;
        seen[oldPos] = true;
    }
    return true;
}

/// @brief order 是否恒等排列（out[j] = v[order[j]] 语义下零移动）。
bool isIdentity(const std::vector<ca::usize>& order)
{
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (order[i] != i) return false;
    }
    return true;
}

/// @brief 按置换重排向量：out[j] = std::move(v[order[j]])，order[newPos] = oldPos。
///
/// 成员条目整体移动（连带属性槽/稳定身份/已 promote 的 CodeUnit），内容不被
/// 触碰——双面落盘语义的机械部分，原先是结构级 pass 的手搓样板。
template<class T>
void reorderBy(std::vector<T>& values, const std::vector<ca::usize>& order)
{
    std::vector<T> out;
    out.reserve(values.size());
    for (const ca::usize oldPos : order) {
        out.push_back(std::move(values[oldPos]));
    }
    values = std::move(out);
}

}   // namespace

Result<void, StructureEditError> reorderClassFields(jbc::ir::ClassSession&            session,
                                                    const std::vector<ca::usize>& order)
{
    jbc::ir::ClassUnit&   unit = session.unit();
    jbc::core::ClassFile& cf   = session.backing();
    if (!isPermutation(order, unit.fields.size())) {
        return Err(StructureEditError::BadPermutation);
    }
    if (isIdentity(order)) return Ok();   // 恒等零动：不触碰懒解码的属性槽
    // 双面落盘：骨架与 backing 同排列整体移动（backing 是 writeClass 的序列化源，
    // 只排骨架不落字节）；字段无 backingIndex，两面同序即一致。
    reorderBy(cf.fields, order);
    reorderBy(unit.fields, order);
    return Ok();
}

Result<void, StructureEditError> reorderClassMethods(jbc::ir::ClassSession&            session,
                                                     const std::vector<ca::usize>& order)
{
    jbc::ir::ClassUnit&   unit = session.unit();
    jbc::core::ClassFile& cf   = session.backing();
    if (!isPermutation(order, unit.methods.size())) {
        return Err(StructureEditError::BadPermutation);
    }
    if (isIdentity(order)) return Ok();
    reorderBy(cf.methods, order);
    reorderBy(unit.methods, order);
    // §1 backingIndex 义务：重排后逻辑位与 backing 槽位重新自然一致，全量改指
    // 新物理位——单元位置与 backing 槽位在结构变换后不再自然一致，靠这里修复。
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        unit.methods[i].backingIndex = i;
    }
    return Ok();
}

Result<jbc::ir::FieldId, StructureEditError> addClassField(jbc::ir::ClassSession& session,
                                                           std::string_view name,
                                                           std::string_view desc, ca::u32 access)
{
    const jbc::ir::FieldSeed seed{name, desc, access};
    auto                     added = session.addField(seed);
    if (added.is_err()) return Err(mapLower(added.unwrap_err()));
    return Ok(added.unwrap());
}

Result<jbc::ir::MethodId, StructureEditError> addClassMethod(jbc::ir::ClassSession& session,
                                                             std::string_view name,
                                                             std::string_view desc, ca::u32 access)
{
    const jbc::ir::MethodSeed seed{name, desc, access, /*withCode*/ false};
    auto                      added = session.addMethod(seed);
    if (added.is_err()) return Err(mapLower(added.unwrap_err()));
    return Ok(added.unwrap());
}

Result<void, StructureEditError> removeClassField(jbc::ir::ClassSession& session, ca::usize index)
{
    // 下标 → 稳定身份 → 库删除通道；越界在本面拦下（哨兵不进库通道），
    // 身份已失配由库侧 InvalidHandle 统一收口。
    const jbc::ir::FieldId id = session.unit().fieldId(index);
    if (isSentinel(id)) return Err(StructureEditError::BadHandle);
    auto removed = session.removeField(id);
    if (removed.is_err()) return Err(mapLower(removed.unwrap_err()));
    return Ok();
}

Result<void, StructureEditError> removeClassMethod(jbc::ir::ClassSession& session,
                                                   ca::usize index)
{
    const jbc::ir::MethodId id = session.unit().methodId(index);
    if (isSentinel(id)) return Err(StructureEditError::BadHandle);
    auto removed = session.removeMethod(id);
    if (removed.is_err()) return Err(mapLower(removed.unwrap_err()));
    return Ok();
}

}   // namespace mjbase::spi
