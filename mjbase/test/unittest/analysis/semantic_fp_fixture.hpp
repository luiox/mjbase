/// @file        semantic_fp_fixture.hpp
/// @brief       semantic_fp 测试共用的 jasm 装配 + DOM 单点偏差载面。
///
/// 指纹夹具纪律：方法体一律 jasm 文本装配（JvmJasmService）。默认 49
///（免 StackMapTable 帧分析；装配期仍线性栈模拟求 maxStack——体面必须
/// 栈自洽，原字节面的欠栈形一律垫栈或改写）；indy/condy 站点需
/// BootstrapMethods/Dynamic 面，61 装配且体面帧自洽
///（indy_site_inliner_jvm_test 同径）。
///
/// jasm 不能直达的形走 DOM 单点偏差（mutateClassFile，改写前 expectValidateOk
/// 自检 + 各用例申报，以被测 semanticFp 实跑为最终证明）：
/// - 例外①（装配器只发规范形）：LDC_W 强制形态、WIDE 前缀（窄槽位）、
///   invokeinterface count 字节、分支落指令中部（retargetFirstGoto）、
///   JVMS 禁的 BSM 嵌套 Dynamic/InvokeDynamic 实参；
/// - 例外②（jasm 无表达面）：方法级/Code 级/类级原始属性载荷
///   （LineNumberTable/LVT/注解/Signature/Exceptions/SMT/SourceFile 占位
///   字节）、空格名成员（jasm 标识符无法从源码构造真空格名）、异常表原始
///   pc 端点（jasm 异常表只收标签）、lookupswitch 方法体（字节面合法；
///   读侧 decodeInsns 拒收之缺口在用例内钉死申报）。
///
/// CP 重索引夹具（原 CP 前缀垃圾注入）改为 jasm 源内 spacer 方法：在待测
/// 方法之前声明引用宽类型/字符串/整数常量的占位方法，把后续全部语义 CP
/// 槽位真实平移（DOM 常量池只追加不可前插，故走源面）。
#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"

#include "libca/core/tag_cast.hpp"

#include "shared/jasm_fixture.hpp"

namespace mjbase::analysis::fp_test_support {

using Bytes = std::vector<ca::u8>;

/// jasm 装配（major 显式：49 帧less 主体 / 61 indy·condy 面）。
inline Bytes asmFp(const std::string& source, ca::i32 major = 49)
{
    return mjbase::test::assembleJasm(source, major);
}

/// 按名找方法下标（找不到 ADD_FAILURE 并回 npos——夹具机器故障不静默）。
inline ca::usize methodIndex(jbc::core::ClassFile& cf, std::string_view name)
{
    for (ca::usize i = 0; i < cf.methods.size(); ++i) {
        if (cf.cp.utf8Equals(cf.methods[i].nameCp, name)) {
            return i;
        }
    }
    ADD_FAILURE() << "fixture method not found: " << name;
    return static_cast<ca::usize>(-1);
}

/// @brief 通用方法 Code 体编辑：decodeAttr Code → fn(code 字节) → mutate 置脏。
///
/// fn 直接改写 code->code（可增删字节：宽形改写、操作数补丁）；读写失败
/// ADD_FAILURE 并交回空 Bytes。
template <typename Fn>
inline Bytes editMethodCode(const Bytes& bytes, const char* what,
                            std::string_view methodName, Fn fn)
{
    return mjbase::test::mutateClassFile(
        bytes, what, [&](jbc::core::ClassFile& cf) {
            const ca::usize idx = methodIndex(cf, methodName);
            ASSERT_NE(idx, static_cast<ca::usize>(-1));
            if (idx == static_cast<ca::usize>(-1)) return;
            for (jbc::core::AttrSlot& slot : cf.methods[idx].attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
                if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                    ADD_FAILURE() << what << ": Code 解码失败";
                    return;
                }
                auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
                if (code == nullptr) {
                    ADD_FAILURE() << what << ": Code 槽类型不符";
                    return;
                }
                fn(code->code);
                return;
            }
            ADD_FAILURE() << what << ": 无 Code 属性";
        });
}

