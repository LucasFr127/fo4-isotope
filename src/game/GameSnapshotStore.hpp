// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "Fallout4HDR/game/GameHdrSnapshot.hpp"
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>

namespace fo4hdr {

// Writers and readers exchange copies. The mutex protects the complete
// snapshot update, so readers never observe a partially changed value.
class GameSnapshotStore
{
public:
    [[nodiscard]] GameHdrSnapshot Read() const
    {
        const std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    template <class Function>
    void Update(Function&& function)
    {
        // The callback runs under mutex_ and must not call this store. Keeping
        // the read-modify-publish sequence under one lock prevents lost
        // updates when lifecycle and UI observations arrive concurrently.
        static_assert(
            std::is_nothrow_invocable_v<Function&, GameHdrSnapshot&>);
        const std::scoped_lock lock(mutex_);
        const auto nextGeneration = snapshot_.generation + 1;
        std::invoke(
            std::forward<Function>(function),
            snapshot_);
        snapshot_.generation = nextGeneration;
    }

private:
    mutable std::mutex mutex_;
    GameHdrSnapshot snapshot_{};
};

// A world transition invalidates every copied engine-object identity. The
// queue sequence and cumulative drop count are retained so the single menu
// consumer can continue draining monotonically; no retained menu value is
// advertised as current until a later synchronization boundary succeeds.
constexpr void InvalidateWorldDependentGameSnapshot(
    GameHdrSnapshot& snapshot,
    bool loading) noexcept
{
    constexpr auto dependent =
        static_cast<std::uint32_t>(GameSnapshotFeature::ImageSpace) |
        static_cast<std::uint32_t>(GameSnapshotFeature::Renderer) |
        static_cast<std::uint32_t>(GameSnapshotFeature::RenderTargets) |
        static_cast<std::uint32_t>(GameSnapshotFeature::Ui);
    snapshot.availableFeatures = static_cast<GameSnapshotFeature>(
        static_cast<std::uint32_t>(snapshot.availableFeatures) & ~dependent);
    snapshot.failedFeatures = static_cast<GameSnapshotFeature>(
        static_cast<std::uint32_t>(snapshot.failedFeatures) & ~dependent);

    snapshot.frameId = 0;
    snapshot.uiContexts = UiContextFlag::None;
    snapshot.loading = loading;
    snapshot.interior = false;
    snapshot.underwater = false;
    snapshot.imageSpaceOverride = false;
    snapshot.holdExposure = false;
    snapshot.taaEnabled = false;
    snapshot.dynamicResolutionEnabled = false;
    snapshot.rendererInitialized = false;
    snapshot.rendererBorderless = false;
    snapshot.rendererVsync = false;
    snapshot.lastMenuOpening = false;
    snapshot.menuHistoryComplete = true;
    snapshot.menuHistoryCaughtUp = false;
    snapshot.menuStateSynchronized = false;
    snapshot.menuTransitionWitnessValid = true;
    snapshot.activeImageSpace = {};
    snapshot.dynamicWidthRatio = 1.0F;
    snapshot.dynamicHeightRatio = 1.0F;
    snapshot.renderWidth = 0;
    snapshot.renderHeight = 0;
    snapshot.rendererDeviceIdentity = 0;
    snapshot.rendererContextIdentity = 0;
    snapshot.imageSpaceIdentity = 0;
    snapshot.baseImageSpaceIdentity = 0;
    snapshot.overrideImageSpaceIdentity = 0;
    snapshot.underwaterImageSpaceIdentity = 0;
    snapshot.consoleImageSpaceIdentity = 0;
    snapshot.rendererWindowCount = 0;
    snapshot.populatedRenderTargetCount = 0;
    snapshot.populatedDepthTargetCount = 0;
    snapshot.populatedCubeTargetCount = 0;
    snapshot.imageSpaceEffectCount = 0;
    snapshot.lastMenuName = {};
    snapshot.imageSpaceEffects = {};
    snapshot.rendererWindows = {};
    snapshot.renderTargets = {};
    snapshot.luts = {};
    snapshot.activeLutCount = 0;
    snapshot.pauseMenuPair = {};
    snapshot.pipBoyMenuPair = {};
}

constexpr void ApplyGameLifecycleTransition(
    GameHdrSnapshot& snapshot,
    GameLifecycleEvent event,
    bool succeeded) noexcept
{
    MarkGameSnapshotFeatureAvailable(
        snapshot,
        GameSnapshotFeature::Lifecycle);
    snapshot.lifecycleEvent = event;
    ++snapshot.lifecycleGeneration;

    if (event == GameLifecycleEvent::PreLoadGame ||
        event == GameLifecycleEvent::NewGame)
    {
        ++snapshot.resourceGeneration;
        InvalidateWorldDependentGameSnapshot(
            snapshot,
            event == GameLifecycleEvent::PreLoadGame);
    }
    else if (event == GameLifecycleEvent::PostLoadGame && succeeded)
    {
        snapshot.loading = false;
    }
}

}  // namespace fo4hdr

namespace fo4hdr {

// Full game-state observation is requested only from the reviewed one-shot
// task boundary associated with these lifecycle events.
[[nodiscard]] constexpr bool ShouldCollectGameSnapshotAtLifecycle(
    GameLifecycleEvent event,
    bool succeeded) noexcept
{
    return
        (event == GameLifecycleEvent::PostLoadGame && succeeded) ||
        event == GameLifecycleEvent::NewGame ||
        event == GameLifecycleEvent::PreSaveGame;
}

}  // namespace fo4hdr
