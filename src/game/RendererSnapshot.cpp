// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include <RE/Fallout.h>
#include "GameStateObserver.hpp"
#include "GameObservationMemory.hpp"
#include <REX/W32/KERNEL32.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace fo4hdr::game::observation {
void CopyRendererState(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed)
{
    observed.rendererInitialized = renderer.initialized;
    observed.rendererBorderless = renderer.borderlessWindow;
    observed.rendererVsync = renderer.vsync;
    observed.rendererDeviceIdentity =
        reinterpret_cast<std::uintptr_t>(renderer.device);
    observed.rendererContextIdentity =
        reinterpret_cast<std::uintptr_t>(renderer.context);
}

void CopyWindows(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed)
{
    for (std::size_t index = 0;
         index < observed.rendererWindows.size();
         ++index)
    {
        const auto& window = renderer.renderWindow[index];
        auto& destination = observed.rendererWindows[index];
        destination.arrayIndex = static_cast<std::uint32_t>(index);
        destination.windowIdentity =
            reinterpret_cast<std::uintptr_t>(window.hwnd);
        destination.swapChainIdentity =
            reinterpret_cast<std::uintptr_t>(window.swapChain);
        destination.x = window.windowX;
        destination.y = window.windowY;
        destination.width = window.windowWidth;
        destination.height = window.windowHeight;
        destination.present =
            window.hwnd != nullptr || window.swapChain != nullptr;
        if (window.hwnd == nullptr && window.swapChain == nullptr)
        {
            continue;
        }
        ++observed.rendererWindowCount;
    }
}

void CopyTarget(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed, std::size_t index)
{
    const auto& source = renderer.renderTargets[index];
    auto& destination = observed.renderTargets[index];
    destination.arrayIndex = static_cast<std::uint32_t>(index);
    destination.textureIdentity =
        reinterpret_cast<std::uintptr_t>(source.texture);
    destination.renderTargetViewIdentity =
        reinterpret_cast<std::uintptr_t>(source.rtView);
    destination.shaderResourceViewIdentity =
        reinterpret_cast<std::uintptr_t>(source.srView);
    destination.unorderedAccessViewIdentity =
        reinterpret_cast<std::uintptr_t>(source.uaView);
    destination.resourcePresent =
        source.texture != nullptr || source.rtView != nullptr ||
        source.srView != nullptr || source.uaView != nullptr;
    if (destination.resourcePresent)
    {
        ++observed.populatedRenderTargetCount;
    }
}

void CopyTargets(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed)
{
    for (std::size_t index = 0; index < observed.renderTargets.size(); ++index)
    {
        CopyTarget(renderer, observed, index);
    }
}

bool DepthTargetPresent(const auto& source) noexcept
{ return source.texture != nullptr || source.srViewDepth != nullptr || source.srViewStencil != nullptr; }

bool CubeTargetPresent(const auto& source) noexcept
{ return source.texture != nullptr || source.srView != nullptr; }

void CountAuxiliaryTargets(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed)
{
    observed.populatedDepthTargetCount += static_cast<std::uint32_t>(std::ranges::count_if(
        renderer.depthStencilTargets, [](const auto& source) { return DepthTargetPresent(source); }));
    observed.populatedCubeTargetCount += static_cast<std::uint32_t>(std::ranges::count_if(
        renderer.cubeMapRenderTargets, [](const auto& source) { return CubeTargetPresent(source); }));
}
void PublishRenderer(GameSnapshotStore& store, const GameHdrSnapshot& observed, bool targetManagerAvailable)
{
    store.Update(
        [&observed, targetManagerAvailable](
            fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            fo4hdr::MarkGameSnapshotFeatureAvailable(
                snapshot,
                fo4hdr::GameSnapshotFeature::Renderer);
            if (targetManagerAvailable)
            {
                fo4hdr::MarkGameSnapshotFeatureAvailable(
                    snapshot,
                    fo4hdr::GameSnapshotFeature::RenderTargets);
            }
            snapshot.rendererInitialized = observed.rendererInitialized;
            snapshot.rendererBorderless = observed.rendererBorderless;
            snapshot.rendererVsync = observed.rendererVsync;
            snapshot.rendererDeviceIdentity =
                observed.rendererDeviceIdentity;
            snapshot.rendererContextIdentity =
                observed.rendererContextIdentity;
            snapshot.rendererWindows = observed.rendererWindows;
            snapshot.rendererWindowCount = observed.rendererWindowCount;
            snapshot.renderWidth = observed.renderWidth;
            snapshot.renderHeight = observed.renderHeight;
            snapshot.dynamicWidthRatio = observed.dynamicWidthRatio;
            snapshot.dynamicHeightRatio = observed.dynamicHeightRatio;
            snapshot.dynamicResolutionEnabled =
                observed.dynamicResolutionEnabled;
            snapshot.populatedRenderTargetCount =
                observed.populatedRenderTargetCount;
            snapshot.populatedDepthTargetCount =
                observed.populatedDepthTargetCount;
            snapshot.populatedCubeTargetCount =
                observed.populatedCubeTargetCount;
            snapshot.renderTargets = observed.renderTargets;
        });
}

