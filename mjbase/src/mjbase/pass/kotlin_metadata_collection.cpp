#include "mjbase/pass/kotlin_metadata_collection.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"

namespace mjbase::pass {
using ca::core::Err;
using ca::core::Ok;

namespace {

// ── 注解面（类级 RuntimeVisibleAnnotations 中 Lkotlin/Metadata; 条目直采）──

/// Metadata 注解的元素值直采结果：k/mv/xi/xs/pn 标量 + d1/d2 串表。
struct MetadataAnnoFace
{
    bool                     hasK  = false;
    ca::i32                  k     = 0;
    bool                     hasMv = false;
    std::vector<ca::i32>     mv;
    bool                     hasXi = false;
    ca::i32                  xi    = 0;
    bool                     hasXs = false;
    std::string              xs;
    bool                     hasPn = false;
    std::string              pn;
    bool                     hasD1 = false;
    std::vector<std::string> d1;
    bool                     hasD2 = false;
    std::vector<std::string> d2;
};

/// CP String 条目内容（a = Utf8 索引）；非 String 条目返回空。
std::string_view cpStringText(const jbc::core::ClassFile& cf, ca::u32 stringCp)
{
    if (!cf.cp.valid(stringCp) || cf.cp.tagOf(stringCp) != jbc::core::CpTag::String) return {};
    return cf.cp.utf8Of(cf.cp.at(stringCp).a);
}

/// CP Integer 条目值；非 Integer 条目返回 false。
bool cpIntValue(const jbc::core::ClassFile& cf, ca::u32 intCp, ca::i32& out)
{
    if (!cf.cp.valid(intCp) || cf.cp.tagOf(intCp) != jbc::core::CpTag::Integer) return false;
    out = cf.cp.at(intCp).ival;
    return true;
}

/// 注解键值对直采：k/mv/xi/xs/pn/d1/d2；形状不符返回 false（整注解
/// 拒收，fail-closed）。未列名的键忽略（bv 等 v1 不消费面）。
bool readAnnoFace(const jbc::core::ClassFile& cf, const jbc::core::AnnotationsAttr::Annotation& anno,
                  MetadataAnnoFace& face)
{
    for (const auto& [keyCp, value] : anno.values) {
        const std::string_view key = cf.cp.utf8Of(keyCp);
        if (key == "k") {
            ca::i32 v = 0;
            if (value.tag != 'I' || !cpIntValue(cf, value.cp, v)) return false;
            face.hasK = true;
            face.k    = v;
        }
        else if (key == "mv") {
            if (value.tag != '[') return false;
            std::vector<ca::i32> ints;
            ints.reserve(value.items.size());
            for (const auto& item : value.items) {
                ca::i32 v = 0;
                if (item.tag != 'I' || !cpIntValue(cf, item.cp, v)) return false;
                ints.push_back(v);
            }
            face.hasMv = true;
            face.mv    = std::move(ints);
        }
        else if (key == "xi") {
            ca::i32 v = 0;
            if (value.tag != 'I' || !cpIntValue(cf, value.cp, v)) return false;
            face.hasXi = true;
            face.xi    = v;
        }
        else if (key == "xs" || key == "pn") {
            if (value.tag != 's') return false;
            std::string text(cpStringText(cf, value.cp));
            if (key == "xs") {
                face.hasXs = true;
                face.xs    = std::move(text);
            }
            else {
                face.hasPn = true;
                face.pn    = std::move(text);
            }
        }
        else if (key == "d1" || key == "d2") {
            if (value.tag != '[') return false;
            std::vector<std::string> strings;
            strings.reserve(value.items.size());
            for (const auto& item : value.items) {
                if (item.tag != 's') return false;
                strings.emplace_back(cpStringText(cf, item.cp));
            }
            if (key == "d1") {
                face.hasD1 = true;
                face.d1    = std::move(strings);
            }
            else {
                face.hasD2 = true;
                face.d2    = std::move(strings);
            }
        }
    }
    return true;
}

/// 类级注解面读取：无 ``Lkotlin/Metadata;`` 条目返回 Err(NoAnnotation)；
/// 条目在但注解体解码失败或形状不符返回 Err(BadAnnotationShape)。
ca::core::Result<MetadataAnnoFace, KotlinMetadataError> readMetadataAnnoFace(jbc::core::ClassFile& cf)
{
    jbc::core::AttrSlot* slot = cf.findClassAttr("RuntimeVisibleAnnotations");
    if (slot == nullptr) return Err(KotlinMetadataError::NoAnnotation);
    auto decoded = cf.decodeAttr(*slot);
    if (decoded.is_err()) return Err(KotlinMetadataError::BadAnnotationShape);
    const auto* body = ca::core::tag_cast::cast<const jbc::core::AnnotationsAttr*>(slot->peek());
    if (body == nullptr) return Err(KotlinMetadataError::BadAnnotationShape);
    for (const auto& anno : body->annos) {
        if (!cf.cp.utf8Equals(anno.typeCp, "Lkotlin/Metadata;")) continue;
        MetadataAnnoFace face;
        if (!readAnnoFace(cf, anno, face)) return Err(KotlinMetadataError::BadAnnotationShape);
        return Ok(std::move(face));
    }
    return Err(KotlinMetadataError::NoAnnotation);
}

// ── d1 解码（kotlin BitEncoding.decodeBytes 同口径）──

/// CP modified-UTF8 串转 UTF-16 码元序列（0xC0 0x80 按 U+0000；余按标准
/// UTF-8）。d1 的编码域只含 0..0xFFFF 码元，越域（补充平面/非法序列）拒收。
bool utf16UnitsOf(std::string_view utf8, std::vector<ca::u16>& units)
{
    const ca::usize n = utf8.size();
    ca::usize       i = 0;
    while (i < n) {
        const ca::u8 b0 = static_cast<ca::u8>(utf8[i]);
        if (b0 == 0xC0 && i + 1 < n && static_cast<ca::u8>(utf8[i + 1]) == 0x80) {
            units.push_back(0x0000);
            i += 2;
            continue;
        }
        if (b0 < 0x80) {
            units.push_back(static_cast<ca::u16>(b0));
            i += 1;
            continue;
        }
        ca::u32   cp    = 0;
        ca::usize extra = 0;
        if ((b0 & 0xE0) == 0xC0) {
            cp    = b0 & 0x1F;
            extra = 1;
        }
        else if ((b0 & 0xF0) == 0xE0) {
            cp    = b0 & 0x0F;
            extra = 2;
        }
        else if ((b0 & 0xF8) == 0xF0) {
            cp    = b0 & 0x07;
            extra = 3;
        }
        else {
            return false;
        }
        if (extra + 1 > n - i) return false;
        for (ca::usize k = 1; k <= extra; ++k) {
            const ca::u8 cont = static_cast<ca::u8>(utf8[i + k]);
            if ((cont & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cont & 0x3F);
        }
        i += extra + 1;
        if (cp > 0xFFFF) return false;
        units.push_back(static_cast<ca::u16>(cp));
    }
    return true;
}

/// 码元低字节拼接（kotlin combineStringArrayIntoBytes 同口径）。
void combineUnitsToBytes(const std::vector<ca::u16>& units, std::vector<ca::u8>& out)
{
    out.reserve(out.size() + units.size());
    for (const ca::u16 u : units) out.push_back(static_cast<ca::u8>(u & 0xFF));
}

/// 模 128 加（kotlin addModuloByte 同口径；0x7F 即减 1 的逆步）。
void addModulo128(std::vector<ca::u8>& bytes, ca::u8 addend)
{
    for (ca::u8& b : bytes) b = static_cast<ca::u8>((b + addend) & 0x7F);
}

/// 7 位组包还原 8 位字节（kotlin decode7to8 同口径；尾部余位丢弃）。末次
/// 读点下标落在 7*len/8 的换算上界内，不越界。
std::vector<ca::u8> decode7to8(const std::vector<ca::u8>& data)
{
    std::vector<ca::u8> result(static_cast<ca::usize>((data.size() * 7) / 8), 0);
    ca::usize           byteIndex = 0;
    ca::usize           bit       = 0;
    for (ca::usize i = 0; i < result.size(); ++i) {
        const ca::u8 firstPart = static_cast<ca::u8>(data[byteIndex] >> bit);
        byteIndex++;
        const ca::u8 mask       = static_cast<ca::u8>((1 << (bit + 1)) - 1);
        const ca::u8 secondPart = static_cast<ca::u8>((data[byteIndex] & mask) << (7 - bit));
        result[i]               = static_cast<ca::u8>(firstPart + secondPart);
        if (bit == 6) {
            byteIndex++;
            bit = 0;
        }
        else {
            bit++;
        }
    }
    return result;
}

/// d1 串表解码（kotlin BitEncoding.decodeBytes 同口径）。
///
/// 首串首码元 U+0000 = UTF8 模式（码元低字节直拼）；U+FFFF = 7to8 模式带
/// 标记（去标记后走 7 位组包）；无标记直接 7 位组包。7to8 共同尾步 = 低字
/// 节拼接 → 模 128 减 1 → 7 位还原。
ca::core::Result<std::vector<ca::u8>, KotlinMetadataError> decodeData1(const std::vector<std::string>& d1)
{
    if (d1.empty()) return Err(KotlinMetadataError::BadData1Encoding);

    std::vector<ca::u16> firstUnits;
    if (!utf16UnitsOf(d1.front(), firstUnits)) return Err(KotlinMetadataError::BadData1Encoding);

    enum class Mode
    {
        Bits7,
        Utf8,
    };
    Mode      mode        = Mode::Bits7;
    ca::usize markerUnits = 0;
    if (!firstUnits.empty()) {
        if (firstUnits.front() == 0x0000) {
            mode        = Mode::Utf8;
            markerUnits = 1;
        }
        else if (firstUnits.front() == 0xFFFF) {
            mode        = Mode::Bits7;
            markerUnits = 1;
        }
    }

    std::vector<ca::u8> bytes;
    for (ca::usize i = 0; i < d1.size(); ++i) {
        std::vector<ca::u16> units;
        if (i == 0) {
            units = firstUnits;
        }
        else if (!utf16UnitsOf(d1[i], units)) {
            return Err(KotlinMetadataError::BadData1Encoding);
        }
        if (markerUnits > 0 && !units.empty()) {
            units.erase(units.begin(), units.begin() + static_cast<std::ptrdiff_t>(markerUnits));
        }
        combineUnitsToBytes(units, bytes);
    }

    if (mode == Mode::Bits7) {
        addModulo128(bytes, 0x7F);
        return Ok(decode7to8(bytes));
    }
    return Ok(std::move(bytes));
}

// ── protobuf 线格式子集读取 ──

/// 线格式字段：field 号 + 线型载荷（varint 值 / 长度定界字节视图）。
struct WireField
{
    ca::u32          field  = 0;
    ca::u8           wire   = 0;
    ca::u64          varint = 0;
    std::string_view bytes;
};

/// varint 读取（LEB128；截断/超长即损坏）。
bool readVarint(const ca::u8*& p, const ca::u8* end, ca::u64& out)
{
    ca::u64   value = 0;
    ca::usize shift = 0;
    while (p < end) {
        const ca::u8 b = *p++;
        value |= static_cast<ca::u64>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            out = value;
            return true;
        }
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

/// 单字段读取；干净结束返回 Ok(false)，损坏返回 Err(BadProto)。
ca::core::Result<bool, KotlinMetadataError> readWireField(const ca::u8*& p, const ca::u8* end,
                                                          WireField& out)
{
    if (p >= end) return Ok(false);
    ca::u64 tag = 0;
    if (!readVarint(p, end, tag)) return Err(KotlinMetadataError::BadProto);
    out.field  = static_cast<ca::u32>(tag >> 3);
    out.wire   = static_cast<ca::u8>(tag & 0x07);
    out.varint = 0;
    out.bytes  = {};
    switch (out.wire) {
    case 0: {
        ca::u64 v = 0;
        if (!readVarint(p, end, v)) return Err(KotlinMetadataError::BadProto);
        out.varint = v;
        break;
    }
    case 1:
        if (end - p < 8) return Err(KotlinMetadataError::BadProto);
        std::memcpy(&out.varint, p, 8);   // 定长 64 位按位透传（本面不消费该线型）
        p += 8;
        break;
    case 2: {
        ca::u64 len = 0;
        if (!readVarint(p, end, len)) return Err(KotlinMetadataError::BadProto);
        if (static_cast<ca::u64>(end - p) < len) return Err(KotlinMetadataError::BadProto);
        out.bytes = std::string_view(reinterpret_cast<const char*>(p), static_cast<ca::usize>(len));
        p += static_cast<ca::usize>(len);
        break;
    }
    case 5:
        if (end - p < 4) return Err(KotlinMetadataError::BadProto);
        p += 4;
        break;
    default:
        return Err(KotlinMetadataError::BadProto);   // 组线型（3/4）不在 metadata 面
    }
    return Ok(true);
}

/// varint 字段的 i32 视图（kotlin 侧 int32 = 64 位 varint 的低 32 位）。
ca::i32 wireInt(const WireField& f)
{
    return static_cast<ca::i32>(static_cast<ca::u32>(f.varint));
}

// ── d2 串表 resolve（kotlin JvmNameResolver 同口径）──

/// 单条 Record 消息（range 在展开面消费；子串/替换取前两个元素，与
/// kotlin JvmNameResolverBase 的取用口径一致）。
struct StringRecord
{
    ca::i32                  range = 1;
    bool                     hasString = false;
    std::string              string;
    bool                     hasPredefined = false;
    ca::u32                  predefined    = 0;
    ca::u8                   operation     = 0;   ///< 0=NONE 1=INTERNAL_TO_CLASS_ID 2=DESC_TO_CLASS_ID
    std::vector<ca::i32>     substringIndices;
    std::vector<ca::i32>     replaceChars;
};

/// 单条 Record 消息解析（线格式字段直采，未知字段跳过）。
ca::core::Result<StringRecord, KotlinMetadataError> parseRecord(std::string_view bytes)
{
    StringRecord rec;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        switch (f.field) {
        case 1:
            if (f.wire != 0) continue;
            rec.range = wireInt(f);
            break;
        case 2:
            if (f.wire != 0) continue;
            rec.hasPredefined = true;
            rec.predefined    = static_cast<ca::u32>(wireInt(f));
            break;
        case 3:
            if (f.wire != 0) continue;
            rec.operation = static_cast<ca::u8>(wireInt(f) & 0xFF);
            break;
        case 4:
        case 5:
            // packed（单 len 串内多 varint）与散置 varint 双形态都收。
            if (f.wire == 2) {
                const ca::u8* sp   = reinterpret_cast<const ca::u8*>(f.bytes.data());
                const ca::u8* send = sp + f.bytes.size();
                while (sp < send) {
                    ca::u64 v = 0;
                    if (!readVarint(sp, send, v)) return Err(KotlinMetadataError::BadProto);
                    if (f.field == 4) rec.substringIndices.push_back(static_cast<ca::i32>(v));
                    else rec.replaceChars.push_back(static_cast<ca::i32>(v));
                }
            }
            else if (f.wire == 0) {
                if (f.field == 4) rec.substringIndices.push_back(wireInt(f));
                else rec.replaceChars.push_back(wireInt(f));
            }
            break;
        case 6:
            if (f.wire != 2) continue;
            rec.hasString = true;
            rec.string    = std::string(f.bytes);
            break;
        default:
            break;
        }
    }
    return Ok(std::move(rec));
}

/// 预定义串表（kotlin JvmNameResolverBase.PREDEFINED_STRINGS 同表同序，44 条）。
const std::array<const char*, 44>& predefinedStrings()
{
    static const std::array<const char*, 44> table = {
        "kotlin/Any",                             "kotlin/Nothing",
        "kotlin/Unit",                            "kotlin/Throwable",
        "kotlin/Number",                          "kotlin/Byte",
        "kotlin/Double",                          "kotlin/Float",
        "kotlin/Int",                             "kotlin/Long",
        "kotlin/Short",                           "kotlin/Boolean",
        "kotlin/Char",                            "kotlin/CharSequence",
        "kotlin/String",                          "kotlin/Comparable",
        "kotlin/Enum",                            "kotlin/Array",
        "kotlin/ByteArray",                       "kotlin/DoubleArray",
        "kotlin/FloatArray",                      "kotlin/IntArray",
        "kotlin/LongArray",                       "kotlin/ShortArray",
        "kotlin/BooleanArray",                    "kotlin/CharArray",
        "kotlin/Cloneable",                       "kotlin/Annotation",
        "kotlin/collections/Iterable",            "kotlin/collections/MutableIterable",
        "kotlin/collections/Collection",          "kotlin/collections/MutableCollection",
        "kotlin/collections/List",                "kotlin/collections/MutableList",
        "kotlin/collections/Set",                 "kotlin/collections/MutableSet",
        "kotlin/collections/Map",                 "kotlin/collections/MutableMap",
        "kotlin/collections/Map.Entry",           "kotlin/collections/MutableMap.MutableEntry",
        "kotlin/collections/Iterator",            "kotlin/collections/MutableIterator",
        "kotlin/collections/ListIterator",        "kotlin/collections/MutableListIterator",
    };
    return table;
}

/// d2 串表：记录展开 + 按索引 resolve。fail-closed 判据（Recaf
/// hasValidStringTable 同口径）：range 非正或展平总数超出 d2 界整表拒收。
class StringTable
{
public:
    /// 从线格式字节解析 StringTableTypes 并展开记录。
    static ca::core::Result<StringTable, KotlinMetadataError> parse(std::string_view bytes,
                                                                    ca::usize stringCount)
    {
        StringTable   table;
        const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
        const ca::u8* end = p + bytes.size();
        ca::usize     remaining = stringCount;
        for (;;) {
            WireField f;
            auto      next = readWireField(p, end, f);
            if (next.is_err()) return Err(std::move(next).unwrap_err());
            if (!std::move(next).unwrap()) break;
            if (f.field != 1 || f.wire != 2) continue;   ///< local_name 不进 resolve 面
            auto rec = parseRecord(f.bytes);
            if (rec.is_err()) return Err(std::move(rec).unwrap_err());
            StringRecord value = std::move(rec).unwrap();
            if (value.range < 1) return Err(KotlinMetadataError::BadStringTable);
            // 展平：range 份同内容记录（kotlin toExpandedRecordsList 同口径），
            // 总量以 d2 界为上盖（Recaf 同判据）。
            for (ca::i32 r = 0; r < value.range; ++r) {
                if (remaining == 0) return Err(KotlinMetadataError::BadStringTable);
                remaining -= 1;
                table.records_.push_back(value);
            }
        }
        return Ok(std::move(table));
    }

    /// 按串表索引 resolve：串选中（显式串/预定义串/d2 位）→ 子串 → 字符
    /// 替换 → 操作（kotlin JvmNameResolverBase 次序）；越界返回 Err。
    ca::core::Result<std::string, KotlinMetadataError>
    resolve(ca::usize index, const std::vector<std::string>& d2) const
    {
        if (index >= records_.size()) return Err(KotlinMetadataError::BadProto);
        const StringRecord& rec = records_[index];
        std::string         text;
        if (rec.hasString) {
            text = rec.string;
        }
        else if (rec.hasPredefined && rec.predefined < predefinedStrings().size()) {
            text = predefinedStrings()[rec.predefined];
        }
        else {
            if (index >= d2.size()) return Err(KotlinMetadataError::BadProto);
            text = d2[index];
        }
        if (rec.substringIndices.size() >= 2) {
            const ca::i32 begin = rec.substringIndices[0];
            const ca::i32 end   = rec.substringIndices[1];
            if (begin >= 0 && begin <= end && static_cast<ca::usize>(end) <= text.size()) {
                text = text.substr(static_cast<ca::usize>(begin),
                                   static_cast<ca::usize>(end - begin));
            }
        }
        if (rec.replaceChars.size() >= 2 && rec.replaceChars[0] >= 0 && rec.replaceChars[0] < 0x80 &&
            rec.replaceChars[1] >= 0 && rec.replaceChars[1] < 0x80) {
            // ASCII 字节级替换与码点级替换等价（UTF-8 连续字节 >= 0x80 不会
            // 伪命中）；非 ASCII 对保守跳过（申报边界）。
            const char from = static_cast<char>(rec.replaceChars[0]);
            const char to   = static_cast<char>(rec.replaceChars[1]);
            for (char& c : text) {
                if (c == from) c = to;
            }
        }
        if (rec.operation == 1) {
            replaceAll(text, '$', '.');
        }
        else if (rec.operation == 2) {
            if (text.size() >= 2) {
                text = text.substr(1, text.size() - 2);
                replaceAll(text, '$', '.');
            }
        }
        return Ok(std::move(text));
    }

private:
    /// 原位替换单字节字符。
    static void replaceAll(std::string& text, char from, char to)
    {
        for (char& c : text) {
            if (c == from) c = to;
        }
    }

    std::vector<StringRecord> records_;
};

// ── 模型解析（d1 消息面 → Kt 模型）──

/// TypeTable 消息（id 形类型引用的展开源；entries 存消息字节视图，寿命由
/// d1 解码字节承载）。
struct TypeTable
{
    std::vector<std::string_view> entries;
    ca::i32                       firstNullable    = -1;
    bool                          hasFirstNullable = false;

    /// 是否为空表（无表消息的 id 引用落空即该类型未给）。
    bool empty() const noexcept { return entries.empty(); }
};

/// 空 TypeTable（构造器消息无自有 TypeTable，参数 id 引用落此即未给）。
const TypeTable& emptyTypeTable()
{
    static const TypeTable table;
    return table;
}

/// 消息解析上下文。
struct ParseCtx
{
    const StringTable&              table;
    const std::vector<std::string>& d2;
    ca::str::Utf8StringArena&       arena;
};

// 互递归前置声明（Type ↔ Argument ↔ TypeTable）。
ca::core::Result<KtType, KotlinMetadataError>
parseTypeMessage(std::string_view bytes, ParseCtx& ctx, const TypeTable& typeTable);

ca::core::Result<KtType, KotlinMetadataError>
typeFromTable(const TypeTable& typeTable, ca::i32 id, ParseCtx& ctx);

/// resolve 后 intern 为内部名形（`.` 转 `$`，Recaf 口径）。
ca::core::Result<ca::str::Utf8StringRef, KotlinMetadataError> resolveInternal(ParseCtx& ctx, ca::i32 index)
{
    auto text = ctx.table.resolve(static_cast<ca::usize>(index), ctx.d2);
    if (text.is_err()) return Err(std::move(text).unwrap_err());
    std::string resolved = std::move(text).unwrap();
    for (char& c : resolved) {
        if (c == '.') c = '$';
    }
    return Ok(ctx.arena.intern(std::string_view(resolved)));
}

/// Type.Argument 消息解析（投影 + inline/id 类型）。
ca::core::Result<KtType, KotlinMetadataError>
parseTypeArgument(std::string_view bytes, ParseCtx& ctx, const TypeTable& typeTable)
{
    ca::u32 projection = 2;   ///< INV 缺省（kotlin 缺省 INV）
    bool    hasProjection = false;
    bool    hasInline     = false;
    std::string_view inlineBytes;
    bool    hasId = false;
    ca::i32 id    = 0;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        if (f.field == 1 && f.wire == 0) {
            projection    = static_cast<ca::u32>(f.varint);
            hasProjection = true;
        }
        else if (f.field == 2 && f.wire == 2) {
            hasInline   = true;
            inlineBytes = f.bytes;
        }
        else if (f.field == 3 && f.wire == 0) {
            hasId = true;
            id    = wireInt(f);
        }
    }
    if (hasProjection && projection == 3) return Ok(KtType{});   ///< 星号投影：无名类型
    if (hasInline) return parseTypeMessage(inlineBytes, ctx, typeTable);
    if (hasId) return typeFromTable(typeTable, id, ctx);
    return Ok(KtType{});
}

/// TypeTable[id] 展开（firstNullable 起的类型按 kotlin 语义补 Nullable）。
ca::core::Result<KtType, KotlinMetadataError>
typeFromTable(const TypeTable& typeTable, ca::i32 id, ParseCtx& ctx)
{
    if (typeTable.empty() || id < 0 || static_cast<ca::usize>(id) >= typeTable.entries.size()) {
        return Err(KotlinMetadataError::BadProto);
    }
    auto type = parseTypeMessage(typeTable.entries[static_cast<ca::usize>(id)], ctx, typeTable);
    if (type.is_err()) return type;
    KtType value = std::move(type).unwrap();
    if (typeTable.hasFirstNullable && typeTable.firstNullable >= 0 && id >= typeTable.firstNullable) {
        value.nullability = KtNullability::Nullable;
    }
    return Ok(std::move(value));
}

/// Type 消息解析（Recaf mapType 的名字面优先序 + TypeTable 展开）。
ca::core::Result<KtType, KotlinMetadataError>
parseTypeMessage(std::string_view bytes, ParseCtx& ctx, const TypeTable& typeTable)
{
    KtType  type;
    bool    hasClassName         = false;
    ca::i32 className            = 0;
    bool    hasTypeParameter     = false;
    ca::i32 typeParameter        = 0;
    bool    hasTypeParameterName = false;
    ca::i32 typeParameterName    = 0;
    bool    hasTypeAliasName     = false;
    ca::i32 typeAliasName       = 0;
    bool    hasNullable          = false;
    bool    nullable             = false;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        switch (f.field) {
        case 2:
            if (f.wire != 2) continue;
            {
                auto arg = parseTypeArgument(f.bytes, ctx, typeTable);
                if (arg.is_err()) return Err(std::move(arg).unwrap_err());
                type.arguments.push_back(std::move(arg).unwrap());
            }
            break;
        case 3:
            if (f.wire != 0) continue;
            hasNullable = true;
            nullable    = f.varint != 0;
            break;
        case 6:
            if (f.wire != 0) continue;
            hasClassName = true;
            className    = wireInt(f);
            break;
        case 7:
            if (f.wire != 0) continue;
            hasTypeParameter = true;
            typeParameter    = wireInt(f);
            break;
        case 9:
            if (f.wire != 0) continue;
            hasTypeParameterName = true;
            typeParameterName    = wireInt(f);
            break;
        case 12:
            if (f.wire != 0) continue;
            hasTypeAliasName = true;
            typeAliasName    = wireInt(f);
            break;
        default:
            break;   ///< outer_type/flexible/abbreviated/flags 不进收集面（申报边界）
        }
    }
    if (hasClassName) {
        auto name = resolveInternal(ctx, className);
        if (name.is_err()) return Err(std::move(name).unwrap_err());
        type.name    = std::move(name).unwrap();
        type.hasName = true;
    }
    else if (hasTypeParameter) {
        auto name = resolveInternal(ctx, typeParameter);
        if (name.is_err()) return Err(std::move(name).unwrap_err());
        type.name    = std::move(name).unwrap();
        type.hasName = true;
    }
    else if (hasTypeParameterName) {
        auto name = resolveInternal(ctx, typeParameterName);
        if (name.is_err()) return Err(std::move(name).unwrap_err());
        type.name    = std::move(name).unwrap();
        type.hasName = true;
    }
    else if (hasTypeAliasName) {
        auto name = resolveInternal(ctx, typeAliasName);
        if (name.is_err()) return Err(std::move(name).unwrap_err());
        type.name    = std::move(name).unwrap();
        type.hasName = true;
    }
    if (hasNullable) {
        type.nullability = nullable ? KtNullability::Nullable : KtNullability::NonNull;
    }
    return Ok(std::move(type));
}

/// TypeTable 消息解析（field 1 = Type 消息条目，field 2 = first_nullable）。
ca::core::Result<TypeTable, KotlinMetadataError> parseTypeTable(std::string_view bytes)
{
    TypeTable     table;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        if (f.field == 1 && f.wire == 2) table.entries.push_back(f.bytes);
        else if (f.field == 2 && f.wire == 0) {
            table.hasFirstNullable = true;
            table.firstNullable    = wireInt(f);
        }
    }
    return Ok(std::move(table));
}

