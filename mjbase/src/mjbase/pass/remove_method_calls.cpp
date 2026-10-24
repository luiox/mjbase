#include "mjbase/pass/remove_method_calls.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_resource_pending_commit.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;

// ── 装载 ─────────────────────────────────────────────────────────────────────

/// 装载完成的类：会话（backing + IR 同生共死）+ 写回目标。
struct LoadedClass
{
    std::string                            name;   ///< 类内部名（会话解析面）
    std::string                            uri;    ///< 容器条目
    mjbase::model::ClassResource*          resource = nullptr;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 容器内全部 class 资源可解析性预检 + 装载：任一空 bytes / 不可解析 /
/// 无类名返回 false（整 pass 放弃——句柄钉死证明需要全容器可读）。
bool loadAllClasses(mjbase::model::ResourceContainer& container,
                    std::vector<LoadedClass>&         out)
{
    for (auto& [uri, resource] : container.getResources()) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr) continue;
        if (cls->get().empty()) return false;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) return false;
        LoadedClass entry;
        entry.session = std::move(sessionResult).unwrap();
        entry.name.assign(entry.session->backing().thisName());
        if (entry.name.empty()) return false;
        entry.uri      = uri;
        entry.resource = const_cast<mjbase::model::ClassResource*>(cls);
        out.push_back(std::move(entry));
    }
    return true;
}

// ── 描述符解析 ───────────────────────────────────────────────────────────────

/// 方法描述符的栈形态：实参槽位（long/double 占 2）、逐参类型字符、返回类别。
struct CallShape
{
    bool              valid     = false;
    ca::u32           argSlots  = 0;
    std::vector<char> argTypes;      ///< 逐参描述符首字符（J/D 为 cat2）
    ca::u8            retCat = 0;   ///< 0 = void，1 = cat1，2 = cat2
};

/// 解析方法描述符；畸形文法返回 invalid（调用点保守放弃）。
CallShape analyzeMethodDesc(std::string_view desc)
{
    CallShape s;
    if (desc.empty() || desc.front() != '(') return s;
    ca::usize pos = 1;
    bool      closed = false;
    while (pos < desc.size()) {
        const char c = desc[pos];
        if (c == ')') {
            closed = true;
            ++pos;
            break;
        }
        switch (c) {
        case 'J':
        case 'D':
            s.argSlots += 2;
            s.argTypes.push_back(c);
            ++pos;
            break;
        case 'B':
        case 'C':
        case 'S':
        case 'Z':
        case 'I':
        case 'F':
            s.argSlots += 1;
            s.argTypes.push_back(c);
            ++pos;
            break;
        case 'L':
        {
            const auto semi = desc.find(';', pos);
            if (semi == std::string_view::npos) return CallShape{};
            s.argSlots += 1;
            s.argTypes.push_back('L');
            pos = semi + 1;
            break;
        }
        case '[':
        {
            ca::usize j = pos;
            while (j < desc.size() && desc[j] == '[') ++j;
            if (j >= desc.size()) return CallShape{};
            if (desc[j] == 'L') {
                const auto semi = desc.find(';', j);
                if (semi == std::string_view::npos) return CallShape{};
                pos = semi + 1;
            }
            else {
                pos = j + 1;
            }
            s.argSlots += 1;
            s.argTypes.push_back('[');
            break;
        }
        default:
            return CallShape{};
        }
    }
    if (!closed || pos >= desc.size()) return s;
    switch (desc[pos]) {
    case 'V':
        s.retCat = 0;
        break;
    case 'J':
    case 'D':
        s.retCat = 2;
        break;
    default:
        s.retCat = 1;
        break;
    }
    s.valid = true;
    return s;
}

bool isCat2Arg(char typeChar) noexcept { return typeChar == 'J' || typeChar == 'D'; }

// ── 规格匹配 ─────────────────────────────────────────────────────────────────

/// 规格命中：owner/name 精确；desc 规格非空时还需精确，空 = 任意。
bool specMatches(const MethodCallSpec& spec, std::string_view owner, std::string_view name,
                 std::string_view desc)
{
    return spec.owner == owner && spec.name == name &&
           (spec.desc.empty() || spec.desc == desc);
}

