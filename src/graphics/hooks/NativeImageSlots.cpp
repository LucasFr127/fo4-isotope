// SPDX-License-Identifier: GPL-3.0-or-later

#include "NativeImageSlots.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace fo4hdr {
namespace {

constexpr std::size_t maximumFileBytes = 64 * 1024 * 1024;
constexpr DWORD maximumImageBytes = 128 * 1024 * 1024;
constexpr DWORD maximumRelocationBytes = 8 * 1024 * 1024;

bool Range(std::size_t extent, std::uint64_t offset, std::uint64_t bytes) noexcept
{
    return offset <= extent && bytes <= extent - offset;
}

template <class Value>
bool Read(std::span<const std::byte> file, std::size_t offset, Value& value) noexcept
{
    if (!Range(file.size(), offset, sizeof(Value))) return false;
    std::memcpy(&value, file.data() + offset, sizeof(Value));
    return true;
}

std::uint64_t SectionExtent(const IMAGE_SECTION_HEADER& section) noexcept
{
    return std::max(section.Misc.VirtualSize, section.SizeOfRawData);
}

bool SectionsOverlap(const IMAGE_SECTION_HEADER& left, const IMAGE_SECTION_HEADER& right) noexcept
{
    return left.VirtualAddress < right.VirtualAddress + SectionExtent(right) &&
        right.VirtualAddress < left.VirtualAddress + SectionExtent(left);
}

bool ValidSection(const IMAGE_SECTION_HEADER& section, std::size_t fileBytes,
    DWORD imageBytes) noexcept
{
    if (!Range(imageBytes, section.VirtualAddress, SectionExtent(section))) return false;
    return Range(fileBytes, section.PointerToRawData, section.SizeOfRawData);
}

bool ValidSectionTable(std::size_t count, std::size_t offset,
    std::size_t headerBytes, std::size_t fileBytes) noexcept
{
    if (count == 0 || count > 96) return false;
    const auto bytes = count * sizeof(IMAGE_SECTION_HEADER);
    return Range(headerBytes, offset, bytes) && Range(fileBytes, offset, bytes);
}

bool ValidOptionalHeader(const IMAGE_OPTIONAL_HEADER64& header, std::size_t fileBytes) noexcept
{
    if (header.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    if (header.ImageBase == 0) return false;
    if (header.SizeOfImage < 4096 || header.SizeOfImage > maximumImageBytes) return false;
    if (header.SizeOfHeaders == 0 || header.SizeOfHeaders > fileBytes) return false;
    if (header.SizeOfHeaders > header.SizeOfImage) return false;
    return header.NumberOfRvaAndSizes == IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
}

bool ValidNtHeader(const IMAGE_NT_HEADERS64& header) noexcept
{
    if (header.Signature != IMAGE_NT_SIGNATURE) return false;
    if (header.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    if (header.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64)) return false;
    return (header.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
}

bool ValidRelocationDirectory(const IMAGE_DATA_DIRECTORY& directory) noexcept
{
    if (directory.VirtualAddress == 0 || directory.Size == 0) return false;
    return directory.Size <= maximumRelocationBytes;
}

bool ValidRelocationBlock(const IMAGE_BASE_RELOCATION& block, std::size_t remaining) noexcept
{
    if (block.SizeOfBlock < sizeof(block) || block.SizeOfBlock > remaining) return false;
    return block.SizeOfBlock % sizeof(WORD) == 0 && block.VirtualAddress % 4096 == 0;
}

bool RelocationTouchesSlot(std::uint64_t relocation, std::uint32_t slot) noexcept
{
    return relocation < std::uint64_t{slot} + sizeof(void*) &&
        slot < relocation + sizeof(void*);
}

struct RelocationTarget
{
    DWORD imageBytes;
    std::uint32_t slot;
};

bool CountRelocation(WORD entry, DWORD page, RelocationTarget search, unsigned& count) noexcept
{
    const auto type = entry >> 12;
    if (type == IMAGE_REL_BASED_ABSOLUTE) return true;
    if (type != IMAGE_REL_BASED_DIR64) return false;
    const auto target = std::uint64_t{page} + (entry & 0x0FFF);
    if (!Range(search.imageBytes, target, sizeof(void*))) return false;
    if (!RelocationTouchesSlot(target, search.slot)) return true;
    if (target != search.slot) return false;
    return ++count == 1;
}

}  // namespace

bool NativeImageSlots::Parse(std::span<const std::byte> file) noexcept
{
    parsed_ = false;
    file_ = file;
    if (file.size() > maximumFileBytes) return false;
    if (!ReadLayout()) return false;
    if (!ReadSections()) return false;
    parsed_ = true;
    return true;
}

bool NativeImageSlots::ReadLayout() noexcept
{
    IMAGE_DOS_HEADER dos{};
    if (!Read(file_, 0, dos)) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos)) return false;
    const auto nt = static_cast<std::size_t>(dos.e_lfanew);
    if (!Read(file_, nt, header_)) return false;
    if (!ValidNtHeader(header_)) return false;
    sectionOffset_ = nt + sizeof(header_);
    return ValidOptionalHeader(header_.OptionalHeader, file_.size());
}