/// ValueParameter 消息解析（名 + inline/id 类型）。vararg 元素类型不进面
/// （metadata 的 type 字段本身已是数组形态，申报边界）。
ca::core::Result<KtVariable, KotlinMetadataError>
parseValueParameter(std::string_view bytes, ParseCtx& ctx, const TypeTable& typeTable)
{
    KtVariable var;
    bool       hasName   = false;
    ca::i32    name      = 0;
    bool       hasInline = false;
    std::string_view inlineBytes;
    bool       hasId = false;
    ca::i32    id    = 0;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        if (f.field == 2 && f.wire == 0) {
            hasName = true;
            name    = wireInt(f);
        }
        else if (f.field == 3 && f.wire == 2) {
            hasInline   = true;
            inlineBytes = f.bytes;
        }
        else if (f.field == 5 && f.wire == 0) {
            hasId = true;
            id    = wireInt(f);
        }
    }
    if (hasName) {
        auto nameText = ctx.table.resolve(static_cast<ca::usize>(name), ctx.d2);
        if (nameText.is_err()) return Err(std::move(nameText).unwrap_err());
        var.name = ctx.arena.intern(std::string_view(std::move(nameText).unwrap()));
    }
    if (hasInline) {
        auto type = parseTypeMessage(inlineBytes, ctx, typeTable);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        var.type    = std::move(type).unwrap();
        var.hasType = true;
    }
    else if (hasId) {
        auto type = typeFromTable(typeTable, id, ctx);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        var.type    = std::move(type).unwrap();
        var.hasType = true;
    }
    return Ok(std::move(var));
}

