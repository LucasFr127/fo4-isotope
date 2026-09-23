// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Fallout4HDR/game/ImageSpaceSemantic.hpp"

namespace fo4hdr {

enum class UiContextFlag : std::uint32_t
{
    None = 0,
    Gameplay = 1u << 0,
    Hud = 1u << 1,
    Subtitles = 1u << 2,
    PipBoy = 1u << 3,
    PowerArmor = 1u << 4,
    Pause = 1u << 5,
    Loading = 1u << 6,
    MainMenu = 1u << 7,
    Dialogue = 1u << 8,
    Workshop = 1u << 9,
    Terminal = 1u << 10,
    Barter = 1u << 11,
    Container = 1u << 12,
    Cursor = 1u << 13,
    Console = 1u << 14,
    Scope = 1u << 15,
    Vats = 1u << 16,
    Favorites = 1u << 17,
    MessageBox = 1u << 18,
    Lockpicking = 1u << 19
};

enum class GameSnapshotFeature : std::uint32_t
{
    None = 0,
    Messaging = 1u << 0,
    ImageSpace = 1u << 1,
    Renderer = 1u << 2,
    RenderTargets = 1u << 3,
    Ui = 1u << 4,
    Lifecycle = 1u << 6
};

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
    GameDataReady,
    MenuChanged
};

