// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace fo4hdr::game::direct_call_patch {
using CallBytes = std::array<std::uint8_t, 5>;

// First native-site rejection since the last take, for startup diagnostics.
// Records are per thread, so the F4SE load thread reads only its own attempt.
struct SiteMismatch
{
    std::uintptr_t address{};
    std::uint64_t id{};  // Nonzero when the Address Library mapping failed.
    std::uintptr_t mappedRva{};  // The ID's mapped RVA, or zero when absent.
    std::array<std::uint8_t, 16> observed{};
    std::size_t observedSize{};
    bool recorded{};
};

inline SiteMismatch& CurrentSiteMismatch() noexcept
{
    thread_local SiteMismatch mismatch;
    return mismatch;
}

inline SiteMismatch TakeSiteMismatch() noexcept { return std::exchange(CurrentSiteMismatch(), {}); }

inline void RecordSiteMismatch(std::uintptr_t address, std::span<const std::uint8_t> observed,
    std::uint64_t id = 0, std::uintptr_t mappedRva = 0) noexcept
{
    auto& mismatch = CurrentSiteMismatch();
    if (mismatch.recorded) return;
    mismatch = {.address = address, .id = id, .mappedRva = mappedRva, .recorded = true};
    mismatch.observedSize = (std::min)(observed.size(), mismatch.observed.size());
    std::copy_n(observed.begin(), mismatch.observedSize, mismatch.observed.begin());
}

template <class Reader>
bool MatchesNativeCode(std::uintptr_t address, std::span<const std::uint8_t> expected,
    Reader read) noexcept
{
    std::array<std::uint8_t, 130> actual{};
    if (expected.size() > actual.size()) return false;
    const bool readable = read(reinterpret_cast<const void*>(address), actual.data(), expected.size());
    if (readable && std::equal(expected.begin(), expected.end(), actual.begin())) return true;
    RecordSiteMismatch(address, readable ? std::span{actual.data(), expected.size()} :
        std::span<const std::uint8_t>{});
    return false;
}

template <class Mappings>
bool MatchesMapping(const Mappings& mappings, std::uintptr_t base, std::uint64_t id,
    std::uintptr_t rva) noexcept
{
    const auto found = std::find_if(mappings.begin(), mappings.end(),
        [id](const auto& entry) { return entry.id == id; });
    if (found != mappings.end() && found->offset == rva) return true;
    RecordSiteMismatch(base + rva, {}, id, found != mappings.end() ? found->offset : 0);
    return false;
}

inline bool ValidCallSite(std::uintptr_t site) noexcept
{ return site != 0 && site <= UINTPTR_MAX - 5; }

inline bool EncodeCall(std::uintptr_t site, std::uintptr_t target, CallBytes& bytes) noexcept
{
    if (!ValidCallSite(site) || !target) return false;
    const auto next = site + 5;
    const auto positive = target >= next;
    const auto distance = positive ? target - next : next - target;
    if (distance > (positive ? std::uint64_t{INT32_MAX} : std::uint64_t{INT32_MAX} + 1))
        return false;
    const auto relative = static_cast<std::int32_t>(positive ?
        static_cast<std::int64_t>(distance) : -static_cast<std::int64_t>(distance));
    bytes[0] = 0xE8;
    std::memcpy(bytes.data() + 1, &relative, sizeof(relative));
    return true;
}

inline std::uintptr_t CallTarget(std::uintptr_t site, const CallBytes& bytes) noexcept
{
    if (!ValidCallSite(site) || bytes[0] != 0xE8) return 0;
    std::int32_t displacement{};
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    const auto next = site + 5;
    if (displacement < 0) {
        const auto distance = static_cast<std::uintptr_t>(-static_cast<std::int64_t>(displacement));
        return distance <= next ? next - distance : 0;
    }
    return next <= UINTPTR_MAX - displacement ? next + displacement : 0;
}

struct Site
{
    std::uintptr_t address{};
    CallBytes original{}, replacement{};
    std::uint32_t protection{};
};

template<std::size_t N, class Backend>
bool ReplaceSites(const std::array<Site, N>& sites, Backend& backend) noexcept
{
    for (const auto& site : sites) {
        if (!backend.Replace(site, site.original, site.replacement)) return false;
    }
    bool installed = true;
    for (const auto& site : sites)
        installed = backend.Matches(site.address, site.replacement) && installed;
    return installed;
}

template<std::size_t N, class Backend>
void RestoreOwnedSites(const std::array<Site, N>& sites, Backend& backend) noexcept
{
    // Never restore a foreign patch. The process-lifetime relay and next
    // remain valid if cleanup fails; observation is not published.
    for (auto it = sites.rbegin(); it != sites.rend(); ++it)
        if (backend.Matches(it->address, it->replacement))
            (void)backend.Replace(*it, it->replacement, it->original);
}

// Backend Replace checks ownership again, writes, restores protection, flushes
// the instruction cache and verifies the result. A false return can mean that
// bytes were written but final protection/cache verification failed.
template<std::size_t N, class Backend>
bool InstallPair(const std::array<Site, N>& sites, Backend& backend) noexcept
{
    for (const auto& site : sites)
        if (!backend.Matches(site.address, site.original)) return false;
    const bool installed = ReplaceSites(sites, backend);
    if (!installed) RestoreOwnedSites(sites, backend);
    return installed;
}
}
