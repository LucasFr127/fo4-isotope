// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <type_traits>

namespace fo4hdr {

enum class GameLifecycleEvent : std::uint8_t
{
    None,
    PostLoad,
    PostPostLoad,
    PreLoadGame,
    PostLoadGame,
    PreSaveGame,
    PostSaveGame,
    DeleteGame,
    InputLoaded,
    NewGame,
    GameLoaded,
    GameDataReady
};

struct GameHdrSnapshot
{
    std::uint64_t generation{};
    std::uint64_t resourceGeneration{};
    std::uint64_t lifecycleGeneration{};
    std::uint64_t observationGeneration{};
    std::uint64_t lastMenuSequence{};
    std::uint64_t menuEventsDropped{};
    std::uint64_t menuResetFloor{};
    GameLifecycleEvent lifecycleEvent{};
    std::uint32_t collectorThreadId{};
    std::uint32_t lastMenuSourceThread{};
    std::uint8_t lastMenuCode{};
    bool lastMenuOpening{};
    bool menuEventAvailable{};
    // Completion is relative to the last drain cutoff; newer publications can remain.
    bool menuHistoryCaughtUp{true};
    bool lifecycleSucceeded{};
    bool loading{};
    bool messagingAvailable{};
    bool taskCollectionAvailable{};
    bool collectorFailed{};
    bool rendererInitialized{};
    bool rendererBorderless{};
    bool rendererVsync{};

    // Opaque identities carry no ownership and must not be dereferenced.
    std::uint64_t rendererDeviceIdentity{};
    std::uint64_t rendererContextIdentity{};
};

static_assert(std::is_trivially_copyable_v<GameHdrSnapshot>);

struct GraphicsLifecyclePublication
{
    bool gameplayReady{};
    std::uint64_t resetEpoch{};
};

// One copied publication; it conveys no game pointers or render-context ownership.
[[nodiscard]] GraphicsLifecyclePublication ReadGraphicsLifecycle() noexcept;

}  // namespace fo4hdr