bool anySpecMatches(const std::vector<MethodCallSpec>& specs, std::string_view owner,
                    std::string_view name, std::string_view desc)
{
    for (const MethodCallSpec& spec : specs) {
        if (specMatches(spec, owner, name, desc)) return true;
    }
    return false;
}

bool anyPtrSpecMatches(const std::vector<const MethodCallSpec*>& specs, std::string_view owner,
                       std::string_view name, std::string_view desc)
{
    for (const MethodCallSpec* spec : specs) {
        if (specMatches(*spec, owner, name, desc)) return true;
    }
    return false;
}

// ── 内建白名单 ───────────────────────────────────────────────────────────────

/// 旧栈纯函数名单（逐条对齐）：Math/StrictMath 无异常纯函数。
constexpr std::string_view kSafeMathNames[] = {
    "abs",  "max",       "min",      "sin",      "cos",           "tan",       "asin",
    "acos", "atan",      "atan2",    "sqrt",     "cbrt",          "ceil",      "floor",
    "rint", "round",     "exp",      "expm1",    "log",           "log10",     "log1p",
    "pow",  "signum",    "copySign", "hypot",    "IEEEremainder", "toRadians", "toDegrees",
    "ulp",  "nextAfter", "nextUp",   "nextDown", "scalb",
};

bool isSafeMathName(std::string_view name)
{
    for (const std::string_view safe : kSafeMathNames) {
        if (safe == name) return true;
    }
    return false;
}

/// 零参、无可观察副作用、返回 cat-2 的 JDK 调用（结果被丢弃时可整体删除）。
bool isZeroArgCat2JdkCall(std::string_view owner, std::string_view name,
                          std::string_view desc)
{
    return ((owner == "java/lang/System" &&
             (name == "nanoTime" || name == "currentTimeMillis")) ||
            (owner == "java/lang/Math" && name == "random")) &&
           desc == (name == "random" ? "()D" : "()J");
}

/// 指令是否为推送 typeChar 类型原始常量的纯常量生产者（Math 整窗用）。
bool pushesPrimitiveConstOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                            char typeChar)
{
    const jbc::core::Op op = in.op;
    switch (typeChar) {
    case 'B':
    case 'C':
    case 'S':
    case 'Z':
    case 'I':
        if (op == Op::ICONST_M1 || (op >= Op::ICONST_0 && op <= Op::ICONST_5) ||
            op == Op::BIPUSH || op == Op::SIPUSH) {
            return true;
        }
        if (op == Op::LDC || op == Op::LDC_W) {
            jbc::ir::ClassSession::ConstSem constant;
            return session.constOf(in, constant) &&
                   constant.kind == jbc::ir::ClassSession::ConstSem::Kind::Int;
        }
        return false;
    case 'J':
        if (op == Op::LCONST_0 || op == Op::LCONST_1) return true;
        if (op == Op::LDC2_W) {
            jbc::ir::ClassSession::ConstSem constant;
            return session.constOf(in, constant) &&
                   constant.kind == jbc::ir::ClassSession::ConstSem::Kind::Long;
        }
        return false;
    case 'F':
        if (op >= Op::FCONST_0 && op <= Op::FCONST_2) return true;
        if (op == Op::LDC || op == Op::LDC_W) {
            jbc::ir::ClassSession::ConstSem constant;
            return session.constOf(in, constant) &&
                   constant.kind == jbc::ir::ClassSession::ConstSem::Kind::Float;
        }
        return false;
    case 'D':
        if (op == Op::DCONST_0 || op == Op::DCONST_1) return true;
        if (op == Op::LDC2_W) {
            jbc::ir::ClassSession::ConstSem constant;
            return session.constOf(in, constant) &&
                   constant.kind == jbc::ir::ClassSession::ConstSem::Kind::Double;
        }
        return false;
    default:
        return false;
    }
}

// ── 栈形态辅助 ───────────────────────────────────────────────────────────────

