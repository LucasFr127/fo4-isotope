// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/runtime/RuntimeProfile.hpp"
#include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.10.984/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.11.240/RuntimeIdentity.generated.hpp"
#include <algorithm>
#include <atomic>
#include <span>

namespace fo4hdr {
namespace {
std::atomic<const RuntimeProfile*> g_profile{};

// Packed builds of pinned CommonLibF4's supported OG, NG and AE runtimes; it
// terminates the process on any other executable.
constexpr std::uint32_t kOgRuntimes[]{0x010A0A30};
constexpr std::uint32_t kNgRuntimes[]{0x010A3D40, 0x010A3D80};
constexpr std::uint32_t kAeRuntimes[]{
    0x010B0890, 0x010B09F0, 0x010B0A90, 0x010B0BF0, 0x010B0DD0, 0x010B0F00};

struct RuntimeFamily
{
    const RuntimeProfile* profile;
    std::span<const std::uint32_t> runtimes;
};

constexpr RuntimeFamily kFamilies[]{
    {&generated::og::kProfile, kOgRuntimes},
    {&generated::ng::kProfile, kNgRuntimes},
    {&generated::ae::kProfile, kAeRuntimes}};
}

RuntimeSelection SelectRuntimeProfile(std::uint32_t runtimeVersion) noexcept
{
    for (const auto& family : kFamilies)
        if (std::ranges::find(family.runtimes, runtimeVersion) != family.runtimes.end())
            return {family.profile, RuntimeSelectionReason::Selected};
    return {};
}

std::string_view ToString(RuntimeSelectionReason reason) noexcept
{
    switch (reason)
    {
    case RuntimeSelectionReason::Selected:
        return "selected";
    case RuntimeSelectionReason::UnsupportedRuntime:
        return "unsupported-runtime";
    }
    return "unknown-selection-reason";
}

const RuntimeProfile* FindRuntimeProfile(std::uint32_t runtimeVersion) noexcept
{
    const auto selection = SelectRuntimeProfile(runtimeVersion);
    return selection.reason == RuntimeSelectionReason::Selected ? selection.profile : nullptr;
}

bool InitializeRuntimeProfile(std::uint32_t runtimeVersion) noexcept
{
    const auto* profile = FindRuntimeProfile(runtimeVersion);
    if (!profile) return false;
    const RuntimeProfile* expected{};
    return g_profile.compare_exchange_strong(expected, profile, std::memory_order_acq_rel) ||
        expected == profile;
}

const RuntimeProfile* ActiveRuntimeProfile() noexcept
{
    return g_profile.load(std::memory_order_acquire);
}

bool IsOriginalRuntime() noexcept
{
    return ActiveRuntimeProfile() == &generated::og::kProfile;
}
}  // namespace fo4hdr
