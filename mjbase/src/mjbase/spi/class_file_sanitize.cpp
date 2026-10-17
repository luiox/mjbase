#include "mjbase/spi/class_file_sanitize.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>

#include "jbc/core/constant_pool.hpp"

namespace mjbase::spi {
namespace {

using ca::i32;
using ca::i64;
using ca::u8;
using ca::u32;
using ca::u64;
using ca::usize;
using ca::str::Utf8String;
using ca::str::Utf8StringBuilder;

/// 诊断条数上限：敌意样本防护（一个坏类不该产出海量诊断拖垮日志/报告）。
constexpr usize kMaxIssues = 32;

// JVMS 4.4 CP tag 值（净化器读原始字节，用整数值比对）；
// static_assert 锚定 libjbc CpTag，两处口径漂移即编译失败。
constexpr u32 kTagUtf8               = 1;
constexpr u32 kTagInteger            = 3;
constexpr u32 kTagFloat              = 4;
constexpr u32 kTagLong               = 5;
constexpr u32 kTagDouble             = 6;
constexpr u32 kTagClass              = 7;
constexpr u32 kTagString             = 8;
constexpr u32 kTagFieldref           = 9;
constexpr u32 kTagMethodref          = 10;
constexpr u32 kTagInterfaceMethodref = 11;
constexpr u32 kTagNameAndType        = 12;
constexpr u32 kTagMethodHandle       = 15;
constexpr u32 kTagMethodType         = 16;
constexpr u32 kTagDynamic            = 17;
constexpr u32 kTagInvokeDynamic      = 18;
constexpr u32 kTagModule             = 19;
constexpr u32 kTagPackage            = 20;

static_assert(static_cast<u32>(jbc::core::CpTag::Utf8) == kTagUtf8 &&
                  static_cast<u32>(jbc::core::CpTag::Integer) == kTagInteger &&
                  static_cast<u32>(jbc::core::CpTag::Float) == kTagFloat &&
                  static_cast<u32>(jbc::core::CpTag::Long) == kTagLong &&
                  static_cast<u32>(jbc::core::CpTag::Double) == kTagDouble &&
                  static_cast<u32>(jbc::core::CpTag::Class) == kTagClass &&
                  static_cast<u32>(jbc::core::CpTag::String) == kTagString &&
                  static_cast<u32>(jbc::core::CpTag::Fieldref) == kTagFieldref &&
                  static_cast<u32>(jbc::core::CpTag::Methodref) == kTagMethodref &&
                  static_cast<u32>(jbc::core::CpTag::InterfaceMethodref) ==
                      kTagInterfaceMethodref &&
                  static_cast<u32>(jbc::core::CpTag::NameAndType) == kTagNameAndType &&
                  static_cast<u32>(jbc::core::CpTag::MethodHandle) == kTagMethodHandle &&
                  static_cast<u32>(jbc::core::CpTag::MethodType) == kTagMethodType &&
                  static_cast<u32>(jbc::core::CpTag::Dynamic) == kTagDynamic &&
                  static_cast<u32>(jbc::core::CpTag::InvokeDynamic) == kTagInvokeDynamic &&
                  static_cast<u32>(jbc::core::CpTag::Module) == kTagModule &&
                  static_cast<u32>(jbc::core::CpTag::Package) == kTagPackage,
              "sanitizer tag table must match jbc::core::CpTag");

using TagMask = u32;

constexpr TagMask maskOf(u32 tag) { return tag < 32 ? (1u << tag) : 0; }

/// bootstrap 参数允许的 loadable 常量集合（JVMS 4.7.23）。
constexpr TagMask kLoadableMask = maskOf(kTagInteger) | maskOf(kTagFloat) | maskOf(kTagLong) |
                                  maskOf(kTagDouble) | maskOf(kTagClass) | maskOf(kTagString) |
                                  maskOf(kTagMethodHandle) | maskOf(kTagMethodType) |
                                  maskOf(kTagDynamic);

const char* tagName(u32 tag)
{
    switch (tag) {
    case kTagUtf8: return "CONSTANT_Utf8";
    case kTagInteger: return "CONSTANT_Integer";
    case kTagFloat: return "CONSTANT_Float";
    case kTagLong: return "CONSTANT_Long";
    case kTagDouble: return "CONSTANT_Double";
    case kTagClass: return "CONSTANT_Class";
    case kTagString: return "CONSTANT_String";
    case kTagFieldref: return "CONSTANT_Fieldref";
    case kTagMethodref: return "CONSTANT_Methodref";
    case kTagInterfaceMethodref: return "CONSTANT_InterfaceMethodref";
    case kTagNameAndType: return "CONSTANT_NameAndType";
    case kTagMethodHandle: return "CONSTANT_MethodHandle";
    case kTagMethodType: return "CONSTANT_MethodType";
    case kTagDynamic: return "CONSTANT_Dynamic";
    case kTagInvokeDynamic: return "CONSTANT_InvokeDynamic";
    case kTagModule: return "CONSTANT_Module";
    case kTagPackage: return "CONSTANT_Package";
    default: return "reserved";
    }
}

/// 引用期望集合的可读名（只出现在诊断消息里）。
void expectedNames(TagMask mask, char* buf, usize bufSize)
{
    if (mask == maskOf(kTagUtf8)) {
        std::snprintf(buf, bufSize, "CONSTANT_Utf8");
    }
    else if (mask == maskOf(kTagClass)) {
        std::snprintf(buf, bufSize, "CONSTANT_Class");
    }
    else if (mask == maskOf(kTagNameAndType)) {
        std::snprintf(buf, bufSize, "CONSTANT_NameAndType");
    }
    else if (mask == maskOf(kTagMethodHandle)) {
        std::snprintf(buf, bufSize, "CONSTANT_MethodHandle");
    }
    else if (mask == (maskOf(kTagMethodref) | maskOf(kTagInterfaceMethodref))) {
        std::snprintf(buf, bufSize, "CONSTANT_Methodref/InterfaceMethodref");
    }
    else if (mask == kLoadableMask) {
        std::snprintf(buf, bufSize, "loadable constant");
    }
    else {
        std::snprintf(buf, bufSize, "tag set 0x%x", mask);
    }
}

/// 一条待校验的 CP 引用：src 条目（0 = 结构级引用，如 this_class）的 label 字段
/// 指向 target，target 必须命中 expectedMask。label 值存储：动态拼的名字
/// （Fieldref/Methodref/… 前缀）不能指向迭代期栈缓冲。
struct RefCheck
{
    u32         srcIdx;
    u32         targetIdx;
    TagMask     expectedMask;
    std::string label;
};

/// BootstrapMethods 属性的一个条目（环检测需要 Dynamic → 其 bsm args 的边）。
struct BsmEntry
{
    u32              handleRef = 0;
    std::vector<u32> args;
};

/// 边界检查读取游标：越界读取置 truncated 并返回 0，由调用方收尾。
struct Cursor
{
    const u8* data      = nullptr;
    i32       size      = 0;
    i32       off       = 0;
    bool      truncated = false;