/// Function 消息的公共字段集（name/return/receiver/params/type_table）。
struct FunctionParts
{
    bool                          hasName = false;
    ca::i32                       name    = 0;
    bool                          hasReturnTypeInline = false;
    std::string_view              returnTypeInline;
    bool                          hasReturnTypeId = false;
    ca::i32                       returnTypeId    = 0;
    bool                          hasReceiverInline = false;
    std::string_view              receiverInline;
    bool                          hasReceiverId = false;
    ca::i32                       receiverId    = 0;
    std::vector<std::string_view> parameters;
    TypeTable                     typeTable;
};

/// Function 消息字段遍历（flags/type_parameter/context receiver 不进面，
/// 申报边界）。
ca::core::Result<FunctionParts, KotlinMetadataError> parseFunctionParts(std::string_view bytes)
{
    FunctionParts parts;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        switch (f.field) {
        case 2:
            if (f.wire != 0) continue;
            parts.hasName = true;
            parts.name    = wireInt(f);
            break;
        case 3:
            if (f.wire != 2) continue;
            parts.hasReturnTypeInline = true;
            parts.returnTypeInline    = f.bytes;
            break;
        case 5:
            if (f.wire != 2) continue;
            parts.hasReceiverInline = true;
            parts.receiverInline    = f.bytes;
            break;
        case 6:
            if (f.wire != 2) continue;
            parts.parameters.push_back(f.bytes);
            break;
        case 7:
            if (f.wire != 0) continue;
            parts.hasReturnTypeId = true;
            parts.returnTypeId    = wireInt(f);
            break;
        case 8:
            if (f.wire != 0) continue;
            parts.hasReceiverId = true;
            parts.receiverId    = wireInt(f);
            break;
        case 30:
            if (f.wire != 2) continue;
            {
                auto table = parseTypeTable(f.bytes);
                if (table.is_err()) return Err(std::move(table).unwrap_err());
                parts.typeTable = std::move(table).unwrap();
            }
            break;
        default:
            break;
        }
    }
    return Ok(std::move(parts));
}

