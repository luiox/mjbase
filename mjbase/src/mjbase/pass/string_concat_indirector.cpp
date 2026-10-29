#include "mjbase/pass/string_concat_indirector.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;

constexpr std::string_view kSbClass = "java/lang/StringBuilder";
constexpr std::string_view kConcatBsmDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)"
    "Ljava/lang/invoke/CallSite;";
constexpr ca::u32 kMinClassMajor = 53;   // Java 9：StringConcatFactory 入场线

/// 拼接实参槽位（JVMS 4.7.23 makeConcatWithConstants 配方）。
constexpr char kRecipeArgSlot = '\x01';

/// 成员引用四段一致（指令码 + owner/name/desc）。
bool isMember(const ClassSession& session, const Insn& in, Op op,
              std::string_view owner, std::string_view name, std::string_view desc)
{
    if (in.op != op) return false;
    ClassSession::MemberSem m;
    if (!session.memberOf(in, m)) return false;
    return m.kind == ClassSession::MemberSem::Kind::Method && m.owner == owner
           && m.name == name && m.desc == desc;
}

/// 实参链一步的值收支（cat2 记一个值）；非链指令返回 false。纯面 =
/// 常量推入（condy 不认领——bootstrap 可抛）、原始局部装载族、二元纯
/// 算术与比较、取负与数值转换；栈操作/调用/字段/数组/分配不入链。
bool chainFlow(const ClassSession& session, const Insn& in, int& consume, int& produce)
{
    const auto inRange = [](Op v, Op first, Op last) { return v >= first && v <= last; };
    // 常量装载族（数值经窄化查询面、引用经 CP 语义面；condy 拒收）。
    {
        ca::i32          iv = 0;
        ca::i64          lv = 0;
        float            fv = 0.0F;
        double           dv = 0.0;
        std::string_view sv;
        if (method_rewrite_util::intValueOf(session, in, iv)
            || method_rewrite_util::longValueOf(session, in, lv)
            || method_rewrite_util::floatValueOf(session, in, fv)
            || method_rewrite_util::doubleValueOf(session, in, dv)
            || method_rewrite_util::stringValueOf(session, in, sv)) {
            consume = 0;
            produce = 1;
            return true;
        }
    }
    if (in.op == Op::ACONST_NULL) {
        consume = 0;
        produce = 1;
        return true;
    }
    {
        ClassSession::ConstSem cs;
        if (session.constOf(in, cs)) {
            using K = ClassSession::ConstSem::Kind;
            switch (cs.kind) {
            case K::Int:
            case K::Long:
            case K::Float:
            case K::Double:
            case K::String:
            case K::Class:
            case K::MethodType:
            case K::MethodHandle:
                consume = 0;
                produce = 1;
                return true;
            case K::Dynamic: return false;   // condy 实参不认领
            }
        }
    }
    // 原始局部装载族（引用装载 ALOAD 静态型未知不入链）。
    if ((in.op == Op::ILOAD || inRange(in.op, Op::ILOAD_0, Op::ILOAD_3))
        || (in.op == Op::LLOAD || inRange(in.op, Op::LLOAD_0, Op::LLOAD_3))
        || (in.op == Op::FLOAD || inRange(in.op, Op::FLOAD_0, Op::FLOAD_3))
        || (in.op == Op::DLOAD || inRange(in.op, Op::DLOAD_0, Op::DLOAD_3))) {
        consume = 0;
        produce = 1;
        return true;
    }
    // 二元纯算术/移位/位/比较：两值进一出。
    switch (in.op) {
    case Op::IADD: case Op::ISUB: case Op::IMUL: case Op::IDIV:
    case Op::IREM: case Op::IAND: case Op::IOR: case Op::IXOR:
    case Op::ISHL: case Op::ISHR: case Op::IUSHR:
    case Op::LADD: case Op::LSUB: case Op::LMUL: case Op::LDIV:
    case Op::LREM: case Op::LAND: case Op::LOR: case Op::LXOR:
    case Op::LSHL: case Op::LSHR: case Op::LUSHR:
    case Op::FADD: case Op::FSUB: case Op::FMUL: case Op::FDIV:
    case Op::FREM:
    case Op::DADD: case Op::DSUB: case Op::DMUL: case Op::DDIV:
    case Op::DREM:
    case Op::LCMP: case Op::FCMPL: case Op::FCMPG: case Op::DCMPL:
    case Op::DCMPG:
        consume = 2;
        produce = 1;
        return true;
    default: break;
    }
    // 取负与数值转换：一值进出。
    switch (in.op) {
    case Op::INEG: case Op::LNEG: case Op::FNEG: case Op::DNEG:
    case Op::I2L: case Op::I2F: case Op::I2D: case Op::I2B: case Op::I2C:
    case Op::I2S:
    case Op::L2I: case Op::L2F: case Op::L2D:
    case Op::F2I: case Op::F2L: case Op::F2D:
    case Op::D2I: case Op::D2L: case Op::D2F:
        consume = 1;
        produce = 1;
        return true;
    default: break;
    }
    return false;
}

