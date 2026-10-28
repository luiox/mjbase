#include "mjbase/pass/stack_trace_recover.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;

constexpr std::string_view kThrowable          = "java/lang/Throwable";
constexpr std::string_view kPrintStackTrace    = "printStackTrace";
constexpr std::string_view kPrintStackTraceSig = "()V";
constexpr ca::u32         kLineWrapThreshold  = 65535;   // 旧栈 32767-(-32768)

/// Utf8 查加一体（命中既有池不膨胀，miss 才追加）。
ca::u32 internUtf8(jbc::core::ClassFile& cf, std::string_view s)
{
    if (ca::u32 idx = cf.cp.findUtf8(s); idx != 0) return idx;
    return cf.cp.addUtf8(s);
}

/// 类内名 → 源文件名（对齐旧栈 name.replace('/', ".") + ".java"）。
std::string sourceFileName(std::string_view internalName)
{
    std::string out;
    out.reserve(internalName.size() + 5);
    for (const char c : internalName) out.push_back(c == '/' ? '.' : c);
    out.append(".java");
    return out;
}

/// 按名找成员属性槽（类级有 findClassAttr，成员级线性扫描同形）。
jbc::core::AttrSlot* findMemberAttr(jbc::core::ClassFile&        cf,
                                    std::vector<jbc::core::AttrSlot>& attrs,
                                    std::string_view                name)
{
    for (jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// SourceFile/SourceDebug 覆盖式写入（已有槽改写、缺失槽新建）。
bool writeSourceAttrs(jbc::core::ClassFile& cf, const std::string& file)
{
    const ca::u32 fileUtf8 = internUtf8(cf, file);

    jbc::core::AttrSlot* sf = cf.findClassAttr("SourceFile");
    if (sf == nullptr) {
        auto body   = std::make_unique<jbc::core::SourceFileAttr>();
        body->sourceFileCp = fileUtf8;
        cf.attrs.push_back(jbc::core::AttrSlot::makeNew(
            internUtf8(cf, "SourceFile"), std::move(body)));
    } else {
        if (cf.decodeAttr(*sf).is_err()) return false;
        auto* mut = sf->mutate();
        if (mut == nullptr) return false;
        static_cast<jbc::core::SourceFileAttr*>(mut)->sourceFileCp = fileUtf8;
    }

    // SourceDebug（JSR-45 非标准属性）：载荷 = u2 Utf8 索引，RawAttr 承载。
    jbc::core::AttrSlot* sd = cf.findClassAttr("SourceDebug");
    const std::vector<ca::u8> payload{static_cast<ca::u8>(fileUtf8 >> 8),
                                      static_cast<ca::u8>(fileUtf8)};
    if (sd == nullptr) {
        cf.attrs.push_back(jbc::core::AttrSlot::makeNew(
            internUtf8(cf, "SourceDebug"),
            std::make_unique<jbc::core::RawAttr>(payload)));
    } else {
        sd->body  = std::make_unique<jbc::core::RawAttr>(payload);
        sd->state = jbc::core::AttrSlot::State::Modified;
    }
    return true;
}

/// 逐类恢复：source 属性 + handler 打印调用 + LNT 行。任一步失败返回
/// false（整件放弃，不留半截容器变更）。
bool recoverClass(jbc::ir::ClassSession& session)
{
    jbc::core::ClassFile& backing = session.backing();
    if (!writeSourceAttrs(backing, sourceFileName(backing.thisName()))) return false;

    jbc::ir::ClassUnit& unit = session.unit();
    ca::u32             line = 1;   // 类级跨方法递增（对齐旧栈）
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (!jbc::ir::hasCode(session, idx)) continue;
        auto promoted = jbc::ir::promoteMethod(session, idx);
        if (promoted.is_err()) return false;
        jbc::ir::CodeUnit&                 code = *promoted.unwrap().get();
        const std::vector<jbc::ir::InsnId> ids  = code.dom.order();

        // 标签锚点表（handler 定位 + LNT 行生成共用）；行按标签逐条
        // 递增（同边界多标签多行，对齐旧栈每 LabelNode 一条 LNN）。
        std::vector<std::pair<jbc::ir::LabelId, jbc::ir::InsnId>> labelAnchors;
        std::vector<jbc::core::LineNumberTableAttr::Row>          rows;
        for (const jbc::ir::InsnId id : ids) {
            const jbc::ir::Insn* in = code.dom.peek(id);
            if (in == nullptr) return false;
            for (const jbc::ir::LabelId label : code.dom.labelsAt(id)) {
                labelAnchors.emplace_back(label, id);
                rows.push_back({in->srcPc, line});
                line += 1;
                if (line > kLineWrapThreshold) line = 1;
            }
        }
        const auto anchorOf = [&](jbc::ir::LabelId label) -> jbc::ir::InsnId {
            for (const auto& [l, id] : labelAnchors) {
                if (l == label) return id;
            }
            return jbc::ir::kNoInsn;
        };

        // handler 聚合：单型取该型；catch-all 或多型共享取 Throwable。聚合
        // 键取 handler 锚定指令——旧栈 LabelNode 按 pc 去重，同一边界的
        // 异常表条目必落同一节点，IR 侧 LabelId 异同不作数。
        struct HandlerOwner
        {
            jbc::ir::InsnId anchor;
            std::string     owner;
            bool            conflicted = false;
        };
        std::vector<HandlerOwner> handlers;
        for (const jbc::ir::CodeUnit::Exc& exc : code.exc) {
            const jbc::ir::InsnId anchor = anchorOf(exc.handler);
            if (anchor == jbc::ir::kNoInsn) continue;   // 未锚定 handler 跳过（旧栈同形）
            const std::string type = exc.catchCp == 0
                                         ? std::string{}
                                         : std::string{backing.className(exc.catchCp)};
            auto hit = std::find_if(handlers.begin(), handlers.end(),
                                    [&](const HandlerOwner& h) {
                                        return h.anchor == anchor;
                                    });
            if (hit == handlers.end()) {
                handlers.push_back({anchor, type, false});
            } else if (hit->owner != type) {
                hit->conflicted = true;
            }
        }

        bool touched = false;
        for (const HandlerOwner& handler : handlers) {
            const jbc::ir::InsnId anchor = handler.anchor;
            const std::string owner =
                (handler.conflicted || handler.owner.empty())
                    ? std::string{kThrowable}
                    : handler.owner;
            const ca::u32 mref = session.internMethodRef(owner, kPrintStackTrace,
                                                         kPrintStackTraceSig);
            jbc::ir::CodeEditor editor(code);
            auto dup = editor.insertBefore(anchor, jbc::ir::CodeEditor::simple(Op::DUP));
            if (dup.is_err()) return false;
            if (editor
                    .insertAfter(dup.unwrap(),
                                 jbc::ir::CodeEditor::cpInsn(Op::INVOKEVIRTUAL, mref))
                    .is_err()) {
                return false;
            }
            touched = true;
        }
        if (touched) {
            code.maxStack = static_cast<ca::u16>(code.maxStack + 1);
        }

        if (rows.empty()) continue;
        // LNT 行直写 backing Code 属性（旧 pc 坐标，lower 前落位）：布局
        // 变化由 lowerMethod 按 pcmap 重定向；未变化时坐标恒等。
        jbc::core::MemberInfo& member = backing.methods[unit.methods[idx].backingIndex];
        auto codeDecoded = backing.decodeCode(member);
        if (codeDecoded.is_err()) return false;
        jbc::core::CodeAttr* codeAttr = codeDecoded.unwrap();
        const ca::u32 lntName = internUtf8(backing, "LineNumberTable");
        jbc::core::AttrSlot* lntSlot =
            jbc::core::ClassFile::findAttrByCp(codeAttr->attrs, lntName);
        if (lntSlot == nullptr) {
            auto body = std::make_unique<jbc::core::LineNumberTableAttr>();
            for (const auto& row : rows) body->rows.push_back(row);
            codeAttr->attrs.push_back(
                jbc::core::AttrSlot::makeNew(lntName, std::move(body)));
        } else {
            if (backing.decodeAttr(*lntSlot).is_err()) return false;
            auto* lnt = static_cast<jbc::core::LineNumberTableAttr*>(lntSlot->mutate());
            if (lnt == nullptr) return false;
            for (const auto& row : rows) lnt->rows.push_back(row);
        }
        // 外层 Code 槽置脏（writer 按 needsReencode 重编码含嵌套属性）。
        if (jbc::core::AttrSlot* codeSlot =
                findMemberAttr(backing, member.attrs, "Code")) {
            codeSlot->state = jbc::core::AttrSlot::State::Modified;
        } else {
            return false;
        }
        if (touched && jbc::ir::lowerMethod(session, idx).is_err()) return false;
    }
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses StackTraceRecover::run(
    mjbase::model::ResourceContainer&   container,
    mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
    };
    std::vector<PendingWrite> pending;
    for (auto& [uri, res] : container.getResources()) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(res.get());
        if (cls == nullptr) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) return mjbase::spi::PreservedAnalyses::all();
        auto session = std::move(sessionResult).unwrap();
        if (!recoverClass(*session)) return mjbase::spi::PreservedAnalyses::all();
        auto written = jbc::core::writeClass(session->backing());
        if (written.is_err()) return mjbase::spi::PreservedAnalyses::all();
        pending.push_back({std::string{uri}, std::move(written).unwrap()});
    }
    for (PendingWrite& item : pending) {
        container.replaceResource(
            item.uri,
            std::make_unique<mjbase::model::ClassResource>(item.uri,
                                                           std::move(item.bytes)));
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
