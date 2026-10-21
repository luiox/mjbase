/// @file        alias_analysis_test.cpp
/// @brief       analysis 别名分析测试：值代数契约、摘要驻留键、全程序固定点、
///              反射闭包作用域、静态槽迟变更收敛与引用化摘要绑定。
///
/// 夹具为手拼 classfile 字节（class_bytes_builder.hpp）；MemberKey 键经
/// aliasMemberKey 构造（与实现共用驻留池，内容寻址等值）。
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/analysis/alias_analysis.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

using jbc::core::Op;
using mjbase::analysis::AliasAnalysis;
using mjbase::analysis::AliasFrame;
using mjbase::analysis::AliasMethodSummary;
using mjbase::analysis::AliasValue;
using mjbase::analysis::AliasValueKind;
using mjbase::analysis::aliasInterned;
using mjbase::analysis::aliasMemberKey;
using mjbase::analysis::mergeAliasFrames;
using mjbase::analysis::mergeAliasValues;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::FieldSpec;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;

namespace {

MethodSpec m(std::string_view name, std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals,
             std::function<void(CodeEmit&)> emit)
{
    MethodSpec out;
    out.name      = name;
    out.desc      = desc;
    out.maxStack  = maxStack;
    out.maxLocals = maxLocals;
    out.emit      = std::move(emit);
    return out;
}

void addClass(ResourceContainer& container, std::string_view name, const Bytes& bytes)
{
    container.addResource(std::string(name) + ".class",
                          std::make_unique<ClassResource>(std::string(name) + ".class", bytes));
}

// ---------------------------------------------------------------------------
// 主夹具：AliasFixture
// ---------------------------------------------------------------------------

std::vector<FieldSpec> aliasFixtureFields()
{
    return {
        FieldSpec{0x0009, "slot", "Ljava/lang/Object;"},          // public static
        FieldSpec{0x0009, "classSlot", "Ljava/lang/Class;"},      // public static
        FieldSpec{0x0001, "value", "Ljava/lang/Object;"},         // public
        FieldSpec{0x0001, "instanceClass", "Ljava/lang/Class;"},  // public
    };
}

const char* const kEchoDesc = "(Ljava/lang/Object;)Ljava/lang/Object;";
const char* const kGetMethodDesc =
    "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;";
const char* const kListAdd   = "(Ljava/lang/Object;)Z";
const char* const kListGet   = "(I)Ljava/lang/Object;";
const char* const kMapPut    = "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;";
const char* const kMapGet    = "(Ljava/lang/Object;)Ljava/lang/Object;";

std::vector<MethodSpec> aliasFixtureMethods()
{
    std::vector<MethodSpec> ms;
    ms.push_back(m("echo", kEchoDesc, 1, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("relay", kEchoDesc, 1, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.invoke(Op::INVOKESTATIC, "AliasFixture", "echo", kEchoDesc);
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("setField", "(Ljava/lang/Object;)V", 1, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.fieldInsn(Op::PUTSTATIC, "AliasFixture", "slot", "Ljava/lang/Object;");
        e.op(Op::RETURN);
    }));
    ms.push_back(m("getField", "()Ljava/lang/Object;", 1, 0, [](CodeEmit& e) {
        e.fieldInsn(Op::GETSTATIC, "AliasFixture", "slot", "Ljava/lang/Object;");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("setClassSlot", "()V", 1, 0, [](CodeEmit& e) {
        e.ldcClass("java/lang/String");
        e.fieldInsn(Op::PUTSTATIC, "AliasFixture", "classSlot", "Ljava/lang/Class;");
        e.op(Op::RETURN);
    }));
    ms.push_back(m("getClassSlot", "()Ljava/lang/Class;", 1, 0, [](CodeEmit& e) {
        e.fieldInsn(Op::GETSTATIC, "AliasFixture", "classSlot", "Ljava/lang/Class;");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("relayClassSlot", "()Ljava/lang/Class;", 1, 0, [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "AliasFixture", "getClassSlot", "()Ljava/lang/Class;");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("lookupFromAlias", "()V", 3, 0, [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "AliasFixture", "getClassSlot", "()Ljava/lang/Class;");
        e.ldcString("oldMethod");
        e.i32const(0);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod", kGetMethodDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    ms.push_back(m("setInstanceClass", "(LAliasFixture;)V", 2, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.ldcClass("java/lang/String");
        e.fieldInsn(Op::PUTFIELD, "AliasFixture", "instanceClass", "Ljava/lang/Class;");
        e.op(Op::RETURN);
    }));
    ms.push_back(m("lookupFromInstanceHelper", "(LAliasFixture;)V", 4, 1, [](CodeEmit& e) {
        e.varInsn(Op::ALOAD, 0);
        e.invoke(Op::INVOKESTATIC, "AliasFixture", "setInstanceClass", "(LAliasFixture;)V");
        e.varInsn(Op::ALOAD, 0);
        e.fieldInsn(Op::GETFIELD, "AliasFixture", "instanceClass", "Ljava/lang/Class;");
        e.ldcString("oldMethod");
        e.i32const(0);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod", kGetMethodDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    ms.push_back(m("arrayEcho", kEchoDesc, 4, 1, [](CodeEmit& e) {
        e.i32const(1);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Object");
        e.op(Op::DUP);
        e.i32const(0);
        e.varInsn(Op::ALOAD, 0);
        e.op(Op::AASTORE);
        e.i32const(0);
        e.op(Op::AALOAD);
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("writeArray", "([Ljava/lang/Object;ILjava/lang/Object;)V", 3, 3,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ILOAD, 1);
                       e.varInsn(Op::ALOAD, 2);
                       e.op(Op::AASTORE);
                       e.op(Op::RETURN);
                   }));
    ms.push_back(m("arrayHelperRoundTrip", kEchoDesc, 4, 1, [](CodeEmit& e) {
        e.i32const(1);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Object");
        e.op(Op::DUP);
        e.i32const(0);
        e.varInsn(Op::ALOAD, 0);
        e.invoke(Op::INVOKESTATIC, "AliasFixture", "writeArray",
                 "([Ljava/lang/Object;ILjava/lang/Object;)V");
        e.i32const(0);
        e.op(Op::AALOAD);
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("containerRoundTrip",
                   "(Ljava/util/List;Ljava/lang/Object;)Ljava/lang/Object;", 2, 2,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invokeInterface("java/util/List", "add", kListAdd, 2);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.i32const(0);
                       e.invokeInterface("java/util/List", "get", kListGet, 2);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("concreteContainerRoundTrip",
                   "(Ljava/util/ArrayList;Ljava/lang/Object;)Ljava/lang/Object;", 2, 2,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invoke(Op::INVOKEVIRTUAL, "java/util/ArrayList", "add", kListAdd);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.i32const(0);
                       e.invoke(Op::INVOKEVIRTUAL, "java/util/ArrayList", "get", kListGet);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("clearedContainer",
                   "(Ljava/util/ArrayList;Ljava/lang/Object;)Ljava/lang/Object;", 2, 2,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invoke(Op::INVOKEVIRTUAL, "java/util/ArrayList", "add", kListAdd);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.invoke(Op::INVOKEVIRTUAL, "java/util/ArrayList", "clear", "()V");
                       e.varInsn(Op::ALOAD, 0);
                       e.i32const(0);
                       e.invoke(Op::INVOKEVIRTUAL, "java/util/ArrayList", "get", kListGet);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("addToList", "(Ljava/util/ArrayList;Ljava/lang/Object;)V", 2, 2,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invoke(Op::INVOKEVIRTUAL, "java/util/ArrayList", "add", kListAdd);
                       e.op(Op::POP);
                       e.op(Op::RETURN);
                   }));
    ms.push_back(m("newListAndRead", kEchoDesc, 3, 1, [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "java/util/ArrayList");
        e.op(Op::DUP);
        e.invoke(Op::INVOKESPECIAL, "java/util/ArrayList", "<init>", "()V");
        e.op(Op::DUP);
        e.varInsn(Op::ALOAD, 0);
        e.invoke(Op::INVOKESTATIC, "AliasFixture", "addToList",
                 "(Ljava/util/ArrayList;Ljava/lang/Object;)V");
        e.i32const(0);
        e.invoke(Op::INVOKEVIRTUAL, "java/util/ArrayList", "get", kListGet);
        e.op(Op::ARETURN);
    }));
    // tryAlias：try { return echo(x); } catch (Throwable t) { return x; }——
    // 处理器与正常路径同帧合流，走 CFG 精化。
    {
        MethodSpec tryAlias = m("tryAlias", kEchoDesc, 1, 1, [](CodeEmit& e) {
            const std::size_t start   = e.newLabel();
            const std::size_t end     = e.newLabel();
            const std::size_t handler = e.newLabel();
            e.bindLabel(start);
            e.varInsn(Op::ALOAD, 0);
            e.invoke(Op::INVOKESTATIC, "AliasFixture", "echo", kEchoDesc);
            e.op(Op::ARETURN);
            e.bindLabel(end);
            e.bindLabel(handler);
            e.op(Op::POP);
            e.varInsn(Op::ALOAD, 0);
            e.op(Op::ARETURN);
        });
        tryAlias.exceptions = {{0, 1, 2, "java/lang/Throwable"}};
        ms.push_back(std::move(tryAlias));
    }
    ms.push_back(m("fieldRoundTrip", kEchoDesc, 3, 1, [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "AliasFixture");
        e.op(Op::DUP);
        e.invoke(Op::INVOKESPECIAL, "AliasFixture", "<init>", "()V");
        e.op(Op::DUP);
        e.varInsn(Op::ALOAD, 0);
        e.fieldInsn(Op::PUTFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.fieldInsn(Op::GETFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("writeInstanceField", "(LAliasFixture;Ljava/lang/Object;)V", 2, 2,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.fieldInsn(Op::PUTFIELD, "AliasFixture", "value",
                                   "Ljava/lang/Object;");
                       e.op(Op::RETURN);
                   }));
    ms.push_back(m("newAndRead", kEchoDesc, 3, 1, [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "AliasFixture");
        e.op(Op::DUP);
        e.invoke(Op::INVOKESPECIAL, "AliasFixture", "<init>", "()V");
        e.op(Op::DUP);
        e.varInsn(Op::ALOAD, 0);
        e.invoke(Op::INVOKESTATIC, "AliasFixture", "writeInstanceField",
                 "(LAliasFixture;Ljava/lang/Object;)V");
        e.fieldInsn(Op::GETFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("chainedFields", "()Ljava/lang/Object;", 5, 0, [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "AliasFixture");
        e.op(Op::DUP);
        e.typeInsn(Op::NEW, "AliasFixture");
        e.op(Op::DUP);
        e.typeInsn(Op::NEW, "java/lang/String");
        e.fieldInsn(Op::PUTFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.fieldInsn(Op::PUTFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.fieldInsn(Op::GETFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.fieldInsn(Op::GETFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("mergedFieldTargets", "(Z)Ljava/lang/Object;", 3, 2, [](CodeEmit& e) {
        const std::size_t elseLbl = e.newLabel();
        const std::size_t joinLbl = e.newLabel();
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, elseLbl);
        e.typeInsn(Op::NEW, "AliasFixture");
        e.op(Op::DUP);
        e.typeInsn(Op::NEW, "java/lang/String");
        e.fieldInsn(Op::PUTFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.varInsn(Op::ASTORE, 1);
        e.branch(Op::GOTO, joinLbl);
        e.bindLabel(elseLbl);
        e.typeInsn(Op::NEW, "AliasFixture");
        e.op(Op::DUP);
        e.typeInsn(Op::NEW, "java/lang/StringBuilder");
        e.fieldInsn(Op::PUTFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.varInsn(Op::ASTORE, 1);
        e.bindLabel(joinLbl);
        e.varInsn(Op::ALOAD, 1);
        e.fieldInsn(Op::GETFIELD, "AliasFixture", "value", "Ljava/lang/Object;");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("mergedHelperField", "(ZLjava/lang/Object;)Ljava/lang/Object;", 2, 3,
                   [](CodeEmit& e) {
                       const std::size_t elseLbl = e.newLabel();
                       const std::size_t joinLbl = e.newLabel();
                       e.varInsn(Op::ILOAD, 0);
                       e.branch(Op::IFEQ, elseLbl);
                       e.typeInsn(Op::NEW, "AliasFixture");
                       e.varInsn(Op::ASTORE, 2);
                       e.branch(Op::GOTO, joinLbl);
                       e.bindLabel(elseLbl);
                       e.typeInsn(Op::NEW, "AliasFixture");
                       e.varInsn(Op::ASTORE, 2);
                       e.bindLabel(joinLbl);
                       e.varInsn(Op::ALOAD, 2);
                       e.varInsn(Op::ALOAD, 1);
                       e.invoke(Op::INVOKESTATIC, "AliasFixture", "writeInstanceField",
                                "(LAliasFixture;Ljava/lang/Object;)V");
                       e.varInsn(Op::ALOAD, 2);
                       e.fieldInsn(Op::GETFIELD, "AliasFixture", "value",
                                   "Ljava/lang/Object;");
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("deepBase", "()Ljava/lang/Object;", 1, 0, [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "java/lang/String");
        e.op(Op::ARETURN);
    }));
    const auto addForwarder = [&](const char* name, const char* callee) {
        ms.push_back(m(name, "()Ljava/lang/Object;", 1, 0, [callee](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "AliasFixture", callee, "()Ljava/lang/Object;");
            e.op(Op::ARETURN);
        }));
    };
    addForwarder("deep1", "deepBase");
    addForwarder("deep2", "deep1");
    addForwarder("deep3", "deep2");
    addForwarder("deep4", "deep3");
    ms.push_back(m("multiTarget", "()Ljava/lang/Object;", 1, 0, [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "java/lang/String");
        e.op(Op::ARETURN);
        e.typeInsn(Op::NEW, "java/lang/StringBuilder");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("multiRelay", "()Ljava/lang/Object;", 1, 0, [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "AliasFixture", "multiTarget", "()Ljava/lang/Object;");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("indexedArray", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                   4, 2, [](CodeEmit& e) {
                       e.i32const(2);
                       e.typeInsn(Op::ANEWARRAY, "java/lang/Object");
                       e.op(Op::DUP);
                       e.i32const(0);
                       e.varInsn(Op::ALOAD, 0);
                       e.op(Op::AASTORE);
                       e.op(Op::DUP);
                       e.i32const(1);
                       e.varInsn(Op::ALOAD, 1);
                       e.op(Op::AASTORE);
                       e.i32const(0);
                       e.op(Op::AALOAD);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("separateArrays", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                   4, 2, [](CodeEmit& e) {
                       e.i32const(1);
                       e.typeInsn(Op::ANEWARRAY, "java/lang/Object");
                       e.op(Op::DUP);
                       e.i32const(0);
                       e.varInsn(Op::ALOAD, 0);
                       e.op(Op::AASTORE);
                       e.op(Op::POP);
                       e.i32const(1);
                       e.typeInsn(Op::ANEWARRAY, "java/lang/Object");
                       e.op(Op::DUP);
                       e.i32const(0);
                       e.varInsn(Op::ALOAD, 1);
                       e.op(Op::AASTORE);
                       e.i32const(0);
                       e.op(Op::AALOAD);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("conflictingArray",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", 4, 2,
                   [](CodeEmit& e) {
                       e.i32const(1);
                       e.typeInsn(Op::ANEWARRAY, "java/lang/Object");
                       e.op(Op::DUP);
                       e.i32const(0);
                       e.varInsn(Op::ALOAD, 0);
                       e.op(Op::AASTORE);
                       e.op(Op::DUP);
                       e.i32const(0);
                       e.varInsn(Op::ALOAD, 1);
                       e.op(Op::AASTORE);
                       e.i32const(0);
                       e.op(Op::AALOAD);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("branchTarget", "(Z)Ljava/lang/Object;", 1, 1, [](CodeEmit& e) {
        const std::size_t elseLbl = e.newLabel();
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, elseLbl);
        e.typeInsn(Op::NEW, "java/lang/String");
        e.op(Op::ARETURN);
        e.bindLabel(elseLbl);
        e.typeInsn(Op::NEW, "java/lang/StringBuilder");
        e.op(Op::ARETURN);
    }));
    ms.push_back(m("branchLocal", "(ZLjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                   1, 4, [](CodeEmit& e) {
                       const std::size_t elseLbl = e.newLabel();
                       const std::size_t joinLbl = e.newLabel();
                       e.varInsn(Op::ILOAD, 0);
                       e.branch(Op::IFEQ, elseLbl);
                       e.varInsn(Op::ALOAD, 1);
                       e.varInsn(Op::ASTORE, 3);
                       e.branch(Op::GOTO, joinLbl);
                       e.bindLabel(elseLbl);
                       e.varInsn(Op::ALOAD, 2);
                       e.varInsn(Op::ASTORE, 3);
                       e.bindLabel(joinLbl);
                       e.varInsn(Op::ALOAD, 3);
                       e.op(Op::ARETURN);
                   }));
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;";
    ms.push_back(m("mapRoundTrip",
                   "(Ljava/util/Map;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                   3, 3, [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.varInsn(Op::ALOAD, 2);
                       e.invokeInterface("java/util/Map", "put", kMapPut, 3);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invokeInterface("java/util/Map", "get", kMapGet, 2);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("putOnly", "(Ljava/util/Map;Ljava/lang/Object;)V", 3, 2,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.ldcString("shared");
                       e.varInsn(Op::ALOAD, 1);
                       e.invokeInterface("java/util/Map", "put", kMapPut, 3);
                       e.op(Op::POP);
                       e.op(Op::RETURN);
                   }));
    ms.push_back(m("getOnly", "(Ljava/util/Map;)Ljava/lang/Object;", 2, 1,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.ldcString("shared");
                       e.invokeInterface("java/util/Map", "get", kMapGet, 2);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("getOrDefault",
                   "(Ljava/util/Map;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                   3, 3, [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.varInsn(Op::ALOAD, 2);
                       e.invokeInterface("java/util/Map", "getOrDefault", kMapPut, 3);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("unknownContainerCall",
                   "(Ljava/util/List;Ljava/lang/Object;)Ljava/lang/Object;", 2, 2,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invokeInterface("java/util/List", "add", kListAdd, 2);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.invoke(Op::INVOKESTATIC, "pkg/Unknown", "touch",
                                "(Ljava/util/List;)V");
                       e.varInsn(Op::ALOAD, 0);
                       e.i32const(0);
                       e.invokeInterface("java/util/List", "get", kListGet, 2);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("mapGetOrDefaultRoundTrip",
                   "(Ljava/util/Map;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/"
                   "Object;)Ljava/lang/Object;",
                   3, 4, [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.varInsn(Op::ALOAD, 2);
                       e.invokeInterface("java/util/Map", "put", kMapPut, 3);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.varInsn(Op::ALOAD, 3);
                       e.invokeInterface("java/util/Map", "getOrDefault", kMapPut, 3);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("queuePeek", "(Ljava/util/Queue;Ljava/lang/Object;)Ljava/lang/Object;", 2,
                   2, [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invokeInterface("java/util/Queue", "offer", "(Ljava/lang/Object;)Z",
                                         2);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.invokeInterface("java/util/Queue", "peek",
                                         "()Ljava/lang/Object;", 1);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("queuePollTwice", "(Ljava/util/Queue;Ljava/lang/Object;)Ljava/lang/Object;",
                   2, 2, [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invokeInterface("java/util/Queue", "offer", "(Ljava/lang/Object;)Z",
                                         2);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.invokeInterface("java/util/Queue", "poll",
                                         "()Ljava/lang/Object;", 1);
                       e.op(Op::POP);
                       e.varInsn(Op::ALOAD, 0);
                       e.invokeInterface("java/util/Queue", "poll",
                                         "()Ljava/lang/Object;", 1);
                       e.op(Op::ARETURN);
                   }));
    ms.push_back(m("dequePop", "(Ljava/util/Deque;Ljava/lang/Object;)Ljava/lang/Object;", 2,
                   2, [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.varInsn(Op::ALOAD, 1);
                       e.invokeInterface("java/util/Deque", "push", "(Ljava/lang/Object;)V",
                                         2);
                       e.varInsn(Op::ALOAD, 0);
                       e.invokeInterface("java/util/Deque", "pop", "()Ljava/lang/Object;", 1);
                       e.op(Op::ARETURN);
                   }));
    return ms;
}

Bytes buildAliasFixture()
{
    ClassBytesBuilder b("AliasFixture");
    return b.assemble(aliasFixtureMethods());
}

std::vector<MethodSpec> malformedFixtureMethods()
{
    return {
        m("broken", "()V", 1, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "MalformedMethodFixture", "callee",
                     "not-a-descriptor");
            e.op(Op::RETURN);
        }),
        m("healthy", "()Ljava/lang/Object;", 1, 0, [](CodeEmit& e) {
            e.op(Op::ACONST_NULL);
            e.op(Op::ARETURN);
        }),
    };
}

Bytes buildMalformedMethodFixture()
{
    ClassBytesBuilder b("MalformedMethodFixture");
    return b.assemble(malformedFixtureMethods());
}

// ---------------------------------------------------------------------------
// 反射闭包夹具（RefA/RefB/StatC/Plain）
// ---------------------------------------------------------------------------

std::vector<MethodSpec> closureRefAMethods()
{
    return {
        m("look", "()V", 3, 0, [](CodeEmit& e) {
            e.fieldInsn(Op::GETSTATIC, "StatC", "kls", "Ljava/lang/Class;");
            e.op(Op::POP);
            e.invoke(Op::INVOKESTATIC, "RefB", "work", "()V");
            e.ldcClass("RefB");
            e.ldcString("work");
            e.op(Op::ACONST_NULL);
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getMethod",
                     "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
    };
}

std::vector<MethodSpec> closureRefBMethods()
{
    return {
        m("work", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }),
    };
}

std::vector<FieldSpec> closureStatCFields()
{
    return {FieldSpec{0x0009, "kls", "Ljava/lang/Class;"}};
}

std::vector<MethodSpec> closureStatCMethods()
{
    return {
        m("setK", "()V", 1, 0, [](CodeEmit& e) {
            e.ldcClass("StatC");
            e.fieldInsn(Op::PUTSTATIC, "StatC", "kls", "Ljava/lang/Class;");
            e.op(Op::RETURN);
        }),
    };
}

std::vector<MethodSpec> closurePlainMethods()
{
    return {
        m("zero", "()I", 1, 0, [](CodeEmit& e) {
            e.i32const(0);
            e.op(Op::IRETURN);
        }),
    };
}

void addClosureFixture(ResourceContainer& container)
{
    addClass(container, "RefA",
             ClassBytesBuilder("RefA").assemble(closureRefAMethods()));
    addClass(container, "RefB",
             ClassBytesBuilder("RefB").assemble(closureRefBMethods()));
    {
        ClassBytesBuilder b("StatC");
        for (const FieldSpec& f : closureStatCFields()) b.addField(f);
        addClass(container, "StatC", b.assemble(closureStatCMethods()));
    }
    addClass(container, "Plain",
             ClassBytesBuilder("Plain").assemble(closurePlainMethods()));
}

/// 与 alias_analysis.cpp 内部 sameSummary 同构的观测面比较（单测侧逐字段）。
bool sameObservableSummary(const AliasMethodSummary& lhs, const AliasMethodSummary& rhs)
{
    if (lhs.hasExceptionalExit != rhs.hasExceptionalExit ||
        lhs.parameters.size() != rhs.parameters.size() ||
        lhs.fieldWrites.size() != rhs.fieldWrites.size() ||
        lhs.arrayWrites.size() != rhs.arrayWrites.size() ||
        lhs.containerWrites.size() != rhs.containerWrites.size() ||
        lhs.reflectionReceivers.size() != rhs.reflectionReceivers.size() ||
        !(lhs.returnValue == rhs.returnValue)) {
        return false;
    }
    for (ca::usize index = 0; index < lhs.parameters.size(); ++index) {
        if (!(lhs.parameters[index] == rhs.parameters[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.fieldWrites.size(); ++index) {
        const auto& l = lhs.fieldWrites[index];
        const auto& r = rhs.fieldWrites[index];
        if (l.field != r.field || !(l.receiver == r.receiver) || !(l.value == r.value) ||
            l.isStatic != r.isStatic) {
            return false;
        }
    }
    for (ca::usize index = 0; index < lhs.arrayWrites.size(); ++index) {
        const auto& l = lhs.arrayWrites[index];
        const auto& r = rhs.arrayWrites[index];
        if (!(l.array == r.array) || !(l.index == r.index) || !(l.value == r.value)) return false;
    }
    for (ca::usize index = 0; index < lhs.containerWrites.size(); ++index) {
        const auto& l = lhs.containerWrites[index];
        const auto& r = rhs.containerWrites[index];
        if (!(l.container == r.container) || !(l.key == r.key) || !(l.value == r.value)) {
            return false;
        }
    }
    for (ca::usize index = 0; index < lhs.reflectionReceivers.size(); ++index) {
        if (lhs.reflectionReceivers[index].instructionIndex !=
                rhs.reflectionReceivers[index].instructionIndex ||
            !(lhs.reflectionReceivers[index].receiver ==
              rhs.reflectionReceivers[index].receiver)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 静态槽迟变更夹具（StatC2 写 / Reader 读）
// ---------------------------------------------------------------------------

std::vector<MethodSpec> lateChangeWriterMethods()
{
    return {
        m("setK", "()V", 1, 0, [](CodeEmit& e) {
            e.ldcClass("StatC2");
            e.fieldInsn(Op::PUTSTATIC, "StatC2", "kls", "Ljava/lang/Class;");
            e.op(Op::RETURN);
        }),
    };
}

std::vector<MethodSpec> lateChangeReaderMethods()
{
    return {
        m("get", "()Ljava/lang/Class;", 1, 0, [](CodeEmit& e) {
            e.fieldInsn(Op::GETSTATIC, "StatC2", "kls", "Ljava/lang/Class;");
            e.op(Op::ARETURN);
        }),
    };
}

// ---------------------------------------------------------------------------
// 引用化摘要夹具（BindA）
// ---------------------------------------------------------------------------

std::vector<FieldSpec> bindingFixtureFields()
{
    return {
        FieldSpec{0x0009, "slot", "Ljava/lang/Object;"},
        FieldSpec{0x0001, "value", "Ljava/lang/Object;"},
    };
}

std::vector<MethodSpec> bindingFixtureMethods()
{
    return {
        m("setSlot", "(Ljava/lang/Object;)V", 1, 1, [](CodeEmit& e) {
            e.varInsn(Op::ALOAD, 0);
            e.fieldInsn(Op::PUTSTATIC, "BindA", "slot", "Ljava/lang/Object;");
            e.op(Op::RETURN);
        }),
        m("callSet", "()V", 2, 1, [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "java/lang/Object");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
            e.varInsn(Op::ASTORE, 0);
            e.varInsn(Op::ALOAD, 0);
            e.invoke(Op::INVOKESTATIC, "BindA", "setSlot", "(Ljava/lang/Object;)V");
            e.op(Op::RETURN);
        }),
        m("setInstance", "(LBindA;Ljava/lang/Object;)V", 2, 2, [](CodeEmit& e) {
            e.varInsn(Op::ALOAD, 0);
            e.varInsn(Op::ALOAD, 1);
            e.fieldInsn(Op::PUTFIELD, "BindA", "value", "Ljava/lang/Object;");
            e.op(Op::RETURN);
        }),
        m("chainInstance", "(LBindA;Ljava/lang/Object;)V", 2, 2, [](CodeEmit& e) {
            e.varInsn(Op::ALOAD, 0);
            e.varInsn(Op::ALOAD, 1);
            e.invoke(Op::INVOKESTATIC, "BindA", "setInstance",
                     "(LBindA;Ljava/lang/Object;)V");
            e.op(Op::RETURN);
        }),
        m("looper", "(Ljava/lang/Object;)V", 1, 1, [](CodeEmit& e) {
            const std::size_t endLbl = e.newLabel();
            e.varInsn(Op::ALOAD, 0);
            e.branch(Op::IFNULL, endLbl);
            e.varInsn(Op::ALOAD, 0);
            e.invoke(Op::INVOKESTATIC, "BindA", "setSlot", "(Ljava/lang/Object;)V");
            e.varInsn(Op::ALOAD, 0);
            e.invoke(Op::INVOKESTATIC, "BindA", "looper", "(Ljava/lang/Object;)V");
            e.bindLabel(endLbl);
            e.op(Op::RETURN);
        }),
    };
}

}   // namespace

// ---------------------------------------------------------------------------
// 值代数与帧契约
// ---------------------------------------------------------------------------

TEST(MjAnalysisAliasContractTest, ConflictingValuesBecomeUnknown)
{
    const auto left  = AliasValue::object("owner:A");
    const auto right = AliasValue::object("owner:B");
    EXPECT_EQ(mergeAliasValues(left, right).kind, AliasValueKind::Unknown);
    EXPECT_EQ(mergeAliasValues(left, AliasValue::unknown()).kind, AliasValueKind::Unknown);
}

TEST(MjAnalysisAliasContractTest, EqualValuesSurviveMerge)
{
    const auto value = AliasValue::array("local:2");
    EXPECT_EQ(mergeAliasValues(value, value), value);
}

TEST(MjAnalysisAliasContractTest, ExceptionalPredecessorIsExplicit)
{
    AliasFrame frame;
    frame.locals[1]                 = AliasValue::object("param:0");
    frame.hasExceptionalPredecessor = true;
    EXPECT_TRUE(frame.hasExceptionalPredecessor);
    EXPECT_EQ(frame.local(1), AliasValue::object("param:0"));
    EXPECT_EQ(frame.local(2).kind, AliasValueKind::Unknown);
}

TEST(MjAnalysisAliasContractTest, FrameMergeFailsClosedAcrossExceptionalPredecessor)
{
    AliasFrame normal;
    normal.locals[0] = AliasValue::object("param:0");
    AliasFrame exceptional;
    exceptional.locals[0]                 = AliasValue::object("param:0");
    exceptional.locals[1]                 = AliasValue::object("handler:Throwable");
    exceptional.hasExceptionalPredecessor = true;

    const auto merged = mergeAliasFrames(normal, exceptional);
    EXPECT_TRUE(merged.hasExceptionalPredecessor);
    EXPECT_EQ(merged.local(0), AliasValue::object("param:0"));
    EXPECT_EQ(merged.local(1).kind, AliasValueKind::Unknown);
}

TEST(MjAnalysisAliasContractTest, FrameMergeIsSymmetricForMissingSlots)
{
    AliasFrame left;
    left.locals[0] = AliasValue::object("param:0");
    AliasFrame right;
    right.locals[1] = AliasValue::object("param:1");

    const auto lr = mergeAliasFrames(left, right);
    const auto rl = mergeAliasFrames(right, left);
    EXPECT_EQ(lr.local(0).kind, AliasValueKind::Unknown);
    EXPECT_EQ(lr.local(1).kind, AliasValueKind::Unknown);
    EXPECT_EQ(lr.local(0), rl.local(0));
    EXPECT_EQ(lr.local(1), rl.local(1));
}

// ---------------------------------------------------------------------------
// 摘要驻留键契约
// ---------------------------------------------------------------------------

TEST(MjAnalysisAliasContractTest, SummariesUseStableMethodKeys)
{
    AliasAnalysis      analysis;
    AliasMethodSummary summary;
    summary.method      = aliasMemberKey("pkg/A", "make", "()Ljava/lang/Object;");
    summary.returnValue = AliasValue::object("new:pkg/A");
    analysis.addSummary(summary);

    const auto* found = analysis.summaryOf("pkg/A", "make", "()Ljava/lang/Object;");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->returnValue, summary.returnValue);
}

TEST(MjAnalysisAliasContractTest, IndexesSummaryReturnsInAndersenSolver)
{
    AliasAnalysis      analysis;
    AliasMethodSummary summary;
    summary.method      = aliasMemberKey("pkg/A", "make", "()Ljava/lang/Object;");
    summary.returnValue = AliasValue::object("new:pkg/A.make:0:pkg/Value");
    analysis.addSummary(summary);

    EXPECT_EQ(analysis.andersenReturnPointsToOf("pkg/A", "make", "()Ljava/lang/Object;"),
              std::vector<std::string>{"new:pkg/A.make:0:pkg/Value"});
}

TEST(MjAnalysisAliasContractTest, UnknownInstanceWriteDoesNotPolluteStaticField)
{
    AliasAnalysis      analysis;
    AliasMethodSummary instanceSummary;
    instanceSummary.method = aliasMemberKey("pkg/A", "writeInstance", "(Ljava/lang/Object;)V");
    instanceSummary.fieldWrites.push_back(
        {aliasMemberKey("pkg/A", "value", "Ljava/lang/Object;"), AliasValue::unknown(),
         AliasValue::object("new:pkg/A.writeInstance:0:pkg/Bad"), false});
    analysis.addSummary(std::move(instanceSummary));
    EXPECT_EQ(analysis.fieldValueOf("pkg/A", "value", "Ljava/lang/Object;"), nullptr);

    AliasMethodSummary staticSummary;
    staticSummary.method = aliasMemberKey("pkg/A", "writeStatic", "()V");
    staticSummary.fieldWrites.push_back(
        {aliasMemberKey("pkg/A", "value", "Ljava/lang/Object;"), AliasValue::unknown(),
         AliasValue::object("new:pkg/A.writeStatic:0:pkg/Good"), true});
    analysis.addSummary(std::move(staticSummary));
    const auto* value = analysis.fieldValueOf("pkg/A", "value", "Ljava/lang/Object;");
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, AliasValue::object("new:pkg/A.writeStatic:0:pkg/Good"));
}

TEST(MjAnalysisAliasContractTest, ResolvesArrayReadsAcrossMergedAllocationTargets)
{
    AliasAnalysis      analysis;
    AliasMethodSummary summary;
    summary.method   = aliasMemberKey("pkg/A", "writeArrays", "()V");
    const auto index = AliasValue::primitive("int:0");
    summary.arrayWrites.push_back(
        {AliasValue::array("array:a"), index,
         AliasValue::object("new:pkg/A.writeArrays:0:java/lang/String")});
    summary.arrayWrites.push_back(
        {AliasValue::array("array:b"), index,
         AliasValue::object("new:pkg/A.writeArrays:1:java/lang/StringBuilder")});
    analysis.addSummary(std::move(summary));

    auto merged     = AliasValue::unknown();
    merged.pointsTo = {aliasInterned("array:a"), aliasInterned("array:b")};
    const auto value = analysis.resolvedArrayValueOf(merged, index);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->kind, AliasValueKind::Unknown);
    EXPECT_EQ(analysis.possibleTypesOf(*value),
              (std::vector<std::string>{"java/lang/String", "java/lang/StringBuilder"}));
}

// ---------------------------------------------------------------------------
// 全程序固定点（主夹具）
// ---------------------------------------------------------------------------

TEST(MjAnalysisAliasContractTest, BuildsParameterReturnSummaryFromClassResource)
{
    ResourceContainer container;
    addClass(container, "AliasFixture", buildAliasFixture());

    AliasAnalysis analysis;
    analysis.build(container);

    EXPECT_GT(analysis.convergenceRounds(), 1u);
    EXPECT_LT(analysis.convergenceRounds(), analysis.summaries().size() + 1u);

    const auto* summary = analysis.summaryOf("AliasFixture", "echo", kEchoDesc);
    ASSERT_NE(summary, nullptr);
    ASSERT_EQ(summary->parameters.size(), 1u);
    EXPECT_EQ(summary->parameters[0], AliasValue::object("param:0"));
    EXPECT_EQ(summary->returnValue, AliasValue::object("param:0"));
    EXPECT_EQ(analysis.andersenReturnPointsToOf("AliasFixture", "echo", kEchoDesc),
              std::vector<std::string>{});

    const auto* relay = analysis.summaryOf("AliasFixture", "relay", kEchoDesc);
    ASSERT_NE(relay, nullptr);
    EXPECT_EQ(relay->returnValue, AliasValue::object("param:0"));

    const auto* field = analysis.fieldValueOf("AliasFixture", "slot", "Ljava/lang/Object;");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(*field, AliasValue::object("param:0"));
    const auto* getter = analysis.summaryOf("AliasFixture", "getField", "()Ljava/lang/Object;");
    ASSERT_NE(getter, nullptr);
    EXPECT_EQ(getter->returnValue, AliasValue::object("param:0"));
    const auto* array = analysis.summaryOf("AliasFixture", "arrayEcho", kEchoDesc);
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(array->returnValue, AliasValue::object("param:0"));
    const auto* arrayHelper =
        analysis.summaryOf("AliasFixture", "arrayHelperRoundTrip", kEchoDesc);
    ASSERT_NE(arrayHelper, nullptr);
    const auto* arrayWriter =
        analysis.summaryOf("AliasFixture", "writeArray",
                           "([Ljava/lang/Object;ILjava/lang/Object;)V");
    ASSERT_NE(arrayWriter, nullptr);
    ASSERT_EQ(arrayWriter->parameters.size(), 3u);
    EXPECT_EQ(arrayWriter->parameters[0].kind, AliasValueKind::Array);
    EXPECT_EQ(arrayWriter->parameters[1].kind, AliasValueKind::Primitive);
    EXPECT_EQ(arrayWriter->parameters[2], AliasValue::object("param:2"));
    ASSERT_EQ(arrayWriter->arrayWrites.size(), 1u);
    // arrayHelper 自身不再持有内联自 writeArray 的数组写入（改持绑定），
    // 展开视图与内联物化逐字段等值。
    ASSERT_EQ(arrayHelper->arrayWrites.size(), 0u);
    const auto helperExpanded = analysis.expandedFactsOf("AliasFixture", "arrayHelperRoundTrip",
                                                         kEchoDesc);
    ASSERT_EQ(helperExpanded.arrayWrites.size(), 1u);
    EXPECT_EQ(arrayWriter->arrayWrites[0].array, AliasValue::array("param:0"));
    const auto expectedArrayIndex = AliasValue::primitive("param:1");
    EXPECT_EQ(arrayWriter->arrayWrites[0].index, expectedArrayIndex);
    EXPECT_EQ(arrayWriter->arrayWrites[0].value, AliasValue::object("param:2"));
    EXPECT_EQ(helperExpanded.arrayWrites[0].index.identity, "int:0");
    EXPECT_EQ(helperExpanded.arrayWrites[0].value, AliasValue::object("param:0"));
    EXPECT_EQ(arrayHelper->returnValue, AliasValue::object("param:0"));
    const auto* containerSummary = analysis.summaryOf(
        "AliasFixture", "containerRoundTrip",
        "(Ljava/util/List;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(containerSummary, nullptr);
    EXPECT_EQ(containerSummary->parameters[0].kind, AliasValueKind::Container);
    EXPECT_EQ(containerSummary->returnValue, AliasValue::object("param:1"));
    const auto* concreteContainer = analysis.summaryOf(
        "AliasFixture", "concreteContainerRoundTrip",
        "(Ljava/util/ArrayList;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(concreteContainer, nullptr);
    ASSERT_EQ(concreteContainer->parameters.size(), 2u);
    EXPECT_EQ(concreteContainer->parameters[0].kind, AliasValueKind::Container);
    EXPECT_EQ(concreteContainer->returnValue, AliasValue::object("param:1"));
    const auto* cleared = analysis.summaryOf(
        "AliasFixture", "clearedContainer",
        "(Ljava/util/ArrayList;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(cleared, nullptr);
    EXPECT_EQ(cleared->returnValue.kind, AliasValueKind::Unknown);
    const auto* helperContainer =
        analysis.summaryOf("AliasFixture", "newListAndRead", kEchoDesc);
    ASSERT_NE(helperContainer, nullptr);
    EXPECT_EQ(helperContainer->returnValue, AliasValue::object("param:0"));
    const auto* exceptional = analysis.summaryOf("AliasFixture", "tryAlias", kEchoDesc);
    ASSERT_NE(exceptional, nullptr);
    EXPECT_TRUE(exceptional->hasExceptionalExit);
    EXPECT_EQ(exceptional->returnValue, AliasValue::object("param:0"));
    const auto* instanceField =
        analysis.summaryOf("AliasFixture", "fieldRoundTrip", kEchoDesc);
    ASSERT_NE(instanceField, nullptr);
    EXPECT_EQ(instanceField->returnValue, AliasValue::object("param:0"));
    ASSERT_EQ(instanceField->fieldWrites.size(), 1u);
    EXPECT_TRUE(instanceField->fieldWrites[0].receiver.isKnown());
    const auto* helperField = analysis.summaryOf("AliasFixture", "newAndRead", kEchoDesc);
    ASSERT_NE(helperField, nullptr);
    EXPECT_EQ(helperField->returnValue, AliasValue::object("param:0"));
    const auto receiverPoints = analysis.andersenParameterPointsToOf(
        "AliasFixture", "writeInstanceField", "(LAliasFixture;Ljava/lang/Object;)V", 0);
    ASSERT_FALSE(receiverPoints.empty());
    bool sawAliasFixtureAllocation = false;
    for (const auto& point : receiverPoints) {
        if (point.rfind(":AliasFixture") != std::string::npos) {
            sawAliasFixtureAllocation = true;
            break;
        }
    }
    EXPECT_TRUE(sawAliasFixtureAllocation);
    const auto* chained =
        analysis.summaryOf("AliasFixture", "chainedFields", "()Ljava/lang/Object;");
    ASSERT_NE(chained, nullptr);
    EXPECT_EQ(analysis.possibleTypesOf(chained->returnValue),
              std::vector<std::string>{"java/lang/String"});
    const std::vector<std::string> expectedTypes{"java/lang/String", "java/lang/StringBuilder"};
    const auto* mergedFields =
        analysis.summaryOf("AliasFixture", "mergedFieldTargets", "(Z)Ljava/lang/Object;");
    ASSERT_NE(mergedFields, nullptr);
    EXPECT_EQ(mergedFields->returnValue.kind, AliasValueKind::Unknown);
    EXPECT_EQ(analysis.possibleTypesOf(mergedFields->returnValue), expectedTypes);
    const auto* mergedHelper = analysis.summaryOf(
        "AliasFixture", "mergedHelperField", "(ZLjava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(mergedHelper, nullptr);
    EXPECT_EQ(mergedHelper->returnValue, AliasValue::object("param:1"));
    EXPECT_EQ(analysis.possibleReturnTypesOf("AliasFixture", "deep4", "()Ljava/lang/Object;"),
              std::vector<std::string>{"java/lang/String"});
    const auto* multi = analysis.summaryOf("AliasFixture", "multiTarget", "()Ljava/lang/Object;");
    ASSERT_NE(multi, nullptr);
    EXPECT_EQ(multi->returnValue.kind, AliasValueKind::Unknown);
    EXPECT_EQ(analysis.possibleReturnTypesOf("AliasFixture", "multiTarget",
                                             "()Ljava/lang/Object;"),
              expectedTypes);
    EXPECT_EQ(analysis.possibleReturnTypesOf("AliasFixture", "multiRelay",
                                             "()Ljava/lang/Object;"),
              expectedTypes);
    const auto* indexed = analysis.summaryOf(
        "AliasFixture", "indexedArray",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(indexed, nullptr);
    EXPECT_EQ(indexed->returnValue, AliasValue::object("param:0"));
    const auto* separate = analysis.summaryOf(
        "AliasFixture", "separateArrays",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(separate, nullptr);
    EXPECT_EQ(separate->returnValue, AliasValue::object("param:1"));
    const auto* conflicting = analysis.summaryOf(
        "AliasFixture", "conflictingArray",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(conflicting, nullptr);
    EXPECT_EQ(conflicting->returnValue.kind, AliasValueKind::Unknown);
    const auto* map = analysis.summaryOf(
        "AliasFixture", "mapRoundTrip",
        "(Ljava/util/Map;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(map, nullptr);
    ASSERT_EQ(map->parameters.size(), 3u);
    EXPECT_EQ(map->parameters[0].kind, AliasValueKind::Container);
    EXPECT_EQ(map->returnValue, AliasValue::object("param:2"));
    const auto* isolatedReader =
        analysis.summaryOf("AliasFixture", "getOnly", "(Ljava/util/Map;)Ljava/lang/Object;");
    ASSERT_NE(isolatedReader, nullptr);
    EXPECT_EQ(isolatedReader->returnValue.kind, AliasValueKind::Unknown);
    const auto* defaultReader = analysis.summaryOf(
        "AliasFixture", "getOrDefault",
        "(Ljava/util/Map;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(defaultReader, nullptr);
    EXPECT_EQ(defaultReader->returnValue.kind, AliasValueKind::Unknown);
    const auto* unknownContainer = analysis.summaryOf(
        "AliasFixture", "unknownContainerCall",
        "(Ljava/util/List;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(unknownContainer, nullptr);
    EXPECT_EQ(unknownContainer->returnValue.kind, AliasValueKind::Unknown);
    const auto* knownDefaultReader = analysis.summaryOf(
        "AliasFixture", "mapGetOrDefaultRoundTrip",
        "(Ljava/util/Map;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/"
        "Object;)Ljava/lang/Object;");
    ASSERT_NE(knownDefaultReader, nullptr);
    EXPECT_EQ(knownDefaultReader->returnValue, AliasValue::object("param:2"));
    const auto* peek =
        analysis.summaryOf("AliasFixture", "queuePeek",
                           "(Ljava/util/Queue;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(peek, nullptr);
    EXPECT_EQ(peek->returnValue, AliasValue::object("param:1"));
    const auto* secondPoll = analysis.summaryOf(
        "AliasFixture", "queuePollTwice",
        "(Ljava/util/Queue;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(secondPoll, nullptr);
    EXPECT_EQ(secondPoll->returnValue.kind, AliasValueKind::Unknown);
    const auto* popped = analysis.summaryOf(
        "AliasFixture", "dequePop", "(Ljava/util/Deque;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->returnValue, AliasValue::object("param:1"));
    const auto* branch = analysis.summaryOf("AliasFixture", "branchTarget",
                                            "(Z)Ljava/lang/Object;");
    ASSERT_NE(branch, nullptr);
    EXPECT_EQ(branch->returnValue.kind, AliasValueKind::Unknown);
    EXPECT_EQ(analysis.possibleReturnTypesOf("AliasFixture", "branchTarget",
                                             "(Z)Ljava/lang/Object;"),
              expectedTypes);
    const auto* branchLocal = analysis.summaryOf(
        "AliasFixture", "branchLocal",
        "(ZLjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(branchLocal, nullptr);
    EXPECT_EQ(branchLocal->returnValue.kind, AliasValueKind::Unknown);
    EXPECT_EQ(branchLocal->returnValue.pointsTo,
              (std::vector<std::string_view>{aliasInterned("param:1"),
                                             aliasInterned("param:2")}));
    const auto* classSlot =
        analysis.summaryOf("AliasFixture", "getClassSlot", "()Ljava/lang/Class;");
    ASSERT_NE(classSlot, nullptr);
    EXPECT_EQ(classSlot->returnValue, AliasValue::object("class:java/lang/String"));
    const auto* relayClassSlot =
        analysis.summaryOf("AliasFixture", "relayClassSlot", "()Ljava/lang/Class;");
    ASSERT_NE(relayClassSlot, nullptr);
    EXPECT_EQ(relayClassSlot->returnValue, AliasValue::object("class:java/lang/String"));
    const auto* lookup = analysis.summaryOf("AliasFixture", "lookupFromAlias", "()V");
    ASSERT_NE(lookup, nullptr);
    ASSERT_EQ(lookup->reflectionReceivers.size(), 1u);
    EXPECT_EQ(lookup->reflectionReceivers[0].receiver,
              AliasValue::object("class:java/lang/String"));
    EXPECT_EQ(analysis.reflectionReceiverOf("AliasFixture",
                                            "lookupFromAlias",
                                            "()V",
                                            lookup->reflectionReceivers[0].instructionIndex),
              &lookup->reflectionReceivers[0].receiver);
    const auto* instanceLookup =
        analysis.summaryOf("AliasFixture", "lookupFromInstanceHelper", "(LAliasFixture;)V");
    ASSERT_NE(instanceLookup, nullptr);
    ASSERT_EQ(instanceLookup->reflectionReceivers.size(), 1u);
    EXPECT_EQ(instanceLookup->reflectionReceivers[0].receiver,
              AliasValue::object("class:java/lang/String"));
}

TEST(MjAnalysisAliasContractTest, SkipsMalformedMethodAndKeepsOtherMethodSummaries)
{
    ResourceContainer container;
    addClass(container, "MalformedMethodFixture", buildMalformedMethodFixture());

    AliasAnalysis analysis;
    analysis.build(container);

    EXPECT_EQ(analysis.summaryOf("MalformedMethodFixture", "broken", "()V"), nullptr);
    const auto* healthy =
        analysis.summaryOf("MalformedMethodFixture", "healthy", "()Ljava/lang/Object;");
    ASSERT_NE(healthy, nullptr);
    EXPECT_EQ(healthy->returnValue.kind, AliasValueKind::Null);
}

// ---------------------------------------------------------------------------
// 反射闭包作用域契约（R1/R2/R3/R4 + 作用域外零摘要 + 行为不变）
// ---------------------------------------------------------------------------

TEST(MjAnalysisAliasReflectionClosureTest, ScopeExcludesUnrelatedClassesAndKeepsClosure)
{
    ResourceContainer container;
    addClosureFixture(container);

    AliasAnalysis scoped;
    scoped.buildReflectionClosure(container);

    // R1/R2/R3/R4 成员全部有摘要。
    EXPECT_NE(scoped.summaryOf("RefA", "look", "()V"), nullptr);
    EXPECT_NE(scoped.summaryOf("RefB", "work", "()V"), nullptr);
    EXPECT_NE(scoped.summaryOf("StatC", "setK", "()V"), nullptr);
    // 作用域外零摘要（消费端等价于"无 callee 事实"的既有 fail-open 语义）。
    EXPECT_EQ(scoped.summaryOf("Plain", "zero", "()I"), nullptr);
}

TEST(MjAnalysisAliasReflectionClosureTest, InScopeSummariesMatchFullBuild)
{
    ResourceContainer container;
    addClosureFixture(container);

    AliasAnalysis full;
    full.build(container);
    AliasAnalysis scoped;
    scoped.buildReflectionClosure(container);

    const auto* fullLook   = full.summaryOf("RefA", "look", "()V");
    const auto* scopedLook = scoped.summaryOf("RefA", "look", "()V");
    ASSERT_NE(fullLook, nullptr);
    ASSERT_NE(scopedLook, nullptr);
    EXPECT_TRUE(sameObservableSummary(*fullLook, *scopedLook));
    // 反射接收者事实是唯一消费面，单列断言。
    ASSERT_EQ(scopedLook->reflectionReceivers.size(), 1u);
    EXPECT_EQ(scopedLook->reflectionReceivers[0].receiver, AliasValue::object("class:RefB"));

    const auto* fullWork   = full.summaryOf("RefB", "work", "()V");
    const auto* scopedWork = scoped.summaryOf("RefB", "work", "()V");
    ASSERT_NE(fullWork, nullptr);
    ASSERT_NE(scopedWork, nullptr);
    EXPECT_TRUE(sameObservableSummary(*fullWork, *scopedWork));

    // Class 形静态槽跨类读取（RefA 读 StatC.kls）与全罐一致。
    const auto* fullSlot   = full.fieldValueOf("StatC", "kls", "Ljava/lang/Class;");
    const auto* scopedSlot = scoped.fieldValueOf("StatC", "kls", "Ljava/lang/Class;");
    ASSERT_NE(fullSlot, nullptr);
    ASSERT_NE(scopedSlot, nullptr);
    EXPECT_EQ(*fullSlot, *scopedSlot);
    EXPECT_EQ(*scopedSlot, AliasValue::object("class:StatC"));
}

TEST(MjAnalysisAliasReflectionClosureTest, ConvergenceRoundsReflectClosureOnly)
{
    ResourceContainer container;
    addClosureFixture(container);

    AliasAnalysis scoped;
    scoped.buildReflectionClosure(container);

    // 闭包 = RefA/RefB/StatC（Plain 不参与）；轮数上限 = 闭包方法数+1 = 4。
    EXPECT_GE(scoped.convergenceRounds(), 1u);
    EXPECT_LE(scoped.convergenceRounds(), 4u);
    // out-of-scope 的 Plain 不进入摘要集合。
    EXPECT_EQ(scoped.summaries().size(), 3u);
}

// ---------------------------------------------------------------------------
// 静态槽迟变更（Jacobi 槽依赖收敛钉）
// ---------------------------------------------------------------------------

TEST(MjAnalysisAliasWorklistTest, StaticSlotLateChangePropagatesBeforeConvergence)
{
    ResourceContainer container;
    {
        ClassBytesBuilder b("StatC2");
        b.addField(FieldSpec{0x0009, "kls", "Ljava/lang/Class;"});
        addClass(container, "StatC2", b.assemble(lateChangeWriterMethods()));
    }
    addClass(container, "Reader",
             ClassBytesBuilder("Reader").assemble(lateChangeReaderMethods()));

    AliasAnalysis analysis;
    analysis.build(container);

    // 手算 Jacobi 轮数：S1（缺省 field: 身份）→ S2（class:StatC2）→ S3 稳定。
    EXPECT_EQ(analysis.convergenceRounds(), 3u);

    // 关键断言：收敛值必须是槽稳定后的 class:StatC2，而非轮 0 的缺省身份。
    const auto* get = analysis.summaryOf("Reader", "get", "()Ljava/lang/Class;");
    ASSERT_NE(get, nullptr);
    EXPECT_EQ(get->returnValue, AliasValue::object("class:StatC2"));

    const auto* slot = analysis.fieldValueOf("StatC2", "kls", "Ljava/lang/Class;");
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(*slot, AliasValue::object("class:StatC2"));
}

TEST(MjAnalysisAliasWorklistTest, MemoizedBuildReproducesFullBuildSummaries)
{
    // build 的确定性契约（同一容器跑两次 build）：输出恒为
    // 全量固定点，两次独立实例的摘要逐字段一致且轮数一致。
    ResourceContainer container;
    addClosureFixture(container);

    AliasAnalysis first;
    first.build(container);
    AliasAnalysis second;
    second.build(container);

    EXPECT_EQ(first.convergenceRounds(), second.convergenceRounds());
    const auto* a = first.summaryOf("RefA", "look", "()V");
    const auto* b = second.summaryOf("RefA", "look", "()V");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(sameObservableSummary(*a, *b));
}

// ---------------------------------------------------------------------------
// 引用化摘要：绑定 + 惰性展开
// ---------------------------------------------------------------------------

TEST(MjAnalysisAliasReferenceSummaryTest, CallBindingsReplaceInlinedCalleeFacts)
{
    ResourceContainer container;
    {
        ClassBytesBuilder b("BindA");
        for (const FieldSpec& f : bindingFixtureFields()) b.addField(f);
        addClass(container, "BindA", b.assemble(bindingFixtureMethods()));
    }

    AliasAnalysis analysis;
    analysis.build(container);

    const auto* writer = analysis.summaryOf("BindA", "setSlot", "(Ljava/lang/Object;)V");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(writer->fieldWrites.size(), 1u);
    EXPECT_TRUE(writer->fieldWrites[0].isStatic);

    const auto* caller = analysis.summaryOf("BindA", "callSet", "()V");
    ASSERT_NE(caller, nullptr);
    EXPECT_TRUE(caller->fieldWrites.empty());
    ASSERT_EQ(caller->callBindings.size(), 1u);
    EXPECT_EQ(caller->callBindings[0].callee,
              aliasMemberKey("BindA", "setSlot", "(Ljava/lang/Object;)V"));

    const auto* looper = analysis.summaryOf("BindA", "looper", "(Ljava/lang/Object;)V");
    ASSERT_NE(looper, nullptr);
    EXPECT_TRUE(looper->fieldWrites.empty());
    EXPECT_EQ(looper->callBindings.size(), 2u);
    EXPECT_LT(analysis.convergenceRounds(), 10u);
}

TEST(MjAnalysisAliasReferenceSummaryTest, LazyExpansionReproducesInlinedFactStream)
{
    ResourceContainer container;
    {
        ClassBytesBuilder b("BindA");
        for (const FieldSpec& f : bindingFixtureFields()) b.addField(f);
        addClass(container, "BindA", b.assemble(bindingFixtureMethods()));
    }

    AliasAnalysis analysis;
    analysis.build(container);

    const auto expanded = analysis.expandedFactsOf("BindA", "callSet", "()V");
    ASSERT_EQ(expanded.fieldWrites.size(), 1u);
    EXPECT_TRUE(expanded.fieldWrites[0].isStatic);
    EXPECT_EQ(expanded.fieldWrites[0].field,
              aliasMemberKey("BindA", "slot", "Ljava/lang/Object;"));
    EXPECT_EQ(expanded.fieldWrites[0].value,
              AliasValue::object("new:BindA.callSet()V:0:java/lang/Object"));

    // 形参链：callee 的 param:0 经绑定实例化后仍是调用方的 param:0（σ 透传）。
    const auto chained = analysis.expandedFactsOf("BindA", "chainInstance",
                                                  "(LBindA;Ljava/lang/Object;)V");
    ASSERT_EQ(chained.fieldWrites.size(), 1u);
    EXPECT_FALSE(chained.fieldWrites[0].isStatic);
    EXPECT_EQ(chained.fieldWrites[0].field,
              aliasMemberKey("BindA", "value", "Ljava/lang/Object;"));
    EXPECT_EQ(chained.fieldWrites[0].receiver, AliasValue::object("param:0"));
    EXPECT_EQ(chained.fieldWrites[0].value, AliasValue::object("param:1"));
}

TEST(MjAnalysisAliasReferenceSummaryTest, CyclicCallGraphConvergesWithConstantSummarySize)
{
    ResourceContainer container;
    {
        ClassBytesBuilder b("BindA");
        for (const FieldSpec& f : bindingFixtureFields()) b.addField(f);
        addClass(container, "BindA", b.assemble(bindingFixtureMethods()));
    }

    AliasAnalysis analysis;
    analysis.build(container);

    EXPECT_LT(analysis.convergenceRounds(), 10u);

    const auto expanded = analysis.expandedFactsOf("BindA", "looper", "(Ljava/lang/Object;)V");
    ASSERT_EQ(expanded.fieldWrites.size(), 1u);
    EXPECT_TRUE(expanded.fieldWrites[0].isStatic);
    EXPECT_EQ(expanded.fieldWrites[0].value, AliasValue::object("param:0"));

    const auto* slot = analysis.fieldValueOf("BindA", "slot", "Ljava/lang/Object;");
    ASSERT_NE(slot, nullptr);
    // 槽聚合 = merge(param:0, new:BindA.callSet()V:0:java/lang/Object)：
    // setSlot 直写贡献形参、callSet 绑定实例化贡献具体分配点，分歧收敛为
    // Unknown 并保留诊断 pointsTo（与物化聚合同语义）。
    EXPECT_EQ(slot->kind, AliasValueKind::Unknown);
    const std::string_view callSetAlloc = aliasInterned("new:BindA.callSet()V:0:java/lang/Object");
    EXPECT_TRUE(std::find(slot->pointsTo.begin(), slot->pointsTo.end(), callSetAlloc) !=
                slot->pointsTo.end());
}
