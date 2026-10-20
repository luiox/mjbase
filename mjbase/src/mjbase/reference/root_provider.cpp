#include "mjbase/reference/root_provider.hpp"

#include "mjbase/reference/entry_points.hpp"
#include "mjbase/reference/keep_roots.hpp"

namespace mjbase::reference {

RootSet KeepRootProvider::collect(const ReferenceIndex& index) const
{
    // 复用既有翻译面：规则匹配、档位门槛、池内视图语义都不在本层重写。
    GlobalReachabilityOptions options;
    applyKeepRoots(KeepRootSpec{&rules_, tier_}, index, options);

    RootSet roots;
    roots.classes = std::move(options.rootClasses);
    roots.methods = std::move(options.rootMethods);
    roots.fields  = std::move(options.rootFields);
    return roots;
}

RootSet MainMethodRootProvider::collect(const ReferenceIndex& index) const
{
    RootSet roots;
    for (const MethodDeclaration& decl : index.methodDeclarations()) {
        if (isJavaMainMethod(decl)) {
            roots.methods.push_back(decl.method);
        }
    }
    return roots;
}

void appendRoots(const RootSet& roots, GlobalReachabilityOptions& options)
{
    options.rootClasses.insert(options.rootClasses.end(), roots.classes.begin(),
                               roots.classes.end());
    options.rootMethods.insert(options.rootMethods.end(), roots.methods.begin(),
                               roots.methods.end());
    options.rootFields.insert(options.rootFields.end(), roots.fields.begin(),
                              roots.fields.end());
}

void appendRoots(const std::vector<const IRootProvider*>& providers,
                 const ReferenceIndex& index, GlobalReachabilityOptions& options)
{
    for (const IRootProvider* provider : providers) {
        if (provider == nullptr) {
            continue;
        }
        appendRoots(provider->collect(index), options);
    }
}

}   // namespace mjbase::reference
