#include "mjbase/pass/illegal_class_file_cleaner.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/type/descriptor.hpp"
#include "jclassfile/io/class_file_reader.hpp"
#include "jclassfile/io/class_file_writer.hpp"
#include "jclassfile/transform/illegal_stripping_transformer.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_resource_pending_commit.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::MemberInfo;

/// @brief 前置级：libjclassfile 敌意输入清洗（中间层设计 §3.0 裁决的落地）。
///
/// 读入（畸形容忍、全量属性建模 + 指令解码）→ IllegalStrippingTransformer 全量
/// 规则面 → 写出。规则是否实际触发按产物与输入的字节比较判定（与旧栈
/// ）；读/transform/写任一步抛出该级
/// 零产出，返回 false、调用方拿原 bytes 继续走 libjbc 级（旧实现 catch 全部
/// 的对齐；边界外不许异常逃逸）。
///
/// transformer 以整体接入：除 5 条挂账规则（属性级 CP 校验剔除、越界跳转/
/// LDC 的 RETURN 桩替换、LVT/LVTT/LNT 修剪、BSM 表压缩与 condy 池手术、无效
/// MethodHandle 池清除）外，还携带同一规则面一体的单例属性去重与
/// type-annotation 目标修剪——拆出单项需改 libjclassfile 公共面，本线不碰
/// 库源码（偏差点在头注逐条列明）。
bool runFrontStage(const std::vector<ca::u8>& input, std::vector<ca::u8>& output)
{
    try {
        jclassfile::model::ClassFile parsed = jclassfile::io::ClassFileReader().read(input);
        jclassfile::transform::IllegalStrippingTransformer(parsed).transform();
        output = jclassfile::io::ClassFileWriter().write(parsed);
    }
    catch (...) {
        return false;
    }
    return output != input;
}

