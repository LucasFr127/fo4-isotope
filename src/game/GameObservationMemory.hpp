// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GameSnapshotStore.hpp"
#include <REX/LOG.h>
// CommonLib's Win32 declarations must precede the Windows SDK headers.
#include <REX/W32/KERNEL32.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace fo4hdr::game::observation {
inline bool ReadableProtection(DWORD flags) noexcept
{
    constexpr std::array protections{PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY,
        PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY};
    return std::ranges::find(protections, flags & 0xFF) != protections.end();
}

inline bool QueryReadableRegion(std::uintptr_t cursor, MEMORY_BASIC_INFORMATION& region) noexcept
{
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region))
        return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    return ReadableProtection(region.Protect);
}
inline bool AdvanceRegion(const MEMORY_BASIC_INFORMATION& region, std::uintptr_t end,
    std::uintptr_t& cursor) noexcept
{
    const auto regionStart =
        reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (regionStart >
        (std::numeric_limits<std::uintptr_t>::max)() - region.RegionSize)
    {
        return false;
    }
    const auto regionEnd = regionStart + region.RegionSize;
    if (regionEnd <= cursor)
    {
        return false;
    }
    cursor = (std::min)(end, regionEnd);
    return true;
}

inline bool ValidReadRange(const void* memory, std::size_t size) noexcept
{
    return memory != nullptr && size != 0 && reinterpret_cast<std::uintptr_t>(memory) <=
        (std::numeric_limits<std::uintptr_t>::max)() - size;
}
[[nodiscard]] inline bool IsReadableMemory(
    const void* memory,
    std::size_t size,
    bool requireGameImage) noexcept
{
    if (!ValidReadRange(memory, size)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(memory);
    const auto end = start + size;
    const auto gameModule = REX::W32::GetModuleHandleW(nullptr);
    auto cursor = start;

    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (!QueryReadableRegion(cursor, region)) return false;
        if (requireGameImage && region.AllocationBase != gameModule) return false;
        if (!AdvanceRegion(region, end, cursor)) return false;
    }
    return true;
}

template <class T>
[[nodiscard]] bool GuardedReadValue(
    const void* source,
    T& destination) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    SIZE_T copied{};
    return source != nullptr &&
           ReadProcessMemory(
               GetCurrentProcess(),
               source,
               &destination,
               sizeof(destination),
               &copied) != FALSE &&
           copied == sizeof(destination);
}

template <class T, std::size_t Size>
[[nodiscard]] bool ReadSnapshotField(
    const std::array<std::byte, Size>& snapshot,
    std::size_t offset,
    T& destination) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > Size || sizeof(T) > Size - offset)
    {
        return false;
    }
    std::memcpy(&destination, snapshot.data() + offset, sizeof(T));
    return true;
}

template <class T>
[[nodiscard]] T* ValidateCommonLibObject(
    T* object,
    std::size_t objectSize,
    std::string_view symbol)
{
    // PluginMain accepts the exact executable and pinned Address Library
    // hashes before registering this observer. CommonLib therefore owns IDDB
    // resolution; this boundary validates only the resulting live object.
    const auto objectValid =
        object != nullptr &&
        reinterpret_cast<std::uintptr_t>(object) % alignof(T) == 0 &&
        IsReadableMemory(object, objectSize, false);

    REX::INFO(
        "m6 commonlib_mapping symbol={} object_valid={}",
        symbol,
        objectValid);
    return objectValid ? object : nullptr;
}

inline void ReportFeatureFailure(
    fo4hdr::GameSnapshotStore& store,
    fo4hdr::GameSnapshotFeature feature,
    std::string_view name,
    std::string_view reason) noexcept
{
    try
    {
        store.Update(
            [feature](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
                fo4hdr::MarkGameSnapshotFeatureFailed(snapshot, feature);
            });
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "Fallout4HDR: snapshot failure-state publication failed.\n");
    }
    try
    {
        REX::WARN(
            "m2 feature={} available=false reason={}",
            name,
            reason);
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "Fallout4HDR: feature failure logging failed.\n");
    }
}

}