/// dup/dup2 族与 swap：调用点前一条命中即放弃（幸存引用可能永远停留
/// uninitialized——`new; dup; invokespecial <init>` 的直接后果）。
bool isDupFamilyOrSwap(Op op) noexcept
{
    return op == Op::DUP || op == Op::DUP_X1 || op == Op::DUP_X2 || op == Op::DUP2 ||
           op == Op::DUP2_X1 || op == Op::DUP2_X2 || op == Op::SWAP;
}

/// 局部变量读写族（存/取族一一配对；long/double 各自独立成族）。
enum class LocalFamily : ca::u8
{
    None = 0,
    Int,
    Long,
    Float,
    Ref,
    Double,
};

LocalFamily loadFamilyOf(Op op) noexcept
{
    if (op == Op::ALOAD || (op >= Op::ALOAD_0 && op <= Op::ALOAD_3)) return LocalFamily::Ref;
    if (op == Op::ILOAD || (op >= Op::ILOAD_0 && op <= Op::ILOAD_3)) return LocalFamily::Int;
    if (op == Op::LLOAD || (op >= Op::LLOAD_0 && op <= Op::LLOAD_3)) return LocalFamily::Long;
    if (op == Op::FLOAD || (op >= Op::FLOAD_0 && op <= Op::FLOAD_3)) return LocalFamily::Float;
    if (op == Op::DLOAD || (op >= Op::DLOAD_0 && op <= Op::DLOAD_3)) return LocalFamily::Double;
    return LocalFamily::None;
}

LocalFamily storeFamilyOf(Op op) noexcept
{
    if (op == Op::ASTORE || (op >= Op::ASTORE_0 && op <= Op::ASTORE_3)) return LocalFamily::Ref;
    if (op == Op::ISTORE || (op >= Op::ISTORE_0 && op <= Op::ISTORE_3)) return LocalFamily::Int;
    if (op == Op::LSTORE || (op >= Op::LSTORE_0 && op <= Op::LSTORE_3)) return LocalFamily::Long;
    if (op == Op::FSTORE || (op >= Op::FSTORE_0 && op <= Op::FSTORE_3)) return LocalFamily::Float;
    if (op == Op::DSTORE || (op >= Op::DSTORE_0 && op <= Op::DSTORE_3)) return LocalFamily::Double;
    return LocalFamily::None;
}

/// 局部读写指令的槽位（_n 形态从 opcode 折算，其余取 a 操作数）。
ca::u32 localIndexOf(Op op, const jbc::ir::Insn& in) noexcept
{
    if (op >= Op::ILOAD_0 && op <= Op::ILOAD_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::ILOAD_0));
    if (op >= Op::LLOAD_0 && op <= Op::LLOAD_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::LLOAD_0));
    if (op >= Op::FLOAD_0 && op <= Op::FLOAD_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::FLOAD_0));
    if (op >= Op::DLOAD_0 && op <= Op::DLOAD_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::DLOAD_0));
    if (op >= Op::ALOAD_0 && op <= Op::ALOAD_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::ALOAD_0));
    if (op >= Op::ISTORE_0 && op <= Op::ISTORE_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::ISTORE_0));
    if (op >= Op::LSTORE_0 && op <= Op::LSTORE_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::LSTORE_0));
    if (op >= Op::FSTORE_0 && op <= Op::FSTORE_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::FSTORE_0));
    if (op >= Op::DSTORE_0 && op <= Op::DSTORE_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::DSTORE_0));
    if (op >= Op::ASTORE_0 && op <= Op::ASTORE_3) return static_cast<ca::u32>(static_cast<ca::u32>(op) - static_cast<ca::u32>(Op::ASTORE_0));
    return static_cast<ca::u32>(in.a);
}

/// 存族对应的取族（死局部判定按同族同槽配对）。
LocalFamily loadFamilyOfStore(LocalFamily store) noexcept
{
    switch (store) {
    case LocalFamily::Int: return LocalFamily::Int;
    case LocalFamily::Long: return LocalFamily::Long;
    case LocalFamily::Float: return LocalFamily::Float;
    case LocalFamily::Ref: return LocalFamily::Ref;
    case LocalFamily::Double: return LocalFamily::Double;
    default: return LocalFamily::None;
    }
}

