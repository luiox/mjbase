#include "mjbase/reference/reference_index_cache_writer.hpp"

#include <cstdio>
#include <fstream>

#include "mjbase/reference/reference_index.hpp"
#include "mjbase/reference/reference_index_cache_format.hpp"

namespace mjbase::reference {

namespace {

void writeU64LE(std::ofstream& out, ca::u64 value)
{
    ca::u8 bytes[8];
    for (ca::usize i = 0; i < 8; ++i) {
        bytes[i] = static_cast<ca::u8>((value >> (i * 8)) & 0xffU);
    }
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writeI32LE(std::ofstream& out, ca::i32 value)
{
    writeU64LE(out, static_cast<ca::u64>(static_cast<ca::u32>(value)));
}

void writeU8(std::ofstream& out, ca::u8 value)
{
    out.put(static_cast<char>(value));
}

void writeBool(std::ofstream& out, bool value)
{
    writeU8(out, value ? 1u : 0u);
}

/// u64 长度前缀 + 裸 UTF-8 字节（视图直写，不落新存储；std::string 只在
/// 本字节边界使用）。
void writeString(std::ofstream& out, std::string_view value)
{
    writeU64LE(out, static_cast<ca::u64>(value.size()));
    if (!value.empty()) {
        out.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
}

void writeRefKey(std::ofstream& out, std::string_view owner, std::string_view name,
                 std::string_view desc)
{
    writeString(out, owner);
    writeString(out, name);
    writeString(out, desc);
}

void writeRefSite(std::ofstream& out, const ReferenceSite& site)
{
    writeString(out, site.sourceClass);
    writeString(out, site.sourceMethod);
    writeString(out, site.sourceDesc);
    writeI32LE(out, site.opcode);
    writeI32LE(out, site.insnIndex);
}

void writeHeader(std::ofstream& out, const ridx::Header& hdr)
{
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
}

/// @brief 把 hex 字符串指纹展开成 32 字节，写入 Header。
///
/// fingerprint 由 XrefCache::fingerprint 产出，是 64 字符的 lowercase hex
/// SHA-256。长度不是恰好 64 时返回 false，调用方应跳过落盘而不是写半截。
bool packFingerprint(ridx::Header& hdr, const std::string& fingerprint)
{
    if (fingerprint.size() != 64) return false;
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (ca::usize i = 0; i < 32; ++i) {
        int hi = hexVal(fingerprint[i * 2]);
        int lo = hexVal(fingerprint[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        hdr.fingerprintSha256[i] = static_cast<ca::u8>((hi << 4) | lo);
    }
    return true;
}

}   // namespace

bool writeReferenceIndexCache(const char* outputPath,
                              const ReferenceIndex& index,
                              const std::string& fingerprint)
{
    ridx::Header hdr{};
    hdr.magic   = ridx::kMagic;
    hdr.version = ridx::kVersion;
    if (!packFingerprint(hdr, fingerprint)) return false;
    hdr.classDeclsCount  = static_cast<ca::u64>(index.classDeclarationCount());
    hdr.methodDeclsCount = static_cast<ca::u64>(index.methodDeclarationCount());
    hdr.fieldDeclsCount  = static_cast<ca::u64>(index.fieldDeclarationCount());
    hdr.methodRefsCount  = static_cast<ca::u64>(index.methodReferenceCount());
    hdr.fieldRefsCount   = static_cast<ca::u64>(index.fieldReferenceCount());
    hdr.typeRefsCount    = static_cast<ca::u64>(index.typeReferenceCount());

    // 先写临时文件，再 rename，保证其他读者不会看到半截文件。
    std::string tmpPath = std::string(outputPath) + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        writeHeader(out, hdr);

        for (const auto& decl : index.classDeclarations()) {
            writeString(out, decl.name);
            writeString(out, decl.superName);
            writeU64LE(out, static_cast<ca::u64>(decl.interfaces.size()));
            for (const auto& iface : decl.interfaces) writeString(out, iface);
            writeI32LE(out, static_cast<ca::i32>(decl.access));
        }

        for (const auto& decl : index.methodDeclarations()) {
            writeRefKey(out, decl.method.owner, decl.method.name, decl.method.desc);
            writeI32LE(out, static_cast<ca::i32>(decl.access));
        }

        for (const auto& decl : index.fieldDeclarations()) {
            writeRefKey(out, decl.field.owner, decl.field.name, decl.field.desc);
            writeI32LE(out, static_cast<ca::i32>(decl.access));
        }

        for (const auto& ref : index.methodReferences()) {
            writeRefSite(out, ref.site);
            writeRefKey(out, ref.target.owner, ref.target.name, ref.target.desc);
            writeString(out, ref.reflectionArgDesc);
            writeBool(out, ref.isInterface);
            writeU8(out, static_cast<ca::u8>(ref.kind));
        }

        for (const auto& ref : index.fieldReferences()) {
            writeRefSite(out, ref.site);
            writeRefKey(out, ref.target.owner, ref.target.name, ref.target.desc);
            writeU8(out, static_cast<ca::u8>(ref.kind));
        }

        for (const auto& ref : index.typeReferences()) {
            writeRefSite(out, ref.site);
            writeString(out, ref.targetType);
            writeU8(out, static_cast<ca::u8>(ref.kind));
        }

        out.flush();
        if (!out.good()) {
            std::remove(tmpPath.c_str());
            return false;
        }
    }

    if (std::rename(tmpPath.c_str(), outputPath) != 0) {
        // Windows 的 rename 不覆盖已存在目标：重写既有缓存（如超限/损坏
        // 文件重建后的覆写）必须先移除旧文件再换入。
        std::remove(outputPath);
        if (std::rename(tmpPath.c_str(), outputPath) != 0) {
            std::remove(tmpPath.c_str());
            return false;
        }
    }
    return true;
}

}   // namespace mjbase::reference