bool NativeImageSlots::ReadSections() noexcept
{
    const auto count = header_.FileHeader.NumberOfSections;
    if (!ValidSectionTable(count, sectionOffset_, header_.OptionalHeader.SizeOfHeaders, file_.size()))
        return false;
    const auto bytes = count * sizeof(IMAGE_SECTION_HEADER);
    std::memcpy(sections_.data(), file_.data() + sectionOffset_, bytes);
    const auto end = sections_.begin() + count;
    if (!std::all_of(sections_.begin(), end, [this](const auto& section) {
            return ValidSection(section, file_.size(), header_.OptionalHeader.SizeOfImage);
        })) return false;
    return DistinctSections();
}

bool NativeImageSlots::DistinctSections() const noexcept
{
    for (std::size_t index = 0; index < header_.FileHeader.NumberOfSections; ++index)
    {
        for (std::size_t previous = 0; previous < index; ++previous)
            if (SectionsOverlap(sections_[index], sections_[previous])) return false;
    }
    return true;
}

std::optional<std::size_t> NativeImageSlots::FileOffset(std::uint32_t rva,
    std::size_t bytes, DWORD required, DWORD forbidden) const noexcept
{
    for (std::size_t index = 0; index < header_.FileHeader.NumberOfSections; ++index)
    {
        const auto& section = sections_[index];
        if (rva < section.VirtualAddress) continue;
        const auto offset = std::uint64_t{rva} - section.VirtualAddress;
        if (!Range(section.SizeOfRawData, offset, bytes)) continue;
        if ((section.Characteristics & required) != required) return {};
        if ((section.Characteristics & forbidden) != 0) return {};
        return section.PointerToRawData + static_cast<std::size_t>(offset);
    }
    return {};
}

bool NativeImageSlots::ExecutableRva(std::uint32_t rva) const noexcept
{
    return FileOffset(rva, 1, IMAGE_SCN_MEM_EXECUTE, 0).has_value();
}

std::optional<unsigned> NativeImageSlots::RelocationsAt(std::uint32_t slotRva) const noexcept
{
    const auto directory = header_.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress == 0 && directory.Size == 0) return 0;
    if (!ValidRelocationDirectory(directory)) return {};
    const auto offset = FileOffset(directory.VirtualAddress, directory.Size, IMAGE_SCN_MEM_READ, 0);
    if (!offset) return {};
    return CountRelocations(*offset, directory.Size, slotRva);
}

std::optional<unsigned> NativeImageSlots::CountRelocations(std::size_t offset,
    std::size_t bytes, std::uint32_t slotRva) const noexcept
{
    std::size_t cursor{};
    unsigned count{};
    while (cursor < bytes)
    {
        IMAGE_BASE_RELOCATION block{};
        if (!Read(file_, offset + cursor, block)) return {};
        if (!ValidRelocationBlock(block, bytes - cursor)) return {};
        if (!ReadRelocationEntries(offset + cursor, block, slotRva, count)) return {};
        cursor += block.SizeOfBlock;
    }
    return count;
}

bool NativeImageSlots::ReadRelocationEntries(std::size_t offset,
    const IMAGE_BASE_RELOCATION& block, std::uint32_t slotRva, unsigned& count) const noexcept
{
    const RelocationTarget target{header_.OptionalHeader.SizeOfImage, slotRva};
    const auto entries = (block.SizeOfBlock - sizeof(block)) / sizeof(WORD);
    for (std::size_t index = 0; index < entries; ++index)
    {
        WORD entry{};
        if (!Read(file_, offset + sizeof(block) + index * sizeof(WORD), entry)) return false;
        if (!CountRelocation(entry, block.VirtualAddress, target, count)) return false;
    }
    return true;
}

std::optional<std::uint32_t> NativeImageSlots::StaticTarget(std::uint32_t slotRva) const noexcept
{
    const auto offset = FileOffset(slotRva, sizeof(std::uint64_t), IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_EXECUTE);
    if (!offset) return {};
    std::uint64_t preferred{};
    if (!Read(file_, *offset, preferred)) return {};
    const auto base = header_.OptionalHeader.ImageBase;
    if (preferred < base) return {};
    const auto target = preferred - base;
    if (target >= header_.OptionalHeader.SizeOfImage) return {};
    if (!ExecutableRva(static_cast<std::uint32_t>(target))) return {};
    return static_cast<std::uint32_t>(target);
}

bool NativeImageSlots::RelocationMatches(std::uintptr_t loadedBase, std::uint32_t slotRva) const noexcept
{
    const auto relocations = RelocationsAt(slotRva);
    if (!relocations) return false;
    return *relocations == 1 || loadedBase == header_.OptionalHeader.ImageBase;
}

void* NativeImageSlots::Expected(std::uintptr_t loadedBase, std::uint32_t slotRva) const noexcept
{
    if (!parsed_ || loadedBase == 0) return nullptr;
    if (header_.OptionalHeader.SizeOfImage > std::numeric_limits<std::uintptr_t>::max() - loadedBase)
        return nullptr;
    const auto target = StaticTarget(slotRva);
    if (!target) return nullptr;
    if (!RelocationMatches(loadedBase, slotRva)) return nullptr;
    return reinterpret_cast<void*>(loadedBase + *target);
}

}  // namespace fo4hdr