/// @brief 方法内首条 LDC 的操作数单点改指 build(cf.cp) 新建的 CP 条目；
/// forceWide 时连同 opcode 改写为 LDC_W（0x12→0x13 + 操作数扩 u16）。
///
/// 例外①载面：ldc 的 MethodHandle/MethodType 载荷源码字面形未实证、
/// LDC_W 强制形装配器不直达——装配占位（ldc 整数）后改锚到 DOM 追加的
/// CP 条目。
template <typename CpBuilder>
inline Bytes pointLdcAt(const Bytes& bytes, std::string_view methodName,
                        CpBuilder build, bool forceWide,
                        const char* what = "pointLdcAt")
{
    return mjbase::test::mutateClassFile(bytes, what, [&](jbc::core::ClassFile& cf) {
        const ca::usize idx = methodIndex(cf, methodName);
        ASSERT_NE(idx, static_cast<ca::usize>(-1));
        if (idx == static_cast<ca::usize>(-1)) return;
        for (jbc::core::AttrSlot& slot : cf.methods[idx].attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
            if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                ADD_FAILURE() << what << ": Code 解码失败";
                return;
            }
            auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
            if (code == nullptr) {
                ADD_FAILURE() << what << ": Code 槽类型不符";
                return;
            }
            for (ca::usize i = 0; i < code->code.size(); ++i) {
                if (code->code[i] != 0x12) continue;   // LDC
                const ca::u32 cp = build(cf.cp);
                if (!forceWide && cp <= 0xFF) {
                    code->code[i + 1] = static_cast<ca::u8>(cp);
                    return;
                }
                code->code[i] = 0x13;   // LDC_W
                code->code[i + 1] = static_cast<ca::u8>(cp >> 8);
                code->code.insert(code->code.begin() + static_cast<std::ptrdiff_t>(i) + 2,
                                  static_cast<ca::u8>(cp));
                return;
            }
            ADD_FAILURE() << what << ": 未找到 LDC";
            return;
        }
        ADD_FAILURE() << what << ": 无 Code 属性";
    });
}

/// 窄 ILOAD（0x15，非 WIDE 背板）单点扩为 WIDE 形（前插 0xC4 + 槽位扩 u16）。
inline Bytes widenVarInsn(const Bytes& bytes, std::string_view methodName,
                          const char* what = "widenVarInsn")
{
    return editMethodCode(bytes, what, methodName, [&](std::vector<ca::u8>& code) {
        for (ca::usize i = 0; i < code.size(); ++i) {
            if (code[i] != 0x15) continue;   // ILOAD
            if (i > 0 && code[i - 1] == 0xC4) continue;   // 已在 WIDE 背板内
            const ca::u8 slot = code[i + 1];
            code[i + 1] = 0;
            code.insert(code.begin() + static_cast<std::ptrdiff_t>(i), 0xC4);
            code.insert(code.begin() + static_cast<std::ptrdiff_t>(i) + 2, slot);
            return;
        }
        ADD_FAILURE() << what << ": 未找到窄 ILOAD";
    });
}

/// 窄 IINC（0x84）单点扩为 WIDE 形（前插 0xC4 + 槽位/增量各扩 u16）。
inline Bytes widenIinc(const Bytes& bytes, std::string_view methodName,
                       const char* what = "widenIinc")
{
    return editMethodCode(bytes, what, methodName, [&](std::vector<ca::u8>& code) {
        for (ca::usize i = 0; i < code.size(); ++i) {
            if (code[i] != 0x84) continue;   // IINC
            if (i > 0 && code[i - 1] == 0xC4) continue;
            const ca::u8 slot = code[i + 1];
            const ca::u8 delta = code[i + 2];
            code[i + 1] = 0;
            code[i + 2] = 0;
            code.insert(code.begin() + static_cast<std::ptrdiff_t>(i), 0xC4);
            code.insert(code.begin() + static_cast<std::ptrdiff_t>(i) + 3, slot);
            code.insert(code.begin() + static_cast<std::ptrdiff_t>(i) + 5, delta);
            return;
        }
        ADD_FAILURE() << what << ": 未找到窄 IINC";
    });
}

/// invokeinterface（0xB9）的 count 字节单点改写（装配器按描述符导出）。
inline Bytes patchIfaceCount(const Bytes& bytes, std::string_view methodName,
                             ca::u8 count, const char* what = "patchIfaceCount")
{
    return editMethodCode(bytes, what, methodName, [&](std::vector<ca::u8>& code) {
        for (ca::usize i = 0; i + 3 < code.size(); ++i) {
            if (code[i] != 0xB9) continue;   // INVOKEINTERFACE
            code[i + 3] = count;
            return;
        }
        ADD_FAILURE() << what << ": 未找到 INVOKEINTERFACE";
    });
}

/// 方法访问标志单点改写（set ∪ clear；改写前自检由调用方 expectValidateOk）。
inline Bytes setMethodAccess(const Bytes& bytes, std::string_view methodName,
                             ca::u16 set, ca::u16 clear,
                             const char* what = "setMethodAccess")
{
    return mjbase::test::mutateClassFile(
        bytes, what, [&](jbc::core::ClassFile& cf) {
            const ca::usize idx = methodIndex(cf, methodName);
            ASSERT_NE(idx, static_cast<ca::usize>(-1));
            if (idx == static_cast<ca::usize>(-1)) return;
            auto& m = cf.methods[idx];
            m.access = static_cast<ca::u16>((m.access | set) & ~clear);
        });
}

