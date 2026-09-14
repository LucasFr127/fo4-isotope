// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <REX/W32/KERNEL32.h>
#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace fo4hdr::game::observation {

[[nodiscard]] inline bool ValidMemoryBounds(const void* address, std::size_t size) noexcept
{
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    return start != 0 && size != 0 && size <= UINTPTR_MAX - start;
}

[[nodiscard]] inline bool ReadableProtection(const MEMORY_BASIC_INFORMATION& region) noexcept
{
    if (region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) != 0) return false;
    constexpr std::array readable{PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY,
        PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY};
    for (const auto protection : readable)
        if ((region.Protect & 0xFFU) == protection) return true;
    return false;
}

[[nodiscard]] inline bool AdvanceReadableRegion(std::uintptr_t& cursor) noexcept
{
    MEMORY_BASIC_INFORMATION region{};
    if (::VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) == 0 ||
        !ReadableProtection(region)) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (base > cursor || region.RegionSize > UINTPTR_MAX - base) return false;
    const auto end = base + region.RegionSize;
    if (end <= cursor) return false;
    cursor = end;
    return true;
}

[[nodiscard]] inline bool IsReadableMemory(const void* address, std::size_t size) noexcept
{
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (!ValidMemoryBounds(address, size)) return false;
    auto cursor = start;
    while (cursor < start + size)
    {
        if (!AdvanceReadableRegion(cursor)) return false;
    }
    return true;
}

[[nodiscard]] inline bool ReadBytes(const void* source, void* destination, std::size_t size) noexcept
{
    if (!IsReadableMemory(source, size)) return false;
    SIZE_T copied{};
    return ::ReadProcessMemory(::GetCurrentProcess(), source, destination, size, &copied) != FALSE &&
        copied == size;
}

// The caller supplies object lifetime and synchronization. A guarded byte copy
// bounds a failed read; it cannot make unsynchronized engine mutation safe.
template<class T>
[[nodiscard]] bool GuardedReadValue(const void* source, T& destination) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    T candidate{};
    if (!ReadBytes(source, &candidate, sizeof(T))) return false;
    destination = candidate;
    return true;
}

template<class T>
[[nodiscard]] bool ReadSnapshotField(std::span<const std::byte> source,
    std::size_t offset, T& destination) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > source.size() || sizeof(T) > source.size() - offset) return false;
    std::memcpy(&destination, source.data() + offset, sizeof(T));
    return true;
}

template<std::size_t Size>
[[nodiscard]] bool ReadBoundedText(const char* source, std::array<char, Size>& destination) noexcept
{
    if (!ValidMemoryBounds(source, Size)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(source);
    for (std::size_t index = 0; index < Size; ++index)
    {
        if (!GuardedReadValue(reinterpret_cast<const void*>(start + index), destination[index])) return false;
        if (destination[index] == '\0') return true;
    }
    return false;
}

}  // namespace fo4hdr::game::observation