/// 存族类别：LSTORE/DSTORE 消费 cat2，其余 cat1；None 返回 0。
ca::u8 storeCategory(LocalFamily store) noexcept
{
    switch (store) {
    case LocalFamily::Long:
    case LocalFamily::Double: return 2;
    case LocalFamily::Int:
    case LocalFamily::Float:
    case LocalFamily::Ref: return 1;
    default: return 0;
    }
}

/// 全方法「被读取的局部槽」集合（死局部判定的保守底账：iinc 与 ret 视同读）。
void scanLoadedLocals(const jbc::ir::CodeUnit& cu,
                      std::unordered_set<ca::u64>& out)
{
    const auto key = [](ca::u8 family, ca::u32 slot) {
        return (static_cast<ca::u64>(family) << 32) | static_cast<ca::u64>(slot);
    };
    for (const jbc::ir::InsnId id : cu.dom.order()) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) continue;
        const LocalFamily family = loadFamilyOf(in->op);
        if (family != LocalFamily::None) {
            out.insert(key(static_cast<ca::u8>(family), localIndexOf(in->op, *in)));
            continue;
        }
        if (in->op == Op::IINC) {
            out.insert(key(static_cast<ca::u8>(LocalFamily::Int), static_cast<ca::u32>(in->a)));
        }
        else if (in->op == Op::RET) {
            // ret 读出返回地址：该槽视同已读（jsr/ret 保守保留面）。
            out.insert(key(static_cast<ca::u8>(LocalFamily::Ref), static_cast<ca::u32>(in->a)));
        }
    }
}

// ── 句柄钉死集 ───────────────────────────────────────────────────────────────

/// CP 引用三元组（句柄钉死集的元素；std::string 仅作内部键）。
struct MemberTriple
{
    std::string owner;
    std::string name;
    std::string desc;

    bool operator==(const MemberTriple& o) const noexcept
    {
        return owner == o.owner && name == o.name && desc == o.desc;
    }
};

struct MemberTripleHash
{
    ca::usize operator()(const MemberTriple& t) const noexcept
    {
        std::hash<std::string> h;
        const ca::usize seed = h(t.owner);
        return seed ^ (h(t.name) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2)) ^
               (h(t.desc) + 0x9e3779b97f4a7c15ULL);
    }
};

/// 解析成员 ref（Methodref/InterfaceMethodref）为三元组；畸形返回 false。
bool resolveMemberTriple(const jbc::core::ConstantPool& cp, ca::u32 refIdx, MemberTriple& out)
{
    if (!cp.valid(refIdx)) return false;
    const auto& ref = cp.at(refIdx);
    if (ref.tag != jbc::core::CpTag::Methodref &&
        ref.tag != jbc::core::CpTag::InterfaceMethodref) {
        return false;
    }
    if (!cp.valid(ref.a) || !cp.valid(ref.b)) return false;
    const auto& cls = cp.at(ref.a);
    if (cls.tag != jbc::core::CpTag::Class) return false;
    const auto& nt = cp.at(ref.b);
    if (nt.tag != jbc::core::CpTag::NameAndType) return false;
    out.owner.assign(cp.utf8Of(cls.a));
    out.name.assign(cp.utf8Of(nt.a));
    out.desc.assign(cp.utf8Of(nt.b));
    return true;
}

/// 全容器常量池 MethodHandle 引用面（JVMS reference_kind 5..9 为方法）：
/// ldc、invokedynamic/condy 的 bootstrap 面（BSM 本体与实参）最终都落在
/// CP MethodHandle 条目上，逐条目扫描即完整覆盖。
void collectHandlePins(const jbc::core::ClassFile&  cf,
                       std::unordered_set<MemberTriple, MemberTripleHash>& pins)
{
    const jbc::core::ConstantPool& cp = cf.cp;
    for (ca::u32 idx = 1; idx < cp.size(); ++idx) {
        const auto& entry = cp.at(idx);
        if (entry.tag != jbc::core::CpTag::MethodHandle) continue;
        // reference_kind 1..4 为字段柄；方法柄 5..9（含 newInvokeSpecial）。
        if (entry.a < 5 || entry.a > 9) continue;
        MemberTriple triple;
        if (resolveMemberTriple(cp, entry.b, triple)) pins.insert(std::move(triple));
    }
}

