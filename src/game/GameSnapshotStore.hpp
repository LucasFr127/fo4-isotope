// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/game/GameHdrSnapshot.hpp"
#include "MenuEventQueue.hpp"

#include <atomic>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace fo4hdr {

class GraphicsLifecyclePublisher
{
public:
    void PublishTransition(GameLifecycleEvent event, bool succeeded) noexcept
    {
        if (event == GameLifecycleEvent::PreLoadGame)
        {
            Publish(false, true);
        }
        else if (event == GameLifecycleEvent::NewGame)
        {
            Publish(true, true);
        }
        else if (event == GameLifecycleEvent::PostLoadGame && succeeded)
        {
            Publish(true, false);
        }
    }

    void Disable() noexcept
    {
        packed_.fetch_or(failedMask, std::memory_order_release);
    }

    [[nodiscard]] GraphicsLifecyclePublication Read() const noexcept
    {
        const auto packed = packed_.load(std::memory_order_acquire);
        return {(packed & (readyMask | failedMask)) == readyMask, packed >> 2};
    }

private:
    void Publish(bool ready, bool advanceReset) noexcept
    {
        auto previous = packed_.load(std::memory_order_relaxed);
        if ((previous & failedMask) != 0)
        {
            return;
        }
        const auto epoch = previous >> 2;
        if (advanceReset && epoch == maximumEpoch)
        {
            // Exhaustion must not reuse an epoch that a graphics owner could retain.
            Disable();
            return;
        }
        const auto next = ((epoch + (advanceReset ? 1 : 0)) << 2) | (ready ? readyMask : 0);
        // Normal writers share the snapshot-store lock; racing failure is terminal.
        if (!packed_.compare_exchange_strong(previous, next,
                std::memory_order_release, std::memory_order_relaxed))
        {
            Disable();
        }
    }

    static constexpr std::uint64_t readyMask = 1;
    static constexpr std::uint64_t failedMask = 2;
    static constexpr std::uint64_t maximumEpoch = std::numeric_limits<std::uint64_t>::max() >> 2;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> packed_{};
};

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
        // Keep the locked callback plain-data-only; it must not reenter this store.
        static_assert(std::is_nothrow_invocable_v<Function&, GameHdrSnapshot&>);
        const std::scoped_lock lock(mutex_);
        // Capture before the callback so replacing the value cannot reset generation.
        const auto nextGeneration = snapshot_.generation + 1;
        std::invoke(std::forward<Function>(function), snapshot_);
        snapshot_.generation = nextGeneration;
    }

private:
    mutable std::mutex mutex_;
    GameHdrSnapshot snapshot_{};
};

[[nodiscard]] constexpr std::optional<bool> DecodeGameLifecycleOutcome(
    GameLifecycleEvent event, std::uint32_t dataLength, std::uintptr_t dataBits) noexcept
{
    if (event != GameLifecycleEvent::PostLoadGame && event != GameLifecycleEvent::GameDataReady)
    {
        return true;
    }
    // F4SE encodes these BOOL outcomes in pointer bits, including null for false.
    if (dataLength != 1 || dataBits > 1)
    {
        return std::nullopt;
    }
    return dataBits == 1;
}

constexpr void CopyObservedRenderer(GameHdrSnapshot& destination, const GameHdrSnapshot& observed) noexcept
{
    destination.rendererInitialized = observed.rendererInitialized;
    destination.rendererBorderless = observed.rendererBorderless;
    destination.rendererVsync = observed.rendererVsync;
    destination.rendererDeviceIdentity = observed.rendererDeviceIdentity;
    destination.rendererContextIdentity = observed.rendererContextIdentity;
    destination.rendererObserved = observed.rendererObserved;
    destination.renderTargetManagerAvailable = observed.renderTargetManagerAvailable;
    destination.rendererWindowCount = observed.rendererWindowCount;
    destination.populatedRenderTargetCount = observed.populatedRenderTargetCount;
    destination.populatedDepthTargetCount = observed.populatedDepthTargetCount;
    destination.populatedCubeTargetCount = observed.populatedCubeTargetCount;
    destination.renderWidth = observed.renderWidth;
    destination.renderHeight = observed.renderHeight;
    destination.dynamicWidthRatio = observed.dynamicWidthRatio;
    destination.dynamicHeightRatio = observed.dynamicHeightRatio;
    destination.dynamicResolutionEnabled = observed.dynamicResolutionEnabled;
    destination.rendererWindows = observed.rendererWindows;
    destination.renderTargets = observed.renderTargets;
}

constexpr void ApplyGameLifecycleTransition(
    GameHdrSnapshot& snapshot, GameLifecycleEvent event, bool succeeded,
    std::uint64_t menuPublication) noexcept
{
    snapshot.lifecycleEvent = event;
    snapshot.lifecycleSucceeded = succeeded;
    ++snapshot.lifecycleGeneration;
    if (event == GameLifecycleEvent::PreLoadGame || event == GameLifecycleEvent::NewGame)
    {
        ++snapshot.resourceGeneration;
        snapshot.loading = event == GameLifecycleEvent::PreLoadGame;
        CopyObservedRenderer(snapshot, {});
        snapshot.imageSpace = {};
        // This floor separates copied publications, not the source event's world lifetime.
        snapshot.menuResetFloor = menuPublication;
        snapshot.menuEventAvailable = false;
        snapshot.lastMenuCode = 0;
        snapshot.lastMenuOpening = false;
        snapshot.lastMenuSourceThread = 0;
        snapshot.menuHistoryCaughtUp = snapshot.lastMenuSequence >= menuPublication;
    }
    else if (event == GameLifecycleEvent::PostLoadGame && succeeded)
    {
        snapshot.loading = false;
    }
}

[[nodiscard]] constexpr bool ShouldCollectGameSnapshotAtLifecycle(
    GameLifecycleEvent event, bool succeeded) noexcept
{
    return (event == GameLifecycleEvent::PostLoadGame && succeeded) ||
        event == GameLifecycleEvent::NewGame || event == GameLifecycleEvent::PreSaveGame;
}

constexpr void DisableGameSnapshotCollection(GameHdrSnapshot& snapshot) noexcept
{
    snapshot.collectorFailed = true;
    snapshot.taskCollectionAvailable = false;
}

[[nodiscard]] constexpr bool PublishCollectedGameSnapshot(
    GameHdrSnapshot& snapshot, std::uint64_t resourceGeneration, std::uint32_t threadId) noexcept
{
    if (!snapshot.taskCollectionAvailable || snapshot.collectorFailed)
    {
        return false;
    }
    if (snapshot.resourceGeneration != resourceGeneration)
    {
        return false;
    }
    const bool threadChanged =
        snapshot.collectorThreadId != 0 && snapshot.collectorThreadId != threadId;
    if (threadId == 0 || threadChanged)
    {
        DisableGameSnapshotCollection(snapshot);
        return false;
    }
    snapshot.collectorThreadId = threadId;
    ++snapshot.observationGeneration;
    return true;
}

template <std::size_t Capacity>
[[nodiscard]] bool CollectGameMenuSnapshot(GameHdrSnapshot& snapshot,
    MenuEventQueue<Capacity>& queue, std::uint64_t resourceGeneration,
    std::uint32_t threadId) noexcept
{
    if (!PublishCollectedGameSnapshot(snapshot, resourceGeneration, threadId))
    {
        return false;
    }
    // Reset and cursor advancement share the store's single publication boundary.
    DrainMenuEvents(snapshot, queue);
    return true;
}

}  // namespace fo4hdr
