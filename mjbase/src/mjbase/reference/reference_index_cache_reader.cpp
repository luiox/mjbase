#include "mjbase/reference/reference_index_cache_reader.hpp"

#include <deque>
#include <string>
#include <vector>

#include "mjbase/reference/reference_index.hpp"
#include "mjbase/reference/reference_index_cache_format.hpp"

namespace mjbase::reference {

namespace {

/// @brief 带越界校验的内存读取游标。
///
/// 所有读取操作都在推进前检查剩余字节，越界时整体返回失败。这让上层逻辑
/// 可以放心假设读到的字段是完整的，而不必在每个调用点重复写边界检查。
struct Cursor
{
    const ca::u8* data = nullptr;
    ca::usize     size = 0;
    ca::usize     pos  = 0;

    bool canRead(ca::usize n) const { return pos + n <= size && pos + n >= pos; }

    bool readU64(ca::u64& out)
    {
        if (!canRead(8)) return false;
        ca::u64 v = 0;
        for (ca::usize i = 0; i < 8; ++i) {
            v |= static_cast<ca::u64>(data[pos + i]) << (i * 8);
        }
        pos += 8;
        out = v;
        return true;
    }

    bool readI32(ca::i32& out)
    {
        ca::u64 v = 0;
        if (!readU64(v)) return false;
        out = static_cast<ca::i32>(static_cast<ca::u32>(v));
        return true;
    }

    bool readU8(ca::u8& out)
    {
        if (!canRead(1)) return false;
        out = data[pos++];
        return true;
    }

    bool readBool(bool& out)
    {
        ca::u8 v = 0;
        if (!readU8(v)) return false;
        out = (v != 0);
        return true;
    }

