// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/game/GameHdrSnapshot.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace fo4hdr {

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

constexpr void ApplyGameLifecycleTransition(
    GameHdrSnapshot& snapshot, GameLifecycleEvent event, bool succeeded) noexcept
{
    snapshot.lifecycleEvent = event;
    snapshot.lifecycleSucceeded = succeeded;
    ++snapshot.lifecycleGeneration;
    if (event == GameLifecycleEvent::PreLoadGame || event == GameLifecycleEvent::NewGame)
    {
        ++snapshot.resourceGeneration;
        // NewGame precedes load completion; this event cannot establish world readiness.
        snapshot.loading = true;
        snapshot.rendererInitialized = false;
        snapshot.rendererBorderless = false;
        snapshot.rendererVsync = false;
        snapshot.rendererDeviceIdentity = 0;
        snapshot.rendererContextIdentity = 0;
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

}  // namespace fo4hdr
