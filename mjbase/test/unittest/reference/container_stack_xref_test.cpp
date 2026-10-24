// 多容器合并视图喂索引面的跨容器归并验收（RFC 0001 §10-P5「多容器 xref 归并
// 测试」）：MountedContainerSet::mergedView 的产出直接进 ReferenceIndex::build
// （既有扫描面不感知容器边界），断言三件事——两容器的事实并存归并、同 FQN
// 冲突按 first-wins 取解析序首位容器的事实面、卸载容器后索引重扫的引用一致。
// 夹具沿用本目录 class_bytes_builder（手拼 classfile，字节受控）。
#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/model/container_stack.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/reference_index.hpp"

using jbc::core::Op;
using mjbase::model::ClassResource;
using mjbase::model::ContainerRole;
using mjbase::model::MountedContainerSet;
using mjbase::model::ResourceContainer;
using mjbase::reference::MethodReference;
using mjbase::reference::MethodReferenceKind;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::MethodSpec;

namespace {

constexpr ca::u16 kAccPublic = 0x0001;
constexpr ca::u16 kAccStatic = 0x0008;

MethodSpec m(std::string_view name, std::string_view desc,
             std::function<void(CodeEmit&)> emit)
{
    MethodSpec out;
    out.name      = name;
    out.desc      = desc;
    out.maxStack  = 1;
    out.maxLocals = 0;
    out.emit      = std::move(emit);
    return out;
}

void addClassBytes(ResourceContainer& container, std::string_view name, const Bytes& bytes)
{
    container.addResource(std::string(name) + ".class",
                          std::make_unique<ClassResource>(std::string(name) + ".class", bytes));
}

/// 主容器：A.caller 调 B.callee；B 自身无出边（first-wins 的判别面）。
Bytes buildCallerClass()
{
    ClassBytesBuilder b("A");
    std::vector<MethodSpec> ms;
    ms.push_back(m("caller", "()V", [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "B", "callee", "()V");
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

Bytes buildQuietB()
{
    ClassBytesBuilder b("B");
    std::vector<MethodSpec> ms;
    ms.push_back(m("callee", "()V", [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

/// 库容器的同名 B：方法面不同（libOnly 调 C.libCall）——若 first-wins 被破坏，
/// 索引里会出现 B.libOnly 事实面。
Bytes buildLibB()
{
    ClassBytesBuilder b("B");
    std::vector<MethodSpec> ms;
    ms.push_back(m("libOnly", "()V", [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "C", "libCall", "()V");
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

Bytes buildLibC()
{
    ClassBytesBuilder b("C");
    std::vector<MethodSpec> ms;
    ms.push_back(m("libCall", "()V", [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

bool hasMethodEdge(const ReferenceIndex& index, std::string_view sourceOwner,
                   std::string_view sourceName, std::string_view targetOwner,
                   std::string_view targetName, std::string_view targetDesc)
{
    for (const MethodReference& ref : index.methodReferences()) {
        if (ref.kind == MethodReferenceKind::Reflection) continue;
        if (ref.site.sourceClass == sourceOwner && ref.site.sourceMethod == sourceName &&
            ref.target.owner == targetOwner && ref.target.name == targetName &&
            ref.target.desc == targetDesc) {
            return true;
        }
    }
    return false;
}

}   // namespace

TEST(MjContainerStackXrefTest, mergedViewFeedsIndexAcrossContainers)
{
    ResourceContainer primary;
    addClassBytes(primary, "A", buildCallerClass());
    addClassBytes(primary, "B", buildQuietB());
    ResourceContainer library;
    addClassBytes(library, "B", buildLibB());
    addClassBytes(library, "C", buildLibC());

    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &primary).is_err());
    ASSERT_FALSE(stack.mount("lib", ContainerRole::Library, &library).is_err());

    auto merged = stack.mergedView();
    ASSERT_FALSE(merged.is_err());
    ReferenceIndex index;
    index.build(*std::move(merged).unwrap());

    // 跨容器归并：主容器 A→B 事实与库容器 C 声明并存。
    EXPECT_TRUE(hasMethodEdge(index, "A", "caller", "B", "callee", "()V"));
    EXPECT_NE(index.declaredClass("C"), nullptr);
    EXPECT_NE(index.declaredMethod("C", "libCall", "()V"), nullptr);
    EXPECT_NE(index.declaredMethod("B", "callee", "()V"), nullptr);

    // first-wins：B 的方法面取 primary——库容器 B 的 libOnly 事实面不进索引。
    EXPECT_EQ(index.declaredMethod("B", "libOnly", "()V"), nullptr);
    EXPECT_FALSE(hasMethodEdge(index, "B", "libOnly", "C", "libCall", "()V"));
}

TEST(MjContainerStackXrefTest, indexRebuildAfterUnmountKeepsReferencesConsistent)
{
    ResourceContainer primary;
    addClassBytes(primary, "A", buildCallerClass());
    addClassBytes(primary, "B", buildQuietB());
    ResourceContainer library;
    addClassBytes(library, "C", buildLibC());

    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &primary).is_err());
    ASSERT_FALSE(stack.mount("lib", ContainerRole::Library, &library).is_err());

    auto merged = stack.mergedView();
    ASSERT_FALSE(merged.is_err());
    ReferenceIndex index;
    index.build(*std::move(merged).unwrap());
    EXPECT_TRUE(hasMethodEdge(index, "A", "caller", "B", "callee", "()V"));
    EXPECT_NE(index.declaredClass("C"), nullptr);

    // 卸载库容器 → 重建视图重扫：C 事实面退场，主容器事实不受牵连。
    ASSERT_TRUE(stack.unmount("lib"));
    auto afterUnmount = stack.mergedView();
    ASSERT_FALSE(afterUnmount.is_err());
    ReferenceIndex rebuilt;
    rebuilt.build(*std::move(afterUnmount).unwrap());
    EXPECT_TRUE(hasMethodEdge(rebuilt, "A", "caller", "B", "callee", "()V"));
    EXPECT_EQ(rebuilt.declaredClass("C"), nullptr);
}
