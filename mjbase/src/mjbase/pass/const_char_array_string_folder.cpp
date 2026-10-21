#include "mjbase/pass/const_char_array_string_folder.hpp"

#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "jstr/conversion.hpp"
#include "libca/str/utf16_string.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// JVMS newarray 的 atype 码：T_CHAR = 5（仅 char 数组参与折叠）。
constexpr ca::i32 kAtypeChar = 5;

/// 窗内指令的常量整型求值片段：消费若干条指令，把值压入 stack。
/// 载入与运算面与旧栈 evalIntStack 逐条对齐（见 hpp 注释）。
bool evalConstStep(const jbc::ir::ClassSession& session, const Insn& in, std::vector<ca::i32>& stack)
{
    using jbc::ir::ClassSession;

    ca::i32 loaded = 0;
    if (method_rewrite_util::intValueOf(session, in, loaded)) {
        stack.push_back(loaded);
        return true;
    }

    const auto binary = [&](ca::i32 (*fn)(ca::u32, ca::u32)) -> bool {
        if (stack.size() < 2) return false;
        const ca::u32 b = static_cast<ca::u32>(stack.back());
        stack.pop_back();
        const ca::u32 a = static_cast<ca::u32>(stack.back());
        stack.pop_back();
        stack.push_back(fn(a, b));
        return true;
    };
    const auto unary = [&](ca::i32 (*fn)(ca::u32)) -> bool {
        if (stack.empty()) return false;
        stack.back() = fn(static_cast<ca::u32>(stack.back()));
        return true;
    };

    bool ok = true;
    switch (in.op) {
    case Op::IADD:
        ok = binary([](ca::u32 a, ca::u32 b) { return static_cast<ca::i32>(a + b); });
        break;
    case Op::ISUB:
        ok = binary([](ca::u32 a, ca::u32 b) { return static_cast<ca::i32>(a - b); });
        break;
    case Op::IMUL:
        ok = binary([](ca::u32 a, ca::u32 b) { return static_cast<ca::i32>(a * b); });
        break;
    case Op::IXOR:
        ok = binary([](ca::u32 a, ca::u32 b) { return static_cast<ca::i32>(a ^ b); });
        break;
    case Op::IAND:
        ok = binary([](ca::u32 a, ca::u32 b) { return static_cast<ca::i32>(a & b); });
        break;
    case Op::IOR:
        ok = binary([](ca::u32 a, ca::u32 b) { return static_cast<ca::i32>(a | b); });
        break;
    case Op::ISHL:
        ok = binary([](ca::u32 a, ca::u32 b) { return static_cast<ca::i32>(a << (b & 31u)); });
        break;
    case Op::ISHR:
        ok = binary([](ca::u32 a, ca::u32 b) {
            return static_cast<ca::i32>(static_cast<ca::i32>(a) >> (b & 31u));
        });
        break;
    case Op::IUSHR:
        ok = binary([](ca::u32 a, ca::u32 b) {
            return static_cast<ca::i32>(a >> (b & 31u));
        });
        break;
    case Op::INEG:
        ok = unary([](ca::u32 a) { return static_cast<ca::i32>(0u - a); });
        break;
    case Op::I2C:
        ok = unary([](ca::u32 a) { return static_cast<ca::i32>(a & 0xFFFFu); });
        break;
    case Op::I2B:
        ok = unary([](ca::u32 a) {
            return static_cast<ca::i32>(static_cast<ca::i8>(a));
        });
        break;
    case Op::I2S:
        ok = unary([](ca::u32 a) {
            return static_cast<ca::i32>(static_cast<ca::i16>(a));
        });
        break;
    default:
        return false;
    }
    return ok;
}

// 在快照 ids 的 [startPos, ...) 上求值常量整数栈，直到 op == terminator 的
// 指令（终止指令本身不消费）。窗内任一成员锚定 incoming 标签即失配（旧栈
// LABEL/LINE/FRAME 节点失配面的新栈等价形态）；终遇不支持指令、栈下溢或
// 越过结尾都判失配。成功时 termPos 指向终止指令位置。
bool evalIntWindow(const jbc::ir::ClassSession& session, const CodeDom& dom,
                   const std::vector<InsnId>& ids, ca::usize startPos, Op terminator,
                   std::vector<ca::i32>& stack, ca::usize& termPos)
{
    stack.clear();
    for (ca::usize p = startPos; p < ids.size(); ++p) {
        const Insn* n = dom.peek(ids[p]);
        if (n == nullptr) return false;
        // 锚定标签 = 控制流可从中间进入。
        if (method_rewrite_util::labelsAnchorAt(dom, ids[p])) return false;
        if (n->op == terminator) {
            termPos = p;
            return true;
        }
        if (!evalConstStep(session, *n, stack)) return false;
    }
    return false;
}

/// UTF-16 码元序列 → MUTF-8 字节（与旧栈 Utf16String → UTF-8 → writer 编码
/// 同构，接 CharArrToStrSimplifier 同款）。不可编码（不成代理等）返回 false
/// = 整组失配。
bool utf16ToMutf8(const std::vector<ca::u16>& codeUnits, std::string& out)
{
    ca::str::Utf16String utf16(codeUnits.data(), codeUnits.size());
    try {
        const std::string utf8 = utf16.to_utf8_string().ref().to_std_string();
        const auto       mutf8 = jstr::utf8_to_mutf8(utf8);
        if (mutf8.is_err()) return false;
        out = mutf8.unwrap().bytes();
    }
    catch (...) {
        return false;
    }
    return true;
}

