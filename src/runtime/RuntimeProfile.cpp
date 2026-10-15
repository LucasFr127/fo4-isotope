// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/runtime/RuntimeProfile.hpp"
#include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.11.240/RuntimeIdentity.generated.hpp"
#include <atomic>

namespace fo4hdr {
namespace {
std::atomic<const RuntimeProfile*> g_profile{};
}

const RuntimeProfile* FindRuntimeProfile(
    std::uint32_t runtimeVersion, std::uint32_t f4seVersion) noexcept
{
    for (const auto* profile : {&generated::og::kProfile, &generated::ae::kProfile})
        if (profile->runtimeVersion == runtimeVersion && profile->f4seVersion == f4seVersion)
            return profile;
    return nullptr;
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
