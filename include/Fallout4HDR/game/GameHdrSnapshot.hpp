// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include <array>
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

struct RendererWindowSnapshot
{
    std::uint64_t windowIdentity{};
    std::uint64_t swapChainIdentity{};
    std::uint32_t arrayIndex{};
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};
    bool present{};
};

struct EngineRenderTargetSnapshot
{
    std::uint64_t textureIdentity{};
    std::uint64_t renderTargetViewIdentity{};
    std::uint64_t shaderResourceViewIdentity{};
    std::uint64_t unorderedAccessViewIdentity{};
    std::uint32_t arrayIndex{};
    bool resourcePresent{};
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
    bool rendererObserved{};
    bool renderTargetManagerAvailable{};
    std::uint32_t rendererWindowCount{};
    std::uint32_t populatedRenderTargetCount{};
    std::uint32_t populatedDepthTargetCount{};
    std::uint32_t populatedCubeTargetCount{};
    // Manager properties are not correlated with resource slots; these remain unobserved.
    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};
    float dynamicWidthRatio{};
    float dynamicHeightRatio{};
    bool dynamicResolutionEnabled{};
    std::array<RendererWindowSnapshot, 32> rendererWindows{};
    std::array<EngineRenderTargetSnapshot, 101> renderTargets{};
    ImageSpaceSnapshot imageSpace{};
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