// 匹配从 ids[i] 起的 `new String(常量 char[])` 形状。命中时 members 装入
// [i, init] 的完整连续成员（含尾部 invokespecial）、text 装入解码后的
// MUTF-8 字节；shape 不合、求值失败、覆盖不完整、非法 surrogate 都失配
// （整组原指令保留）。
bool matchNewStringCharArray(const jbc::ir::ClassSession& session, const CodeDom& dom,
                             const std::vector<InsnId>& ids, ca::usize i,
                             std::vector<InsnId>& members, std::string& text)
{
    const Insn* nNew = dom.peek(ids[i]);
    if (nNew == nullptr || nNew->op != Op::NEW) return false;
    if (session.classOf(*nNew) != "java/lang/String") return false;

    if (i + 1 >= ids.size()) return false;
    const Insn* nDup = dom.peek(ids[i + 1]);
    if (nDup == nullptr || nDup->op != Op::DUP) return false;
    // 首成员与次成员之间不允许标签。
    if (method_rewrite_util::labelsAnchorAt(dom, ids[i + 1])) return false;
    members.push_back(ids[i]);
    members.push_back(ids[i + 1]);

    std::vector<ca::i32> stack;
    ca::usize            newArrayPos = 0;
    if (!evalIntWindow(session, dom, ids, i + 2, Op::NEWARRAY, stack, newArrayPos)) return false;
    if (stack.size() != 1) return false;
    const ca::i32 length = stack[0];
    if (length < 0 || length > 65535) return false;

    const Insn* nArray = dom.peek(ids[newArrayPos]);
    if (nArray == nullptr || nArray->op != Op::NEWARRAY || nArray->a != kAtypeChar) return false;
    for (ca::usize k = i + 2; k <= newArrayPos; ++k) members.push_back(ids[k]);

    // -1 作哨兵表示未写入；同下标多次 castore 取最后一次（Java 语义）。
    std::vector<ca::i32> units(static_cast<ca::usize>(length), -1);
    ca::usize            pos = newArrayPos + 1;
    for (std::size_t k = 0; k < units.size(); ++k) {
        if (pos >= ids.size()) return false;
        const Insn* dup = dom.peek(ids[pos]);
        if (dup == nullptr || dup->op != Op::DUP) return false;
        if (method_rewrite_util::labelsAnchorAt(dom, ids[pos])) return false;
        members.push_back(ids[pos]);

        std::size_t castorePos = 0;
        if (!evalIntWindow(session, dom, ids, pos + 1, Op::CASTORE, stack, castorePos)) return false;
        if (stack.size() != 2) return false;

        const ca::i32 idx = stack[0];
        const ca::i32 val = stack[1];
        if (idx < 0 || idx >= length) return false;
        units[static_cast<ca::usize>(idx)] =
            static_cast<ca::i32>(static_cast<ca::u32>(val) & 0xFFFFu);
        for (ca::usize w = pos + 1; w <= castorePos; ++w) members.push_back(ids[w]);
        pos = castorePos + 1;
    }

    // 全部下标须恰好覆盖（存在未写入哨兵即整组保留）。
    for (const auto unit : units) {
        if (unit < 0) return false;
    }

    if (pos >= ids.size()) return false;
    const Insn* nInit = dom.peek(ids[pos]);
    if (nInit == nullptr || nInit->op != Op::INVOKESPECIAL) return false;
    jbc::ir::ClassSession::MemberSem init;
    if (!session.memberOf(*nInit, init) || init.owner != "java/lang/String"
        || init.name != "<init>" || init.desc != "([C)V") {
        return false;
    }
    if (method_rewrite_util::labelsAnchorAt(dom, ids[pos])) return false;
    members.push_back(ids[pos]);

    std::vector<ca::u16> codeUnits(static_cast<ca::usize>(length));
    for (ca::usize k = 0; k < codeUnits.size(); ++k) {
        codeUnits[k] = static_cast<ca::u16>(units[k]);
    }
    return utf16ToMutf8(codeUnits, text);
}

}   // namespace

mjbase::spi::PreservedAnalyses ConstCharArrayStringFolder::run(mjbase::spi::MethodScope&       scope,
                                                               mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::ClassSession& session = scope.session;
    CodeDom&               dom     = scope.code.dom;
    jbc::ir::CodeEditor    editor(scope.code);

    const std::vector<InsnId> ids = dom.order();
    bool                      modified = false;
    for (ca::usize i = 0; i < ids.size();) {
        std::vector<InsnId> members;
        std::string         text;
        if (!matchNewStringCharArray(session, dom, ids, i, members, text)) {
            ++i;
            continue;
        }

        // 替换头插到首成员（NEW）之前；首成员锚定的标签迁移到新 ldc（旧栈
        // LabelNode 留原位、折叠结果顶替原序列头的落点语义）。池下标超 u1
        // 域升格 ldc_w（比照 ClassSession::setString 的宽度陷阱口径）——
        // 本 pass 产出的 String 常量视角上应等于池中真实形态。
        jbc::ir::Insn ldc;
        const ca::u32 cp = session.internString(text);
        ldc.op           = cp <= 0xFF ? Op::LDC : Op::LDC_W;
        ldc.cp           = cp;
        auto ldcId       = editor.insertBefore(members.front(), ldc);
        if (ldcId.is_err()) {
            ++i;
            continue;
        }
        for (const auto label : dom.labelsAt(members.front())) {
            (void)editor.bindLabel(label, ldcId.unwrap());
        }

        // 守卫保证替换头之外全部成员无锚定标签，异常表引用由 use 保护兜底；
        // erase 批删原子失败即中止（已插入的 ldc 保留，语义无害——末尾
        // invokespecial 被折叠语义吞掉属于管线容错档位，不在成功路径）。
        if (!editor.eraseBatch(members, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed).is_ok()) {
            break;
        }
        modified = true;
        i += members.size();   // 快照推进：跳过整组被消费的成员
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
