#include "mjbase/pass/static_field_default_inliner.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {
namespace {

constexpr ca::u16 kAccStatic = 0x0008;

/// 字段身份键：name+desc（PUTSTATIC 侧已校验 owner==本类，同旧栈
/// className+name+desc 键等价——本类 clinit 内本类字段的三个分量全确定）。
std::string keyOf(std::string_view name, std::string_view desc)
{
    std::string key;
    key.reserve(name.size() + desc.size());
    key.append(name).append(desc);
    return key;
}

/// 字段已带 ConstantValue 属性（Raw 槽名判断，与写入面同口径）。
bool hasConstantValue(const jbc::core::ClassFile& cf, const jbc::core::MemberInfo& f)
{
    for (const jbc::core::AttrSlot& slot : f.attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, "ConstantValue")) return true;
    }
    return false;
}

/// 识别到的常量值：cat2（long/double）与 cat1（int/float）分槽，
/// 位图按 JVMS 原始位存（Float/Double 保 NaN payload 与 -0.0）。
struct LoadedConst
{
    ca::i64 num  = 0;   ///< Int / Long 值
    ca::u64 bits = 0;   ///< Float / Double 原始位（Float 零扩展于低 32 位）
};

/// 读一条常量装载指令的值：cat2 ← LCONST/DCONST/LDC2_W(Long/Double)；
/// cat1 ← ICONST 族/BIPUSH/SIPUSH/LDC(LDC_W)(Int/Float)。不匹配返回 false。
/// 与 strength_reducer 的 materializeIntConst/LongConst 互为镜像。
bool constValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                  bool wantCat2, LoadedConst& out)
{
    using jbc::core::Op;
    if (wantCat2) {
        switch (in.op) {
        case Op::LCONST_0: out = {0, 0}; return true;
        case Op::LCONST_1: out = {1, 0}; return true;
        case Op::DCONST_0: out = {0, 0}; return true;
        case Op::DCONST_1: out = {0, 0x3FF0000000000000ULL}; return true;
        default: break;
        }
    } else {
        switch (in.op) {
        case Op::ICONST_M1: out = {-1, 0}; return true;
        case Op::ICONST_0: out = {0, 0}; return true;
        case Op::ICONST_1: out = {1, 0}; return true;
        case Op::ICONST_2: out = {2, 0}; return true;
        case Op::ICONST_3: out = {3, 0}; return true;
        case Op::ICONST_4: out = {4, 0}; return true;
        case Op::ICONST_5: out = {5, 0}; return true;
        case Op::FCONST_0: out = {0, 0}; return true;
        case Op::FCONST_1: out = {0, 0x3F800000ULL}; return true;
        case Op::FCONST_2: out = {0, 0x40000000ULL}; return true;
        case Op::BIPUSH:
        case Op::SIPUSH: out = {in.a, 0}; return true;
        default: break;
        }
    }
    // ldc 族（含 LDC2_W）经 constOf 语义视图分派；kind 与期望类别不符
    // （如 J 字段配 String 装载）按不匹配处理。
    jbc::ir::ClassSession::ConstSem c;
    if (!session.constOf(in, c)) return false;
    using Kind = jbc::ir::ClassSession::ConstSem::Kind;
    if (wantCat2) {
        if (c.kind == Kind::Long) {
            out = {c.num, 0};
            return true;
        }
        if (c.kind == Kind::Double) {
            out = {0, c.bits};
            return true;
        }
    } else if (c.kind == Kind::Int) {
        out = {c.num, 0};
        return true;
    } else if (c.kind == Kind::Float) {
        out = {0, c.bits};
        return true;
    }
    return false;
}

/// 宿主 CP 查找或追加 Utf8（intern 纪律：命中既有池不膨胀；与
/// tool_class_combiner 同款）。
ca::u32 findOrAddUtf8(jbc::core::ClassFile& cf, std::string_view s)
{
    if (const ca::u32 found = cf.cp.findUtf8(s); found != 0) return found;
    return cf.cp.addUtf8(s);
}

/// 把常量写入字段的 ConstantValue 属性（新建 RawAttr 槽）。值条目按描述符
/// 选型：J→Long / D→Double / I→Integer / F→Float；描述符不在四类内时
/// 调用方已先行排除。
void writeConstantValue(jbc::core::ClassFile& cf, jbc::core::MemberInfo& field,
                        std::string_view desc, const LoadedConst& v)
{
    ca::u32 valueCp = 0;
    if (desc == "J") {
        valueCp = cf.cp.addLong(v.num);
    } else if (desc == "D") {
        valueCp = cf.cp.addDoubleBits(v.bits);
    } else if (desc == "I") {
        valueCp = cf.cp.addInteger(static_cast<ca::i32>(v.num));
    } else {
        valueCp = cf.cp.addFloatBits(static_cast<ca::u32>(v.bits));
    }
    const std::vector<ca::u8> payload{static_cast<ca::u8>(valueCp >> 8),
                                      static_cast<ca::u8>(valueCp)};
    field.attrs.push_back(jbc::core::AttrSlot::makeNew(
        findOrAddUtf8(cf, "ConstantValue"), std::make_unique<jbc::core::RawAttr>(payload)));
}