    /// 字符串长度上限用剩余字节兜底，防止 header 被恶意构造出天文数字。
    bool readString(std::string& out)
    {
        ca::u64 len = 0;
        if (!readU64(len)) return false;
        if (!canRead(static_cast<ca::usize>(len))) return false;
        out.assign(reinterpret_cast<const char*>(data + pos), static_cast<ca::usize>(len));
        pos += static_cast<ca::usize>(len);
        return true;
    }
};

/// @brief 把 Header 里的 32 字节指纹展开成 64 字符 lowercase hex。
std::string unpackFingerprint(const ridx::Header& hdr)
{
    static const char* kHex = "0123456789abcdef";
    std::string        out;
    out.reserve(64);
    for (ca::usize i = 0; i < 32; ++i) {
        ca::u8 b = hdr.fingerprintSha256[i];
        out.push_back(kHex[(b >> 4) & 0xf]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
}

}   // namespace

bool loadReferenceIndexCache(ReferenceIndex& out,
                             std::string& outFingerprint,
                             const void* data,
                             ca::usize size)
{
    if (data == nullptr || size < sizeof(ridx::Header)) return false;

    Cursor c{static_cast<const ca::u8*>(data), size, sizeof(ridx::Header)};

    const auto* hdr = static_cast<const ridx::Header*>(data);
    if (hdr->magic != ridx::kMagic || hdr->version != ridx::kVersion) return false;

    // 两阶段：字符串先全部读入本地存储（deque：元素地址在追加下稳定），随后
    // 以视图组装六主表交付 restore（restore 内部重驻留进索引自有池）。
    std::deque<std::string> strings;

    auto sv = [&strings](std::string s) {
        strings.push_back(std::move(s));
        return std::string_view(strings.back());
    };

    auto readSv = [&](std::string_view& out) {
        std::string text;
        if (!c.readString(text)) return false;
        out = sv(text);
        return true;
    };

    std::vector<ClassDeclaration>  classDecls;
    std::vector<MethodDeclaration> methodDecls;
    std::vector<FieldDeclaration>  fieldDecls;
    std::vector<MethodReference>   methodRefs;
    std::vector<FieldReference>    fieldRefs;
    std::vector<TypeReference>     typeRefs;

    for (ca::u64 i = 0; i < hdr->classDeclsCount; ++i) {
        ClassDeclaration decl;
        ca::u64          ifaceCount = 0;
        if (!readSv(decl.name) || !readSv(decl.superName) || !c.readU64(ifaceCount)) return false;
        decl.interfaces.reserve(static_cast<ca::usize>(ifaceCount));
        for (ca::u64 j = 0; j < ifaceCount; ++j) {
            std::string_view iface;
            if (!readSv(iface)) return false;
            decl.interfaces.push_back(iface);
        }
        ca::i32 access = 0;
        if (!c.readI32(access)) return false;
        decl.access = static_cast<ca::u32>(access);
        classDecls.push_back(std::move(decl));
    }

    for (ca::u64 i = 0; i < hdr->methodDeclsCount; ++i) {
        MethodDeclaration decl;
        if (!readSv(decl.method.owner) || !readSv(decl.method.name) ||
            !readSv(decl.method.desc)) {
            return false;
        }
        ca::i32 access = 0;
        if (!c.readI32(access)) return false;
        decl.access = static_cast<ca::u32>(access);
        methodDecls.push_back(std::move(decl));
    }

    for (ca::u64 i = 0; i < hdr->fieldDeclsCount; ++i) {
        FieldDeclaration decl;
        if (!readSv(decl.field.owner) || !readSv(decl.field.name) ||
            !readSv(decl.field.desc)) {
            return false;
        }
        ca::i32 access = 0;
        if (!c.readI32(access)) return false;
        decl.access = static_cast<ca::u32>(access);
        fieldDecls.push_back(std::move(decl));
    }

    for (ca::u64 i = 0; i < hdr->methodRefsCount; ++i) {
        MethodReference ref;
        if (!readSv(ref.site.sourceClass) || !readSv(ref.site.sourceMethod) ||
            !readSv(ref.site.sourceDesc) || !c.readI32(ref.site.opcode) ||
            !c.readI32(ref.site.insnIndex)) {
            return false;
        }
        if (!readSv(ref.target.owner) || !readSv(ref.target.name) ||
            !readSv(ref.target.desc) || !readSv(ref.reflectionArgDesc) ||
            !c.readBool(ref.isInterface)) {
            return false;
        }
        ca::u8 kind = 0;
        if (!c.readU8(kind)) return false;
        if (kind > ridx::kMethodKindMax) return false;
        ref.kind = static_cast<MethodReferenceKind>(kind);
        methodRefs.push_back(std::move(ref));
    }

    for (ca::u64 i = 0; i < hdr->fieldRefsCount; ++i) {
        FieldReference ref;
        if (!readSv(ref.site.sourceClass) || !readSv(ref.site.sourceMethod) ||
            !readSv(ref.site.sourceDesc) || !c.readI32(ref.site.opcode) ||
            !c.readI32(ref.site.insnIndex)) {
            return false;
        }
        if (!readSv(ref.target.owner) || !readSv(ref.target.name) || !readSv(ref.target.desc)) {
            return false;
        }
        ca::u8 kind = 0;
        if (!c.readU8(kind)) return false;
        if (kind > ridx::kFieldKindMax) return false;
        ref.kind = static_cast<FieldReferenceKind>(kind);
        fieldRefs.push_back(std::move(ref));
    }

    for (ca::u64 i = 0; i < hdr->typeRefsCount; ++i) {
        TypeReference ref;
        if (!readSv(ref.site.sourceClass) || !readSv(ref.site.sourceMethod) ||
            !readSv(ref.site.sourceDesc) || !c.readI32(ref.site.opcode) ||
            !c.readI32(ref.site.insnIndex) || !readSv(ref.targetType)) {
            return false;
        }
        ca::u8 kind = 0;
        if (!c.readU8(kind)) return false;
        if (kind > ridx::kTypeKindMax) return false;
        ref.kind = static_cast<TypeReferenceKind>(kind);
        typeRefs.push_back(std::move(ref));
    }

    // 六主表全部完整读出后才触碰 out（损坏文件不产半截状态）。
    out.restore(std::move(classDecls), std::move(methodDecls), std::move(fieldDecls),
                std::move(methodRefs), std::move(fieldRefs), std::move(typeRefs));
    outFingerprint = unpackFingerprint(*hdr);
    return true;
}

}   // namespace mjbase::reference