    bool avail(i64 n) const { return !truncated && off >= 0 && (i64) off + n <= (i64) size; }
    u32  u1()
    {
        if (!avail(1)) {
            truncated = true;
            return 0;
        }
        return data[off++];
    }
    u32 u2()
    {
        if (!avail(2)) {
            truncated = true;
            return 0;
        }
        u32 v = ((u32) data[off] << 8) | data[off + 1];
        off  += 2;
        return v;
    }
    u32 u2At(i32 at) const
    {
        if (!truncated && at >= 0 && (i64) at + 2 <= (i64) size) {
            return ((u32) data[at] << 8) | data[at + 1];
        }
        return 0;
    }
    u64 u4()
    {
        if (!avail(4)) {
            truncated = true;
            return 0;
        }
        u64 v = ((u64) data[off] << 24) | ((u64) data[off + 1] << 16) |
                ((u64) data[off + 2] << 8) | (u64) data[off + 3];
        off  += 4;
        return v;
    }
};

class Sanitizer
{
public:
    Sanitizer(const u8* data, i32 size)
    {
        cur_.data = data;
        cur_.size = size;
    }

    void add(SanitizeIssueKind kind, u32 cpIndex, u32 offset, const char* fmt, ...)
    {
        if (issues_.size() >= kMaxIssues) return;
        char    buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        issues_.push_back(SanitizeIssue{kind, cpIndex, offset, Utf8String::from_cstr(buf)});
    }