/// 点名规格是否被任一句柄钉死（规格 desc 空 = 钉死集内同名任意描述符都算）。
bool specIsPinned(const MethodCallSpec&                              spec,
                  const std::unordered_set<MemberTriple, MemberTripleHash>& pins)
{
    for (const MemberTriple& pin : pins) {
        if (pin.owner == spec.owner && pin.name == spec.name &&
            (spec.desc.empty() || pin.desc == spec.desc)) {
            return true;
        }
    }
    return false;
}

// ── 调用点规划 ───────────────────────────────────────────────────────────────

/// 单个调用点的删除计划：待删指令批 + 吞参 pop 序列。
///
/// 执行序（先插后删）：吞参 pop 逐条 insertBefore 到调用点之前，再整批删除
/// ——锚点恒为调用点自身（各计划的删除集互不相交，调用点不可能被其他计划
/// 删除），不存在「锚点先死」的执行序依赖；插入与删除之间由引用预检保证
/// 批删必然成功。
struct SitePlan
{
    ca::usize                    orderPos = 0;   ///< 扫描序（诊断/稳定序用）
    jbc::ir::InsnId              invoke{};
    jbc::ir::InsnId              consumer{};    ///< pop/pop2/死局部 store；void 时无效
    std::vector<jbc::ir::Insn>   pops;          ///< 吞实参槽 + 接收者的 pop 序列
    std::vector<jbc::ir::InsnId> window;        ///< 整窗形态额外删除的常量生产者
};

/// 通用删点形态（点名面与用户白名单条目共用）：吞实参/接收者 + 返回值消费面
/// 必须是 pop（cat1）/ pop2（cat2）/ void（无消费面）/ 死局部 store。
SitePlan planGenericSite(const jbc::ir::CodeUnit& cu, ca::usize i, Op op,
                         std::string_view desc)
{
    SitePlan plan;
    const CallShape shape = analyzeMethodDesc(desc);
    if (!shape.valid) return {};

    const auto& order = cu.dom.order();
    // dup/dup2 族与 swap 前缀：放弃（宁可留调用，不可产生非法栈）。
    if (i > 0) {
        const jbc::ir::Insn* prev = cu.dom.peek(order[i - 1]);
        if (prev != nullptr && isDupFamilyOrSwap(prev->op)) return {};
    }

    // 吞实参（自栈顶向栈底 = 描述符逆序）与接收者。
    for (ca::usize a = shape.argTypes.size(); a-- > 0;) {
        plan.pops.push_back(
            jbc::ir::CodeEditor::simple(isCat2Arg(shape.argTypes[a]) ? Op::POP2 : Op::POP));
    }
    if (op != Op::INVOKESTATIC) {
        plan.pops.push_back(jbc::ir::CodeEditor::simple(Op::POP));
    }

    plan.invoke = order[i];

    if (shape.retCat == 0) return plan;   // void：无返回值消费面

    if (i + 1 >= order.size()) return {};
    const jbc::ir::InsnId nextId = order[i + 1];
    const jbc::ir::Insn*  next   = cu.dom.peek(nextId);
    if (next == nullptr) return {};

    if (shape.retCat == 1 && next->op == Op::POP) {
        plan.consumer = nextId;
    }
    else if (shape.retCat == 2 && next->op == Op::POP2) {
        plan.consumer = nextId;
    }
    else {
        // 存入死局部：该局部全方法再无同族 load（含 iinc/ret 底账）才可随删。
        const LocalFamily store = storeFamilyOf(next->op);
        if (store == LocalFamily::None || storeCategory(store) != shape.retCat) return {};
        std::unordered_set<ca::u64> loaded;
        scanLoadedLocals(cu, loaded);
        const ca::u64 key = (static_cast<ca::u64>(static_cast<ca::u8>(
                                 loadFamilyOfStore(store)))
                             << 32) |
                            static_cast<ca::u64>(localIndexOf(next->op, *next));
        if (loaded.count(key) != 0) return {};
        plan.consumer = nextId;
    }
    return plan;
}

