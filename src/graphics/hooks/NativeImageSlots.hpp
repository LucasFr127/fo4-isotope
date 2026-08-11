// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace fo4hdr {

// File bytes are held immutable for this cold inspection. Expected values come
// from static same-image absolute entries, never from a live vtable snapshot.
class NativeImageSlots
{
public:
    [[nodiscard]] bool Parse(std::span<const std::byte> file) noexcept;
    [[nodiscard]] void* Expected(std::uintptr_t loadedBase, std::uint32_t slotRva) const noexcept;
    [[nodiscard]] const IMAGE_NT_HEADERS64& Header() const noexcept { return header_; }
    [[nodiscard]] std::span<const std::byte> HeaderBytes() const noexcept
    {
        return parsed_ ? file_.first(header_.OptionalHeader.SizeOfHeaders) : std::span<const std::byte>{};
    }

private:
    [[nodiscard]] bool ReadLayout() noexcept;
    [[nodiscard]] bool ReadSections() noexcept;
    [[nodiscard]] bool DistinctSections() const noexcept;
    [[nodiscard]] std::optional<std::size_t> FileOffset(std::uint32_t rva,
        std::size_t bytes, DWORD required, DWORD forbidden) const noexcept;
    [[nodiscard]] std::optional<unsigned> RelocationsAt(std::uint32_t slotRva) const noexcept;
    [[nodiscard]] std::optional<unsigned> CountRelocations(std::size_t offset,
        std::size_t bytes, std::uint32_t slotRva) const noexcept;
    [[nodiscard]] bool ReadRelocationEntries(std::size_t offset,
        const IMAGE_BASE_RELOCATION& block, std::uint32_t slotRva, unsigned& count) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> StaticTarget(std::uint32_t slotRva) const noexcept;
    [[nodiscard]] bool RelocationMatches(std::uintptr_t loadedBase, std::uint32_t slotRva) const noexcept;
    [[nodiscard]] bool ExecutableRva(std::uint32_t rva) const noexcept;

    std::span<const std::byte> file_;
    IMAGE_NT_HEADERS64 header_{};
    std::array<IMAGE_SECTION_HEADER, 96> sections_{};
    std::size_t sectionOffset_{};
    bool parsed_{};
};

}  // namespace fo4hdr