    bool full() const { return issues_.size() >= kMaxIssues; }

    Cursor&                     cursor() { return cur_; }
    std::vector<SanitizeIssue>& issues() { return issues_; }
    std::vector<RefCheck>&      refs() { return refs_; }

    /// CP 条目表（scanConstantPool 填充；tag/off 为 0 = 保留槽或未解析）。
    std::vector<u8>&  entryTag() { return entryTag_; }
    std::vector<i32>& entryOff() { return entryOff_; }
    std::vector<u32>& bsmIndexOf() { return bsmIndexOf_; }
    const std::vector<u8>&  entryTag() const { return entryTag_; }
    const std::vector<i32>& entryOff() const { return entryOff_; }
    const Cursor&           cursor() const { return cur_; }
    u32               cpCount() const { return cpCount_; }
    void              setCpCount(u32 n)
    {
        cpCount_   = n;
        entryTag_.assign(n, 0);
        entryOff_.assign(n, 0);
        bsmIndexOf_.assign(n, 0);
    }

    std::vector<BsmEntry>& bsmTable() { return bsmTable_; }
    bool                   bsmSeen() const { return bsmSeen_; }
    void                   setBsmSeen() { bsmSeen_ = true; }

private:
    Cursor                     cur_;
    std::vector<SanitizeIssue> issues_;
    std::vector<RefCheck>      refs_;
    std::vector<u8>            entryTag_;
    std::vector<i32>           entryOff_;
    std::vector<u32>           bsmIndexOf_;
    std::vector<BsmEntry>      bsmTable_;
    bool                       bsmSeen_ = false;
    u32                        cpCount_ = 0;
};

void addRef(Sanitizer& s, u32 srcIdx, u32 targetIdx, TagMask mask, const char* label)
{
    if (s.full()) return;
    s.refs().push_back(RefCheck{srcIdx, targetIdx, mask, label});
}

/// ref 的诊断归属条目：CP 条目引用记 src，结构级引用记 target。
u32 refOwnerIndex(const RefCheck& ref) { return ref.srcIdx != 0 ? ref.srcIdx : ref.targetIdx; }

/// ref 的诊断定位偏移（条目 tag 字节处；结构级引用为 0）。
u32 refOffsetOf(const Sanitizer& s, const RefCheck& ref)
{
    return ref.srcIdx != 0 ? (u32) s.entryOff()[ref.srcIdx] : 0;
}

// ── 常量池扫描（布局推进镜像 classfile 布局，只读不改）──
// 返回 false = CP 结构已坏（坏 tag/截断），后续结构走查无意义。
bool scanConstantPool(Sanitizer& s)
{
    Cursor& cur = s.cursor();
    if (!cur.avail(10)) {
        s.add(SanitizeIssueKind::BadHeader, 0, 0, "truncated class header: %d bytes (< 10)",
              cur.size);
        return false;
    }
    const u32 magic = ((u32) cur.data[0] << 24) | ((u32) cur.data[1] << 16) |
                      ((u32) cur.data[2] << 8) | (u32) cur.data[3];
    if (magic != 0xCAFEBABEu) {
        s.add(SanitizeIssueKind::BadHeader, 0, 0, "bad magic 0x%x", magic);
        return false;
    }
    s.setCpCount(cur.u2At(8));
    const i32 cpCount = (i32) s.cpCount();
    cur.off           = 10;

    const TagMask refTag2 = maskOf(kTagMethodref) | maskOf(kTagInterfaceMethodref);

    i32  idx    = 1;
    bool broken = false;
    while (idx < cpCount) {
        if (!cur.avail(1)) {
            s.add(SanitizeIssueKind::LengthOutOfRange, (u32) idx, (u32) cur.off,
                  "cp[%d] entry header exceeds file tail @%d", idx, cur.off);
            broken = true;
            break;
        }
        const i32 entryOff = cur.off;
        const u32 tag      = (u32) cur.data[entryOff];
        s.entryTag()[idx]  = (u8) tag;
        s.entryOff()[idx]  = entryOff;
        switch (tag) {
        case kTagUtf8:
        {
            if (!cur.avail(3)) {
                broken = true;
                break;
            }
            const u32 len = cur.u2At(entryOff + 1);
            if (!cur.avail((i64) 3 + (i64) len)) {
                s.add(SanitizeIssueKind::LengthOutOfRange, (u32) idx, (u32) entryOff,
                      "cp[%d] utf8 length %u exceeds file tail @%d", idx, len, entryOff);
                broken = true;
                break;
            }
            cur.off = entryOff + 3 + (i32) len;
            break;
        }
        case kTagInteger:
        case kTagFloat:
            if (!cur.avail(5)) broken = true;
            else cur.off = entryOff + 5;
            break;
        case kTagFieldref:
        case kTagMethodref:
        case kTagInterfaceMethodref:
        {
            if (!cur.avail(5)) {
                broken = true;
                break;
            }
            const char* name = tag == kTagFieldref    ? "Fieldref"
                               : tag == kTagMethodref ? "Methodref"
                                                      : "InterfaceMethodref";
            char        label[64];
            std::snprintf(label, sizeof(label), "%s.class_index", name);
            addRef(s, (u32) idx, cur.u2At(entryOff + 1), maskOf(kTagClass), label);
            std::snprintf(label, sizeof(label), "%s.name_and_type_index", name);
            addRef(s, (u32) idx, cur.u2At(entryOff + 3), maskOf(kTagNameAndType), label);
            cur.off = entryOff + 5;
            break;
        }
        case kTagNameAndType:
            if (!cur.avail(5)) {
                broken = true;
                break;
            }
            addRef(s, (u32) idx, cur.u2At(entryOff + 1), maskOf(kTagUtf8),
                   "NameAndType.name_index");
            addRef(s, (u32) idx, cur.u2At(entryOff + 3), maskOf(kTagUtf8),
                   "NameAndType.descriptor_index");
            cur.off = entryOff + 5;
            break;
        case kTagMethodHandle:
        {
            if (!cur.avail(4)) {
                broken = true;
                break;
            }
            const u32 kind = (u32) cur.data[entryOff + 1];
            if (kind < 1 || kind > 9) {
                s.add(SanitizeIssueKind::BadMethodHandleKind, (u32) idx, (u32) entryOff,
                      "cp[%d] MethodHandle.reference_kind=%u not in [1,9]", idx, kind);
            }
            // JVMS 4.4.8：kind 1-4 → Fieldref；5-9 → Methodref/InterfaceMethodref。
            // kind 无效时期望集合放宽到全部 ref tag：坏 kind 已单独报告，不叠加错报。
            const TagMask refMask = (kind >= 1 && kind <= 4) ? maskOf(kTagFieldref) : refTag2;
            addRef(s, (u32) idx, cur.u2At(entryOff + 2), refMask, "MethodHandle.reference_index");
            cur.off = entryOff + 4;
            break;
        }
        case kTagClass:
            if (!cur.avail(3)) {
                broken = true;
                break;
            }
            addRef(s, (u32) idx, cur.u2At(entryOff + 1), maskOf(kTagUtf8), "Class.name_index");
            cur.off = entryOff + 3;
            break;
        case kTagString:
            if (!cur.avail(3)) {
                broken = true;
                break;
            }
            addRef(s, (u32) idx, cur.u2At(entryOff + 1), maskOf(kTagUtf8), "String.string_index");
            cur.off = entryOff + 3;
            break;
        case kTagMethodType:
            if (!cur.avail(3)) {
                broken = true;
                break;
            }
            addRef(s, (u32) idx, cur.u2At(entryOff + 1), maskOf(kTagUtf8),
                   "MethodType.descriptor_index");
            cur.off = entryOff + 3;
            break;
        case kTagModule:
            if (!cur.avail(3)) {
                broken = true;
                break;
            }
            addRef(s, (u32) idx, cur.u2At(entryOff + 1), maskOf(kTagUtf8), "Module.name_index");
            cur.off = entryOff + 3;
            break;
        case kTagPackage:
            if (!cur.avail(3)) {
                broken = true;
                break;
            }
            addRef(s, (u32) idx, cur.u2At(entryOff + 1), maskOf(kTagUtf8), "Package.name_index");
            cur.off = entryOff + 3;
            break;
        case kTagDynamic:
        case kTagInvokeDynamic:
            if (!cur.avail(5)) {
                broken = true;
                break;
            }
            s.bsmIndexOf()[idx] = cur.u2At(entryOff + 1);
            addRef(s, (u32) idx, cur.u2At(entryOff + 3), maskOf(kTagNameAndType),
                   tag == kTagDynamic ? "Dynamic.name_and_type_index"
                                      : "InvokeDynamic.name_and_type_index");
            cur.off = entryOff + 5;
            break;
        case kTagLong:
        case kTagDouble:
            if (!cur.avail(9)) {
                broken = true;
                break;
            }
            cur.off = entryOff + 9;
            idx++;   // long/double 占双槽，第二槽保留（entryTag/entryOff 保持 0）
            break;
        default:
            s.add(SanitizeIssueKind::BadCpTag, (u32) idx, (u32) entryOff,
                  "cp[%d] unknown constant tag %u @%d", idx, tag, entryOff);
            broken = true;
            break;
        }
        if (broken) break;
        ++idx;
    }
    return !broken;
}

/// 定位 name_index 指向的 Utf8 内容并与静态串比较（不 intern、不分配）。
/// Utf8 条目布局：[tag(1)][length(2)][bytes]，entryOff 指向 tag 字节。
bool utf8Equals(const Sanitizer& s, u32 nameIdx, const char* str)
{
    if (nameIdx == 0 || nameIdx >= s.cpCount()) return false;
    if (s.entryTag()[nameIdx] != kTagUtf8) return false;
    const i32 entryOff = s.entryOff()[nameIdx];
    if (entryOff < 0 || (i64) entryOff + 3 > (i64) s.cursor().size) return false;
    const u32 len = s.cursor().u2At(entryOff + 1);
    if ((i64) entryOff + 3 + (i64) len > (i64) s.cursor().size) return false;
    for (u32 i = 0; i < len; ++i) {
        if (str[i] == '\0') return false;
        if ((char) s.cursor().data[entryOff + 3 + (i32) i] != str[i]) return false;
    }
    return str[len] == '\0';
}

/// 解析 BootstrapMethods 属性体：登记 method_ref/args 引用并记录 bsm 表。
/// 返回 false = 结构已坏。
bool parseBootstrapMethods(Sanitizer& s, i32 contentOff)
{
    Cursor& cur  = s.cursor();
    Cursor  view = cur;
    view.off     = contentOff;
    if (!view.avail(2)) {
        s.add(SanitizeIssueKind::LengthOutOfRange, 0, (u32) contentOff,
              "BootstrapMethods attribute body truncated @%d", contentOff);
        return false;
    }
    const u32 count = view.u2();
    s.setBsmSeen();
    s.bsmTable().resize(count);
    for (u32 j = 0; j < count; ++j) {
        if (!view.avail(4)) {
            s.add(SanitizeIssueKind::LengthOutOfRange, 0, (u32) view.off,
                  "BootstrapMethods[%u] entry truncated @%d", j, view.off);
            return false;
        }
        BsmEntry& entry = s.bsmTable()[j];
        entry.handleRef = view.u2();
        const u32 argc  = view.u2();
        char      label[64];
        std::snprintf(label, sizeof(label), "BootstrapMethods[%u].method_ref", j);
        addRef(s, 0, entry.handleRef, maskOf(kTagMethodHandle), label);
        entry.args.resize(argc);
        for (u32 a = 0; a < argc; ++a) {
            if (!view.avail(2)) {
                s.add(SanitizeIssueKind::LengthOutOfRange, 0, (u32) view.off,
                      "BootstrapMethods[%u] argument list truncated @%d", j, view.off);
                return false;
            }
            entry.args[a] = view.u2();
            std::snprintf(label, sizeof(label), "BootstrapMethods[%u].argument[%u]", j, a);
            addRef(s, 0, entry.args[a], kLoadableMask, label);
        }
    }
    return true;
}

/// 属性表走查：校验 name_index/length；class 级拦截 BootstrapMethods 并解析。
/// 返回 false = 结构已坏。
bool walkAttributes(Sanitizer& s, bool classLevel)
{
    Cursor& cur = s.cursor();
    if (!cur.avail(2)) return false;
    const u32 count = cur.u2();
    for (u32 a = 0; a < count; ++a) {
        if (!cur.avail(6)) return false;
        const u32 nameIdx = cur.u2();
        const u64 len     = cur.u4();
        const i32 attrOff = cur.off;
        addRef(s, 0, nameIdx, maskOf(kTagUtf8),
               classLevel ? "class attribute name_index" : "member attribute name_index");
        if (!cur.avail((i64) len)) {
            s.add(SanitizeIssueKind::LengthOutOfRange, nameIdx, (u32) attrOff,
                  "attribute (name cp[%u]) length %llu exceeds file tail @%d", nameIdx,
                  (unsigned long long) len, attrOff);
            return false;
        }
        if (classLevel && utf8Equals(s, nameIdx, "BootstrapMethods")) {
            if (!parseBootstrapMethods(s, attrOff)) return false;
        }
        cur.off = attrOff + (i32) len;
        if (s.full()) return true;
    }
    return true;
}

// ── CP 之后的结构走查：this/super/ifaces + fields/methods + class 属性表 ──
// 返回 false = 结构已坏。
bool walkClassStructure(Sanitizer& s)
{
    Cursor& cur = s.cursor();

    if (!cur.avail(6)) return false;
    (void) cur.u2();   // access_flags
    const u32 thisIdx  = cur.u2();
    const u32 superIdx = cur.u2();
    addRef(s, 0, thisIdx, maskOf(kTagClass), "this_class");
    if (superIdx != 0) addRef(s, 0, superIdx, maskOf(kTagClass), "super_class");

    if (!cur.avail(2)) return false;
    const u32 ifaceCount = cur.u2();
    for (u32 i = 0; i < ifaceCount; ++i) {
        if (!cur.avail(2)) return false;
        char label[32];
        std::snprintf(label, sizeof(label), "interface[%u]", i);
        addRef(s, 0, cur.u2(), maskOf(kTagClass), label);
    }

    // 字段/方法共用布局：access(2) name(2) descriptor(2) attributes。
    auto walkMembers = [&](const char* what) -> bool {
        if (!cur.avail(2)) return false;
        const u32 count = cur.u2();
        for (u32 m = 0; m < count; ++m) {
            if (!cur.avail(6)) return false;
            (void) cur.u2();   // access_flags
            char label[40];
            std::snprintf(label, sizeof(label), "%s name_index", what);
            addRef(s, 0, cur.u2(), maskOf(kTagUtf8), label);
            std::snprintf(label, sizeof(label), "%s descriptor_index", what);
            addRef(s, 0, cur.u2(), maskOf(kTagUtf8), label);
            if (!walkAttributes(s, false)) return false;
            if (s.full()) return true;   // 诊断已满：余下结构不再有增量信息
        }
        return true;
    };
    if (!walkMembers("field")) return false;
    if (!walkMembers("method")) return false;
    return walkAttributes(s, true);
}

// ── 引用校验：越界 / 保留槽 / tag 错配，通过者进环检测图 ──
void validateRefs(Sanitizer& s, std::vector<std::vector<u32>>& adj)
{
    const u32 cpCount = s.cpCount();
    adj.assign(cpCount, {});
    for (const RefCheck& ref : s.refs()) {
        const char* owner   = ref.srcIdx != 0 ? "cp[%u] " : "";   // 结构级引用无 CP 归属
        u32         ownerIdx = refOwnerIndex(ref);
        if (ref.targetIdx == 0) {
            s.add(SanitizeIssueKind::RefIndexOutOfRange, refOwnerIndex(ref),
                  refOffsetOf(s, ref), "%s%s=0 (null reference)", owner,
                  ref.label.c_str());
            continue;
        }
        if (ref.targetIdx >= cpCount) {
            s.add(SanitizeIssueKind::RefIndexOutOfRange, refOwnerIndex(ref),
                  refOffsetOf(s, ref), "%s%s=%u out of range [1,%u)", owner,
                  ref.label.c_str(), ref.targetIdx, cpCount);
            continue;
        }
        const u8 tag = s.entryTag()[ref.targetIdx];
        if (tag == 0) {
            s.add(SanitizeIssueKind::RefIndexOutOfRange, refOwnerIndex(ref),
                  refOffsetOf(s, ref), "%s%s=%u -> long/double reserved slot", owner,
                  ref.label.c_str(), ref.targetIdx);
            continue;
        }
        if ((maskOf(tag) & ref.expectedMask) == 0) {
            char expect[64];
            expectedNames(ref.expectedMask, expect, sizeof(expect));
            s.add(SanitizeIssueKind::RefTagMismatch, refOwnerIndex(ref), refOffsetOf(s, ref),
                  "%s%s=%u -> expected %s, got %s", owner, ref.label.c_str(), ref.targetIdx,
                  expect, tagName(tag));
            continue;
        }
        if (ref.srcIdx != 0) adj[ref.srcIdx].push_back(ref.targetIdx);
    }
}

// ── bootstrap_method_attr_index 越界校验 ──
void validateBootstrapIndexes(Sanitizer& s)
{
    const i32 n = (i32) s.cpCount();
    for (i32 idx = 1; idx < n; ++idx) {
        const u8 tag = s.entryTag()[idx];
        if (tag != kTagDynamic && tag != kTagInvokeDynamic) continue;
        if (!s.bsmSeen()) {
            s.add(SanitizeIssueKind::BootstrapIndexOutOfRange, (u32) idx,
                  (u32) s.entryOff()[idx],
                  "cp[%d] %s requires a BootstrapMethods attribute, but none present", idx,
                  tag == kTagDynamic ? "Dynamic" : "InvokeDynamic");
            continue;
        }
        const u32 b    = s.bsmIndexOf()[idx];
        const u32 bsmN = (u32) s.bsmTable().size();
        if (b >= bsmN) {
            s.add(SanitizeIssueKind::BootstrapIndexOutOfRange, (u32) idx,
                  (u32) s.entryOff()[idx],
                  "cp[%d] %s.bootstrap_method_attr_index=%u out of range [0,%u)", idx,
                  tag == kTagDynamic ? "Dynamic" : "InvokeDynamic", b, bsmN);
        }
    }
}

// ── 环检测：迭代 DFS（防敌意长链打爆本函数自己的栈）──
// Dynamic → 其 bootstrap 参数的边是 tag-legal 引用图里唯一的环来源，正是
// 常量解析无界递归（栈溢出）的向量；其余 JVMS-legal 边按 tag 层级分层、天然无环。
// 只在全部引用校验通过后调用（图里只含 tag-legal 边）。
void detectCycles(Sanitizer& s, std::vector<std::vector<u32>>& adj)
{
    const i32 n = (i32) s.cpCount();
    for (i32 idx = 1; idx < n; ++idx) {
        if (s.entryTag()[idx] != kTagDynamic) continue;
        const u32 b = s.bsmIndexOf()[idx];
        if (b >= s.bsmTable().size()) continue;   // 越界已单独报告
        for (u32 arg : s.bsmTable()[b].args) {
            if (arg > 0 && arg < (u32) n) adj[idx].push_back(arg);
        }
    }

    std::vector<u8>                    color(n, 0);   // 0=未访问 1=在栈 2=完成
    std::vector<std::pair<u32, usize>> stack;         // 节点 + 下一条边序号
    for (i32 start = 1; start < n && !s.full(); ++start) {
        if (color[start] != 0) continue;
        stack.clear();
        stack.push_back({(u32) start, 0});
        color[start] = 1;
        bool cycleFound = false;
        while (!stack.empty()) {
            const u32 node = stack.back().first;
            if (stack.back().second < adj[node].size()) {
                const u32 next = adj[node][stack.back().second++];
                if (color[next] == 1) {
                    s.add(SanitizeIssueKind::CpCycle, next, (u32) s.entryOff()[next],
                          "cp[%u] constant pool cycle via bootstrap args "
                          "(unbounded readConst recursion)",
                          next);
                    cycleFound = true;
                    break;
                }
                if (color[next] == 0) {
                    color[next] = 1;
                    stack.push_back({next, 0});
                }
            }
            else {
                color[node] = 2;
                stack.pop_back();
            }
        }
        // 起点遍历以染色做全局去重：灰节点（本环路径）置完成，后续起点不重复入栈。
        for (auto& [node, ei] : stack) {
            (void) ei;
            color[node] = 2;
        }
        if (cycleFound) return;   // 一期：报告首个环即可
    }
}

}   // namespace

std::vector<SanitizeIssue> sanitizeClassFile(const ca::u8* data, ca::i32 size)
{
    Sanitizer s(data, size);

    if (!scanConstantPool(s)) return std::move(s.issues());
    if (s.full()) return std::move(s.issues());
    if (!walkClassStructure(s)) return std::move(s.issues());
    if (s.full()) return std::move(s.issues());

    validateBootstrapIndexes(s);
    if (s.full()) return std::move(s.issues());

    std::vector<std::vector<u32>> adj;
    validateRefs(s, adj);
    if (s.full()) return std::move(s.issues());

    detectCycles(s, adj);
    return std::move(s.issues());
}

std::vector<SanitizeIssue> sanitizeClassFile(const std::vector<ca::u8>& bytes)
{
    return sanitizeClassFile(bytes.data(), static_cast<ca::i32>(bytes.size()));
}

ca::str::Utf8String joinSanitizeIssues(const std::vector<SanitizeIssue>& issues,
                                       ca::usize maxIssues)
{
    if (issues.empty()) return Utf8String::from_cstr("");
    char              buf[64];
    Utf8StringBuilder b;
    std::snprintf(buf, sizeof(buf), "%u issue(s): ", (u32) issues.size());
    b.append(buf);
    const usize n = issues.size() < maxIssues ? issues.size() : maxIssues;
    for (usize i = 0; i < n; ++i) {
        if (i != 0) b.append("; ");
        b.append(issues[i].message);
    }
    if (issues.size() > n) {
        std::snprintf(buf, sizeof(buf), "; ...(+%u more)", (u32) (issues.size() - n));
        b.append(buf);
    }
    return b.build();
}

}   // namespace mjbase::spi
