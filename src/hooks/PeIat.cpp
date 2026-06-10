// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/hooks/PeIat.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace fo4hdr {
namespace {

static_assert(sizeof(void*) == sizeof(std::uint64_t));

bool Range(std::span<std::byte> image, std::uint64_t offset, std::uint64_t bytes) noexcept
{
    return offset <= image.size() && bytes <= image.size() - offset;
}

template <class T>
bool Read(std::span<std::byte> image, std::uint64_t offset, T& value) noexcept
{
    if (!Range(image, offset, sizeof(T)))
    {
        return false;
    }
    std::memcpy(&value, image.data() + static_cast<std::size_t>(offset), sizeof(T));
    return true;
}

char LowerAscii(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool EqualModule(std::string_view left, std::string_view right) noexcept
{
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
        [](char a, char b) noexcept { return LowerAscii(a) == LowerAscii(b); });
}

bool ReadString(std::span<std::byte> image, std::uint64_t rva, std::string_view& value) noexcept
{
    if (!Range(image, rva, 1))
    {
        return false;
    }
    const auto* first = reinterpret_cast<const char*>(image.data() + rva);
    const auto* last = static_cast<const char*>(std::memchr(first, 0, image.size() - rva));
    if (last == nullptr)
    {
        return false;
    }
    value = {first, static_cast<std::size_t>(last - first)};
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

struct ImportDirectory
{
    std::uint32_t rva{};
    std::uint32_t size{};
};

// PE32+ places eight-byte directories after 112 fixed optional-header bytes.
// Microsoft PE/COFF Specification, Revision 9.3 (December 29, 2015).
PeIatStatus ReadImportDirectory(std::span<std::byte> image, std::uint64_t optional,
    std::uint16_t optionalSize, ImportDirectory& imports) noexcept
{
    std::uint32_t count{};
    static_cast<void>(Read(image, optional + 108, count));
    const auto available = static_cast<std::uint32_t>((optionalSize - 112) / 8);
    if (count > available)
    {
        return PeIatStatus::invalidNt;
    }
    if (count < 2)
    {
        return PeIatStatus::importsUnavailable;
    }
    static_cast<void>(Read(image, optional + 120, imports.rva));
    static_cast<void>(Read(image, optional + 124, imports.size));
    if (imports.rva == 0 || imports.size == 0)
    {
        return PeIatStatus::importsUnavailable;
    }
    if (imports.size < sizeof(ImportDescriptor) || !Range(image, imports.rva, imports.size))
    {
        return PeIatStatus::malformedImports;
    }
    return PeIatStatus::success;
}

bool ReadDosOffset(std::span<std::byte> image, std::int32_t& ntOffset) noexcept
{
    std::uint16_t dosMagic{};
    if (!Read(image, 0, dosMagic) || dosMagic != 0x5A4D)
    {
        return false;
    }
    return Read(image, 0x3C, ntOffset);
}

bool ReadNtLayout(std::span<std::byte> image, std::uint64_t nt,
    std::uint16_t& machine, std::uint16_t& optionalSize) noexcept
{
    if (!Range(image, nt, 24))
    {
        return false;
    }
    std::uint32_t signature{};
    static_cast<void>(Read(image, nt, signature));
    static_cast<void>(Read(image, nt + 4, machine));
    static_cast<void>(Read(image, nt + 20, optionalSize));
    if (signature != 0x00004550)
    {
        return false;
    }
    return optionalSize >= 112 && Range(image, nt + 24, optionalSize);
}

PeIatStatus ReadPeImports(std::span<std::byte> image, ImportDirectory& imports) noexcept
{
    std::int32_t ntOffset{};
    if (!ReadDosOffset(image, ntOffset))
    {
        return PeIatStatus::invalidDos;
    }
    if (ntOffset < 0x40)
    {
        return PeIatStatus::invalidNt;
    }
    const auto nt = static_cast<std::uint64_t>(ntOffset);
    std::uint16_t machine{};
    std::uint16_t optionalSize{};
    if (!ReadNtLayout(image, nt, machine, optionalSize))
    {
        return PeIatStatus::invalidNt;
    }
    std::uint16_t magic{};
    static_cast<void>(Read(image, nt + 24, magic));
    if (machine != 0x8664 || magic != 0x20B)
    {
        return PeIatStatus::notPe32PlusAmd64;
    }
    return ReadImportDirectory(image, nt + 24, optionalSize, imports);
}

bool IsTerminator(const ImportDescriptor& descriptor) noexcept
{
    return (descriptor.originalFirstThunk | descriptor.timeDateStamp | descriptor.forwarderChain |
        descriptor.name | descriptor.firstThunk) == 0;
}

class ImportLookup
{
public:
    ImportLookup(std::span<std::byte> image, std::string_view module,
        std::string_view symbol) noexcept : image_(image), module_(module), symbol_(symbol)
    {}

    PeIatResult Find(const ImportDirectory& imports) noexcept
    {
        const auto count = imports.size / sizeof(ImportDescriptor);
        for (std::size_t index = 0; index < count; ++index)
        {
            ImportDescriptor descriptor{};
            static_cast<void>(Read(image_,
                imports.rva + index * sizeof(ImportDescriptor), descriptor));
            if (IsTerminator(descriptor))
            {
                return Finish();
            }
            const auto status = ScanDescriptor(descriptor);
            if (status != PeIatStatus::success)
            {
                return {status};
            }
        }
        return {PeIatStatus::malformedImports};
    }

private:
    PeIatResult Finish() const noexcept
    {
        if (foundSlot_ != nullptr)
        {
            return {PeIatStatus::success, foundSlot_};
        }
        return {moduleSeen_ ? PeIatStatus::symbolNotFound : PeIatStatus::moduleNotFound};
    }

    PeIatStatus ScanDescriptor(const ImportDescriptor& descriptor) noexcept
    {
        std::string_view module;
        if (descriptor.name == 0 || descriptor.firstThunk == 0)
        {
            return PeIatStatus::malformedImports;
        }
        if (!ReadString(image_, descriptor.name, module))
        {
            return PeIatStatus::malformedImports;
        }
        if (!EqualModule(module, module_))
        {
            return PeIatStatus::success;
        }
        moduleSeen_ = true;
        if (descriptor.originalFirstThunk == 0)
        {
            return PeIatStatus::malformedImports;
        }
        return ScanThunks(descriptor);
    }

    PeIatStatus ScanThunks(const ImportDescriptor& descriptor) noexcept
    {
        const auto names = descriptor.originalFirstThunk;
        const auto slots = descriptor.firstThunk;
        if (!Range(image_, names, 8) || !Range(image_, slots, 8))
        {
            return PeIatStatus::malformedImports;
        }
        const auto count = (std::min)((image_.size() - names) / 8, (image_.size() - slots) / 8);
        for (std::size_t index = 0; index < count; ++index)
        {
            std::uint64_t name{};
            std::uint64_t target{};
            const auto displacement = index * 8;
            static_cast<void>(Read(image_, names + displacement, name));
            static_cast<void>(Read(image_, slots + displacement, target));
            if (name == 0)
            {
                return target == 0 ? PeIatStatus::success : PeIatStatus::malformedImports;
            }
            if ((name & (std::uint64_t{1} << 63)) == 0)
            {
                const auto status = MatchThunk(name, slots + displacement, target);
                if (status != PeIatStatus::success)
                {
                    return status;
                }
            }
        }
        return PeIatStatus::malformedImports;
    }

    PeIatStatus MatchThunk(std::uint64_t name, std::uint64_t slotOffset,
        std::uint64_t target) noexcept
    {
        std::string_view symbol;
        if (name > 0x7FFFFFFF)
        {
            return PeIatStatus::malformedImports;
        }
        if (!Range(image_, name, 2) || !ReadString(image_, name + 2, symbol))
        {
            return PeIatStatus::malformedImports;
        }
        if (symbol != symbol_)
        {
            return PeIatStatus::success;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(image_.data() + slotOffset);
        if (address % alignof(void*) != 0)
        {
            return PeIatStatus::malformedImports;
        }
        if (target == 0)
        {
            return PeIatStatus::nullTarget;
        }
        if (foundSlot_ != nullptr)
        {
            return PeIatStatus::duplicateSymbol;
        }
        foundSlot_ = reinterpret_cast<void**>(address);
        return PeIatStatus::success;
    }

    std::span<std::byte> image_;
    std::string_view module_;
    std::string_view symbol_;
    bool moduleSeen_{};
    void** foundSlot_{};
};

}  // namespace

PeIatResult FindPe32PlusIatSlot(std::span<std::byte> image,
    std::string_view importedModule, std::string_view importedSymbol) noexcept
{
    if (image.empty())
    {
        return {PeIatStatus::invalidArgument};
    }
    if (importedModule.empty() || importedSymbol.empty())
    {
        return {PeIatStatus::invalidArgument};
    }
    ImportDirectory imports;
    const auto status = ReadPeImports(image, imports);
    if (status != PeIatStatus::success)
    {
        return {status};
    }
    return ImportLookup(image, importedModule, importedSymbol).Find(imports);
}

}  // namespace fo4hdr
