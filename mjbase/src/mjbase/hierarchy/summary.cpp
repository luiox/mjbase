/// @file        summary.cpp
/// @brief       fillSummary：jbc 会话骨架 → ClassSummary。
#include "summary.hpp"

namespace mjbase::hierarchy {

ClassSummary fillSummary(const jbc::ir::ClassSession& session, KeyPool& pool)
{
    const jbc::ir::ClassUnit& u   = session.unit();
    const jbc::ir::SymbolPool& sym = u.symbols();

    ClassSummary s;
    s.access = u.access;
    s.self   = classKeyOf(pool, sym.str(u.thisClass));

    // 无父类（Object）：ClassSession 把缺失的 super_name 物化为空串。
    const std::string_view superName = sym.str(u.superClass);
    s.super = superName.empty() ? ClassKey{} : classKeyOf(pool, superName);

    s.interfaces.reserve(u.interfaces.size());
    for (const jbc::ir::SymbolId i : u.interfaces) {
        s.interfaces.push_back(classKeyOf(pool, sym.str(i)));
    }

    s.methods.reserve(u.methods.size());
    for (const jbc::ir::MethodUnit& m : u.methods) {
        MemberSummary ms;
        ms.name     = pool.intern(sym.str(m.name));
        ms.desc     = pool.intern(sym.str(m.desc));
        ms.access   = m.access;
        ms.isStatic = (m.access & kAccStatic) != 0;
        s.methods.push_back(ms);
    }

    s.fields.reserve(u.fields.size());
    for (const jbc::ir::FieldUnit& f : u.fields) {
        MemberSummary fs;
        fs.name     = pool.intern(sym.str(f.name));
        fs.desc     = pool.intern(sym.str(f.desc));
        fs.access   = f.access;
        fs.isStatic = (f.access & kAccStatic) != 0;
        s.fields.push_back(fs);
    }
    return s;
}

}   // namespace mjbase::hierarchy
