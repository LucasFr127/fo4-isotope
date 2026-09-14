// SPDX-License-Identifier: GPL-3.0-or-later

#include "GameStateObserver.hpp"
#include "GameObservationMemory.hpp"
#include "Fallout4HDR/game/GameHdrSnapshot.hpp"
#include <RE/Fallout.h>
#include <Windows.h>
#include <algorithm>
#include <type_traits>

namespace fo4hdr::game {
namespace {

void CopyWindows(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed) noexcept
{
    for (std::size_t index = 0; index < observed.rendererWindows.size(); ++index)
    {
        const auto& source = renderer.renderWindow[index];
        auto& destination = observed.rendererWindows[index];
        destination.arrayIndex = static_cast<std::uint32_t>(index);
        destination.windowIdentity = reinterpret_cast<std::uintptr_t>(source.hwnd);
        destination.swapChainIdentity = reinterpret_cast<std::uintptr_t>(source.swapChain);
        destination.x = source.windowX;
        destination.y = source.windowY;
        destination.width = source.windowWidth;
        destination.height = source.windowHeight;
        destination.present = source.hwnd != nullptr || source.swapChain != nullptr;
        observed.rendererWindowCount += destination.present ? 1 : 0;
    }
}

void CopyTargets(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed) noexcept
{
    for (std::size_t index = 0; index < observed.renderTargets.size(); ++index)
    {
        const auto& source = renderer.renderTargets[index];
        auto& destination = observed.renderTargets[index];
        destination.arrayIndex = static_cast<std::uint32_t>(index);
        destination.textureIdentity = reinterpret_cast<std::uintptr_t>(source.texture);
        destination.renderTargetViewIdentity = reinterpret_cast<std::uintptr_t>(source.rtView);
        destination.shaderResourceViewIdentity = reinterpret_cast<std::uintptr_t>(source.srView);
        destination.unorderedAccessViewIdentity = reinterpret_cast<std::uintptr_t>(source.uaView);
        destination.resourcePresent = source.texture || source.rtView || source.srView || source.uaView;
        observed.populatedRenderTargetCount += destination.resourcePresent ? 1 : 0;
    }
}

void CountAuxiliaryTargets(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed) noexcept
{
    observed.populatedDepthTargetCount = static_cast<std::uint32_t>(std::ranges::count_if(
        renderer.depthStencilTargets, [](const auto& source) {
            return source.texture || source.srViewDepth || source.srViewStencil;
        }));
    observed.populatedCubeTargetCount = static_cast<std::uint32_t>(std::ranges::count_if(
        renderer.cubeMapRenderTargets, [](const auto& source) { return source.texture || source.srView; }));
}

bool TargetManagerAvailable()
{
    // The accessor returns a value; inspect only the declared singleton pointer.
    REL::Relocation<RE::BSGraphics::RenderTargetManager*> manager{
        RE::ID::BSGraphics::RenderTargetManager::Singleton};
    return observation::IsReadableMemory(manager.get(), sizeof(RE::BSGraphics::RenderTargetManager));
}

class RendererLock
{
public:
    explicit RendererLock(CRITICAL_SECTION* lock) noexcept : lock_(lock) {}
    ~RendererLock() { ::LeaveCriticalSection(lock_); }
    RendererLock(const RendererLock&) = delete;
    RendererLock& operator=(const RendererLock&) = delete;
private:
    CRITICAL_SECTION* lock_;
};

void CopyLockedRenderer(const RE::BSGraphics::RendererData& renderer, GameHdrSnapshot& observed) noexcept
{
    observed.rendererInitialized = renderer.initialized;
    observed.rendererBorderless = renderer.borderlessWindow;
    observed.rendererVsync = renderer.vsync;
    observed.rendererDeviceIdentity = reinterpret_cast<std::uintptr_t>(renderer.device);
    observed.rendererContextIdentity = reinterpret_cast<std::uintptr_t>(renderer.context);
    CopyWindows(renderer, observed);
    CopyTargets(renderer, observed);
    CountAuxiliaryTargets(renderer, observed);
    observed.rendererObserved = true;
}

bool RendererReady(const RE::BSGraphics::RendererData& renderer) noexcept
{
    return renderer.initialized && renderer.device != nullptr && renderer.context != nullptr;
}

}  // namespace

bool ReadRendererSnapshot(GameHdrSnapshot& destination) noexcept
{
    try
    {
        auto* renderer = RE::BSGraphics::GetRendererData();
        if (!observation::IsReadableMemory(renderer, sizeof(RE::BSGraphics::RendererData))) return false;
        auto* lock = reinterpret_cast<CRITICAL_SECTION*>(&renderer->rendererLock.criticalSection);
        if (!::TryEnterCriticalSection(lock)) return false;
        const RendererLock release(lock);
        if (!RendererReady(*renderer)) return false;
        static_assert(std::extent_v<decltype(RE::BSGraphics::RendererData::renderWindow)> == 32);
        static_assert(std::extent_v<decltype(RE::BSGraphics::RendererData::renderTargets)> == 101);
        static_assert(std::extent_v<decltype(RE::BSGraphics::RendererData::depthStencilTargets)> == 13);
        static_assert(std::extent_v<decltype(RE::BSGraphics::RendererData::cubeMapRenderTargets)> == 2);
        GameHdrSnapshot observed{};
        CopyLockedRenderer(*renderer, observed);
        observed.renderTargetManagerAvailable = TargetManagerAvailable();
        destination = observed;
        return true;
    }
    catch (...) { return false; }
}

}  // namespace fo4hdr::game