/// 实参纯链前向认领：自 a 起贪心收集纯链前缀（验证器保证链为自包含
/// 表达式、净效应恰 +1 值且 append 紧随其后——链内深度不受限，binop 需
/// 两值在栈，收尾深度必须恰 1）。返回链尾位（append 位）；空链/形状
/// 不闭合返回 a。
ca::usize claimArgChainFwd(const ClassSession& session, const CodeDom& dom,
                           const std::vector<InsnId>& order, ca::usize a)
{
    const ca::usize n = order.size();
    int             depth = 0;
    ca::usize       p     = a;
    while (p < n) {
        const Insn* in = dom.peek(order[p]);
        if (in == nullptr) break;
        int consume = 0;
        int produce = 0;
        if (!chainFlow(session, *in, consume, produce)) break;
        if (consume == 2 && depth < 2) return a;   // binop 缺操作数
        depth += produce - consume;
        ++p;
    }
    return depth == 1 ? p : a;
}

/// 追加族指令的单实参类型；非「恰一参、返回 StringBuilder」的 append 返回
/// nullopt。
std::optional<std::string_view> appendParamDesc(const ClassSession& session,
                                                const Insn&         in)
{
    if (in.op != Op::INVOKEVIRTUAL) return std::nullopt;
    ClassSession::MemberSem m;
    if (!session.memberOf(in, m)) return std::nullopt;
    if (m.kind != ClassSession::MemberSem::Kind::Method || m.owner != kSbClass
        || m.name != "append") {
        return std::nullopt;
    }
    constexpr std::string_view kRet = "Ljava/lang/StringBuilder;";
    // 形状：(P)Ljava/lang/StringBuilder; —— 恰一参且返回型精确。
    if (m.desc.size() < 2 + 1 + kRet.size() || m.desc.front() != '(') return std::nullopt;
    if (m.desc.substr(m.desc.size() - kRet.size()) != kRet) return std::nullopt;
    const std::string_view params =
        m.desc.substr(1, m.desc.size() - 1 - kRet.size() - 1);
    // 单参数：恰一个字段描述符项（数组前缀归并 + 对象终缀）。
    ca::usize i = 0;
    while (i < params.size() && params[i] == '[') ++i;
    if (i >= params.size()) return std::nullopt;
    if (params[i] == 'L') {
        while (i < params.size() && params[i] != ';') ++i;
        if (i >= params.size()) return std::nullopt;
    }
    if (++i != params.size()) return std::nullopt;   // 多参/空参不认领
    return params;
}

}   // namespace

