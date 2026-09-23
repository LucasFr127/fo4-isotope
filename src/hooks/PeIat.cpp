// SPDX-License-Identifier: GPL-3.0-or-later
#include "Fallout4HDR/hooks/PeIat.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fo4hdr {
namespace {

[[nodiscard]] bool Range(
    std::span<std::byte> image,
    std::uint64_t offset,
    std::uint64_t bytes) noexcept
{
    return offset <= image.size() && bytes <= image.size() - offset;
}

template <class T>
[[nodiscard]] bool Read(
    std::span<std::byte> image,
    std::size_t offset,
    T& value) noexcept
{
    if (!Range(image, offset, sizeof(T)))
    {
        return false;
    }
    std::memcpy(&value, image.data() + offset, sizeof(T));
    return true;
}

[[nodiscard]] bool EqualModule(
    std::string_view left,
    std::string_view right) noexcept
{
    return std::equal(
               left.begin(),
               left.end(),
               right.begin(),
               right.end(),
               [](char a, char b) noexcept {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
}

[[nodiscard]] bool ReadString(
    std::span<std::byte> image,
    std::uint32_t rva,
    std::string_view& value) noexcept
{
    if (rva >= image.size()) return false;
    const auto* first = reinterpret_cast<const char*>(image.data() + rva);
    const auto maximum = image.size() - rva;
    const auto* terminator = static_cast<const char*>(
        std::memchr(first, 0, maximum));
    if (terminator == nullptr) return false;
    value = { first, static_cast<std::size_t>(terminator - first) };
    return true;
}

struct ImportDescriptor
{
    std::uint32_t originalFirstThunk;
    std::uint32_t timeDateStamp;
    std::uint32_t forwarderChain;
    std::uint32_t name;
    std::uint32_t firstThunk;
};
static_assert(sizeof(ImportDescriptor) == 20);

struct NtHeader
{
    std::uint16_t machine{};
    std::uint16_t optionalSize{};
    std::uint16_t optionalMagic{};

    [[nodiscard]] bool IsPe32PlusAmd64() const noexcept
    {
        return machine == 0x8664 && optionalMagic == 0x20B && optionalSize >= 128;
    }
};

struct ImportDirectory
{
    std::uint32_t rva{};
    std::uint32_t size{};
};

[[nodiscard]] bool ReadNtHeader(
    std::span<std::byte> image, std::size_t offset, NtHeader& header) noexcept
{
    std::array<std::byte, 26> bytes;
    if (!Read(image, offset, bytes)) return false;
    std::uint32_t signature{};
    std::memcpy(&signature, bytes.data(), sizeof(signature));
    std::memcpy(&header.machine, bytes.data() + 4, sizeof(header.machine));
    std::memcpy(&header.optionalSize, bytes.data() + 20, sizeof(header.optionalSize));
    std::memcpy(&header.optionalMagic, bytes.data() + 24, sizeof(header.optionalMagic));
    return signature == 0x00004550;
}

[[nodiscard]] PeIatStatus ReadImportDirectory(
    std::span<std::byte> image, std::size_t optional, ImportDirectory& imports) noexcept
{
    std::uint32_t directoryCount{};
    if (!Read(image, optional + 108, directoryCount) || directoryCount < 2)
        return PeIatStatus::importsUnavailable;
    if (!Read(image, optional + 120, imports.rva) ||
        !Read(image, optional + 124, imports.size))
        return PeIatStatus::importsUnavailable;
    if (imports.rva == 0 || imports.size < sizeof(ImportDescriptor))
        return PeIatStatus::importsUnavailable;
    return Range(image, imports.rva, imports.size) ?
        PeIatStatus::success : PeIatStatus::malformedImports;
}

[[nodiscard]] PeIatStatus ReadPeImports(
    std::span<std::byte> image, ImportDirectory& imports) noexcept
{
    std::uint16_t dosMagic{};
    std::uint32_t ntOffset{};
    if (!Read(image, 0, dosMagic) || dosMagic != 0x5A4D)
        return PeIatStatus::invalidDos;
    if (!Read(image, 0x3C, ntOffset)) return PeIatStatus::invalidDos;

    NtHeader header;
    if (!ReadNtHeader(image, ntOffset, header)) return PeIatStatus::invalidNt;
    if (!header.IsPe32PlusAmd64()) return PeIatStatus::notPe32PlusAmd64;
    return ReadImportDirectory(image, static_cast<std::size_t>(ntOffset) + 24, imports);
}

[[nodiscard]] bool ReadImportedSymbol(
    std::span<std::byte> image, std::uint64_t nameThunk, std::string_view& symbol) noexcept
{
    if (nameThunk > std::numeric_limits<std::uint32_t>::max() - 2) return false;
    const auto nameRva = static_cast<std::uint32_t>(nameThunk);
    // A readable symbol after the two-byte hint also proves the hint is in range.
    return ReadString(image, nameRva + 2, symbol);
}

[[nodiscard]] bool IsImportTerminator(const ImportDescriptor& descriptor) noexcept
{
    return descriptor.originalFirstThunk == 0 && descriptor.name == 0 &&
        descriptor.firstThunk == 0;
}

class ImportLookup
{
public:
    ImportLookup(std::span<std::byte> image, std::string_view module,
        std::string_view symbol) noexcept : image_(image), module_(module), symbol_(symbol) {}

    [[nodiscard]] PeIatResult Find(const ImportDirectory& imports) noexcept
    {
        const auto descriptorCount = imports.size / sizeof(ImportDescriptor);
        for (std::size_t index = 0; index < descriptorCount; ++index)
        {
            ImportDescriptor descriptor{};
            if (!Read(image_, imports.rva + index * sizeof(ImportDescriptor), descriptor))
                return {PeIatStatus::malformedImports};
            if (IsImportTerminator(descriptor)) break;
            const auto status = ScanDescriptor(descriptor);
            if (status != PeIatStatus::success) return {status};
        }
        if (foundSlot_ != nullptr) return {PeIatStatus::success, foundSlot_};
        return {moduleSeen_ ? PeIatStatus::symbolNotFound : PeIatStatus::moduleNotFound};
    }

private:
    [[nodiscard]] PeIatStatus ScanDescriptor(const ImportDescriptor& descriptor) noexcept
    {
        if (descriptor.name == 0 || descriptor.firstThunk == 0)
            return PeIatStatus::malformedImports;
        std::string_view module;
        if (!ReadString(image_, descriptor.name, module)) return PeIatStatus::malformedImports;
        if (!EqualModule(module, module_)) return PeIatStatus::success;
        moduleSeen_ = true;
        if (descriptor.originalFirstThunk == 0) return PeIatStatus::malformedImports;
        return ScanThunks(descriptor);
    }

    [[nodiscard]] PeIatStatus ScanThunks(const ImportDescriptor& descriptor) noexcept
    {
        const auto names = descriptor.originalFirstThunk;
        for (std::size_t displacement = 0;
             displacement <= std::numeric_limits<std::uint32_t>::max();
             displacement += sizeof(std::uint64_t))
        {
            std::uint64_t nameThunk{};
            if (!Read(image_, names + displacement, nameThunk))
                return PeIatStatus::malformedImports;
            if (nameThunk == 0) return PeIatStatus::success;
            if ((nameThunk & (std::uint64_t{1} << 63)) == 0)
            {
                const auto status = MatchThunk(nameThunk,
                    static_cast<std::uint64_t>(descriptor.firstThunk) + displacement);
                if (status != PeIatStatus::success) return status;
            }
        }
        return PeIatStatus::malformedImports;
    }

    [[nodiscard]] PeIatStatus MatchThunk(
        std::uint64_t nameThunk, std::uint64_t slotOffset) noexcept
    {
        std::string_view symbol;
        if (!ReadImportedSymbol(image_, nameThunk, symbol)) return PeIatStatus::malformedImports;
        if (symbol != symbol_) return PeIatStatus::success;
        if (!Range(image_, slotOffset, sizeof(void*))) return PeIatStatus::malformedImports;
        auto** slot = reinterpret_cast<void**>(image_.data() + slotOffset);
        if (*slot == nullptr) return PeIatStatus::nullTarget;
        if (foundSlot_ != nullptr) return PeIatStatus::duplicateSymbol;
        foundSlot_ = slot;
        return PeIatStatus::success;
    }

    std::span<std::byte> image_;
    std::string_view module_;
    std::string_view symbol_;
    bool moduleSeen_{};
    void** foundSlot_{};
};

}  // namespace

PeIatResult FindPe32PlusIatSlot(
    std::span<std::byte> image,
    std::string_view importedModule,
    std::string_view importedSymbol) noexcept
{
    if (image.empty()) return {PeIatStatus::invalidArgument};
    if (importedModule.empty() || importedSymbol.empty()) return {PeIatStatus::invalidArgument};
    ImportDirectory imports;
    const auto status = ReadPeImports(image, imports);
    if (status != PeIatStatus::success) return {status};
    return ImportLookup(image, importedModule, importedSymbol).Find(imports);
}

}  // namespace fo4hdr
