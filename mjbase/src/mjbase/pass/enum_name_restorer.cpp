#include "mjbase/pass/enum_name_restorer.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "mjbase/mapping/identifier_util.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjbase::pass {
namespace {

using mjbase::mapping::IdentifierUtil;
using mjbase::mapping::IntermediateMapping;

constexpr ca::u16 kAccEnum = 0x4000;

/// javac 枚举支撑成员的规范名（数组字段 / 数组合成方法）。
constexpr std::string_view kCanonicalValuesField  = "$VALUES";
constexpr std::string_view kCanonicalValuesMethod = "$values";

/// 直线段扫描的最大步数（astore/aload 中转窗口上限；javac 现代形状无中转，
/// 步限只为旧编译器/混淆器形态兜底）。
constexpr ca::usize kStraightLimit = 8;

ca::str::Utf8StringRef utf8Ref(std::string_view value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// 名字是否非法 Java 标识符（产出面口径：只有非法被混淆名才恢复）。
bool isIllegalIdentifier(std::string_view name)
{
    return !IdentifierUtil::isValidJavaIdentifier(utf8Ref(name));
}

/// 读一条 int 常量装载指令的值：ICONST 族 / BIPUSH / SIPUSH / LDC(Int)。
/// 不匹配返回 false。
bool constIntOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i32& out)
{
    switch (in.op) {
    case jbc::core::Op::ICONST_M1: out = -1; return true;
    case jbc::core::Op::ICONST_0: out = 0; return true;
    case jbc::core::Op::ICONST_1: out = 1; return true;
    case jbc::core::Op::ICONST_2: out = 2; return true;
    case jbc::core::Op::ICONST_3: out = 3; return true;
    case jbc::core::Op::ICONST_4: out = 4; return true;
    case jbc::core::Op::ICONST_5: out = 5; return true;
    case jbc::core::Op::BIPUSH:
    case jbc::core::Op::SIPUSH: out = static_cast<ca::i32>(in.a); return true;
    default: break;
    }
    jbc::ir::ClassSession::ConstSem c;
    if (!session.constOf(in, c)) return false;
    if (c.kind != jbc::ir::ClassSession::ConstSem::Kind::Int) return false;
    out = static_cast<ca::i32>(c.num);
    return true;
}

/// 读 ldc 族的 String 常量；非 String 返回 false。
bool constStringOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                   std::string_view& out)
{
    jbc::ir::ClassSession::ConstSem c;
    if (!session.constOf(in, c)) return false;
    if (c.kind != jbc::ir::ClassSession::ConstSem::Kind::String) return false;
    out = c.str;
    return true;
}

/// 构造点候选：clinit 内一处泄漏（ordinal + 泄漏真名 + 被混淆字段名）。
struct ConstPoint
{
    ca::i32          ordinal = 0;
    std::string_view leak;
    std::string_view field;
};

/// 字段 access（本类声明内查 (name, desc)；未声明返回 nullopt）。
std::optional<ca::u16> declaredFieldAccess(const jbc::ir::ClassUnit& unit, std::string_view name,
                                           std::string_view desc)
{
    for (const jbc::ir::FieldUnit& f : unit.fields) {
        if (unit.symbols().str(f.name) == name && unit.symbols().str(f.desc) == desc) {
            return f.access;
        }
    }
    return std::nullopt;
}