/// 字段描述符是否属于本 pass 的四类常量面。
bool isSupportedDesc(std::string_view desc)
{
    return desc == "I" || desc == "F" || desc == "J" || desc == "D";
}

}   // namespace

mjbase::spi::PreservedAnalyses StaticFieldDefaultInliner::run(jbc::ir::ClassSession&            session,
                                                              mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    jbc::core::ClassFile& cf     = session.backing();
    jbc::ir::ClassUnit&   unit   = session.unit();
    const std::string_view thisName = unit.symbols().str(unit.thisClass);

    // 候选字段：static、无既有 ConstantValue、描述符在四类常量面内。
    // key → unit.fields 下标。
    std::unordered_map<std::string, ca::usize> candidates;
    for (ca::usize i = 0; i < unit.fields.size(); ++i) {
        const jbc::ir::FieldUnit& f = unit.fields[i];
        if ((f.access & kAccStatic) == 0) continue;
        if (!isSupportedDesc(unit.symbols().str(f.desc))) continue;
        if (hasConstantValue(cf, cf.fields[f.backingIndex])) continue;
        candidates.emplace(keyOf(unit.symbols().str(f.name), unit.symbols().str(f.desc)), i);
    }
    if (candidates.empty()) return mjbase::spi::PreservedAnalyses::all();

    const ca::usize clinitIdx = unit.findMethod("<clinit>", "()V");
    if (clinitIdx == jbc::ir::kNoMethod) return mjbase::spi::PreservedAnalyses::all();
    if (!jbc::ir::hasCode(session, clinitIdx)) return mjbase::spi::PreservedAnalyses::all();

    auto promoted = jbc::ir::promoteMethod(session, clinitIdx);
    if (promoted.is_err()) return mjbase::spi::PreservedAnalyses::all();
    jbc::ir::CodeUnit& cu = *promoted.unwrap().get();

    const std::vector<jbc::ir::InsnId> order = cu.dom.order();

    // 预扫描：owner==本类的 PUTSTATIC 按 (name,desc) 计数。非本类写入是
    // 别的字段，不给本类字段挡刀（旧栈全数口径的精确化，见头注）。
    std::unordered_map<std::string, int> putCount;
    for (const jbc::ir::InsnId id : order) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr || in->op != jbc::core::Op::PUTSTATIC) continue;
        jbc::ir::ClassSession::MemberSem ref;
        if (!session.memberOf(*in, ref) || ref.owner != thisName) continue;
        ++putCount[keyOf(ref.name, ref.desc)];
    }

    // 规划内联对：PUTSTATIC 顺序前驱为匹配类别的常量装载。
    struct Planned
    {
        jbc::ir::InsnId load;
        jbc::ir::InsnId put;
        ca::usize       fieldIdx;
        LoadedConst     value;
    };
    std::vector<Planned> planned;
    for (ca::usize i = 1; i < order.size(); ++i) {
        const jbc::ir::Insn* put = cu.dom.peek(order[i]);
        if (put == nullptr || put->op != jbc::core::Op::PUTSTATIC) continue;
        jbc::ir::ClassSession::MemberSem ref;
        if (!session.memberOf(*put, ref) || ref.owner != thisName) continue;
        const std::string key = keyOf(ref.name, ref.desc);
        const auto        cand = candidates.find(key);
        if (cand == candidates.end()) continue;
        const auto count = putCount.find(key);
        if (count == putCount.end() || count->second != 1) continue;
        const jbc::ir::FieldUnit& field = unit.fields[cand->second];
        const std::string_view   desc   = unit.symbols().str(field.desc);
        const jbc::ir::Insn*     prev   = cu.dom.peek(order[i - 1]);
        if (prev == nullptr) continue;
        LoadedConst value;
        if (!constValueOf(session, *prev, desc == "J" || desc == "D", value)) continue;
        planned.push_back({order[i - 1], order[i], cand->second, value});
    }
    if (planned.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 指令面：逐对删除。标签锚定（分支目标/异常表/调试边界）的指令对
    // 保守放弃——erase 自带 StillUsed 防线，锚定检查提前分流诊断面。
    jbc::ir::CodeEditor editor(cu);
    std::vector<Planned> inlined;
    inlined.reserve(planned.size());
    for (const Planned& p : planned) {
        if (method_rewrite_util::labelsAnchorAt(cu.dom, p.load)) continue;
        if (method_rewrite_util::labelsAnchorAt(cu.dom, p.put)) continue;
        if (editor.erase(p.load).is_err()) continue;
        if (editor.erase(p.put).is_err()) continue;
        inlined.push_back(p);
    }
    if (inlined.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 落盘顺序：lower 成功才写字段面——lower 失败时 backing 的 Code/CP/
    // 字段均未触碰，会话弃置即无半内联态（见头注一致性节）。
    if (jbc::ir::lowerMethod(session, clinitIdx).is_err()) {
        return mjbase::spi::PreservedAnalyses::all();
    }
    for (const Planned& p : inlined) {
        const jbc::ir::FieldUnit& field = unit.fields[p.fieldIdx];
        writeConstantValue(cf, cf.fields[field.backingIndex],
                           unit.symbols().str(field.desc), p.value);
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
