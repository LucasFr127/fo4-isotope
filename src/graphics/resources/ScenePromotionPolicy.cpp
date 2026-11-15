// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ScenePromotionPolicy.hpp"

#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "../GraphicsDiagnostics.hpp"

#include <atomic>

namespace fo4hdr {
namespace {

// Where the slot-33/34 registrar calls return; zero until the game layer publishes them.
std::array<std::atomic<std::uintptr_t>, 2> g_lightingSites{};
std::atomic<DXGI_FORMAT> g_sceneTargetFormat{DXGI_FORMAT_R16G16B16A16_FLOAT};

}  // namespace

void PublishLightingTargetSites(std::span<const std::uintptr_t, 2> returns) noexcept
{
    for (std::size_t index = 0; index < returns.size(); ++index)
        g_lightingSites[index].store(returns[index], std::memory_order_release);
}

void LatchSceneTargetFormat(DXGI_FORMAT format) noexcept
{
    g_sceneTargetFormat.store(format, std::memory_order_release);
}

DXGI_FORMAT LatchedSceneTargetFormat() noexcept
{
    return g_sceneTargetFormat.load(std::memory_order_acquire);
}

bool LatchScenePromotion(bool hdrGeneration, RenderPrecision precision) noexcept
{
    LatchSceneTargetFormat(SceneTargetFormat(hdrGeneration, precision));
    return PromotesSceneTargets(hdrGeneration, precision);
}

std::array<std::uintptr_t, 2> VanillaLightingSites(RenderPrecision precision) noexcept
{
    const std::array sites{g_lightingSites[0].load(std::memory_order_acquire),
        g_lightingSites[1].load(std::memory_order_acquire)};
    if (precision != RenderPrecision::medium || std::ranges::find(sites, 0U) != sites.end())
        return {};
    return sites;
}

bool KeepGameLightingFormat(std::span<const std::uintptr_t, 2> sites) noexcept
{
    // Full, or no verified sites: the generation latched zeros.
    if (sites[0] == 0) return false;
    std::array<void*, 24> frames{};
    const auto count = CaptureCallerFrames(frames);
    if (!KeepsVanillaLightingTarget(std::span{frames}.first(count), sites)) return false;
    g_evidence.vanillaLightingTargetCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace fo4hdr