mjbase::spi::PreservedAnalyses StringConcatIndirector::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    if (scope.session.backing().major < kMinClassMajor) {
        return mjbase::spi::PreservedAnalyses::all();
    }
    CodeDom&                  dom   = scope.code.dom;
    const std::vector<InsnId> order = dom.order();
    if (order.size() < 4) return mjbase::spi::PreservedAnalyses::all();

    // 窗口收集：非重叠、逐面门禁全过才成案。
    struct ConcatWindow
    {
        ca::usize                     newpos = 0;      // NEW 位（入批整擦）
        ca::usize                     toStringPos = 0; // INDY 落位
        std::vector<InsnId>           erase;   // NEW/dup/init/appends
        std::vector<InsnId>           args;    // 实参指令（保留，随窗门禁）
        std::vector<std::string_view> argDescs;   // indy 实参静态型，栈序
    };
    std::vector<ConcatWindow> windows;
    std::set<ca::u32>         claimed;
    const auto                isClaimed = [&](ca::usize p) {
        return claimed.count(order[p].slot) != 0;
    };

    for (ca::usize i = 0; i + 3 < order.size(); ++i) {
        const Insn* newInsn = dom.peek(order[i]);
        if (newInsn == nullptr || newInsn->op != Op::NEW) continue;
        if (scope.session.classOf(*newInsn) != kSbClass) continue;
        if (isClaimed(i)) continue;

        const Insn* dup = dom.peek(order[i + 1]);
        if (dup == nullptr || dup->op != Op::DUP) continue;

        ConcatWindow w;
        w.newpos = i;
        w.erase.push_back(order[i]);      // NEW 整擦（INDY 落 toString 位）
        w.erase.push_back(order[i + 1]);

        // [LDC String]?（保留为 indy 首实参）+ <init>。
        ca::usize   p     = i + 2;
        const Insn* maybe = p < order.size() ? dom.peek(order[p]) : nullptr;
        bool        initStr = false;
        if (maybe != nullptr) {
            std::string_view sv;
            if (method_rewrite_util::stringValueOf(scope.session, *maybe, sv)) {
                initStr = true;
                w.args.push_back(order[p]);
                w.argDescs.push_back(std::string_view("Ljava/lang/String;"));
                ++p;
            }
        }
        const Insn* init = p < order.size() ? dom.peek(order[p]) : nullptr;
        if (init == nullptr) continue;
        if (!isMember(scope.session, *init, Op::INVOKESPECIAL, kSbClass, "<init>",
                      initStr ? "(Ljava/lang/String;)V" : "()V")) {
            continue;
        }
        w.erase.push_back(order[p]);
        ++p;

        // (CHAIN; append)+ —— 实参为自包含纯计算链（反向需值抽取；栈操
        // 作/调用/字段/数组/condy 不入链），append 恰一参。静态型取
        // append 形参描述符——原方法经验证器，链顶值与形参匹配有保证；
        // 链指令保留原位（改写后在更低栈深执行，maxStack 天然覆盖）。
        while (true) {
            if (p >= order.size()) break;
            const ca::usize chainEnd = claimArgChainFwd(scope.session, dom, order, p);
            if (chainEnd == p) break;   // 空链：无实参或链外指令截断
            const Insn* app = dom.peek(order[chainEnd]);
            if (app == nullptr) break;
            const std::optional<std::string_view> paramDesc =
                appendParamDesc(scope.session, *app);
            if (!paramDesc.has_value()) break;
            for (ca::usize q = p; q < chainEnd; ++q) {
                w.args.push_back(order[q]);
            }
            w.argDescs.push_back(*paramDesc);
            w.erase.push_back(order[chainEnd]);
            p = chainEnd + 1;
        }
        // toString 收尾（至少一次 append 才成窗——新窗起点约束同排空）。
        // INDY 落位在 toString：栈机按序执行，实参指令保留原位先压栈，
        // INDY 末位消费——原 new/dup/init/append 面整擦后栈序恰好成立。
        const Insn* ts = p < order.size() ? dom.peek(order[p]) : nullptr;
        if (ts == nullptr
            || !isMember(scope.session, *ts, Op::INVOKEVIRTUAL, kSbClass, "toString",
                         "()Ljava/lang/String;")) {
            continue;
        }
        w.toStringPos = p;

        // 边界守卫：窗内任何成员（擦除面 + 实参位）锚定存活标签（分支/
        // 异常表）→ 整窗拒收——分支目标进入链体在改写后语义不保。
        bool labelsClean = true;
        for (const InsnId id : w.erase) {
            if (method_rewrite_util::labelsAnchorAt(dom, id)) {
                labelsClean = false;
                break;
            }
        }
        if (labelsClean) {
            for (const InsnId id : w.args) {
                if (method_rewrite_util::labelsAnchorAt(dom, id)) {
                    labelsClean = false;
                    break;
                }
            }
        }
        if (!labelsClean) continue;

        for (const InsnId id : w.erase) claimed.insert(id.slot);
        claimed.insert(order[i].slot);
        windows.push_back(std::move(w));
    }

    if (windows.empty()) return mjbase::spi::PreservedAnalyses::all();

    // maxes 面：改写后窗体最深点 = 窗口入口栈深 + 全部实参槽宽（cat2 记
    // 2）——原始 SB 形的最深点在 NEW;DUP;LDC 段，javac 可给出比新形更小
    // 的 maxStack（管线不重算 maxes 时终类 VerifyError）。帧引擎一次
    // （BestEffort 形状档只消费深度）；失败整件跳过。
    {
        jbc::analysis::FrameEngine          engine(scope.session.backing().cp);
        std::vector<jbc::analysis::FrameAt> frames;
        const jbc::analysis::FrameSummary   summary = engine.compute(scope.code, frames);
        if (!summary.ok || frames.size() != order.size()) {
            return mjbase::spi::PreservedAnalyses::all();
        }
        for (ConcatWindow& w : windows) {
            if (!frames[w.newpos].reachable) return mjbase::spi::PreservedAnalyses::all();
            ca::u32 slots = 0;
            for (const std::string_view d : w.argDescs) {
                slots += (!d.empty() && (d[0] == 'J' || d[0] == 'D')) ? 2u : 1u;
            }
            const ca::u64 need = static_cast<ca::u64>(frames[w.newpos].stack.size())
                                 + slots;
            if (need > 0xFFFFu) return mjbase::spi::PreservedAnalyses::all();
            if (scope.code.maxStack < static_cast<ca::u16>(need)) {
                scope.code.maxStack = static_cast<ca::u16>(need);
            }
        }
    }

    // 应用：逐窗独立——BSM/indy 条目 intern（失败弃窗）、eraseBatch 组级
    // 原子擦除（失败弃窗，NEW 原样）、NEW 原位改写 INDY（原位改写不可
    // 失败）。INDY 落位在 NEW 位置：全部拼接实参在其后按原位压栈，顺序
    // 即栈序。
    bool modified = false;
    jbc::ir::CodeEditor editor(scope.code);
    for (ConcatWindow& w : windows) {
        // 配方：全实参槽位形态（常量嵌入属后续精化）。
        std::string recipe;
        recipe.reserve(w.argDescs.size());
        recipe.append(w.argDescs.size(), kRecipeArgSlot);

        std::string indyDesc;
        indyDesc.push_back('(');
        for (const std::string_view d : w.argDescs) indyDesc.append(d);
        indyDesc.append(")Ljava/lang/String;");

        jbc::ir::MethodHandleSpec bsm;
        bsm.referenceKind = 6;   // INVOKESTATIC
        bsm.owner         = "java/lang/invoke/StringConcatFactory";
        bsm.name          = "makeConcatWithConstants";
        bsm.desc          = kConcatBsmDesc;

        std::vector<jbc::ir::BsmArg> bsmArgs(1);
        bsmArgs[0].kind = jbc::ir::BsmArg::Kind::String;
        bsmArgs[0].str  = recipe;
        const auto bsmIdx = scope.session.appendBootstrapMethod(bsm, bsmArgs);
        if (bsmIdx.is_err()) continue;
        // indy CP 条目走底层池面——session.internDynamic 是 CONSTANT_Dynamic
        // （condy，tag 17）语义，v53 类 JVM 拒载；indy 是 tag 18。
        jbc::core::ConstantPool& pool = scope.session.backing().cp;
        const ca::u32 natCp  = pool.addNameAndType(pool.addUtf8("makeConcatWithConstants"),
                                                   pool.addUtf8(indyDesc));
        const ca::u32 indyCp = pool.addInvokeDynamic(bsmIdx.unwrap(), natCp);

        if (editor
                .eraseBatch(w.erase,
                            jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                .is_err()) {
            continue;
        }
        auto got = dom.get(order[w.toStringPos]);
        if (got.is_err()) continue;
        Insn* in = std::move(got).unwrap();
        in->op     = Op::INVOKEDYNAMIC;
        in->cp     = indyCp;
        in->a      = 0;
        in->b      = 0;
        in->wide   = false;
        in->target = jbc::ir::kNoLabel;
        in->targets.clear();
        in->keys.clear();
        modified = true;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
