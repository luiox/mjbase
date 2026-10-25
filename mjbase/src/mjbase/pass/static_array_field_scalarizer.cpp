#include "mjbase/pass/static_array_field_scalarizer.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {
namespace {

constexpr ca::u16 kAccPrivate = 0x0002;
constexpr ca::u16 kAccStatic  = 0x0008;

/// 元素描述符 → newarray atype 码（JVMS table 6.5）。
ca::i32 atypeOf(std::string_view elemDesc)
{
    if (elemDesc == "Z") return 4;
    if (elemDesc == "C") return 5;
    if (elemDesc == "F") return 6;
    if (elemDesc == "D") return 7;
    if (elemDesc == "B") return 8;
    if (elemDesc == "S") return 9;
    if (elemDesc == "I") return 10;
    if (elemDesc == "J") return 11;
    return -1;
}

/// 元素描述符 → 数组元素 load/store 操作码（Z/B 共用 baload/bastore）。
jbc::core::Op arrayLoadOf(std::string_view elemDesc)
{
    using jbc::core::Op;
    if (elemDesc == "J") return Op::LALOAD;
    if (elemDesc == "F") return Op::FALOAD;
    if (elemDesc == "D") return Op::DALOAD;
    if (elemDesc == "B" || elemDesc == "Z") return Op::BALOAD;
    if (elemDesc == "C") return Op::CALOAD;
    if (elemDesc == "S") return Op::SALOAD;
    return Op::IALOAD;
}

jbc::core::Op arrayStoreOf(std::string_view elemDesc)
{
    using jbc::core::Op;
    if (elemDesc == "J") return Op::LASTORE;
    if (elemDesc == "F") return Op::FASTORE;
    if (elemDesc == "D") return Op::DASTORE;
    if (elemDesc == "B" || elemDesc == "Z") return Op::BASTORE;
    if (elemDesc == "C") return Op::CASTORE;
    if (elemDesc == "S") return Op::SASTORE;
    return Op::IASTORE;
}

/// 字段描述符是否属于第一切片的原始类型数组面；是则给出去掉 [ 的元素描述符。
bool primitiveArrayElemDesc(std::string_view desc, std::string_view& elem)
{
    if (desc.size() != 2 || desc[0] != '[' || desc[1] == '[' || desc[1] == 'L') return false;
    switch (desc[1]) {
    case 'I': case 'J': case 'F': case 'D': case 'Z': case 'B': case 'C': case 'S':
        elem = desc.substr(1);
        return true;
    default: return false;
    }
}

/// int 常量装载识别（iconst 族/bipush/sipush/ldc(_w) Int），值经 out 带出。
bool intConstOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i32& out)
{
    using jbc::core::Op;
    switch (in.op) {
    case Op::ICONST_M1: out = -1; return true;
    case Op::ICONST_0: out = 0; return true;
    case Op::ICONST_1: out = 1; return true;
    case Op::ICONST_2: out = 2; return true;
    case Op::ICONST_3: out = 3; return true;
    case Op::ICONST_4: out = 4; return true;
    case Op::ICONST_5: out = 5; return true;
    case Op::BIPUSH:
    case Op::SIPUSH: out = static_cast<ca::i32>(in.a); return true;
    default: break;
    }
    jbc::ir::ClassSession::ConstSem c;
    if (session.constOf(in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::Int) {
        out = static_cast<ca::i32>(c.num);
        return true;
    }
    return false;
}

/// 常量装载类别判定：cat2（long/double：LCONST/DCONST/LDC2_W）与 cat1
/// （int/float：iconst 族/bipush/sipush/fconst/ldc 族）。写窗口的值部分
/// 第一切片只认单条常量装载（常量表场景，见 pass 头注）。
bool constLoadMatches(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, bool wantCat2)
{
    using jbc::core::Op;
    using Kind = jbc::ir::ClassSession::ConstSem::Kind;
    if (wantCat2) {
        switch (in.op) {
        case Op::LCONST_0: case Op::LCONST_1:
        case Op::DCONST_0: case Op::DCONST_1: return true;
        default: break;
        }
        jbc::ir::ClassSession::ConstSem c;
        return session.constOf(in, c) && (c.kind == Kind::Long || c.kind == Kind::Double);
    }
    if (in.op == Op::FCONST_0 || in.op == Op::FCONST_1 || in.op == Op::FCONST_2) return true;
    ca::i32 unused = 0;
    if (intConstOf(session, in, unused)) return true;
    jbc::ir::ClassSession::ConstSem c;
    return session.constOf(in, c) && c.kind == Kind::Float;
}

/// 本类是否带 NestHost/NestMembers 属性（nest 语义下 private 可被 nest 内
/// 他类访问，本类扫描不闭包——整 pass 放弃）。
bool classHasNestAttr(const jbc::core::ClassFile& cf)
{
    for (const jbc::core::AttrSlot& slot : cf.attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, "NestHost") ||
            cf.cp.utf8Equals(slot.nameCp, "NestMembers")) {
            return true;
        }
    }
    return false;
}

