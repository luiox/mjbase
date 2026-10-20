/// @file        field_access_graph.cpp
/// @brief       字段访问图实现：直接事实 + 调用边并集不动点。
#include "field_access_graph.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <utility>

#include "jbc/core/opcode.hpp"

namespace mjbase::reference {

namespace {

using jbc::core::Op;

/// 方法访问位：abstract / native。
inline constexpr ca::u32 kAccNative   = 0x0100;
inline constexpr ca::u32 kAccAbstract = 0x0400;

/// 直接字段事实的读写分类。
///
/// 只有 Direct 按 opcode 分读写；MethodHandle 与 ReflectionAccess 可能执行
/// get 或 set，两边都记；Reflection 仅是查找，不计访问。
void classifyFieldReference(const FieldReference& ref, bool& read, bool& write)
{
    read  = false;
    write = false;
    switch (ref.kind) {
    case FieldReferenceKind::Direct:
        switch (static_cast<Op>(ref.site.opcode)) {
        case Op::GETSTATIC:
        case Op::GETFIELD: read = true; break;
        case Op::PUTSTATIC:
        case Op::PUTFIELD: write = true; break;
        default:
            read  = true;   // 编码异常：保守记读写
            write = true;
            break;
        }
        return;
    case FieldReferenceKind::MethodHandle:
    case FieldReferenceKind::ReflectionAccess:
        read  = true;
        write = true;
        return;
    case FieldReferenceKind::Reflection:
        return;
    }
}

/// sorted unique 并集；dst 变化返回 true。
bool uniteInto(std::vector<ca::u32>& dst, const std::vector<ca::u32>& src)
{
    if (src.empty()) {
        return false;
    }
    std::vector<ca::u32> merged;
    merged.reserve(dst.size() + src.size());
    std::set_union(dst.begin(), dst.end(), src.begin(), src.end(),
                   std::back_inserter(merged));
    if (merged.size() == dst.size()) {
        return false;
    }
    dst = std::move(merged);
    return true;
}

}   // namespace

MemberKey FieldAccessGraph::internMethod(std::string_view owner, std::string_view name,
                                         std::string_view desc)
{
    return memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc);
}

MemberKey FieldAccessGraph::internField(std::string_view owner, std::string_view name,
                                        std::string_view desc)
{
    return memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc);
}

ca::u32 FieldAccessGraph::methodIndex(MemberKey method)
{
    const auto it = methodIndexOf_.find(method);
    if (it != methodIndexOf_.end()) {
        return it->second;
    }
    const ca::u32 idx = static_cast<ca::u32>(methodKeys_.size());
    methodKeys_.push_back(method);
    methodIndexOf_.emplace(method, idx);
    unknown_.push_back(false);
    return idx;
}

ca::u32 FieldAccessGraph::fieldIndex(MemberKey field)
{
    const auto it = fieldIndexOf_.find(field);
    if (it != fieldIndexOf_.end()) {
        return it->second;
    }
    const ca::u32 idx = static_cast<ca::u32>(fieldKeys_.size());
    fieldKeys_.push_back(field);
    fieldIndexOf_.emplace(field, idx);
    fieldDeclared_.push_back(false);
    return idx;
}

void FieldAccessGraph::clear()
{
    methodKeys_.clear();
    fieldKeys_.clear();
    fieldDeclared_.clear();
    methodIndexOf_.clear();
    fieldIndexOf_.clear();
    read_.clear();
    write_.clear();
    unknown_.clear();
    methodSummaries_.clear();
    fieldSummaries_.clear();
    hasUnknownEffects_ = false;
    unresolved_        = false;
}