/// @brief 在成员属性表中按名字定位属性槽（逐槽比较名字字节）。
///
/// 常量池 addUtf8 只追加不去重，属性名的 CP 索引与解析期同名条目可能不同，
/// 索引等值不可靠——按名字字节比较是 classfile_support 同款纪律。
AttrSlot* findMemberAttr(ClassFile& cf, std::vector<AttrSlot>& attrs, const char* name)
{
    for (AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// @brief libjbc 级规则 2：剪除 Code 异常表中的结构性垃圾条目。
///
/// 判据（对齐旧栈 removeInvalidInstructions 的 junk try-catch 剪枝，边界换成
/// code_length——旧栈用「最后一条指令的起始偏移」做上界，那需要指令级解码，
/// libjbc core 的 CodeAttr 只持有裸 code 字节；code_length 是 JVMS 意义上的
/// 结构上界，保守保住全部合法条目、只剪结构上不可能的形态）：
///   - startPc >= endPc：空区间（JVMS 要求 start < end）；
///   - startPc/endPc/handlerPc > code_length：越过代码体。
/// 未建模/解码失败的 Code 槽原样跳过（不猜、不丢）。
/// 前置级已带指令级精确剪除，本函数只在 libjclassfile 读失败的兜底路径
/// 单独生效。
void pruneJunkExceptionTable(ClassFile& cf, MemberInfo& member)
{
    AttrSlot* slot = findMemberAttr(cf, member.attrs, "Code");
    if (slot == nullptr) return;
    if (!slot->decoded()) {
        if (cf.decodeAttr(*slot).is_err()) return;
    }
    // 先只读探测，确认确有垃圾再 mutate 置脏——避免把无需重编码的 Code 槽
    // 平白推进重编码路径，破坏 copy-through 保真。
    const auto* peeked = ca::core::tag_cast::cast<const CodeAttr>(slot->peek());
    if (peeked == nullptr) return;
    const ca::u32 codeLength = static_cast<ca::u32>(peeked->code.size());
    const bool   hasJunk    = std::any_of(peeked->excTable.begin(),
                                        peeked->excTable.end(),
                                        [codeLength](const CodeAttr::ExcEntry& e) {
                                            return e.startPc >= e.endPc ||
                                                   e.startPc > codeLength ||
                                                   e.endPc > codeLength ||
                                                   e.handlerPc > codeLength;
                                        });
    if (!hasJunk) return;

    auto*       body        = slot->mutate();
    auto*       code        = ca::core::tag_cast::cast<CodeAttr>(body);
    if (code == nullptr) return;
    auto&       exc         = code->excTable;
    const ca::u32 codeLen2  = static_cast<ca::u32>(code->code.size());
    exc.erase(std::remove_if(exc.begin(),
                             exc.end(),
                             [codeLen2](const CodeAttr::ExcEntry& e) {
                                 return e.startPc >= e.endPc || e.startPc > codeLen2 ||
                                        e.endPc > codeLen2 || e.handlerPc > codeLen2;
                             }),
              exc.end());
}

/// @brief libjbc 级：对单个 classfile 应用本级清洗规则（规则 1 + 规则 2）。
void cleanStructure(ClassFile& cf)
{
    // 规则 1：非法描述符成员剔除（旧栈 transform() 的成员校验，对齐上游
    // 62af8af2——字段按字段描述符、方法按方法描述符校验）。
    cf.fields.erase(std::remove_if(cf.fields.begin(),
                                   cf.fields.end(),
                                   [&cf](const MemberInfo& f) {
                                       return jbc::type::parseFieldType(cf.descOf(f)).is_err();
                                   }),
                    cf.fields.end());
    cf.methods.erase(std::remove_if(cf.methods.begin(),
                                    cf.methods.end(),
                                    [&cf](const MemberInfo& m) {
                                        return jbc::type::parseMethodDescriptor(cf.descOf(m))
                                            .is_err();
                                    }),
                     cf.methods.end());

    // 规则 2：Code 异常表垃圾条目剪除。
    for (MemberInfo& member : cf.methods) {
        pruneJunkExceptionTable(cf, member);
    }
}

}   // namespace

mjbase::spi::PreservedAnalyses IllegalClassFileCleaner::run(mjbase::model::ResourceContainer&     container,
                                               mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    bool                             modified = false;
    mjbase::spi::ClassResourcePendingCommit pending;

    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!resource ||
            !ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) {
            continue;
        }
        auto*       cls      = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource.get());
        const auto& original = cls->get();

        // 两级流水：libjclassfile 前置级（慢而稳，5 条挂账规则）→ libjbc 级
        // （快而净，2 条结构规则）。每级独立原子：级内任一步失败该级零产出、
        // 原样放行，不产出部分结果；级间以字节衔接。
        std::vector<ca::u8> current = original;
        {
            std::vector<ca::u8> frontOutput;
            if (runFrontStage(original, frontOutput)) current = std::move(frontOutput);
        }

        // libjbc 级：读入失败（含空 bytes / 坏魔数 / 截断）保留进入字节——
        // 后续宽容读者还可能用得上。
        auto read = jbc::core::readClass(jbc::core::ByteView(current));
        if (read.is_ok()) {
            // ReadResult 持有模型（move-only）：产物先落名为实体再取引用，
            // 保证 cf 在本迭代内存活。
            jbc::core::ReadResult parsed = std::move(read).unwrap();
            ClassFile&            cf     = *parsed.file;

            cleanStructure(cf);

            // 写出失败同样保留进入字节；
            // 字节未变不进 pending，容器保持原样。
            auto written = jbc::core::writeClass(cf);
            if (written.is_ok()) {
                auto output = std::move(written).unwrap();
                if (output != current) current = std::move(output);
            }
        }

        // 两阶段提交：写回推迟到循环结束，任何资源中途失败时已处理的
        // 资源保持原样（作业级原子性，与）。
        if (current != original) {
            pending.stage(cls, std::move(current));
            modified = true;
        }
    }

    pending.commit();
    return modified ? mjbase::spi::PreservedAnalyses::none() : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
