/// @file        semantic_fp.cpp
/// @brief       方法语义指纹（semantic_fp）v1 实现（面见头文件）。
#include "mjbase/analysis/semantic_fp.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "libca/core/bytes.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/crypto/sha256.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/insn.hpp"
#include "jbc/core/insn_codec.hpp"
#include "jbc/core/opcode.hpp"

namespace mjbase::analysis {
namespace {

using jbc::core::AttrSlot;
using jbc::core::BootstrapMethodsAttr;
using jbc::core::ByteView;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::ConstantPool;
using jbc::core::CpTag;
using jbc::core::MemberInfo;
using jbc::core::Op;
using jbc::core::RawInsn;
using jbc::core::Shape;
using jbc::core::SwitchCase;

// ---- 序列化 sink：变长整数 ULEB128、有符号 zigzag、定长小端、长度前缀串 ----

/// @brief 规范化 token 流的写字面。全部小端；变长整数 ULEB128，有符号走
///        zigzag（映射后无符号变长）；字符串先写字节长度再写原始字节。
class Sink
{
public:
    void u8(ca::u8 v) { buf_.push_back(v); }

    void u16le(ca::u16 v)
    {
        buf_.push_back(static_cast<ca::u8>(v & 0xFF));
        buf_.push_back(static_cast<ca::u8>(v >> 8));
    }

    void u32le(ca::u32 v)
    {
        for (int i = 0; i < 4; ++i) {
            buf_.push_back(static_cast<ca::u8>((v >> (8 * i)) & 0xFF));
        }
    }

    void u64le(ca::u64 v)
    {
        for (int i = 0; i < 8; ++i) {
            buf_.push_back(static_cast<ca::u8>((v >> (8 * i)) & 0xFF));
        }
    }