/// FunctionParts 落地为 KtFunction（名字与类型在此 resolve/intern）。
ca::core::Result<KtFunction, KotlinMetadataError> finishFunction(const FunctionParts& parts, ParseCtx& ctx)
{
    KtFunction fn;
    if (parts.hasName) {
        auto nameText = ctx.table.resolve(static_cast<ca::usize>(parts.name), ctx.d2);
        if (nameText.is_err()) return Err(std::move(nameText).unwrap_err());
        fn.name = ctx.arena.intern(std::string_view(std::move(nameText).unwrap()));
    }
    if (parts.hasReturnTypeInline) {
        auto type = parseTypeMessage(parts.returnTypeInline, ctx, parts.typeTable);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        fn.returnType    = std::move(type).unwrap();
        fn.hasReturnType = true;
    }
    else if (parts.hasReturnTypeId) {
        auto type = typeFromTable(parts.typeTable, parts.returnTypeId, ctx);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        fn.returnType    = std::move(type).unwrap();
        fn.hasReturnType = true;
    }
    if (parts.hasReceiverInline) {
        auto type = parseTypeMessage(parts.receiverInline, ctx, parts.typeTable);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        fn.receiverType = std::move(type).unwrap();
        fn.hasReceiver  = true;
    }
    else if (parts.hasReceiverId) {
        auto type = typeFromTable(parts.typeTable, parts.receiverId, ctx);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        fn.receiverType = std::move(type).unwrap();
        fn.hasReceiver  = true;
    }
    fn.parameters.reserve(parts.parameters.size());
    for (const std::string_view paramBytes : parts.parameters) {
        auto param = parseValueParameter(paramBytes, ctx, parts.typeTable);
        if (param.is_err()) return Err(std::move(param).unwrap_err());
        fn.parameters.push_back(std::move(param).unwrap());
    }
    return Ok(std::move(fn));
}

