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
    GameLifecycleEvent lifecycleEvent{};
    std::uint32_t collectorThreadId{};
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

}  // namespace fo4hdr