[[nodiscard]] constexpr UiContextFlag operator|(
    UiContextFlag left,
    UiContextFlag right) noexcept
{
    return static_cast<UiContextFlag>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool HasUiContext(
    UiContextFlag value,
    UiContextFlag flag) noexcept
{
    return (
        static_cast<std::uint32_t>(value) &
        static_cast<std::uint32_t>(flag)
    ) != 0;
}

[[nodiscard]] constexpr GameSnapshotFeature operator|(
    GameSnapshotFeature left,
    GameSnapshotFeature right) noexcept
{
    return static_cast<GameSnapshotFeature>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

constexpr GameSnapshotFeature& operator|=(
    GameSnapshotFeature& left,
    GameSnapshotFeature right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasGameSnapshotFeature(
    GameSnapshotFeature value,
    GameSnapshotFeature feature) noexcept
{
    return (
        static_cast<std::uint32_t>(value) &
        static_cast<std::uint32_t>(feature)
    ) != 0;
}

struct ImageSpaceEffectSnapshot
{
    std::uint64_t objectIdentity{};
    std::uint16_t declaredIndex{};
    bool present{};
    bool active{};
};

struct EngineRenderTargetSnapshot
{
    std::uint64_t textureIdentity{};
    std::uint64_t renderTargetViewIdentity{};
    std::uint64_t shaderResourceViewIdentity{};
    std::uint64_t unorderedAccessViewIdentity{};
    std::uint32_t arrayIndex{};
    std::uint32_t engineId{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t formatValue{};
    std::uint32_t multiSample{};
    bool resourcePresent{};
    bool propertiesPresent{};
    bool metadataCorrelationValidated{};
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

struct LutSnapshot
{
    // UTF-8, forward-slash-normalized asset path. pathLength excludes the
    // terminating null. Unused bytes are zero.
    std::array<char, 260> utf8Path{};
    std::uint16_t pathLength{};
    std::uint16_t reserved{};

    float weight{};

    // Engine identity plus resource generation prevents pointer reuse from
    // being mistaken for the same texture after renderer resets.
    std::uint64_t engineTextureId{};
    std::uint64_t resourceGeneration{};
};

struct MenuTransitionPairSnapshot
{
    std::uint64_t openSequence{};
    std::uint64_t closeSequence{};
};

struct GameHdrSnapshot
{
    std::uint64_t generation{};
    std::uint64_t frameId{};
    std::uint64_t resourceGeneration{};
    std::uint64_t lifecycleGeneration{};
    std::uint64_t observationGeneration{};

    GameSnapshotFeature availableFeatures{GameSnapshotFeature::None};
    GameSnapshotFeature failedFeatures{GameSnapshotFeature::None};
    GameLifecycleEvent lifecycleEvent{GameLifecycleEvent::None};

    UiContextFlag uiContexts{UiContextFlag::None};

    bool loading{};
    bool interior{};
    bool underwater{};
    bool imageSpaceOverride{};
    bool holdExposure{};
    bool taaEnabled{};
    bool dynamicResolutionEnabled{};
    bool rendererInitialized{};
    bool rendererBorderless{};
    bool rendererVsync{};
    bool lastMenuOpening{};
    bool menuHistoryComplete{true};
    bool menuHistoryCaughtUp{true};
    bool menuStateSynchronized{};
    bool menuTransitionWitnessValid{true};

    ImageSpaceActiveData activeImageSpace{};

    float dynamicWidthRatio{1.0F};
    float dynamicHeightRatio{1.0F};
    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};

    std::uint64_t rendererDeviceIdentity{};
    std::uint64_t rendererContextIdentity{};
    std::uint64_t imageSpaceIdentity{};
    std::uint64_t baseImageSpaceIdentity{};
    std::uint64_t overrideImageSpaceIdentity{};
    std::uint64_t underwaterImageSpaceIdentity{};
    std::uint64_t consoleImageSpaceIdentity{};
    std::uint32_t rendererWindowCount{};
    std::uint32_t populatedRenderTargetCount{};
    std::uint32_t populatedDepthTargetCount{};
    std::uint32_t populatedCubeTargetCount{};
    std::uint32_t imageSpaceEffectCount{};

    std::array<char, 64> lastMenuName{};

    std::array<ImageSpaceEffectSnapshot, 72> imageSpaceEffects{};
    std::array<RendererWindowSnapshot, 32> rendererWindows{};
    std::array<EngineRenderTargetSnapshot, 101> renderTargets{};

    std::array<LutSnapshot, 4> luts{};
    std::uint32_t activeLutCount{};
    std::uint64_t lastMenuSequence{};
    std::uint64_t menuEventsDropped{};
    MenuTransitionPairSnapshot pauseMenuPair{};
    MenuTransitionPairSnapshot pipBoyMenuPair{};
};

constexpr void SetGameSnapshotFeatureStatus(
    GameSnapshotFeature& present,
    GameSnapshotFeature& absent,
    GameSnapshotFeature feature) noexcept
{
    const auto featureBits = static_cast<std::uint32_t>(feature);
    present = static_cast<GameSnapshotFeature>(
        static_cast<std::uint32_t>(present) | featureBits);
    absent = static_cast<GameSnapshotFeature>(
        static_cast<std::uint32_t>(absent) & ~featureBits);
}

constexpr void MarkGameSnapshotFeatureAvailable(
    GameHdrSnapshot& snapshot,
    GameSnapshotFeature feature) noexcept
{
    SetGameSnapshotFeatureStatus(snapshot.availableFeatures, snapshot.failedFeatures, feature);
}

constexpr void MarkGameSnapshotFeatureFailed(
    GameHdrSnapshot& snapshot,
    GameSnapshotFeature feature) noexcept
{
    SetGameSnapshotFeatureStatus(snapshot.failedFeatures, snapshot.availableFeatures, feature);
}

static_assert(std::is_trivially_copyable_v<LutSnapshot>);
static_assert(std::is_trivially_copyable_v<ImageSpaceEffectSnapshot>);
static_assert(std::is_trivially_copyable_v<EngineRenderTargetSnapshot>);
static_assert(std::is_trivially_copyable_v<RendererWindowSnapshot>);
static_assert(std::is_trivially_copyable_v<MenuTransitionPairSnapshot>);
static_assert(std::is_trivially_copyable_v<GameHdrSnapshot>);

[[nodiscard]] constexpr bool HasValidLutCount(
    const GameHdrSnapshot& snapshot) noexcept
{
    return snapshot.activeLutCount <= snapshot.luts.size();
}

}  // namespace fo4hdr
