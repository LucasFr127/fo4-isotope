// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/hooks/PeIat.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <optional>

namespace fo4hdr {
namespace {

[[nodiscard]] bool Range(
    std::span<const std::byte> image,
    std::uint64_t offset,
    std::uint64_t bytes) noexcept
{
    return offset <= image.size() && bytes <= image.size() - offset;
}

template <class T>
[[nodiscard]] bool Read(
    std::span<const std::byte> image,
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
    std::span<const std::byte> image,
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

struct DataDirectory
{
    std::uint32_t rva{};
    std::uint32_t size{};
};

// IMAGE_EXPORT_DIRECTORY.
struct ExportDirectory
{
    std::uint32_t characteristics;
    std::uint32_t timeDateStamp;
    std::uint16_t majorVersion;
    std::uint16_t minorVersion;
    std::uint32_t name;
    std::uint32_t base;
    std::uint32_t functionCount;
    std::uint32_t nameCount;
    std::uint32_t functions;
    std::uint32_t names;
    std::uint32_t ordinals;
};
static_assert(sizeof(ExportDirectory) == 40);

[[nodiscard]] bool ReadNtHeader(
    std::span<const std::byte> image, std::size_t offset, NtHeader& header) noexcept
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

// The optional header's directory count, then 8-byte RVA/size entries from offset 112.
[[nodiscard]] bool ReadDataDirectory(std::span<const std::byte> image, std::size_t optional,
    std::uint32_t index, DataDirectory& directory) noexcept
{
    std::uint32_t directoryCount{};
    if (!Read(image, optional + 108, directoryCount) || directoryCount <= index) return false;
    const auto entry = optional + 112 + std::size_t{index} * sizeof(DataDirectory);
    return Read(image, entry, directory.rva) && Read(image, entry + 4, directory.size);
}

[[nodiscard]] PeIatStatus ReadImportDirectory(
    std::span<const std::byte> image, std::size_t optional, DataDirectory& imports) noexcept
{
    if (!ReadDataDirectory(image, optional, 1, imports)) return PeIatStatus::importsUnavailable;
    if (imports.rva == 0 || imports.size < sizeof(ImportDescriptor))
        return PeIatStatus::importsUnavailable;
    return Range(image, imports.rva, imports.size) ?
        PeIatStatus::success : PeIatStatus::malformedImports;
}

// Finds the PE32+ AMD64 optional header.
[[nodiscard]] PeIatStatus ReadOptionalHeader(
    std::span<const std::byte> image, std::size_t& optional) noexcept
{
    std::uint16_t dosMagic{};
    std::uint32_t ntOffset{};
    if (!Read(image, 0, dosMagic) || dosMagic != 0x5A4D)
        return PeIatStatus::invalidDos;
    if (!Read(image, 0x3C, ntOffset)) return PeIatStatus::invalidDos;

    NtHeader header;
    if (!ReadNtHeader(image, ntOffset, header)) return PeIatStatus::invalidNt;
    if (!header.IsPe32PlusAmd64()) return PeIatStatus::notPe32PlusAmd64;
    optional = static_cast<std::size_t>(ntOffset) + 24;
    return PeIatStatus::success;
}

[[nodiscard]] PeIatStatus ReadPeImports(
    std::span<const std::byte> image, DataDirectory& imports) noexcept
{
    std::size_t optional{};
    const auto status = ReadOptionalHeader(image, optional);
    return status == PeIatStatus::success ? ReadImportDirectory(image, optional, imports) : status;
}

[[nodiscard]] bool ReadImportedSymbol(
    std::span<const std::byte> image, std::uint64_t nameThunk, std::string_view& symbol) noexcept
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

    [[nodiscard]] PeIatResult Find(const DataDirectory& imports) noexcept
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
    DataDirectory imports;
    const auto status = ReadPeImports(image, imports);
    if (status != PeIatStatus::success) return {status};
    return ImportLookup(image, importedModule, importedSymbol).Find(imports);
}

namespace {

// The answer for an image without a readable export table, or nullopt once
// directory holds one.
[[nodiscard]] std::optional<PeExportStatus> ReadExportDirectory(
    std::span<const std::byte> image, ExportDirectory& directory) noexcept
{
    std::size_t optional{};
    const auto header = ReadOptionalHeader(image, optional);
    // Another machine type can never be a loaded x64 plugin.
    if (header == PeIatStatus::notPe32PlusAmd64) return PeExportStatus::notFound;
    DataDirectory exports;
    if (header != PeIatStatus::success || !ReadDataDirectory(image, optional, 0, exports))
        return PeExportStatus::malformed;
    if (exports.rva == 0) return PeExportStatus::notFound;
    if (!Read(image, exports.rva, directory)) return PeExportStatus::malformed;
    return std::nullopt;
}

[[nodiscard]] PeExportStatus FindExportName(std::span<const std::byte> image,
    const ExportDirectory& directory, std::string_view symbol) noexcept
{
    if (!Range(image, directory.names, std::uint64_t{directory.nameCount} * sizeof(std::uint32_t)))
        return PeExportStatus::malformed;
    for (std::uint32_t index = 0; index < directory.nameCount; ++index)
    {
        std::uint32_t nameRva{};
        std::string_view name;
        if (!Read(image, directory.names + std::size_t{index} * sizeof(nameRva), nameRva) ||
            !ReadString(image, nameRva, name))
            return PeExportStatus::malformed;
        if (name == symbol) return PeExportStatus::found;
    }
    return PeExportStatus::notFound;
}

// An image mapping always commits its first page, which holds the headers.
constexpr std::size_t kHeaderPage = 4096;

// SizeOfImage of a PE32+ AMD64 image, or 0.
[[nodiscard]] std::size_t ImageSize(std::span<const std::byte> headers) noexcept
{
    std::size_t optional{};
    std::uint32_t size{};
    const bool read = ReadOptionalHeader(headers, optional) == PeIatStatus::success &&
        Read(headers, optional + 56, size);
    return read ? size : 0;
}

[[nodiscard]] PeExportStatus FindFileExport(
    const std::filesystem::path& file, std::string_view symbol) noexcept
{
    // Mapped as an image resource, the file is laid out by RVA without running it.
    const auto module = ::LoadLibraryExW(
        file.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (module == nullptr) return PeExportStatus::malformed;
    // The handle's low bits mark a resource mapping; the image starts at the rest.
    const auto* base = reinterpret_cast<const std::byte*>(
        reinterpret_cast<std::uintptr_t>(module) & ~std::uintptr_t{3});
    const std::span headers{base, kHeaderPage};
    const auto size = ImageSize(headers);
    // Without a PE32+ size, the headers still tell another machine type from a broken file.
    const auto status = FindPe32PlusExport(
        size >= kHeaderPage ? std::span<const std::byte>{base, size} : headers, symbol);
    ::FreeLibrary(module);
    return status;
}

[[nodiscard]] bool IsDll(const std::filesystem::path& file)
{
    return ::_wcsicmp(file.extension().c_str(), L".dll") == 0;
}

// What one folder entry adds: anything but a DLL is absent, and a DLL that
// cannot be read is unknown.
[[nodiscard]] PluginExportScan ScanEntry(
    const std::filesystem::directory_entry& entry, std::string_view symbol)
{
    std::error_code error;
    const bool file = entry.is_regular_file(error);
    if (error) return PluginExportScan::unknown;
    if (!file || !IsDll(entry.path())) return PluginExportScan::absent;
    switch (FindFileExport(entry.path(), symbol))
    {
    case PeExportStatus::found: return PluginExportScan::found;
    case PeExportStatus::notFound: return PluginExportScan::absent;
    case PeExportStatus::malformed: break;
    }
    return PluginExportScan::unknown;
}

}  // namespace

PeExportStatus FindPe32PlusExport(
    std::span<const std::byte> image, std::string_view symbol) noexcept
{
    if (image.empty() || symbol.empty()) return PeExportStatus::malformed;
    ExportDirectory directory{};
    if (const auto answer = ReadExportDirectory(image, directory)) return *answer;
    return FindExportName(image, directory, symbol);
}

PluginExportScan ScanPluginExports(
    const std::filesystem::path& directory, std::string_view symbol) noexcept
{
    try
    {
        std::error_code error;
        auto result = PluginExportScan::absent;
        for (std::filesystem::directory_iterator entry(directory, error), end;
             !error && entry != end; entry.increment(error))
        {
            const auto scanned = ScanEntry(*entry, symbol);
            if (scanned == PluginExportScan::found) return scanned;
            if (scanned == PluginExportScan::unknown) result = scanned;
        }
        return error ? PluginExportScan::unknown : result;
    }
    catch (...)
    {
        return PluginExportScan::unknown;
    }
}

}  // namespace fo4hdr
