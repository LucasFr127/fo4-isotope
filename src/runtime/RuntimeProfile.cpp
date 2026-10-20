// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/runtime/RuntimeProfile.hpp"
#include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.10.984/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.11.240/RuntimeIdentity.generated.hpp"
#include <atomic>

namespace fo4hdr {
namespace {
std::atomic<const RuntimeProfile*> g_profile{};
}

RuntimeSelection SelectRuntimeProfile(
    std::uint32_t runtimeVersion, std::uint32_t f4seVersion) noexcept
{
    for (const auto* profile : {&generated::og::kProfile, &generated::ng::kProfile, &generated::ae::kProfile})
        if (profile->runtimeVersion == runtimeVersion)
            return {profile, f4seVersion >= profile->f4seVersion ?
                RuntimeSelectionReason::Selected : RuntimeSelectionReason::F4seBelowMinimum};
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
    case RuntimeSelectionReason::F4seBelowMinimum:
        return "f4se-below-minimum";
    }
    return "unknown-selection-reason";
}

const RuntimeProfile* FindRuntimeProfile(
    std::uint32_t runtimeVersion, std::uint32_t f4seVersion) noexcept
{
    const auto selection = SelectRuntimeProfile(runtimeVersion, f4seVersion);
    return selection.reason == RuntimeSelectionReason::Selected ? selection.profile : nullptr;
}

bool InitializeRuntimeProfile(std::uint32_t runtimeVersion, std::uint32_t f4seVersion) noexcept
{
    const auto* profile = FindRuntimeProfile(runtimeVersion, f4seVersion);
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