/// Property 消息解析（名 + inline/id 类型，经属性自有 TypeTable 展开）。
ca::core::Result<KtVariable, KotlinMetadataError> parseProperty(std::string_view bytes, ParseCtx& ctx)
{
    KtVariable var;
    bool       hasName   = false;
    ca::i32    name      = 0;
    bool       hasInline = false;
    std::string_view inlineBytes;
    bool       hasId = false;
    ca::i32    id    = 0;
    TypeTable  typeTable;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        switch (f.field) {
        case 2:
            if (f.wire != 0) continue;
            hasName = true;
            name    = wireInt(f);
            break;
        case 3:
            if (f.wire != 2) continue;
            hasInline   = true;
            inlineBytes = f.bytes;
            break;
        case 9:
            if (f.wire != 0) continue;
            hasId = true;
            id    = wireInt(f);
            break;
        case 6:
            // setter_parameter 在场 = var 属性（还原件 setter 槽 mutability
            // 门）；消息内容不消费，只记在场位。
            if (f.wire != 2) continue;
            var.hasSetter = true;
            break;
        case 30:
            if (f.wire != 2) continue;
            {
                auto table = parseTypeTable(f.bytes);
                if (table.is_err()) return Err(std::move(table).unwrap_err());
                typeTable = std::move(table).unwrap();
            }
            break;
        default:
            break;   ///< 属性接收者/context receiver 不进面（申报边界）
        }
    }
    if (hasName) {
        auto nameText = ctx.table.resolve(static_cast<ca::usize>(name), ctx.d2);
        if (nameText.is_err()) return Err(std::move(nameText).unwrap_err());
        var.name = ctx.arena.intern(std::string_view(std::move(nameText).unwrap()));
    }
    if (hasInline) {
        auto type = parseTypeMessage(inlineBytes, ctx, typeTable);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        var.type    = std::move(type).unwrap();
        var.hasType = true;
    }
    else if (hasId) {
        auto type = typeFromTable(typeTable, id, ctx);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        var.type    = std::move(type).unwrap();
        var.hasType = true;
    }
    return Ok(std::move(var));
}