/// 从 from 起沿 dom 序向后直线扫描（仅 nop 与 astore/aload 同槽中转对）到
/// PUTSTATIC；返回 PUTSTATIC 在 order 中的下标。形状断裂/越界返回 nullopt。
std::optional<ca::usize> straightToPutstatic(const jbc::ir::CodeUnit&             cu,
                                             const std::vector<jbc::ir::InsnId>&  order,
                                             ca::usize                            from)
{
    bool    haveStore = false;
    bool    haveLoad  = false;
    ca::u16 slot      = 0;
    const ca::usize limit = std::min(order.size(), from + kStraightLimit);
    for (ca::usize j = from; j < limit; ++j) {
        const jbc::ir::Insn* in = cu.dom.peek(order[j]);
        if (in == nullptr) return std::nullopt;
        switch (in->op) {
        case jbc::core::Op::NOP: break;
        case jbc::core::Op::ASTORE_0:
        case jbc::core::Op::ASTORE_1:
        case jbc::core::Op::ASTORE_2:
        case jbc::core::Op::ASTORE_3:
            if (haveStore) return std::nullopt;
            haveStore = true;
            slot      = static_cast<ca::u16>(static_cast<int>(in->op)
                                        - static_cast<int>(jbc::core::Op::ASTORE_0));
            break;
        case jbc::core::Op::ASTORE:
            if (haveStore) return std::nullopt;
            haveStore = true;
            slot      = static_cast<ca::u16>(in->a);
            break;
        case jbc::core::Op::ALOAD_0:
        case jbc::core::Op::ALOAD_1:
        case jbc::core::Op::ALOAD_2:
        case jbc::core::Op::ALOAD_3:
            if (!haveStore || haveLoad) return std::nullopt;
            if (static_cast<ca::u16>(static_cast<int>(in->op)
                                     - static_cast<int>(jbc::core::Op::ALOAD_0))
                != slot) {
                return std::nullopt;
            }
            haveLoad = true;
            break;
        case jbc::core::Op::ALOAD:
            if (!haveStore || haveLoad) return std::nullopt;
            if (static_cast<ca::u16>(in->a) != slot) return std::nullopt;
            haveLoad = true;
            break;
        case jbc::core::Op::PUTSTATIC: return j;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

/// 单类分析：判据全部通过时把映射条目 intern 进 mapping；任一门失败则该类
/// 零产出（fail-closed，不影响其他类）。返回是否产出过条目（仅诊断用）。
bool analyzeEnumClass(jbc::ir::ClassSession& session, IntermediateMapping& mapping)
{
    jbc::ir::ClassUnit&   unit     = session.unit();
    const std::string_view thisName = unit.symbols().str(unit.thisClass);
    if ((session.backing().access & kAccEnum) == 0) return false;

    const std::string constDesc        = "L" + std::string(thisName) + ";";
    const std::string valuesDesc       = "[" + constDesc;
    const std::string valuesMethodDesc = "()" + valuesDesc;   // 数组支撑方法描述符

    // 锚 2：values/valueOf 桥签名在（方法描述符带 () 前缀，勿与数组字段
    // 描述符 [L<本类>; 混用）。
    if (unit.findMethod("values", valuesMethodDesc) == jbc::ir::kNoMethod) return false;
    if (unit.findMethod("valueOf", "(Ljava/lang/String;)" + constDesc) == jbc::ir::kNoMethod) {
        return false;
    }

    const ca::usize clinitIdx = unit.findMethod("<clinit>", "()V");
    if (clinitIdx == jbc::ir::kNoMethod) return false;
    if (!jbc::ir::hasCode(session, clinitIdx)) return false;
    auto promoted = jbc::ir::promoteMethod(session, clinitIdx);
    if (promoted.is_err()) return false;
    auto cuRef = std::move(promoted).unwrap();
    const std::vector<jbc::ir::InsnId> order = cuRef->dom.order();

    // ── 形状 B（先钉数组形状：B 缺失整类拒）──
    // clinit 内恰好一处 INVOKESTATIC 本类 ()[L本类;，随后直线段到 PUTSTATIC
    // 本类 [L本类;。name=="values"（公有桥混入调用位）与二次出现均病态拒。
    std::string_view valuesMethodName;
    std::string_view arrField;
    bool             valuesCallSeen = false;
    for (ca::usize i = 0; i < order.size(); ++i) {
        const jbc::ir::Insn* in = cuRef->dom.peek(order[i]);
        if (in == nullptr || in->op != jbc::core::Op::INVOKESTATIC) continue;
        jbc::ir::ClassSession::MemberSem ref;
        if (!session.memberOf(*in, ref)) continue;
        if (ref.owner != thisName || ref.desc != valuesMethodDesc) continue;
        if (ref.name == "values") return false;   // 公有桥出现在调用位：病态
        if (valuesCallSeen) return false;         // 多处数组初始化调用：病态
        valuesCallSeen   = true;
        valuesMethodName = ref.name;
        const auto put   = straightToPutstatic(*cuRef, order, i + 1);
        if (!put.has_value()) return false;
        const jbc::ir::Insn* putInsn = cuRef->dom.peek(order[*put]);
        if (putInsn == nullptr) return false;
        jbc::ir::ClassSession::MemberSem putRef;
        if (!session.memberOf(*putInsn, putRef)) return false;
        if (putRef.owner != thisName || putRef.desc != valuesDesc) return false;
        arrField = putRef.name;
    }
    if (!valuesCallSeen || arrField.empty()) return false;

    // 数组方法体：GETSTATIC 本类 L本类; 出现序 = 数组序。
    const ca::usize valuesIdx = unit.findMethod(valuesMethodName, valuesMethodDesc);
    if (valuesIdx == jbc::ir::kNoMethod) return false;
    if (!jbc::ir::hasCode(session, valuesIdx)) return false;
    auto valuesPromoted = jbc::ir::promoteMethod(session, valuesIdx);
    if (valuesPromoted.is_err()) return false;
    auto valuesCu = std::move(valuesPromoted).unwrap();
    std::vector<std::string_view> arrayOrder;
    for (const jbc::ir::InsnId id : valuesCu->dom.order()) {
        const jbc::ir::Insn* in = valuesCu->dom.peek(id);
        if (in == nullptr || in->op != jbc::core::Op::GETSTATIC) continue;
        jbc::ir::ClassSession::MemberSem ref;
        if (!session.memberOf(*in, ref)) continue;
        if (ref.owner != thisName || ref.desc != constDesc) continue;
        arrayOrder.push_back(ref.name);
    }
    if (arrayOrder.empty()) return false;

    // ── 形状 A（构造点泄漏收集）──
    std::vector<ConstPoint> points;
    for (ca::usize i = 0; i + 2 < order.size(); ++i) {
        const jbc::ir::Insn* ldc = cuRef->dom.peek(order[i]);
        if (ldc == nullptr || (ldc->op != jbc::core::Op::LDC && ldc->op != jbc::core::Op::LDC_W)) {
            continue;
        }
        std::string_view leak;
        if (!constStringOf(session, *ldc, leak)) continue;
        // 前锚：NEW 本类; DUP（构造点形状的第一二拍）。
        if (i < 2) continue;
        const jbc::ir::Insn* newInsn = cuRef->dom.peek(order[i - 2]);
        const jbc::ir::Insn* dupInsn = cuRef->dom.peek(order[i - 1]);
        if (newInsn == nullptr || dupInsn == nullptr) continue;
        if (newInsn->op != jbc::core::Op::NEW || session.classOf(*newInsn) != thisName) continue;
        if (dupInsn->op != jbc::core::Op::DUP) continue;
        // ordinal：紧随 ldc 的 int 常量。
        ca::i32 ordinal = 0;
        {
            const jbc::ir::Insn* ordInsn = cuRef->dom.peek(order[i + 1]);
            if (ordInsn == nullptr || !constIntOf(session, *ordInsn, ordinal)) continue;
            if (ordinal < 0) continue;
        }
        // <init>：desc 以 (Ljava/lang/String;I 开头（附加参数在序数之后）。
        const jbc::ir::Insn* ctorInsn = cuRef->dom.peek(order[i + 2]);
        if (ctorInsn == nullptr || ctorInsn->op != jbc::core::Op::INVOKESPECIAL) continue;
        jbc::ir::ClassSession::MemberSem ctorRef;
        if (!session.memberOf(*ctorInsn, ctorRef)) continue;
        if (ctorRef.owner != thisName || ctorRef.name != "<init>") continue;
        if (ctorRef.desc.rfind("(Ljava/lang/String;I", 0) != 0) continue;
        // 直线段到 PUTSTATIC 本类字段。
        const auto put = straightToPutstatic(*cuRef, order, i + 3);
        if (!put.has_value()) continue;   // 窗口断裂的点丢弃（互证不齐兜底）
        const jbc::ir::Insn* putInsn = cuRef->dom.peek(order[*put]);
        if (putInsn == nullptr) continue;
        jbc::ir::ClassSession::MemberSem putRef;
        if (!session.memberOf(*putInsn, putRef)) continue;
        if (putRef.owner != thisName || putRef.desc != constDesc) continue;
        // 目标字段须为本类声明且带 ACC_ENUM 位（javac 枚举常量字段标记）。
        const auto access = declaredFieldAccess(unit, putRef.name, putRef.desc);
        if (!access.has_value() || (*access & kAccEnum) == 0) continue;
        points.push_back({ordinal, leak, putRef.name});
    }
    if (points.empty()) return false;

    // ── 双形状互证 ──
    std::sort(points.begin(), points.end(),
              [](const ConstPoint& a, const ConstPoint& b) { return a.ordinal < b.ordinal; });
    if (points.size() != arrayOrder.size()) return false;
    for (ca::usize k = 0; k < points.size(); ++k) {
        if (points[k].ordinal != static_cast<ca::i32>(k)) return false;   // 0..n-1 无重无缺
        if (points[k].field != arrayOrder[k]) return false;               // 字段序逐元素一致
    }

    // ── 产出（映射面最小化：泄漏真名合法 + 被混淆名非法 + 非同名）──
    bool produced = false;
    for (const ConstPoint& p : points) {
        if (p.field == p.leak) continue;
        if (!IdentifierUtil::isValidJavaIdentifier(utf8Ref(p.leak))) continue;
        if (!isIllegalIdentifier(p.field)) continue;
        mapping.addField(utf8Ref(thisName), utf8Ref(constDesc), utf8Ref(p.field), utf8Ref(p.leak));
        produced = true;
    }
    if (arrField != kCanonicalValuesField && isIllegalIdentifier(arrField)) {
        mapping.addField(utf8Ref(thisName), utf8Ref(valuesDesc), utf8Ref(arrField),
                         utf8Ref(kCanonicalValuesField));
        produced = true;
    }
    if (valuesMethodName != kCanonicalValuesMethod && isIllegalIdentifier(valuesMethodName)) {
        mapping.addMethod(utf8Ref(thisName), utf8Ref(valuesMethodDesc), utf8Ref(valuesMethodName),
                          utf8Ref(kCanonicalValuesMethod));
        produced = true;
    }
    return produced;
}

}   // namespace

IntermediateMapping EnumNameMappingAnalysis::run(mjbase::model::ResourceContainer&   container,
                                                 mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    IntermediateMapping mapping;
    for (auto& [uri, res] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(res.get());
        if (cls == nullptr) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) continue;   // 畸形类跳过（class_scan fail-open 同口径）
        analyzeEnumClass(*std::move(sessionResult).unwrap(), mapping);
    }
    return mapping;
}

mjbase::spi::PreservedAnalyses EnumNameRestorer::run(mjbase::model::ResourceContainer&     container,
                                                     mjbase::spi::GlobalAnalysisManager& am)
{
    am.registerAnalysis<EnumNameMappingAnalysis>();   // 幂等：重复注册不覆盖
    (void)am.getResult<EnumNameMappingAnalysis>(container);   // 触发计算并缓存
    return mjbase::spi::PreservedAnalyses::all();   // 只读：字节零改动
}

}   // namespace mjbase::pass