void FieldAccessGraph::build(const ReferenceIndex& index,
                             mjbase::asm_classpath::CallGraph& callGraph)
{
    clear();

    for (const MethodDeclaration& decl : index.methodDeclarations()) {
        methodIndex(internMethod(decl.method.owner, decl.method.name, decl.method.desc));
    }
    for (const FieldDeclaration& decl : index.fieldDeclarations()) {
        const ca::u32 idx = fieldIndex(internField(decl.field.owner, decl.field.name,
                                                   decl.field.desc));
        fieldDeclared_[idx] = true;
    }

    struct DirectFact
    {
        ca::u32 method = 0;
        ca::u32 field  = 0;
        bool    read   = false;
        bool    write  = false;
    };
    std::vector<DirectFact> direct;

    for (const FieldReference& ref : index.fieldReferences()) {
        if (ref.site.sourceMethod.empty() || ref.site.sourceClass.empty() ||
            ref.site.sourceDesc.empty()) {
            continue;
        }
        const std::string_view owner = ref.target.owner;
        const std::string_view name  = ref.target.name;
        const std::string_view desc  = ref.target.desc;
        if (owner.empty() || name.empty() || desc.empty()) {
            unresolved_ = true;
            continue;
        }
        bool read  = false;
        bool write = false;
        classifyFieldReference(ref, read, write);
        if (!read && !write) {
            continue;
        }
        DirectFact fact;
        fact.method = methodIndex(internMethod(ref.site.sourceClass, ref.site.sourceMethod,
                                                ref.site.sourceDesc));
        fact.field = fieldIndex(internField(owner, name, desc));
        fact.read  = read;
        fact.write = write;
        direct.push_back(fact);
    }

    // 调用边：只扫描图中的方法节点；被调方引入的外部方法一并入表（无出边）。
    // 方法表在扫描中增长，按索引循环直到覆盖全部节点。
    struct EdgePair
    {
        ca::u32 caller = 0;
        ca::u32 callee = 0;
        bool operator<(const EdgePair& o) const noexcept
        {
            return caller != o.caller ? caller < o.caller : callee < o.callee;
        }
        bool operator==(const EdgePair& o) const noexcept
        {
            return caller == o.caller && callee == o.callee;
        }
    };
    std::vector<EdgePair> edges;

    const KeyPool& cgPool = callGraph.pool();
    KeyPool&       cgPoolMutable = callGraph.pool();
    for (ca::usize i = 0; i < methodKeys_.size(); ++i) {
        const MemberKey method = methodKeys_[i];
        const MemberKey cgKey  = memberKeyOf(cgPoolMutable,
                                            classKeyOf(cgPoolMutable, pool_.str(method.cls)),
                                            pool_.str(method.name), pool_.str(method.desc));
        for (const mjbase::asm_classpath::CallSite& site : callGraph.outgoingSitesOf(cgKey)) {
            if (site.kind == mjbase::asm_classpath::InvokeKind::Dynamic ||
                site.callee.cls == 0) {
                unknown_[i] = true;
                continue;
            }
            const std::string_view calleeOwner = cgPool.str(site.callee.cls);
            const std::string_view calleeName  = cgPool.str(site.callee.name);
            const std::string_view calleeDesc  = cgPool.str(site.callee.desc);
            if (site.isVirtualDispatch()) {
                unknown_[i] = true;   // 未解析重写目标
            }
            const MethodDeclaration* decl =
                index.declaredMethod(calleeOwner, calleeName, calleeDesc);
            if (decl == nullptr || (decl->access & (kAccAbstract | kAccNative)) != 0) {
                unknown_[i] = true;   // 外部/无体目标
            }
            const ca::u32 callee =
                methodIndex(internMethod(calleeOwner, calleeName, calleeDesc));
            edges.push_back(EdgePair{static_cast<ca::u32>(i), callee});
        }
    }

    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    std::vector<std::vector<ca::u32>> callers(methodKeys_.size());
    for (const EdgePair& e : edges) {
        callers[e.callee].push_back(e.caller);
    }
    // 同一被调方的调用方按方法序（排序主键 caller 保证）。

    read_.assign(methodKeys_.size(), {});
    write_.assign(methodKeys_.size(), {});
    for (const DirectFact& fact : direct) {
        if (fact.read) {
            read_[fact.method].push_back(fact.field);
        }
        if (fact.write) {
            write_[fact.method].push_back(fact.field);
        }
    }
    for (ca::usize i = 0; i < methodKeys_.size(); ++i) {
        std::sort(read_[i].begin(), read_[i].end());
        read_[i].erase(std::unique(read_[i].begin(), read_[i].end()), read_[i].end());
        std::sort(write_[i].begin(), write_[i].end());
        write_[i].erase(std::unique(write_[i].begin(), write_[i].end()), write_[i].end());
    }

    // 并集不动点：环上集合只增，truncated 不存在，必然收敛。
    std::deque<ca::u32> work;
    for (ca::u32 i = 0; i < methodKeys_.size(); ++i) {
        work.push_back(i);
    }
    while (!work.empty()) {
        const ca::u32 m = work.front();
        work.pop_front();
        for (const ca::u32 caller : callers[m]) {
            bool changed = uniteInto(read_[caller], read_[m]);
            changed = uniteInto(write_[caller], write_[m]) || changed;
            if (unknown_[m] && !unknown_[caller]) {
                unknown_[caller] = true;
                changed          = true;
            }
            if (changed) {
                work.push_back(caller);
            }
        }
    }

    methodSummaries_.reserve(methodKeys_.size());
    for (ca::u32 i = 0; i < methodKeys_.size(); ++i) {
        MethodFieldSummary s;
        s.method = methodKeys_[i];
        s.fieldsRead.reserve(read_[i].size());
        for (const ca::u32 f : read_[i]) {
            s.fieldsRead.push_back(fieldKeys_[f]);
        }
        s.fieldsWritten.reserve(write_[i].size());
        for (const ca::u32 f : write_[i]) {
            s.fieldsWritten.push_back(fieldKeys_[f]);
        }
        s.hasUnknownCallees = unknown_[i];
        if (s.hasUnknownCallees) {
            hasUnknownEffects_ = true;
        }
        methodSummaries_.push_back(std::move(s));
    }

    fieldSummaries_.resize(fieldKeys_.size());
    for (ca::usize f = 0; f < fieldKeys_.size(); ++f) {
        fieldSummaries_[f].field    = fieldKeys_[f];
        fieldSummaries_[f].declared = fieldDeclared_[f];
    }
    for (ca::u32 m = 0; m < methodKeys_.size(); ++m) {
        for (const ca::u32 f : read_[m]) {
            fieldSummaries_[f].readers.push_back(methodKeys_[m]);
        }
        for (const ca::u32 f : write_[m]) {
            fieldSummaries_[f].writers.push_back(methodKeys_[m]);
        }
    }
}