/// Class 消息解析（kind=1）。
ca::core::Result<KtClass, KotlinMetadataError> parseClassMessage(std::string_view bytes, ParseCtx& ctx)
{
    KtClass  model;
    model.kind = KtClassKind::Class;
    bool    hasFqName    = false;
    ca::i32 fqName       = 0;
    bool    hasCompanion = false;
    ca::i32 companion    = 0;
    std::vector<ca::i32>          supertypeIds;   ///< 两遍解析：先收字段，type_table 落定后展开
    std::vector<std::string_view> supertypeInline;
    std::vector<std::string_view> constructors;
    std::vector<std::string_view> functions;
    std::vector<std::string_view> properties;
    TypeTable                     typeTable;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        switch (f.field) {
        case 2:
            // supertype_id：packed（单 len 串）与散置 varint 双形态都收。
            if (f.wire == 2) {
                const ca::u8* sp   = reinterpret_cast<const ca::u8*>(f.bytes.data());
                const ca::u8* send = sp + f.bytes.size();
                while (sp < send) {
                    ca::u64 v = 0;
                    if (!readVarint(sp, send, v)) return Err(KotlinMetadataError::BadProto);
                    supertypeIds.push_back(static_cast<ca::i32>(v));
                }
            }
            else if (f.wire == 0) {
                supertypeIds.push_back(wireInt(f));
            }
            break;
        case 3:
            if (f.wire != 0) continue;
            hasFqName = true;
            fqName    = wireInt(f);
            break;
        case 4:
            if (f.wire != 0) continue;
            hasCompanion = true;
            companion    = wireInt(f);
            break;
        case 6:
            if (f.wire != 2) continue;
            supertypeInline.push_back(f.bytes);
            break;
        case 8:
            if (f.wire != 2) continue;
            constructors.push_back(f.bytes);
            break;
        case 9:
            if (f.wire != 2) continue;
            functions.push_back(f.bytes);
            break;
        case 10:
            if (f.wire != 2) continue;
            properties.push_back(f.bytes);
            break;
        case 30:
            if (f.wire != 2) continue;
            {
                auto table = parseTypeTable(f.bytes);
                if (table.is_err()) return Err(std::move(table).unwrap_err());
                typeTable = std::move(table).unwrap();
            }
            break;
        default:
            break;   ///< type_parameter/nested_class/enum_entry/sealed 等不进 v1 面
        }
    }
    if (!hasFqName) return Err(KotlinMetadataError::BadProto);   ///< required 字段缺席
    // type_table 落定后展开父类型（inline 与 id 同表）。
    for (const ca::i32 id : supertypeIds) {
        auto type = typeFromTable(typeTable, id, ctx);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        model.superTypes.push_back(std::move(type).unwrap());
    }
    for (const std::string_view typeBytes : supertypeInline) {
        auto type = parseTypeMessage(typeBytes, ctx, typeTable);
        if (type.is_err()) return Err(std::move(type).unwrap_err());
        model.superTypes.push_back(std::move(type).unwrap());
    }
    auto name = resolveInternal(ctx, fqName);
    if (name.is_err()) return Err(std::move(name).unwrap_err());
    model.name    = std::move(name).unwrap();
    model.hasName = true;
    if (hasCompanion) {
        auto companionName = resolveInternal(ctx, companion);
        if (companionName.is_err()) return Err(std::move(companionName).unwrap_err());
        model.companionObjectName    = std::move(companionName).unwrap();
        model.hasCompanionObjectName = true;
    }
    model.constructors.reserve(constructors.size());
    for (const std::string_view ctorBytes : constructors) {
        // 构造器消息无自有 TypeTable（申报边界）：参数类型只认 inline 形。
        KtConstructor ctor;
        const ca::u8* cp   = reinterpret_cast<const ca::u8*>(ctorBytes.data());
        const ca::u8* cend = cp + ctorBytes.size();
        for (;;) {
            WireField f;
            auto      next = readWireField(cp, cend, f);
            if (next.is_err()) return Err(std::move(next).unwrap_err());
            if (!std::move(next).unwrap()) break;
            if (f.field != 2 || f.wire != 2) continue;
            auto param = parseValueParameter(f.bytes, ctx, emptyTypeTable());
            if (param.is_err()) return Err(std::move(param).unwrap_err());
            ctor.parameters.push_back(std::move(param).unwrap());
        }
        model.constructors.push_back(std::move(ctor));
    }
    model.functions.reserve(functions.size());
    for (const std::string_view fnBytes : functions) {
        auto parts = parseFunctionParts(fnBytes);
        if (parts.is_err()) return Err(std::move(parts).unwrap_err());
        auto fn = finishFunction(std::move(parts).unwrap(), ctx);
        if (fn.is_err()) return Err(std::move(fn).unwrap_err());
        model.functions.push_back(std::move(fn).unwrap());
    }
    model.properties.reserve(properties.size());
    for (const std::string_view propBytes : properties) {
        auto prop = parseProperty(propBytes, ctx);
        if (prop.is_err()) return Err(std::move(prop).unwrap_err());
        model.properties.push_back(std::move(prop).unwrap());
    }
    return Ok(std::move(model));
}

/// Package 消息解析（kind=2/5；name = 定义类二进制名）。
ca::core::Result<KtClass, KotlinMetadataError>
parsePackageMessage(std::string_view bytes, ParseCtx& ctx, ca::str::Utf8StringRef definingName,
                    KtClassKind kind)
{
    KtClass model;
    model.kind    = kind;
    model.name    = definingName;
    model.hasName = true;
    std::vector<std::string_view> functions;
    std::vector<std::string_view> properties;
    const ca::u8* p   = reinterpret_cast<const ca::u8*>(bytes.data());
    const ca::u8* end = p + bytes.size();
    for (;;) {
        WireField f;
        auto      next = readWireField(p, end, f);
        if (next.is_err()) return Err(std::move(next).unwrap_err());
        if (!std::move(next).unwrap()) break;
        if (f.field == 3 && f.wire == 2) functions.push_back(f.bytes);
        else if (f.field == 4 && f.wire == 2) properties.push_back(f.bytes);
    }
    model.functions.reserve(functions.size());
    for (const std::string_view fnBytes : functions) {
        auto parts = parseFunctionParts(fnBytes);
        if (parts.is_err()) return Err(std::move(parts).unwrap_err());
        auto fn = finishFunction(std::move(parts).unwrap(), ctx);
        if (fn.is_err()) return Err(std::move(fn).unwrap_err());
        model.functions.push_back(std::move(fn).unwrap());
    }
    model.properties.reserve(properties.size());
    for (const std::string_view propBytes : properties) {
        auto prop = parseProperty(propBytes, ctx);
        if (prop.is_err()) return Err(std::move(prop).unwrap_err());
        model.properties.push_back(std::move(prop).unwrap());
    }
    return Ok(std::move(model));
}

/// Function 消息解析（kind=3 lambda；name = 定义类二进制名，恰一条函数）。
ca::core::Result<KtClass, KotlinMetadataError>
parseFunctionMessage(std::string_view bytes, ParseCtx& ctx, ca::str::Utf8StringRef definingName)
{
    auto parts = parseFunctionParts(bytes);
    if (parts.is_err()) return Err(std::move(parts).unwrap_err());
    auto fn = finishFunction(std::move(parts).unwrap(), ctx);
    if (fn.is_err()) return Err(std::move(fn).unwrap_err());
    KtClass model;
    model.kind    = KtClassKind::SyntheticClass;
    model.name    = definingName;
    model.hasName = true;
    model.functions.push_back(std::move(fn).unwrap());
    return Ok(std::move(model));
}

// ── 描述符映射工具链 ──