/// 判定期记录的窗口（InsnId 句柄在 CodeUnitRef 存活期间稳定）。
struct Window
{
    enum class Kind : ca::u8 { Alloc, Load, Store };
    Kind            kind = Kind::Load;
    ca::usize       methodIdx = 0;      ///< unit.methods 下标
    jbc::ir::InsnId i0{}, i1{}, i2{}, i3{};   ///< Store 用满四个，其余三个
};

/// 单字段判定产物。
struct FieldPlan
{
    jbc::ir::FieldId          id{};
    std::string_view          name;
    std::string_view          arrayDesc;
    std::string_view          elemDesc;
    ca::i32                   len = -1;         ///< 分配长度
    std::vector<Window>       windows;
    std::unordered_set<ca::i32> accessed;       ///< 被访问下标
};

std::string scalarName(std::string_view base, ca::i32 index)
{
    std::string out(base);
    out += '_';
    out += std::to_string(index);
    return out;
}

}   // namespace

mjbase::spi::PreservedAnalyses StaticArrayFieldScalarizer::run(jbc::ir::ClassSession&            session,
                                                               mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    jbc::core::ClassFile& cf = session.backing();
    if (classHasNestAttr(cf)) return mjbase::spi::PreservedAnalyses::all();
    jbc::ir::ClassUnit&    unit     = session.unit();
    const std::string_view thisName = unit.symbols().str(unit.thisClass);

    // 候选字段：private static + 原始类型数组。字段名集用于新名冲突检查。
    std::vector<FieldPlan> plans;
    std::unordered_set<std::string_view> fieldNames;
    for (const jbc::ir::FieldUnit& f : unit.fields) {
        fieldNames.insert(unit.symbols().str(f.name));
    }
    for (const jbc::ir::FieldUnit& f : unit.fields) {
        if ((f.access & kAccPrivate) == 0 || (f.access & kAccStatic) == 0) continue;
        FieldPlan plan;
        plan.id        = f.id;
        plan.name      = unit.symbols().str(f.name);
        plan.arrayDesc = unit.symbols().str(f.desc);
        if (!primitiveArrayElemDesc(plan.arrayDesc, plan.elemDesc)) continue;
        plans.push_back(std::move(plan));
    }
    if (plans.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 全方法 promote 并保活：判定与改写共用同一批 CodeUnitRef（InsnId 稳定；
    // 重复 promote 会重建 CodeUnit 使句柄失效）。任一方法解码失败则本类
    // 指令面不可闭包，整体放弃。
    struct MethodDom
    {
        ca::usize           idx;
        jbc::ir::CodeUnitRef ref;
    };
    std::vector<MethodDom> doms;
    std::unordered_map<ca::usize, ca::usize> domOf;   ///< methodIdx → doms 下标
    for (ca::usize m = 0; m < unit.methods.size(); ++m) {
        if (!jbc::ir::hasCode(session, m)) continue;
        auto promoted = jbc::ir::promoteMethod(session, m);
        if (promoted.is_err()) return mjbase::spi::PreservedAnalyses::all();
        domOf.emplace(m, doms.size());
        doms.push_back({m, std::move(promoted).unwrap()});
    }

    // ── 判定阶段（零改写；任一失败整字段放弃，dead 标记）──
    std::vector<bool> dead(plans.size(), false);
    for (ca::usize p = 0; p < plans.size(); ++p) {
        FieldPlan& plan = plans[p];
        const jbc::core::Op loadOp   = arrayLoadOf(plan.elemDesc);
        const jbc::core::Op storeOp  = arrayStoreOf(plan.elemDesc);
        const bool          cat2Value = (plan.elemDesc == "J" || plan.elemDesc == "D");
        ca::usize           allocCount = 0;

        for (const MethodDom& md : doms) {
            const std::vector<jbc::ir::InsnId> order = md.ref->dom.order();
            for (ca::usize k = 0; k < order.size(); ++k) {
                const jbc::ir::Insn* in = md.ref->dom.peek(order[k]);
                if (in == nullptr) continue;
                jbc::ir::ClassSession::MemberSem ref;

                // putstatic f：分配窗口尾（前驱 newarray + 常量 LEN）或裸重赋。
                if (in->op == jbc::core::Op::PUTSTATIC && session.memberOf(*in, ref) &&
                    ref.owner == thisName && ref.name == plan.name && ref.desc == plan.arrayDesc) {
                    if (k < 2) { dead[p] = true; break; }
                    const jbc::ir::Insn* na = md.ref->dom.peek(order[k - 1]);
                    const jbc::ir::Insn* ln = md.ref->dom.peek(order[k - 2]);
                    ca::i32 len = 0;
                    if (na == nullptr || ln == nullptr || na->op != jbc::core::Op::NEWARRAY ||
                        na->a != atypeOf(plan.elemDesc) || !intConstOf(session, *ln, len)) {
                        dead[p] = true;   // 重赋整个数组或畸形分配
                        break;
                    }
                    plan.windows.push_back(
                        {Window::Kind::Alloc, md.idx, order[k - 2], order[k - 1], order[k], {}});
                    plan.len = len;
                    ++allocCount;
                    continue;
                }

                // getstatic f：读/写窗口头或整体引用。
                if (in->op == jbc::core::Op::GETSTATIC && session.memberOf(*in, ref) &&
                    ref.owner == thisName && ref.name == plan.name && ref.desc == plan.arrayDesc) {
                    const jbc::ir::Insn* idxInsn = k + 1 < order.size() ? md.ref->dom.peek(order[k + 1]) : nullptr;
                    ca::i32 i = 0;
                    if (idxInsn == nullptr || !intConstOf(session, *idxInsn, i)) {
                        dead[p] = true;   // 非常量下标（含整体引用/arraylength 前驱形态）
                        break;
                    }
                    const jbc::ir::Insn* third  = k + 2 < order.size() ? md.ref->dom.peek(order[k + 2]) : nullptr;
                    const jbc::ir::Insn* fourth = k + 3 < order.size() ? md.ref->dom.peek(order[k + 3]) : nullptr;
                    if (third != nullptr && third->op == loadOp) {
                        plan.windows.push_back(
                            {Window::Kind::Load, md.idx, order[k], order[k + 1], order[k + 2], {}});
                        plan.accessed.insert(i);
                        k += 2;           // 连同 const_i/xALOAD 跳过（下一兴趣点 k+3）
                        continue;
                    }
                    if (third != nullptr && constLoadMatches(session, *third, cat2Value) &&
                        fourth != nullptr && fourth->op == storeOp) {
                        plan.windows.push_back({Window::Kind::Store, md.idx, order[k], order[k + 1],
                                                order[k + 2], order[k + 3]});
                        plan.accessed.insert(i);
                        k += 3;
                        continue;
                    }
                    dead[p] = true;   // arraylength / 传参 / 返回 / 裸引用
                    break;
                }
            }
            if (dead[p]) break;
        }
        if (dead[p]) continue;

        // 汇总校验：唯一分配；有访问必须有分配（null 数组 NPE 语义）；
        // 下标域；标签锚定；新名冲突。失败置 dead，live 收集按 dead 过滤。
        if (allocCount != 1) { dead[p] = true; continue; }   // 0 分配（无访问即非本 pass 面）或多分配
        bool ok = true;
        for (const ca::i32 i : plan.accessed) {
            if (i < 0 || i >= plan.len) { ok = false; break; }
        }
        for (const Window& w : plan.windows) {
            if (!ok) break;
            const jbc::ir::CodeDom& dom = doms[domOf[w.methodIdx]].ref->dom;
            if (method_rewrite_util::labelsAnchorAt(dom, w.i0) ||
                method_rewrite_util::labelsAnchorAt(dom, w.i1) ||
                method_rewrite_util::labelsAnchorAt(dom, w.i2) ||
                (w.kind == Window::Kind::Store && method_rewrite_util::labelsAnchorAt(dom, w.i3))) {
                ok = false;
            }
        }
        if (ok) {
            for (const ca::i32 i : plan.accessed) {
                if (fieldNames.count(scalarName(plan.name, i)) != 0) { ok = false; break; }
            }
        }
        if (!ok) { dead[p] = true; continue; }
        // 纯分配零访问也保留：分配删除是等价化简（无人读的分配）。
    }

    std::vector<FieldPlan> live;
    for (ca::usize p = 0; p < plans.size(); ++p) {
        if (!dead[p]) live.push_back(std::move(plans[p]));
    }
    if (live.empty()) return mjbase::spi::PreservedAnalyses::all();

    // ── 改写阶段（判定期已排除失败面；InsnId 句柄稳定）──
    std::unordered_set<ca::usize> touchedMethods;
    for (const FieldPlan& plan : live) {
        for (const Window& w : plan.windows) {
            MethodDom& md = doms[domOf[w.methodIdx]];
            jbc::ir::CodeDom&    dom    = md.ref->dom;
            jbc::ir::CodeEditor  editor(*md.ref.get());
            ca::i32 index = 0;
            switch (w.kind) {
            case Window::Kind::Alloc:
                editor.erase(w.i0);
                editor.erase(w.i1);
                editor.erase(w.i2);
                break;
            case Window::Kind::Load:
                intConstOf(session, *dom.peek(w.i1), index);
                if (auto r = dom.get(w.i0); r.is_ok()) {
                    session.setMember(*r.unwrap(), jbc::ir::ClassSession::MemberSem::Kind::Field,
                                      thisName, scalarName(plan.name, index), plan.elemDesc);
                }
                editor.erase(w.i1);
                editor.erase(w.i2);
                break;
            case Window::Kind::Store:
                intConstOf(session, *dom.peek(w.i1), index);
                if (auto r = dom.get(w.i3); r.is_ok()) {
                    jbc::ir::Insn* insn = r.unwrap();
                    insn->op = jbc::core::Op::PUTSTATIC;
                    session.setMember(*insn, jbc::ir::ClassSession::MemberSem::Kind::Field,
                                      thisName, scalarName(plan.name, index), plan.elemDesc);
                }
                editor.erase(w.i0);
                editor.erase(w.i1);
                break;
            }
            touchedMethods.insert(w.methodIdx);
        }
    }

    // lower 全部改动方法；失败则会话弃置（backing 无半拆态）。
    for (const ca::usize m : touchedMethods) {
        if (jbc::ir::lowerMethod(session, m).is_err()) {
            return mjbase::spi::PreservedAnalyses::all();
        }
    }

    // 字段面：新标量字段按访问下标升序注入（确定性）+ 旧数组字段删除
    // （引用已清零；FieldId 身份不受 fields 向量序影响）。
    for (const FieldPlan& plan : live) {
        std::vector<ca::i32> idxList(plan.accessed.begin(), plan.accessed.end());
        for (ca::usize a = 1; a < idxList.size(); ++a) {
            for (ca::usize b = a; b > 0 && idxList[b - 1] > idxList[b]; --b) {
                const ca::i32 t = idxList[b - 1];
                idxList[b - 1]  = idxList[b];
                idxList[b]      = t;
            }
        }
        for (const ca::i32 i : idxList) {
            // FieldSeed::name 是借用视图：持有具名 std::string 直到 addField
            // 消化完（临时 string 悬垂会写出首字节被栈复用清零的名字）。
            const std::string newName = scalarName(plan.name, i);
            jbc::ir::FieldSeed seed;
            seed.name   = newName;
            seed.desc   = plan.elemDesc;
            seed.access = kAccPrivate | kAccStatic;
            if (session.addField(seed).is_err()) {
                return mjbase::spi::PreservedAnalyses::all();
            }
        }
        if (session.removeField(plan.id).is_err()) {
            return mjbase::spi::PreservedAnalyses::all();
        }
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