/// 方法名单点改写（空格名等 jasm 不可构造真名经 addUtf8 落池）。
inline Bytes renameMethod(const Bytes& bytes, std::string_view oldName,
                          std::string_view newName,
                          const char* what = "renameMethod")
{
    return mjbase::test::mutateClassFile(
        bytes, what, [&](jbc::core::ClassFile& cf) {
            const ca::usize idx = methodIndex(cf, oldName);
            ASSERT_NE(idx, static_cast<ca::usize>(-1));
            if (idx == static_cast<ca::usize>(-1)) return;
            cf.methods[idx].nameCp = cf.cp.addUtf8(newName);
        });
}

/// @brief 方法内首条指定 opcode 的字段/方法引用操作数单点改锚：以
/// newMemberName + refDesc 重建 NameAndType（owner 不变），patch u16 操作数。
inline Bytes patchMemberRefName(const Bytes& bytes, std::string_view methodName,
                                   ca::u8 opcode, std::string_view newMemberName,
                                   std::string_view refDesc,
                                   const char* what = "patchMemberRefName")
{
    return mjbase::test::mutateClassFile(
        bytes, what, [&](jbc::core::ClassFile& cf) {
            const ca::usize idx = methodIndex(cf, methodName);
            ASSERT_NE(idx, static_cast<ca::usize>(-1));
            if (idx == static_cast<ca::usize>(-1)) return;
            for (jbc::core::AttrSlot& slot : cf.methods[idx].attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
                if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                    ADD_FAILURE() << what << ": Code 解码失败";
                    return;
                }
                auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
                if (code == nullptr) {
                    ADD_FAILURE() << what << ": Code 槽类型不符";
                    return;
                }
                for (ca::usize i = 0; i + 2 < code->code.size(); ++i) {
                    if (code->code[i] != opcode) continue;
                    const ca::u32 oldCp = (static_cast<ca::u32>(code->code[i + 1]) << 8) |
                                          code->code[i + 2];
                    const ca::u32 ownerCp = cf.cp.at(oldCp).a;
                    const ca::u32 nat = cf.cp.addNameAndType(cf.cp.addUtf8(newMemberName),
                                                             cf.cp.addUtf8(refDesc));
                    const ca::u32 newCp = cf.cp.addRef(
                        cf.cp.tagOf(oldCp), ownerCp, nat);
                    code->code[i + 1] = static_cast<ca::u8>(newCp >> 8);
                    code->code[i + 2] = static_cast<ca::u8>(newCp);
                    return;
                }
                ADD_FAILURE() << what << ": 未找到目标 opcode";
                return;
            }
            ADD_FAILURE() << what << ": 无 Code 属性";
        });
}

/// 异常表条目（原始 pc 端点 + catch 类内部名，空串 = any）。
struct ExcPcSpec
{
    ca::u32          start   = 0;
    ca::u32          end     = 0;
    ca::u32          handler = 0;
    std::string_view catchType;
};

/// @brief 方法异常表整体单点改写（原始 pc 端点——jasm 异常表只收标签，
/// 端点映射语义测试需要非指令边界 pc，例外②载面）。
inline Bytes setExcEntries(const Bytes& bytes, std::string_view methodName,
                           const std::vector<ExcPcSpec>& entries,
                           const char* what = "setExcEntries")
{
    return mjbase::test::mutateClassFile(
        bytes, what, [&](jbc::core::ClassFile& cf) {
            const ca::usize idx = methodIndex(cf, methodName);
            ASSERT_NE(idx, static_cast<ca::usize>(-1));
            if (idx == static_cast<ca::usize>(-1)) return;
            for (jbc::core::AttrSlot& slot : cf.methods[idx].attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
                if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                    ADD_FAILURE() << what << ": Code 解码失败";
                    return;
                }
                auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
                if (code == nullptr) {
                    ADD_FAILURE() << what << ": Code 槽类型不符";
                    return;
                }
                code->excTable.clear();
                for (const ExcPcSpec& e : entries) {
                    jbc::core::CodeAttr::ExcEntry entry;
                    entry.startPc   = e.start;
                    entry.endPc     = e.end;
                    entry.handlerPc = e.handler;
                    entry.catchType =
                        e.catchType.empty() ? 0u : cf.cp.addClass(cf.cp.addUtf8(e.catchType));
                    code->excTable.push_back(entry);
                }
                return;
            }
            ADD_FAILURE() << what << ": 无 Code 属性";
        });
}