const MethodFieldSummary* FieldAccessGraph::methodSummary(MemberKey method) const noexcept
{
    const auto it = methodIndexOf_.find(method);
    return it != methodIndexOf_.end() ? &methodSummaries_[it->second] : nullptr;
}

const MethodFieldSummary* FieldAccessGraph::methodSummary(std::string_view owner,
                                                          std::string_view name,
                                                          std::string_view desc) const
{
    return methodSummary(memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc));
}

const FieldAccessSummary* FieldAccessGraph::fieldSummary(MemberKey field) const noexcept
{
    const auto it = fieldIndexOf_.find(field);
    return it != fieldIndexOf_.end() ? &fieldSummaries_[it->second] : nullptr;
}

const FieldAccessSummary* FieldAccessGraph::fieldSummary(std::string_view owner,
                                                         std::string_view name,
                                                         std::string_view desc) const
{
    return fieldSummary(memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc));
}

std::vector<MemberKey> FieldAccessGraph::readersOf(MemberKey field) const
{
    const FieldAccessSummary* s = fieldSummary(field);
    return s != nullptr ? s->readers : std::vector<MemberKey>{};
}

std::vector<MemberKey> FieldAccessGraph::readersOf(std::string_view owner,
                                                   std::string_view name,
                                                   std::string_view desc) const
{
    return readersOf(memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc));
}

std::vector<MemberKey> FieldAccessGraph::writersOf(MemberKey field) const
{
    const FieldAccessSummary* s = fieldSummary(field);
    return s != nullptr ? s->writers : std::vector<MemberKey>{};
}

std::vector<MemberKey> FieldAccessGraph::writersOf(std::string_view owner,
                                                   std::string_view name,
                                                   std::string_view desc) const
{
    return writersOf(memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc));
}

std::vector<MemberKey> FieldAccessGraph::unreadDeclaredFields() const
{
    std::vector<MemberKey> out;
    for (const FieldAccessSummary& s : fieldSummaries_) {
        if (s.declared && s.readers.empty()) {
            out.push_back(s.field);
        }
    }
    return out;
}

std::vector<MemberKey> FieldAccessGraph::untouchedDeclaredFields() const
{
    std::vector<MemberKey> out;
    for (const FieldAccessSummary& s : fieldSummaries_) {
        if (s.declared && s.readers.empty() && s.writers.empty()) {
            out.push_back(s.field);
        }
    }
    return out;
}

}   // namespace mjbase::reference