/// 内建白名单形态（旧栈硬编码白名单的等价面）：整窗删除，不产生吞参 pop。
SitePlan planWhitelistSite(const jbc::ir::ClassSession& session, const jbc::ir::CodeUnit& cu,
                           ca::usize i, Op op, std::string_view owner, std::string_view name,
                           std::string_view desc)
{
    const auto& order = cu.dom.order();
    const CallShape shape = analyzeMethodDesc(desc);
    if (!shape.valid) return {};

    // (a) 零参无副作用 cat-2 JDK 调用：invokestatic; pop2。
    if (op == Op::INVOKESTATIC && shape.retCat == 2 && shape.argSlots == 0 &&
        isZeroArgCat2JdkCall(owner, name, desc)) {
        if (i + 1 >= order.size()) return {};
        const jbc::ir::Insn* next = cu.dom.peek(order[i + 1]);
        if (next == nullptr || next->op != Op::POP2) return {};
        SitePlan plan;
        plan.orderPos = i;
        plan.invoke   = order[i];
        plan.consumer = order[i + 1];
        return plan;
    }

    // (b) String 只读读：ldc(String); invokevirtual length/isEmpty; pop。
    if (op == Op::INVOKEVIRTUAL && owner == "java/lang/String" &&
        (name == "length" || name == "isEmpty") && shape.retCat == 1) {
        if (i == 0 || i + 1 >= order.size()) return {};
        const jbc::ir::Insn* producer = cu.dom.peek(order[i - 1]);
        const jbc::ir::Insn* next     = cu.dom.peek(order[i + 1]);
        if (producer == nullptr || next == nullptr) return {};
        if (producer->op != Op::LDC && producer->op != Op::LDC_W) return {};
        jbc::ir::ClassSession::ConstSem constant;
        if (!session.constOf(*producer, constant) ||
            constant.kind != jbc::ir::ClassSession::ConstSem::Kind::String) {
            return {};
        }
        if (next->op != Op::POP) return {};
        SitePlan plan;
        plan.orderPos = i;
        plan.invoke   = order[i];
        plan.consumer = order[i + 1];
        plan.window.push_back(order[i - 1]);
        return plan;
    }

    // (c) Math/StrictMath 纯函数：全部实参均为相邻原始常量时整窗删。
    if (op == Op::INVOKESTATIC && (owner == "java/lang/Math" || owner == "java/lang/StrictMath") &&
        isSafeMathName(name)) {
        if (i + 1 >= order.size()) return {};
        const jbc::ir::Insn* next = cu.dom.peek(order[i + 1]);
        if (next == nullptr) return {};
        if (!((shape.retCat == 1 && next->op == Op::POP) ||
              (shape.retCat == 2 && next->op == Op::POP2))) {
            return {};
        }
        // 逐参逆序回看：每参恰由一条相邻常量指令推送（cat2 亦单条）。
        SitePlan plan;
        ca::usize cursor = i;
        for (ca::usize a = shape.argTypes.size(); a-- > 0;) {
            if (cursor == 0) return {};
            --cursor;
            const jbc::ir::Insn* producer = cu.dom.peek(order[cursor]);
            if (producer == nullptr ||
                !pushesPrimitiveConstOf(session, *producer, shape.argTypes[a])) {
                return {};
            }
            plan.window.push_back(order[cursor]);
        }
        plan.orderPos = i;
        plan.invoke   = order[i];
        plan.consumer = order[i + 1];
        return plan;
    }

    return {};
}

}   // namespace

// ── pass 主体 ────────────────────────────────────────────────────────────────

MethodCallRemover::MethodCallRemover(MethodCallRemovalConfig config)
    : config_(std::move(config))
{}