    void varint(ca::u64 v)
    {
        while (v >= 0x80) {
            buf_.push_back(static_cast<ca::u8>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        buf_.push_back(static_cast<ca::u8>(v));
    }

    /// @brief 有符号变长整数：zigzag（负数不必为 63 位长度付全价）。
    void zigzag(ca::i64 v)
    {
        const ca::u64 raw = static_cast<ca::u64>(v);
        const ca::u64 mask = v < 0 ? ~ca::u64{0} : ca::u64{0};
        varint((raw << 1) ^ mask);
    }

    void str(std::string_view s)
    {
        varint(s.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
    }

    std::vector<ca::u8>&       buf() noexcept { return buf_; }
    const std::vector<ca::u8>& buf() const noexcept { return buf_; }

private:
    std::vector<ca::u8> buf_;
};

// ---- token 流里逐指令的 tagged kind（「类型化 token 流」的具体化）----

enum InsnKind : ca::u8 {
    kPlain = 0,       ///< u8 opcode + 形状操作数
    kIntConst = 1,    ///< zigzag（iconst/bipush/sipush/ldc int 归一 token）
    kStrConst = 2,    ///< modified-UTF-8 原始字节
    kTypeRef = 3,     ///< 解析后类名
    kFloatBits = 4,   ///< u32 位型
    kLongBits = 5,    ///< u64 位型
    kDoubleBits = 6,  ///< u64 位型
    kMethodType = 7,  ///< 描述符
    kHandle = 8,      ///< kind + （类，名，描述符）
    kCondy = 9,       ///< 名 + 描述符 + BSM 句柄 + 静态参数
    kFieldRef = 10,   ///< （类，名，描述符）
    kMethodRef = 11,  ///< （类，名，描述符）
    kIndy = 12,       ///< 名 + 描述符 + BSM 句柄 + 静态参数
};

constexpr ca::u32 kInvalidCanon = 0xFFFFFFFFu;

/// @brief 32 字节哈希 → 64 位小写 hex 文本。
ca::str::Utf8String hexOf(const std::array<ca::u8, kSemanticFpHashBytes>& bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    char                  text[kSemanticFpHashBytes * 2];
    for (ca::usize i = 0; i < bytes.size(); ++i) {
        text[2 * i]     = kDigits[bytes[i] >> 4];
        text[2 * i + 1] = kDigits[bytes[i] & 0x0F];
    }
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(text), sizeof(text));
}

std::string_view classNameAt(const ClassFile& cf, ca::u32 classCp)
{
    return cf.className(classCp);
}

/// @brief NameAndType → （名，描述符）；索引无效或 tag 不符返回 Err。
ca::core::Result<std::pair<std::string_view, std::string_view>, SemanticFpError>
nameDescOfNat(const ClassFile& cf, ca::u32 natCp)
{
    if (!cf.cp.valid(natCp) || cf.cp.tagOf(natCp) != CpTag::NameAndType) {
        return ca::core::Err(SemanticFpError::BadCpOperand);
    }
    const auto entry = cf.cp.at(natCp);
    return ca::core::Ok(std::make_pair(cf.cp.utf8Of(entry.a), cf.cp.utf8Of(entry.b)));
}

/// @brief 方法句柄三元组成员引用 → （kind，类，名，描述符）。
ca::core::Result<std::tuple<ca::u32, std::string_view, std::string_view, std::string_view>,
                 SemanticFpError>
resolveHandle(const ClassFile& cf, ca::u32 handleCp)
{
    if (!cf.cp.valid(handleCp) || cf.cp.tagOf(handleCp) != CpTag::MethodHandle) {
        return ca::core::Err(SemanticFpError::BadCpOperand);
    }
    const auto handle = cf.cp.at(handleCp);
    if (!cf.cp.valid(handle.b)) {
        return ca::core::Err(SemanticFpError::BadCpOperand);
    }
    const auto ref = cf.cp.at(handle.b);
    if (ref.tag != CpTag::Fieldref && ref.tag != CpTag::Methodref &&
        ref.tag != CpTag::InterfaceMethodref) {
        return ca::core::Err(SemanticFpError::BadCpOperand);
    }
    auto nd = nameDescOfNat(cf, ref.b);
    if (nd.is_err()) {
        return ca::core::Err(nd.unwrap_err());
    }
    return ca::core::Ok(std::make_tuple(handle.a, classNameAt(cf, ref.a), nd.unwrap().first,
                                        nd.unwrap().second));
}

/// @brief 写一个方法句柄 token：kind + （类，名，描述符）。
ca::core::Result<void, SemanticFpError> emitHandleToken(Sink& s, const ClassFile& cf,
                                                        ca::u32 handleCp)
{
    auto h = resolveHandle(cf, handleCp);
    if (h.is_err()) {
        return ca::core::Err(h.unwrap_err());
    }
    s.u8(kHandle);
    s.u8(static_cast<ca::u8>(std::get<0>(h.unwrap())));
    s.str(std::get<1>(h.unwrap()));
    s.str(std::get<2>(h.unwrap()));
    s.str(std::get<3>(h.unwrap()));
    return ca::core::Ok();
}

/// @brief BootstrapMethods 属性的解码面（缺失/解码失败返回 Err）。
ca::core::Result<const BootstrapMethodsAttr*, SemanticFpError>
bootstrapMethodsOf(ClassFile& cf)
{
    AttrSlot* slot = cf.findClassAttr("BootstrapMethods");
    if (slot == nullptr) {
        return ca::core::Err(SemanticFpError::BadBootstrap);
    }
    auto decoded = cf.decodeAttr(*slot);
    if (decoded.is_err()) {
        return ca::core::Err(SemanticFpError::BadBootstrap);
    }
    const auto* bm = ca::core::tag_cast::dyn_cast<BootstrapMethodsAttr>(decoded.unwrap());
    if (bm == nullptr) {
        return ca::core::Err(SemanticFpError::BadBootstrap);
    }
    return ca::core::Ok(bm);
}

/// @brief BSM/indy 静态参数的逐条类型化 token（解一整层，不递归）。
///
/// Class、MethodType、MethodHandle 展开到语义值；参数位再嵌 condy/indy
/// 直接报错（规范八项边界第 7 项口径）。
ca::core::Result<void, SemanticFpError> emitStaticArgToken(Sink& s, const ClassFile& cf,
                                                           ca::u32 cpIdx)
{
    if (!cf.cp.valid(cpIdx)) {
        return ca::core::Err(SemanticFpError::BadCpOperand);
    }
    const auto entry = cf.cp.at(cpIdx);
    switch (entry.tag) {
    case CpTag::Integer:
        s.u8(kIntConst);
        s.zigzag(entry.ival);
        return ca::core::Ok();
    case CpTag::Float:
        s.u8(kFloatBits);
        s.u32le(static_cast<ca::u32>(entry.ival));
        return ca::core::Ok();
    case CpTag::Long:
        s.u8(kLongBits);
        s.u64le(static_cast<ca::u64>(entry.lval));
        return ca::core::Ok();
    case CpTag::Double:
        s.u8(kDoubleBits);
        s.u64le(static_cast<ca::u64>(entry.lval));
        return ca::core::Ok();
    case CpTag::String:
        s.u8(kStrConst);
        s.str(cf.cp.utf8Of(entry.a));
        return ca::core::Ok();
    case CpTag::Class:
        s.u8(kTypeRef);
        s.str(classNameAt(cf, cpIdx));
        return ca::core::Ok();
    case CpTag::MethodType:
        s.u8(kMethodType);
        s.str(cf.cp.utf8Of(entry.a));
        return ca::core::Ok();
    case CpTag::MethodHandle:
        return emitHandleToken(s, cf, cpIdx);
    case CpTag::Dynamic:
    case CpTag::InvokeDynamic:
        return ca::core::Err(SemanticFpError::NestedDynamic);
    default:
        return ca::core::Err(SemanticFpError::BadCpOperand);
    }
}

/// @brief indy/condy 公共体：名 + 描述符 + BSM 句柄 + 全部静态参数。
ca::core::Result<void, SemanticFpError> emitDynamicBody(Sink& s, const ClassFile& cf,
                                                        const ConstantPool::Entry& entry,
                                                        const BootstrapMethodsAttr& bm)
{
    auto nd = nameDescOfNat(cf, entry.b);
    if (nd.is_err()) {
        return ca::core::Err(nd.unwrap_err());
    }
    s.str(nd.unwrap().first);
    s.str(nd.unwrap().second);
    if (entry.a >= bm.entries.size()) {
        return ca::core::Err(SemanticFpError::BadBootstrap);
    }
    const auto& bsm = bm.entries[entry.a];
    auto h = resolveHandle(cf, bsm.methodRef);
    if (h.is_err()) {
        return ca::core::Err(h.unwrap_err());
    }
    s.u8(static_cast<ca::u8>(std::get<0>(h.unwrap())));
    s.str(std::get<1>(h.unwrap()));
    s.str(std::get<2>(h.unwrap()));
    s.str(std::get<3>(h.unwrap()));
    s.varint(bsm.args.size());
    for (const ca::u32 arg : bsm.args) {
        auto r = emitStaticArgToken(s, cf, arg);
        if (r.is_err()) {
            return r;
        }
    }
    return ca::core::Ok();
}

bool isIntConstFamily(Op op) noexcept
{
    return op >= Op::ICONST_M1 && op <= Op::ICONST_5;
}

ca::i32 intConstValue(Op op) noexcept
{
    return static_cast<ca::i32>(op) - static_cast<ca::i32>(Op::ICONST_0);
}

/// @brief 指令是否顺序落入下一条（GOTO/RET/返回/抛出/switch 不落）。
bool fallsThrough(Op op) noexcept
{
    if (op == Op::GOTO || op == Op::GOTO_W) {
        return false;
    }
    if (op >= Op::IRETURN && op <= Op::RETURN) {
        return false;
    }
    if (op == Op::ATHROW || op == Op::TABLESWITCH || op == Op::LOOKUPSWITCH || op == Op::RET) {
        return false;
    }
    return true;
}

/// @brief ldc 家族（LDC/LDC_W/LDC2_W）的载荷 token。
///
/// ldc/ldc_w 与 ldc2_w 的 opcode 本身不进流：同载荷同 token（常量加载
/// 归一）。int 族之外不做任何跨形态折叠——float/double/long 记原始位型。
ca::core::Result<void, SemanticFpError> emitLdcToken(Sink& s, const ClassFile& cf,
                                                     const BootstrapMethodsAttr* bm, Op op,
                                                     ca::u32 cpIdx)
{
    if (!cf.cp.valid(cpIdx)) {
        return ca::core::Err(SemanticFpError::BadCpOperand);
    }
    const auto entry = cf.cp.at(cpIdx);
    const bool twoSlot = op == Op::LDC2_W;
    switch (entry.tag) {
    case CpTag::Integer:
        if (twoSlot) {
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
        s.u8(kIntConst);
        s.zigzag(entry.ival);
        return ca::core::Ok();
    case CpTag::Float:
        if (twoSlot) {
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
        s.u8(kFloatBits);
        s.u32le(static_cast<ca::u32>(entry.ival));
        return ca::core::Ok();
    case CpTag::Long:
        if (!twoSlot) {
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
        s.u8(kLongBits);
        s.u64le(static_cast<ca::u64>(entry.lval));
        return ca::core::Ok();
    case CpTag::Double:
        if (!twoSlot) {
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
        s.u8(kDoubleBits);
        s.u64le(static_cast<ca::u64>(entry.lval));
        return ca::core::Ok();
    case CpTag::String:
        if (twoSlot) {
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
        s.u8(kStrConst);
        s.str(cf.cp.utf8Of(entry.a));
        return ca::core::Ok();
    case CpTag::Class:
        if (twoSlot) {
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
        s.u8(kTypeRef);
        s.str(classNameAt(cf, cpIdx));
        return ca::core::Ok();
    case CpTag::MethodType:
        if (twoSlot) {
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
        s.u8(kMethodType);
        s.str(cf.cp.utf8Of(entry.a));
        return ca::core::Ok();
    case CpTag::MethodHandle:
        if (twoSlot) {
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
        return emitHandleToken(s, cf, cpIdx);
    case CpTag::Dynamic:
    {
        if (twoSlot || bm == nullptr) {
            return ca::core::Err(twoSlot ? SemanticFpError::BadCpOperand
                                         : SemanticFpError::BadBootstrap);
        }
        s.u8(kCondy);
        return emitDynamicBody(s, cf, entry, *bm);
    }
    default:
        return ca::core::Err(SemanticFpError::BadCpOperand);
    }
}

}   // namespace

const char* to_string(SemanticFpError e) noexcept
{
    switch (e) {
    case SemanticFpError::Ok: return "ok";
    case SemanticFpError::BadCodeAttr: return "code attr decode failed";
    case SemanticFpError::BadInsn: return "instruction decode failed";
    case SemanticFpError::BadBranchTarget: return "branch/switch target not at instruction boundary";
    case SemanticFpError::BadExcEntry: return "exception handler not at live instruction boundary";
    case SemanticFpError::BadCpOperand: return "cp operand invalid";
    case SemanticFpError::BadBootstrap: return "bootstrap methods attr missing or invalid";
    case SemanticFpError::NestedDynamic: return "nested dynamic constant in bootstrap args";
    }
    return "unknown";
}

ca::core::Result<SemanticFp, SemanticFpError> semanticFp(ClassFile& cf, MemberInfo& m)
{
    Sink s;

    // 方法头：算法版本、原始访问标志 u16、描述符。方法名/所属类名/
    // Exceptions/Signature/注解/MethodParameters 全程不读不记。
    s.u8(kSemanticFpVersion);
    s.u16le(m.access);
    s.str(cf.descOf(m));

    auto codeR = cf.decodeCode(m);
    if (codeR.is_err()) {
        return ca::core::Err(SemanticFpError::BadCodeAttr);
    }
    CodeAttr* code = codeR.unwrap();

    if (code == nullptr) {
        // native/abstract：无方法体，指纹只盖方法头，永远恒定。
        s.u8(0);
        SemanticFp out;
        const auto digest = ca::crypto::sha256(
            ca::core::ByteSlice{s.buf().data(), s.buf().size()});
        for (ca::usize i = 0; i < kSemanticFpHashBytes; ++i) {
            out.bytes[i] = digest.as_ptr()[i];
        }
        out.hex = hexOf(out.bytes);
        return ca::core::Ok(std::move(out));
    }

    auto insnsR = jbc::core::decodeInsns(ByteView{code->code});
    if (insnsR.is_err()) {
        return ca::core::Err(SemanticFpError::BadInsn);
    }
    const std::vector<RawInsn> insns = std::move(insnsR).unwrap();

    // pc → 指令下标（全量，含死代码；边界判定用）。
    std::map<ca::u32, ca::u32> pcToIdx;
    for (ca::u32 i = 0; i < insns.size(); ++i) {
        pcToIdx.emplace(insns[i].pc, i);
    }

    // 可达性：方法入口 + 全部异常处理器入口（保守超集：守卫区死了处理器也
    // 算可达，宁可多算可达、不漏）。BFS。
    std::vector<ca::u8>  live(insns.size(), 0);
    std::vector<ca::u32> work;
    auto visit = [&](ca::u32 pc,
                     SemanticFpError boundaryErr) -> ca::core::Result<void, SemanticFpError> {
        const auto it = pcToIdx.find(pc);
        if (it == pcToIdx.end()) {
            return ca::core::Err(boundaryErr);
        }
        if (!live[it->second]) {
            live[it->second] = 1;
            work.push_back(it->second);
        }
        return ca::core::Ok();
    };
    if (!insns.empty()) {
        if (auto r = visit(insns.front().pc, SemanticFpError::BadBranchTarget); r.is_err()) {
            return ca::core::Err(r.unwrap_err());
        }
        for (const auto& exc : code->excTable) {
            if (auto r = visit(exc.handlerPc, SemanticFpError::BadExcEntry); r.is_err()) {
                return ca::core::Err(r.unwrap_err());
            }
        }
    }
    while (!work.empty()) {
        const ca::u32  idx = work.back();
        work.pop_back();
        const RawInsn& in = insns[idx];
        auto pushPc = [&](ca::i64 abs) -> ca::core::Result<void, SemanticFpError> {
            if (abs < 0 || abs >= static_cast<ca::i64>(code->code.size())) {
                return ca::core::Err(SemanticFpError::BadBranchTarget);
            }
            return visit(static_cast<ca::u32>(abs), SemanticFpError::BadBranchTarget);
        };
        const Shape sh = jbc::core::shapeOf(in.op);
        if (sh == Shape::Branch2 || sh == Shape::Branch4) {
            if (auto r = pushPc(static_cast<ca::i64>(in.pc) + in.t); r.is_err()) {
                return ca::core::Err(r.unwrap_err());
            }
        }
        else if (sh == Shape::TableSwitch) {
            if (auto r = pushPc(static_cast<ca::i64>(in.pc) + in.t); r.is_err()) {
                return ca::core::Err(r.unwrap_err());
            }
            for (const ca::i32 off : in.offs) {
                if (auto r = pushPc(static_cast<ca::i64>(in.pc) + off); r.is_err()) {
                    return ca::core::Err(r.unwrap_err());
                }
            }
        }
        else if (sh == Shape::LookupSwitch) {
            if (auto r = pushPc(static_cast<ca::i64>(in.pc) + in.t); r.is_err()) {
                return ca::core::Err(r.unwrap_err());
            }
            for (const SwitchCase& c : in.cases) {
                if (auto r = pushPc(static_cast<ca::i64>(in.pc) + c.off); r.is_err()) {
                    return ca::core::Err(r.unwrap_err());
                }
            }
        }
        if (fallsThrough(in.op) && idx + 1 < insns.size()) {
            if (auto r = visit(insns[idx + 1].pc, SemanticFpError::BadBranchTarget);
                r.is_err()) {
                return ca::core::Err(r.unwrap_err());
            }
        }
    }

    // 规范序 = 存活指令按原 pc 升序的保序压缩（删死代码后的序，非布局序）。
    std::vector<ca::u32> liveOrder;
    std::vector<ca::u32> livePcs;
    std::vector<ca::u32> canonIdx(insns.size(), kInvalidCanon);
    for (ca::u32 i = 0; i < insns.size(); ++i) {
        if (live[i]) {
            canonIdx[i] = static_cast<ca::u32>(liveOrder.size());
            liveOrder.push_back(i);
            livePcs.push_back(insns[i].pc);
        }
    }

    // 异常表范围端点 → 「pc 不小于端点的首条存活指令」的规范序号；end 不含，
    // 越过末尾取存活总数。死代码增删/布局漂移不动这些序号。
    const auto firstLiveAtOrAfter = [&](ca::u32 pc) -> ca::u32 {
        const auto it = std::lower_bound(livePcs.begin(), livePcs.end(), pc);
        if (it == livePcs.end()) {
            return static_cast<ca::u32>(liveOrder.size());
        }
        return canonIdx[liveOrder[static_cast<ca::usize>(it - livePcs.begin())]];
    };

    s.u8(1);   // hasCode
    s.u16le(code->maxStack);
    s.u16le(code->maxLocals);

    // 异常表：表项顺序原样保留；catchType 解引用为类名，0 记空。
    s.varint(code->excTable.size());
    for (const auto& exc : code->excTable) {
        s.varint(firstLiveAtOrAfter(exc.startPc));
        s.varint(firstLiveAtOrAfter(exc.endPc));
        const auto it = pcToIdx.find(exc.handlerPc);
        if (it == pcToIdx.end() || !live[it->second] || canonIdx[it->second] == kInvalidCanon) {
            return ca::core::Err(SemanticFpError::BadExcEntry);
        }
        s.varint(canonIdx[it->second]);
        s.str(exc.catchType == 0 ? std::string_view{} : classNameAt(cf, exc.catchType));
    }

    // 指令流：只写可达指令，规范序。
    s.varint(liveOrder.size());
    for (const ca::u32 idx : liveOrder) {
        const RawInsn& in = insns[idx];
        const Op       op = in.op;   // wide 前缀不进指纹（编码形态而非语义）
        const Shape    sh = jbc::core::shapeOf(op);

        // int 常量族跨编码归一（v1 唯一的折叠面）。
        if (sh == Shape::None && isIntConstFamily(op)) {
            s.u8(kIntConst);
            s.zigzag(intConstValue(op));
            continue;
        }
        if (op == Op::BIPUSH || op == Op::SIPUSH) {
            s.u8(kIntConst);
            s.zigzag(in.a);
            continue;
        }
        if (sh == Shape::CP1 || sh == Shape::CP2) {
            if (op == Op::LDC || op == Op::LDC_W || op == Op::LDC2_W) {
                const BootstrapMethodsAttr* bm = nullptr;
                if (cf.cp.valid(in.cp) && cf.cp.tagOf(in.cp) == CpTag::Dynamic) {
                    auto bmR = bootstrapMethodsOf(cf);
                    if (bmR.is_err()) {
                        return ca::core::Err(bmR.unwrap_err());
                    }
                    bm = bmR.unwrap();
                }
                if (auto r = emitLdcToken(s, cf, bm, op, in.cp); r.is_err()) {
                    return ca::core::Err(r.unwrap_err());
                }
                continue;   // ldc 的 opcode 本身不记：载荷 token 已定型
            }
        }

        // 其余：opcode + 形状操作数。
        s.u8(kPlain);
        s.u8(static_cast<ca::u8>(op));
        switch (sh) {
        case Shape::None:
        case Shape::Imm1:
        case Shape::Imm2:
            break;
        case Shape::Local1:
            s.varint(static_cast<ca::u64>(in.a));
            break;
        case Shape::Iinc1:
            s.varint(static_cast<ca::u64>(in.a));
            s.zigzag(in.b);
            break;
        case Shape::Atype1:
            s.varint(static_cast<ca::u64>(in.a));
            break;
        case Shape::Branch2:
        case Shape::Branch4: {
            const auto abs = static_cast<ca::i64>(in.pc) + in.t;
            if (abs < 0 || abs >= static_cast<ca::i64>(code->code.size())) {
                return ca::core::Err(SemanticFpError::BadBranchTarget);
            }
            const auto it = pcToIdx.find(static_cast<ca::u32>(abs));
            if (it == pcToIdx.end() || canonIdx[it->second] == kInvalidCanon) {
                return ca::core::Err(SemanticFpError::BadBranchTarget);
            }
            s.varint(canonIdx[it->second]);
            break;
        }
        case Shape::TableSwitch:
        case Shape::LookupSwitch: {
            auto emitTarget = [&](ca::i32 off) -> ca::core::Result<void, SemanticFpError> {
                const auto abs = static_cast<ca::i64>(in.pc) + off;
                if (abs < 0 || abs >= static_cast<ca::i64>(code->code.size())) {
                    return ca::core::Err(SemanticFpError::BadBranchTarget);
                }
                const auto it = pcToIdx.find(static_cast<ca::u32>(abs));
                if (it == pcToIdx.end() || canonIdx[it->second] == kInvalidCanon) {
                    return ca::core::Err(SemanticFpError::BadBranchTarget);
                }
                s.varint(canonIdx[it->second]);
                return ca::core::Ok();
            };
            if (auto r = emitTarget(in.t); r.is_err()) {
                return ca::core::Err(r.unwrap_err());
            }
            if (sh == Shape::TableSwitch) {
                // key 按 low + 序号重推出连续全列（原始 key 表不进流）。
                s.varint(in.offs.size());
                for (ca::usize i = 0; i < in.offs.size(); ++i) {
                    s.zigzag(static_cast<ca::i64>(in.a) + static_cast<ca::i64>(i));
                    if (auto r = emitTarget(in.offs[i]); r.is_err()) {
                        return ca::core::Err(r.unwrap_err());
                    }
                }
            }
            else {
                // lookupswitch 逐 case 记原始 key 值。
                s.varint(in.cases.size());
                for (const SwitchCase& c : in.cases) {
                    s.zigzag(c.key);
                    if (auto r = emitTarget(c.off); r.is_err()) {
                        return ca::core::Err(r.unwrap_err());
                    }
                }
            }
            break;
        }
        case Shape::CP2Zero2: {   // invokedynamic：名 + 描述符 + BSM 句柄 + 静态参数
            if (op != Op::INVOKEDYNAMIC) {
                return ca::core::Err(SemanticFpError::BadCpOperand);
            }
            if (!cf.cp.valid(in.cp) || cf.cp.tagOf(in.cp) != CpTag::InvokeDynamic) {
                return ca::core::Err(SemanticFpError::BadCpOperand);
            }
            auto bmR = bootstrapMethodsOf(cf);
            if (bmR.is_err()) {
                return ca::core::Err(bmR.unwrap_err());
            }
            s.u8(kIndy);
            if (auto r = emitDynamicBody(s, cf, cf.cp.at(in.cp), *bmR.unwrap()); r.is_err()) {
                return ca::core::Err(r.unwrap_err());
            }
            break;
        }
        case Shape::CP2Count: {   // invokeinterface：count/0 可由描述符导出，不记
            if (op != Op::INVOKEINTERFACE) {
                return ca::core::Err(SemanticFpError::BadCpOperand);
            }
            if (!cf.cp.valid(in.cp) || cf.cp.tagOf(in.cp) != CpTag::InterfaceMethodref) {
                return ca::core::Err(SemanticFpError::BadCpOperand);
            }
            const auto entry = cf.cp.at(in.cp);
            auto      nd = nameDescOfNat(cf, entry.b);
            if (nd.is_err()) {
                return ca::core::Err(nd.unwrap_err());
            }
            s.u8(kMethodRef);
            s.str(classNameAt(cf, entry.a));
            s.str(nd.unwrap().first);
            s.str(nd.unwrap().second);
            break;
        }
        case Shape::CP2Dims: {   // multianewarray：类名 + 维数
            if (op != Op::MULTIANEWARRAY) {
                return ca::core::Err(SemanticFpError::BadCpOperand);
            }
            if (!cf.cp.valid(in.cp) || cf.cp.tagOf(in.cp) != CpTag::Class) {
                return ca::core::Err(SemanticFpError::BadCpOperand);
            }
            s.u8(kTypeRef);
            s.str(classNameAt(cf, in.cp));
            s.varint(static_cast<ca::u64>(in.a));
            break;
        }
        case Shape::CP1:
        case Shape::CP2: {
            // 到这里只剩非 ldc 的 CP1/CP2：字段/调用/类型指令。
            if (!cf.cp.valid(in.cp)) {
                return ca::core::Err(SemanticFpError::BadCpOperand);
            }
            const auto entry = cf.cp.at(in.cp);
            switch (entry.tag) {
            case CpTag::Fieldref:
            case CpTag::Methodref:
            case CpTag::InterfaceMethodref: {
                auto nd = nameDescOfNat(cf, entry.b);
                if (nd.is_err()) {
                    return ca::core::Err(nd.unwrap_err());
                }
                s.u8(entry.tag == CpTag::Fieldref ? kFieldRef : kMethodRef);
                s.str(classNameAt(cf, entry.a));
                s.str(nd.unwrap().first);
                s.str(nd.unwrap().second);
                break;
            }
            case CpTag::Class:
                s.u8(kTypeRef);
                s.str(classNameAt(cf, in.cp));
                break;
            case CpTag::InvokeDynamic: {
                auto bmR = bootstrapMethodsOf(cf);
                if (bmR.is_err()) {
                    return ca::core::Err(bmR.unwrap_err());
                }
                s.u8(kIndy);
                if (auto r = emitDynamicBody(s, cf, entry, *bmR.unwrap()); r.is_err()) {
                    return ca::core::Err(r.unwrap_err());
                }
                break;
            }
            default:
                return ca::core::Err(SemanticFpError::BadCpOperand);
            }
            break;
        }
        default:
            return ca::core::Err(SemanticFpError::BadCpOperand);
        }
    }

    SemanticFp out;
    const auto digest =
        ca::crypto::sha256(ca::core::ByteSlice{s.buf().data(), s.buf().size()});
    for (ca::usize i = 0; i < kSemanticFpHashBytes; ++i) {
        out.bytes[i] = digest.as_ptr()[i];
    }
    out.hex = hexOf(out.bytes);
    return ca::core::Ok(std::move(out));
}

}   // namespace mjbase::analysis