/// kotlin stdlib 转 JVM 等价类型替换表（Recaf mapToJvm switch 同表）。
bool kotlinJvmSubstitution(std::string_view segment, std::string& out)
{
    static const std::array<std::pair<std::string_view, std::string_view>, 56> table = {{
        {"Lkotlin/Boolean;", "Z"},
        {"Lkotlin/BooleanArray;", "[Z"},
        {"Lkotlin/Byte;", "B"},
        {"Lkotlin/ByteArray;", "[B"},
        {"Lkotlin/UByte;", "I"},
        {"Lkotlin/Int;", "I"},
        {"Lkotlin/UInt;", "I"},
        {"Lkotlin/UShort;", "I"},
        {"Lkotlin/UByteArray;", "[I"},
        {"Lkotlin/IntArray;", "[I"},
        {"Lkotlin/UIntArray;", "[I"},
        {"Lkotlin/UShortArray;", "[I"},
        {"Lkotlin/Char;", "C"},
        {"Lkotlin/CharArray;", "[C"},
        {"Lkotlin/Double;", "D"},
        {"Lkotlin/DoubleArray;", "[D"},
        {"Lkotlin/Float;", "F"},
        {"Lkotlin/FloatArray;", "[F"},
        {"Lkotlin/Long;", "J"},
        {"Lkotlin/ULong;", "J"},
        {"Lkotlin/LongArray;", "[J"},
        {"Lkotlin/ULongArray;", "[J"},
        {"Lkotlin/Short;", "S"},
        {"Lkotlin/ShortArray;", "[S"},
        {"Lkotlin/Unit;", "V"},
        {"Lkotlin/Any;", "Ljava/lang/Object;"},
        {"Lkotlin/String;", "Ljava/lang/String;"},
        {"Lkotlin/Enum;", "Ljava/lang/Enum;"},
        {"Lkotlin/Comparable;", "Ljava/lang/Comparable;"},
        {"Lkotlin/Comparator;", "Ljava/lang/Comparator;"},
        {"Lkotlin/reflect/KClass;", "Ljava/lang/Class;"},
        {"Lkotlin/collections/Collection;", "Ljava/util/Collection;"},
        {"Lkotlin/collections/Iterable;", "Ljava/util/Iterable;"},
        {"Lkotlin/collections/Iterator;", "Ljava/util/Iterator;"},
        {"Lkotlin/collections/MutableIterator;", "Ljava/util/Iterator;"},
        {"Lkotlin/collections/ListIterator;", "Ljava/util/ListIterator;"},
        {"Lkotlin/collections/MutableListIterator;", "Ljava/util/ListIterator;"},
        {"Lkotlin/collections/List;", "Ljava/util/List;"},
        {"Lkotlin/collections/MutableList;", "Ljava/util/List;"},
        {"Lkotlin/collections/Set;", "Ljava/util/Set;"},
        {"Lkotlin/collections/MutableSet;", "Ljava/util/Set;"},
        {"Lkotlin/collections/Map;", "Ljava/util/Map;"},
        {"Lkotlin/collections/MutableMap;", "Ljava/util/Map;"},
        {"Lkotlin/Function0;", "Lkotlin/jvm/functions/Function0;"},
        {"Lkotlin/Function1;", "Lkotlin/jvm/functions/Function1;"},
        {"Lkotlin/Function2;", "Lkotlin/jvm/functions/Function2;"},
        {"Lkotlin/Function3;", "Lkotlin/jvm/functions/Function3;"},
        {"Lkotlin/Function4;", "Lkotlin/jvm/functions/Function4;"},
        {"Lkotlin/Function5;", "Lkotlin/jvm/functions/Function5;"},
        {"Lkotlin/Function6;", "Lkotlin/jvm/functions/Function6;"},
        {"Lkotlin/Function7;", "Lkotlin/jvm/functions/Function7;"},
        {"Lkotlin/Function8;", "Lkotlin/jvm/functions/Function8;"},
        {"Lkotlin/Function9;", "Lkotlin/jvm/functions/Function9;"},
        {"Lkotlin/Function10;", "Lkotlin/jvm/functions/Function10;"},
        {"Lkotlin/FunctionN;", "Lkotlin/jvm/functions/FunctionN;"},
    }};
    for (const auto& [from, to] : table) {
        if (segment == from) {
            out.append(to);
            return true;
        }
    }
    return false;
}

/// 描述符文法内的一个类型段长度（数组前缀 + L 类段或原语段）；0 = 病态。
ca::usize typeSegmentLength(std::string_view s)
{
    ca::usize j = 0;
    while (j < s.size() && s[j] == '[') ++j;
    if (j >= s.size()) return 0;
    if (s[j] == 'L') {
        const ca::usize semi = s.find(';', j);
        if (semi == std::string_view::npos) return 0;
        return semi + 1;
    }
    switch (s[j]) {
    case 'B':
    case 'C':
    case 'D':
    case 'F':
    case 'I':
    case 'J':
    case 'S':
    case 'Z':
        break;
    default:
        return 0;
    }
    return j + 1;
}

/// 描述符 walker：method 形 `(参数)返回` 与 field 形单类型段统一走
/// appendSeg 段处理器；文法不符返回 false（调用方原样返回输入）。
template <typename AppendSeg>
bool walkDescriptor(std::string_view desc, AppendSeg&& appendSeg, std::string& out)
{
    if (desc.empty()) return false;
    out.clear();
    const bool       method = desc.front() == '(';
    std::string_view tail   = desc;   ///< field 形：整段即单类型段
    if (method) {
        const ca::usize close = desc.find(')');
        if (close == std::string_view::npos) return false;
        out.push_back('(');
        ca::usize i = 1;
        while (i < close) {
            const ca::usize segLen = typeSegmentLength(desc.substr(i, close - i));
            if (segLen == 0) return false;
            appendSeg(desc.substr(i, segLen), out);
            i += segLen;
        }
        out.push_back(')');
        tail = desc.substr(close + 1);
    }
    if (tail == "V") {
        if (!method) return false;   ///< field 描述符无 V 返回段
        out.push_back('V');
        return true;
    }
    const ca::usize segLen = typeSegmentLength(tail);
    if (segLen == 0 || segLen != tail.size()) return false;
    appendSeg(tail, out);
    return true;
}

}   // namespace

// ── 公开 API ──

const char* to_string(KotlinMetadataError e) noexcept
{
    switch (e) {
    case KotlinMetadataError::Ok: return "ok";
    case KotlinMetadataError::NoAnnotation: return "no annotation";
    case KotlinMetadataError::BadAnnotationShape: return "bad annotation shape";
    case KotlinMetadataError::BadData1Encoding: return "bad d1 encoding";
    case KotlinMetadataError::BadStringTable: return "bad string table";
    case KotlinMetadataError::BadProto: return "bad protobuf payload";
    case KotlinMetadataError::UnsupportedKind: return "unsupported metadata kind";
    }
    return "unknown";
}

std::string ktTypeDescriptor(const KtType& type)
{
    if (!type.hasName) return "Ljava/lang/Object;";
    const std::string name = type.name.to_std_string();
    if (name == "kotlin/Array") {
        if (type.arguments.empty()) return "Ljava/lang/Object;";
        return "[" + ktTypeDescriptor(type.arguments.front());
    }
    return "L" + name + ";";
}

std::string ktFunctionDescriptor(const KtFunction& fn)
{
    std::string out = "(";
    if (fn.hasReceiver) out += ktTypeDescriptor(fn.receiverType);
    for (const KtVariable& param : fn.parameters) {
        out += ktTypeDescriptor(param.hasType ? param.type : KtType{});
    }
    out += ")";
    out += fn.hasReturnType ? ktTypeDescriptor(fn.returnType) : std::string("Ljava/lang/Object;");
    return out;
}