/// 方法级原始属性规格（载荷字节全受控）。
struct RawSpec
{
    std::string_view name;
    Bytes            payload;
};

/// 方法级原始属性批量追加（例外②：LineNumberTable/LVT/注解/Signature/
/// Exceptions 等排除面字节不经 jasm 注解面，按占位载荷手写）。
inline Bytes addMethodRawAttrs(const Bytes& bytes, std::string_view methodName,
                               const std::vector<RawSpec>& attrs,
                               const char* what = "addMethodRawAttrs")
{
    return mjbase::test::mutateClassFile(
        bytes, what, [&](jbc::core::ClassFile& cf) {
            const ca::usize idx = methodIndex(cf, methodName);
            ASSERT_NE(idx, static_cast<ca::usize>(-1));
            if (idx == static_cast<ca::usize>(-1)) return;
            for (const RawSpec& spec : attrs) {
                auto raw = std::make_unique<jbc::core::RawAttr>(spec.payload);
                cf.methods[idx].attrs.push_back(
                    jbc::core::AttrSlot::makeNew(cf.cp.addUtf8(spec.name),
                                                 std::move(raw)));
            }
        });
}

/// Code 嵌套原始属性追加（StackMapTable 等排除面占位字节）。
inline Bytes addCodeRawAttr(const Bytes& bytes, std::string_view methodName,
                            std::string_view attrName, Bytes payload,
                            const char* what = "addCodeRawAttr")
{
    return mjbase::test::mutateClassFile(
        bytes, what, [&](jbc::core::ClassFile& cf) {
            const ca::usize idx = methodIndex(cf, methodName);
            ASSERT_NE(idx, static_cast<ca::usize>(-1));
            if (idx == static_cast<ca::usize>(-1)) return;
            for (jbc::core::AttrSlot& slot : cf.methods[idx].attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
                if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                    ADD_FAILURE() << what << ": Code 解码失败";
                    return;
                }
                auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
                if (code == nullptr) {
                    ADD_FAILURE() << what << ": Code 槽类型不符";
                    return;
                }
                auto raw = std::make_unique<jbc::core::RawAttr>(std::move(payload));
                code->attrs.push_back(
                    jbc::core::AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(raw)));
                return;
            }
            ADD_FAILURE() << what << ": 无 Code 属性";
        });
}

/// 类级原始属性追加（SourceFile 等占位字节）。
inline Bytes addClassRawAttr(const Bytes& bytes, std::string_view attrName,
                             Bytes payload, const char* what = "addClassRawAttr")
{
    return mjbase::test::mutateClassFile(bytes, what, [&](jbc::core::ClassFile& cf) {
        auto raw = std::make_unique<jbc::core::RawAttr>(std::move(payload));
        cf.attrs.push_back(
            jbc::core::AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(raw)));
    });
}

/// @brief BSM 实参单点追加嵌套 Dynamic（indy=true 时为 InvokeDynamic）——
/// JVMS 禁形（BSM 解一层不递归的畸形合成），例外①载面：装配器拒收，
/// 经 DOM 在 BootstrapMethods 属性条目上补 CP 索引。
inline Bytes appendBsmNestedDynamic(const Bytes& bytes, ca::usize bsmEntry,
                                    std::string_view argName, std::string_view argDesc,
                                    bool indy, const char* what = "appendBsmNestedDynamic")
{
    return mjbase::test::mutateClassFile(bytes, what, [&](jbc::core::ClassFile& cf) {
        for (jbc::core::AttrSlot& slot : cf.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "BootstrapMethods")) continue;
            if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                ADD_FAILURE() << what << ": BootstrapMethods 解码失败";
                return;
            }
            auto* bm =
                ca::core::tag_cast::cast<jbc::core::BootstrapMethodsAttr>(slot.mutate());
            if (bm == nullptr) {
                ADD_FAILURE() << what << ": BootstrapMethods 槽类型不符";
                return;
            }
            ASSERT_LT(bsmEntry, bm->entries.size());
            if (bsmEntry >= bm->entries.size()) return;
            const ca::u32 nat = cf.cp.addNameAndType(cf.cp.addUtf8(argName),
                                                     cf.cp.addUtf8(argDesc));
            const ca::u32 cp = indy ? cf.cp.addInvokeDynamic(bsmEntry, nat)
                                    : cf.cp.addDynamic(bsmEntry, nat);
            bm->entries[bsmEntry].args.push_back(cp);
            return;
        }
        ADD_FAILURE() << what << ": 无 BootstrapMethods 属性";
    });
}

}   // namespace mjbase::analysis::fp_test_support
