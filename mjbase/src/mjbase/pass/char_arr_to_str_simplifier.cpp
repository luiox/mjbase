#include "mjbase/pass/char_arr_to_str_simplifier.hpp"

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

/// JVMS newarray 的 atype 码：T_CHAR = 5（CHAR 参与匹配，其余数组不参与）。
constexpr ca::i32 kAtypeChar = 5;

bool isFillConst(const jbc::ir::ClassSession& session, const Insn& in, ca::i32& out)
{
    return method_rewrite_util::intValueOf(session, in, out);
}

/// 匹配从 first 起的完整构建序列；命中时 members 装入全部成员（按序）、
/// codeUnits 装入逐元素 char 值（已按 CASTORE 截断语义取 u16）。
bool matchConstruction(const jbc::ir::ClassSession& session, CodeDom& dom,
                       const std::vector<InsnId>& ids, ca::usize first,
                       std::vector<InsnId>& members, std::vector<ca::u16>& codeUnits)
{
    const Insn* lenInsn = dom.peek(ids[first]);
    if (lenInsn == nullptr) return false;
    ca::i32 len = 0;
    if (!isFillConst(session, *lenInsn, len) || len < 0) return false;
    const Insn* newArray = dom.peek(ids[first + 1]);
    if (newArray == nullptr || newArray->op != Op::NEWARRAY || newArray->a != kAtypeChar)
    {
        return false;
    }

    members.clear();
    codeUnits.clear();
    members.push_back(ids[first]);
    members.push_back(ids[first + 1]);

    ca::usize p = first + 2;
    for (ca::i32 j = 0; j < len; ++j) {
        // 槽位越界 = 失配。
        if (p + 3 >= ids.size()) return false;
        const Insn* dup   = dom.peek(ids[p]);
        const Insn* idx   = dom.peek(ids[p + 1]);
        const Insn* ch    = dom.peek(ids[p + 2]);
        const Insn* store = dom.peek(ids[p + 3]);
        if (dup == nullptr || idx == nullptr || ch == nullptr || store == nullptr) return false;
        // 边界守卫：槽 2..4 的前置边界有标签锚定 → 整组失配。
        if (method_rewrite_util::labelsAnchorAt(dom, ids[p + 1])
            || method_rewrite_util::labelsAnchorAt(dom, ids[p + 2])
            || method_rewrite_util::labelsAnchorAt(dom, ids[p + 3]))
        {
            return false;
        }
        ca::i32 idxVal = 0;
        ca::i32 chVal  = 0;
        if (dup->op != Op::DUP || !isFillConst(session, *idx, idxVal) || idxVal != j
            || !isFillConst(session, *ch, chVal) || store->op != Op::CASTORE)
        {
            return false;
        }
        members.push_back(ids[p]);
        members.push_back(ids[p + 1]);
        members.push_back(ids[p + 2]);
        members.push_back(ids[p + 3]);
        codeUnits.push_back(static_cast<ca::u16>(chVal));   // JVM int→char 截断
        p += 4;
    }
    return true;
}

/// UTF-16 码元序列 → MUTF-8 字节。码元序列不可编码（不成代理等）时返回 false = 整组失配。
bool utf16ToMutf8(const std::vector<ca::u16>& codeUnits, std::string& out)
{
    ca::str::Utf16String utf16(codeUnits.data(), codeUnits.size());
    try {
        const std::string utf8 = utf16.to_utf8_string().ref().to_std_string();
        const auto mutf8 = jstr::utf8_to_mutf8(utf8);
        if (mutf8.is_err()) return false;
        out = mutf8.unwrap().bytes();
    }
    catch (...) {
        return false;
    }
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses CharArrToStrSimplifier::run(mjbase::spi::MethodScope&       scope,
                                                           mjbase::spi::MethodAnalysisManager& mam)
{
    jbc::ir::ClassSession& session = scope.session;
    CodeDom&               dom     = scope.code.dom;
    jbc::ir::CodeEditor    editor(scope.code);

    const std::vector<InsnId> ids = dom.order();
    bool                      modified = false;
    for (ca::usize i = 0; i + 1 < ids.size();) {
        std::vector<InsnId>  members;
        std::vector<ca::u16> codeUnits;
        if (!matchConstruction(session, dom, ids, i, members, codeUnits)) {
            ++i;
            continue;
        }
        std::string text;
        if (!utf16ToMutf8(codeUnits, text)) {
            ++i;
            continue;
        }

        // 替换头插到首成员之前；首成员锚定的标签迁移到替换头。CP intern 先于插入
        // 失败即整组跳过（池无半截追加——intern 自身失败会抛，到不了这）。
        jbc::ir::Insn ldc;
        ldc.op = Op::LDC;
        ldc.cp = session.internString(std::string_view(text));
        auto ldcId = editor.insertBefore(members.front(), ldc);
        if (ldcId.is_err()) {
            ++i;
            continue;
        }
        jbc::ir::Insn toCharArray;
        toCharArray.op = Op::INVOKEVIRTUAL;
        toCharArray.cp = session.internMethodRef("java/lang/String", "toCharArray", "()[C");
        if (editor.insertAfter(ldcId.unwrap(), toCharArray).is_err()) {
            ++i;
            continue;
        }
        for (const auto label : dom.labelsAt(members.front())) {
            (void)editor.bindLabel(label, ldcId.unwrap());
        }

        bool aborted = false;
        for (const InsnId member : members) {
            // 整组成员此刻均无锚定标签（守卫 + 迁移），直接删除；末成员
            // 允许是方法末条（plain erase 不需要后继）。
            if (editor.erase(member, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed).is_err()) {
                aborted = true;
                break;
            }
        }
        if (aborted) break;   // 已插入的替换头保留（多出的 toCharArray 调用
                              // 语义无害），失败语义归管线容错档位。
        modified = true;
        i += members.size();   // 快照推进：跳过整组被消费的成员
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