std::string kotlinToJvmDescriptor(std::string_view descriptor)
{
    std::string out;
    const bool  ok = walkDescriptor(
        descriptor,
        [](std::string_view seg, std::string& sink) {
            ca::usize j = 0;
            while (j < seg.size() && seg[j] == '[') ++j;
            sink.append(seg.substr(0, j));
            if (j >= seg.size() || seg[j] != 'L') {
                if (j < seg.size()) sink.push_back(seg[j]);
                return;
            }
            if (seg.back() != ';') {
                sink.append(seg.substr(j));
                return;
            }
            if (kotlinJvmSubstitution(seg.substr(j), sink)) return;
            sink.append(seg.substr(j));
        },
        out);
    if (!ok) return std::string(descriptor);
    return out;
}

bool KotlinMetadataCollection::add(ca::str::Utf8StringRef binaryName, KtClass&& model)
{
    const auto key = arena_.intern(binaryName);
    if (byBinary_.find(key) != byBinary_.end()) return false;
    const ca::usize index = entries_.size();
    entries_.emplace_back(key, std::move(model));
    byBinary_.emplace(key, index);
    const ca::str::Utf8StringRef kotlinName = entries_.back().second.name;
    if (!kotlinName.is_empty()) byKotlin_.emplace(kotlinName, index);   ///< 同真名首条优先
    return true;
}

const KtClass* KotlinMetadataCollection::findClass(std::string_view binaryName) const
{
    const auto it = byBinary_.find(ca::str::Utf8StringRef::from_string_view(binaryName));
    return it == byBinary_.end() ? nullptr : &entries_[it->second].second;
}

const KtClass* KotlinMetadataCollection::findClassByKotlinName(std::string_view kotlinName) const
{
    const auto it = byKotlin_.find(ca::str::Utf8StringRef::from_string_view(kotlinName));
    return it == byKotlin_.end() ? nullptr : &entries_[it->second].second;
}

ca::str::Utf8StringRef KotlinMetadataCollection::kotlinNameOf(std::string_view binaryName) const
{
    const KtClass* model = findClass(binaryName);
    return model == nullptr ? ca::str::Utf8StringRef{} : model->name;
}

ca::str::Utf8StringRef KotlinMetadataCollection::binaryNameOf(std::string_view kotlinName) const
{
    const auto it = byKotlin_.find(ca::str::Utf8StringRef::from_string_view(kotlinName));
    return it == byKotlin_.end() ? ca::str::Utf8StringRef{} : entries_[it->second].first;
}

std::string reverseMapDescriptor(const KotlinMetadataCollection& collection,
                                 std::string_view                descriptor)
{
    std::string out;
    const bool  ok = walkDescriptor(
        descriptor,
        [&collection](std::string_view seg, std::string& sink) {
            ca::usize j = 0;
            while (j < seg.size() && seg[j] == '[') ++j;
            sink.append(seg.substr(0, j));
            if (j >= seg.size() || seg[j] != 'L' || seg.back() != ';') {
                if (j < seg.size()) sink.push_back(seg[j]);
                return;
            }
            const std::string_view name     = seg.substr(j + 1, seg.size() - j - 2);
            const ca::str::Utf8StringRef mapped = collection.binaryNameOf(name);
            sink.push_back('L');
            sink.append(mapped.is_empty() ? name : std::string_view(mapped));
            sink.push_back(';');
        },
        out);
    if (!ok) return std::string(descriptor);
    return out;
}

ca::core::Result<KtClass, KotlinMetadataError>
parseKotlinMetadata(jbc::core::ClassFile& cf, ca::str::Utf8StringArena& arena)
{
    auto face = readMetadataAnnoFace(cf);
    if (face.is_err()) return Err(std::move(face).unwrap_err());
    MetadataAnnoFace anno = std::move(face).unwrap();
    if (!anno.hasK || !anno.hasD1 || !anno.hasD2) return Err(KotlinMetadataError::BadAnnotationShape);

    auto data1 = decodeData1(anno.d1);
    if (data1.is_err()) return Err(std::move(data1).unwrap_err());
    const std::vector<ca::u8> payload = std::move(data1).unwrap();

    // parseDelimited 口径：varint32 长度前缀的 StringTableTypes 在前，主消
    // 息紧随其后，各占一段。
    const ca::u8* p   = payload.data();
    const ca::u8* end = p + payload.size();
    ca::u64       tableLen = 0;
    if (!readVarint(p, end, tableLen)) return Err(KotlinMetadataError::BadProto);
    if (static_cast<ca::u64>(end - p) < tableLen) return Err(KotlinMetadataError::BadProto);
    auto table = StringTable::parse(
        std::string_view(reinterpret_cast<const char*>(p), static_cast<ca::usize>(tableLen)),
        anno.d2.size());
    if (table.is_err()) return Err(std::move(table).unwrap_err());
    p += static_cast<ca::usize>(tableLen);
    auto    tableValue = std::move(table).unwrap();
    ParseCtx ctx{tableValue, anno.d2, arena};
    const std::string_view mainBytes(reinterpret_cast<const char*>(p),
                                     static_cast<ca::usize>(end - p));

    ca::core::Result<KtClass, KotlinMetadataError> model = Err(KotlinMetadataError::UnsupportedKind);
    switch (static_cast<ca::u32>(anno.k)) {
    case 1: model = parseClassMessage(mainBytes, ctx); break;
    case 2: model = parsePackageMessage(mainBytes, ctx, arena.intern(cf.thisName()), KtClassKind::File); break;
    case 3: model = parseFunctionMessage(mainBytes, ctx, arena.intern(cf.thisName())); break;
    case 5:
        model = parsePackageMessage(mainBytes, ctx, arena.intern(cf.thisName()), KtClassKind::MultiFilePart);
        break;
    default: return Err(KotlinMetadataError::UnsupportedKind);   ///< k=0/4 v1 拒面
    }
    if (model.is_err()) return model;
    KtClass parsed = std::move(model).unwrap();
    if (anno.hasXi) parsed.extraFlags = anno.xi;
    return Ok(std::move(parsed));
}

KotlinMetadataCollection KotlinMetadataCollectionAnalysis::run(mjbase::model::ResourceContainer& container,
                                                              mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    KotlinMetadataCollection collection;
    for (auto& [uri, res] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(res.get());
        if (cls == nullptr) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) continue;   ///< 畸形类资源跳过（fail-open 同口径）
        auto session = std::move(sessionResult).unwrap();
        auto parsed  = parseKotlinMetadata(session->backing(), collection.arena());
        if (parsed.is_err()) continue;   ///< fail-closed：失败类零产出
        (void)collection.add(collection.arena().intern(session->backing().thisName()),
                             std::move(parsed).unwrap());
    }
    return collection;
}

mjbase::spi::PreservedAnalyses KotlinMetadataCollector::run(mjbase::model::ResourceContainer&     container,
                                                           mjbase::spi::GlobalAnalysisManager& am)
{
    am.registerAnalysis<KotlinMetadataCollectionAnalysis>();   ///< 幂等：重复注册不覆盖
    (void)am.getResult<KotlinMetadataCollectionAnalysis>(container);   ///< 触发计算并缓存
    return mjbase::spi::PreservedAnalyses::all();   ///< 只读：字节零改动
}

}   // namespace mjbase::pass