mjbase::spi::PreservedAnalyses MethodCallRemover::run(mjbase::model::ResourceContainer&     container,
                                                      mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    // 默认无配置 = 完全 no-op（opt-in 纪律）。
    const bool whitelistOn = config_.enableUnusedResultWhitelist;
    if (config_.includeCalls.empty() && !whitelistOn) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    std::vector<LoadedClass> loaded;
    if (!loadAllClasses(container, loaded)) return mjbase::spi::PreservedAnalyses::all();
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 句柄钉死集（全容器常量池 MethodHandle 引用面）；被钉死的点名成员整体
    // 放弃——调用点收尾为「成员随后被删」服务，句柄仍钉着成员时删调用与
    // 删除意图矛盾。
    std::unordered_set<MemberTriple, MemberTripleHash> pins;
    for (const LoadedClass& lc : loaded) {
        collectHandlePins(lc.session->backing(), pins);
    }
    std::vector<const MethodCallSpec*> includes;
    for (const MethodCallSpec& spec : config_.includeCalls) {
        if (anySpecMatches(config_.excludeCalls, spec.owner, spec.name, spec.desc)) continue;
        if (specIsPinned(spec, pins)) continue;
        includes.push_back(&spec);
    }
    // 面 1 活跃 = 过滤后的 include 非空；面 2 活跃 = 白名单面开且有内容
    // （内建集或用户追加至少其一）。两者都不活跃 = 配置等价空 → no-op。
    const bool includeActive = !includes.empty();
    const bool whitelistHasContent =
        whitelistOn && (config_.useDefaultWhitelist || !config_.discardableCalls.empty());
    if (!includeActive && !whitelistHasContent) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    // 帧再生底盘：层次图自容器摘要建（分支体 Strict 重算的公共父类求解）。
    mjbase::KeyPool                   pool;
    mjbase::hierarchy::ClassHierarchy hierarchy{pool};
    for (const LoadedClass& lc : loaded) {
        if (hierarchy.findClass(lc.name).valid()) continue;
        auto added = hierarchy.addClass(mjbase::hierarchy::fillSummary(*lc.session, pool));
        if (added.is_err()) return mjbase::spi::PreservedAnalyses::all();
    }
    mjbase::spi::CommonSuperClassResolver  superResolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{superResolver};

    mjbase::spi::ClassResourcePendingCommit pending;
    for (LoadedClass& lc : loaded) {
        jbc::ir::ClassSession& session = *lc.session;
        jbc::ir::ClassUnit&    unit    = session.unit();
        bool                   classModified = false;

        for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
            const jbc::ir::MethodUnit& mu = unit.methods[idx];
            if (!jbc::ir::hasCode(session, idx)) continue;
            auto promoted = jbc::ir::promoteMethod(session, idx);
            if (promoted.is_err()) continue;   // 升格失败的方法保守不动
            auto cuRef = std::move(promoted).unwrap();
            jbc::ir::CodeUnit& cu = *cuRef.get();

            // ── 扫描：收集全部可删调用点 ──
            const std::vector<jbc::ir::InsnId>& order = cu.dom.order();
            std::vector<SitePlan>               plans;
            for (ca::usize i = 0; i < order.size(); ++i) {
                const jbc::ir::Insn* in = cu.dom.peek(order[i]);
                if (in == nullptr) continue;
                const Op op = in->op;
                if (op != Op::INVOKEVIRTUAL && op != Op::INVOKESPECIAL &&
                    op != Op::INVOKESTATIC && op != Op::INVOKEINTERFACE) {
                    continue;
                }
                jbc::ir::ClassSession::MemberSem sem;
                if (!session.memberOf(*in, sem)) continue;
                if (sem.kind == jbc::ir::ClassSession::MemberSem::Kind::InvokeDynamic ||
                    sem.owner.empty()) {
                    // indy 的调用面是 bootstrap 而非成员：永不删。
                    continue;
                }

                SitePlan plan;
                bool have = false;
                if (anyPtrSpecMatches(includes, sem.owner, sem.name, sem.desc)) {
                    plan = planGenericSite(cu, i, op, sem.desc);
                    have = plan.invoke != jbc::ir::kNoInsn;
                }
                if (!have && whitelistOn) {
                    if (anySpecMatches(config_.discardableCalls, sem.owner, sem.name, sem.desc)) {
                        plan = planGenericSite(cu, i, op, sem.desc);
                        have = plan.invoke != jbc::ir::kNoInsn;
                    }
                    if (!have && config_.useDefaultWhitelist) {
                        plan = planWhitelistSite(session, cu, i, op, sem.owner, sem.name,
                                                 sem.desc);
                        have = plan.invoke != jbc::ir::kNoInsn;
                    }
                }
                if (!have) continue;
                plan.orderPos = i;
                plans.push_back(std::move(plan));
            }
            if (plans.empty()) continue;

            // ── 执行：逐点「预检 → 插吞参 pop → 批删」（全有或全无）──
            jbc::ir::CodeEditor ed(cu);
            bool                applied = false;
            for (SitePlan& plan : plans) {
                std::vector<jbc::ir::InsnId> batch;
                batch.push_back(plan.invoke);
                if (plan.consumer != jbc::ir::kNoInsn) batch.push_back(plan.consumer);
                for (const jbc::ir::InsnId producer : plan.window) batch.push_back(producer);
                // 引用预检：批内任一指令被标签锚定（分支目标 / 异常表边界 /
                // 元数据边界）即放弃该点——插了 pop 删不掉调用会双重消费。
                bool anchored = false;
                for (const jbc::ir::InsnId id : batch) {
                    if (!cu.dom.labelsAt(id).empty()) {
                        anchored = true;
                        break;
                    }
                }
                if (anchored) continue;   // 保守保留调用
                for (const jbc::ir::Insn& pop : plan.pops) {
                    // POP/POP2 无操作数，插入不可能失败（锚点存活）；
                    // 失败即契约破坏，fail loud（容器尚未提交，保持原样）。
                    if (!ed.insertBefore(plan.invoke, pop).is_ok()) {
                        throw std::runtime_error("MethodCallRemover: pop insertion failed in '" +
                                                 lc.name + "'");
                    }
                }
                if (ed.eraseBatch(batch, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed)
                        .is_err()) {
                    // 预检已覆盖批删的全部拒收面，走到这里即契约破坏：fail loud
                    // （容器尚未提交，保持原样），不产出半删状态。
                    throw std::runtime_error("MethodCallRemover: erase refused after anchor "
                                             "precheck in '"
                                             + lc.name + "'");
                }
                applied = true;
            }
            if (!applied) continue;

            // ── 写回：嵌套属性布局适配 → lower → 分支体 Strict 帧再生 ──
            mjbase::spi::frame_rewire::prepareNestedAttrsForLayout(session, cu);
            auto lowered = jbc::ir::lowerMethod(session, mu.id);
            if (lowered.is_err()) {
                throw std::runtime_error("MethodCallRemover: lower failed for '" + lc.name + "." +
                                         std::string(unit.symbols().str(mu.name)) + "': " +
                                         jbc::ir::to_string(lowered.unwrap_err()));
            }
            bool needsFrames = !cu.exc.empty();
            if (!needsFrames) {
                const ca::u32 branchBit = 1u << static_cast<ca::u32>(jbc::ir::InsnFamily::Branch);
                for (const jbc::ir::InsnId id : cu.dom.order()) {
                    const jbc::ir::Insn* in = cu.dom.peek(id);
                    if (in != nullptr && (jbc::ir::insnFamilyBit(in->op) & branchBit) != 0) {
                        needsFrames = true;
                        break;
                    }
                }
            }
            if (needsFrames) {
                const mjbase::ClassKey ownerKey = mjbase::classKeyOf(pool, lc.name);
                const mjbase::MemberKey key     = mjbase::memberKeyOf(
                    pool, ownerKey, unit.symbols().str(mu.name), unit.symbols().str(mu.desc));
                auto scope = std::make_unique<mjbase::spi::MethodScope>(mjbase::spi::MethodScope{
                    mjbase::spi::MethodTarget{key, mu.id, unit.symbols().str(mu.name),
                                              unit.symbols().str(mu.desc), mu.access, session},
                    cu});
                mjbase::spi::frame_rewire::rewireFramesAfterLower(session, *scope, typeResolver);
            }
            classModified = true;
        }

        if (!classModified) continue;
        auto written = jbc::core::writeClass(session.backing());
        if (written.is_err()) {
            throw std::runtime_error("MethodCallRemover: write failed for '" + lc.name + "'");
        }
        pending.stage(lc.resource, std::move(written).unwrap());
    }

    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();
    pending.commit();
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