void ReportRenderer(const GameHdrSnapshot& observed, std::string_view trigger, bool targetManagerAvailable)
{
    REX::INFO(
        "renderer available=true trigger={} thread={} initialized={} "
        "device_present={} context_present={} windows={} "
        "borderless={} vsync={}",
        trigger,
        REX::W32::GetCurrentThreadId(),
        observed.rendererInitialized,
        observed.rendererDeviceIdentity != 0,
        observed.rendererContextIdentity != 0,
        observed.rendererWindowCount,
        observed.rendererBorderless,
        observed.rendererVsync);
    REX::INFO(
        "render_targets available={} render_populated={} "
        "depth_populated={} cube_populated={} resource_slots=101/13/2 "
        "manager_present={} manager_metadata_observed=false "
        "semantic_mapping=unvalidated reason=manager-ownership-unproven",
        targetManagerAvailable,
        observed.populatedRenderTargetCount,
        observed.populatedDepthTargetCount,
        observed.populatedCubeTargetCount,
        targetManagerAvailable);
}

bool RendererReady(const RE::BSGraphics::RendererData& renderer) noexcept
{ return renderer.initialized && renderer.device != nullptr && renderer.context != nullptr; }

void ReportRendererFailure(GameSnapshotStore& store, std::string_view rendererReason,
    std::string_view targetReason) noexcept
{
    ReportFeatureFailure(store, GameSnapshotFeature::Renderer, "renderer", rendererReason);
    ReportFeatureFailure(store, GameSnapshotFeature::RenderTargets, "render-targets", targetReason);
}
void ObserveRendererAndTargets(GameSnapshotStore& store, std::string_view trigger)
{
    auto* renderer = ValidateCommonLibObject(
        RE::BSGraphics::GetRendererData(),
        sizeof(RE::BSGraphics::RendererData),
        "RE::BSGraphics::GetRendererData");
    if (renderer == nullptr)
    {
        ReportRendererFailure(store, "relocation-or-object-invalid", "renderer-unavailable");
        return;
    }
    auto* targetManager = ValidateCommonLibObject(
        RE::BSGraphics::RenderTargetManager::GetSingleton(),
        sizeof(RE::BSGraphics::RenderTargetManager),
        "RE::BSGraphics::RenderTargetManager::GetSingleton");
    auto* rendererCriticalSection = reinterpret_cast<LPCRITICAL_SECTION>(
        &renderer->rendererLock.criticalSection);
    if (TryEnterCriticalSection(rendererCriticalSection) == 0)
    {
        ReportRendererFailure(store, "renderer-lock-busy", "renderer-lock-busy");
        return;
    }
    if (!RendererReady(*renderer))
    {
        LeaveCriticalSection(rendererCriticalSection);
        ReportRendererFailure(store, "renderer-not-ready", "renderer-not-ready");
        return;
    }

    fo4hdr::GameHdrSnapshot observed{};
    CopyRendererState(*renderer, observed);
    CopyWindows(*renderer, observed);
    CopyTargets(*renderer, observed);
    CountAuxiliaryTargets(*renderer, observed);
    LeaveCriticalSection(rendererCriticalSection);

    const auto targetManagerAvailable = targetManager != nullptr;
    if (!targetManagerAvailable)
    {
        ReportFeatureFailure(store,
            fo4hdr::GameSnapshotFeature::RenderTargets,
            "render-targets",
            "render-target-manager-relocation-or-object-invalid");
    }

    PublishRenderer(store, observed, targetManagerAvailable);
    ReportRenderer(observed, trigger, targetManagerAvailable);
}
}
