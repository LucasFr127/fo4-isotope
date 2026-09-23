// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fo4hdr {

enum class RenoDxShaderPass : std::uint8_t
{
    toneMapA,
    toneMapB,
    toneMapNewA,
    toneMapNewB,
    lut,
    taa,
    taa2,
    fxaa,
    video,
    dofAuxiliary1,
    dofAuxiliary2,
    dofAuxiliary3,
    dofBlender,
    gameDof,
    vignette
};

enum class PipelinePassGroup : std::uint8_t
{
    toneMapFamily,
    creativeGrade,
    temporalAa,
    postEffect
};

enum class PipelinePassRequirement : std::uint8_t
{
    untracked,
    optional,
    required
};

enum class UnknownShaderOutcome : std::uint8_t
{
    passThrough,
    disableOptionalFeature,
    disableHdrGeneration
};

// Plain-data domain tag tied to the existing renderer resource generation.
// It prevents raw SDR codes, developer BT.709, and production BT.2020 proxy
// values from being interpreted through the wrong output branch.
enum class LinearColorDomain : std::uint8_t
{
    unknown,
    vanillaSdrCodes,
    linearBt709Relative,
    linearBt2020UiRelative
};

struct WideGamutResourceReadiness
{
    bool replacementShaderSet{};
    bool presentationProxySet{};
    bool compositionSet{};
    bool uiSet{};
    bool outputSet{};

    [[nodiscard]] constexpr bool Complete() const noexcept
    {
        return replacementShaderSet && presentationProxySet &&
            compositionSet && uiSet && outputSet;
    }
};

[[nodiscard]] constexpr bool IsWideGamutGenerationReady(
    const WideGamutResourceReadiness& readiness,
    std::uint64_t colorDomainGeneration,
    std::uint64_t resourceGeneration) noexcept
{
    return readiness.Complete() && colorDomainGeneration != 0 &&
        colorDomainGeneration == resourceGeneration;
}

enum class SdrHandoffAction : std::uint8_t
{
    none,
    copyVanillaSdr,
    clearBlack,
    clearBlackAndDisable
};

[[nodiscard]] constexpr SdrHandoffAction DecideSdrHandoff(
    bool hdrRendered,
    bool frameWasRedirected,
    LinearColorDomain frameDomain,
    bool proxyStillAvailable) noexcept
{
    if (hdrRendered || !frameWasRedirected) return SdrHandoffAction::none;
    if (frameDomain == LinearColorDomain::unknown)
        return SdrHandoffAction::clearBlackAndDisable;
    if (frameDomain == LinearColorDomain::vanillaSdrCodes &&
        proxyStillAvailable)
        return SdrHandoffAction::copyVanillaSdr;
    return SdrHandoffAction::clearBlack;
}

struct RenoDxShaderDescriptor
{
    RenoDxShaderPass pass{};
    std::uint32_t crc32{};
    std::string_view name;
    PipelinePassGroup group{};
    PipelinePassRequirement requirement{};
    std::uint8_t sceneInputSlot{};
};

inline constexpr std::array<RenoDxShaderDescriptor, 15>
    kRenoDxFallout4Shaders{{
        {RenoDxShaderPass::toneMapA, 0x8024E8B5u, "tonemap-a",
         PipelinePassGroup::toneMapFamily,
         PipelinePassRequirement::required, 1},
        {RenoDxShaderPass::toneMapB, 0x676B8B5Du, "tonemap-b",
         PipelinePassGroup::toneMapFamily,
         PipelinePassRequirement::required, 1},
        {RenoDxShaderPass::toneMapNewA, 0x67685D89u, "tonemap-new-a",
         PipelinePassGroup::toneMapFamily,
         PipelinePassRequirement::required, 0},
        {RenoDxShaderPass::toneMapNewB, 0xFEBC673Cu, "tonemap-new-b",
         PipelinePassGroup::toneMapFamily,
         PipelinePassRequirement::required, 0},
        {RenoDxShaderPass::lut, 0x80802E60u, "lut",
         PipelinePassGroup::creativeGrade,
         PipelinePassRequirement::required, 0},
        {RenoDxShaderPass::taa, 0x8CAC3BD9u, "taa",
         PipelinePassGroup::temporalAa,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::taa2, 0x61CC29E6u, "taa-2",
         PipelinePassGroup::temporalAa,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::fxaa, 0x4FB4DA20u, "fxaa",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::video, 0x0F2CC0D1u, "video",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::dofAuxiliary1, 0x7543CFBCu, "dof-auxiliary-1",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::dofAuxiliary2, 0x5E75012Au, "dof-auxiliary-2",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::dofAuxiliary3, 0xC7AD6297u, "dof-auxiliary-3",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::dofBlender, 0x8F009507u, "dof-blender",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::gameDof, 0x83660755u, "game-dof",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0},
        {RenoDxShaderPass::vignette, 0xB74B05F4u, "vignette",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0}
    }};

[[nodiscard]] constexpr const RenoDxShaderDescriptor*
FindRenoDxFallout4Shader(std::uint32_t crc32) noexcept
{
    for (const auto& descriptor : kRenoDxFallout4Shaders)
    {
        if (descriptor.crc32 == crc32) return &descriptor;
    }
    return nullptr;
}

[[nodiscard]] constexpr UnknownShaderOutcome DecideUnknownShaderOutcome(
    PipelinePassRequirement requirement) noexcept
{
    switch (requirement)
    {
    case PipelinePassRequirement::untracked:
        return UnknownShaderOutcome::passThrough;
    case PipelinePassRequirement::optional:
        return UnknownShaderOutcome::disableOptionalFeature;
    case PipelinePassRequirement::required:
        return UnknownShaderOutcome::disableHdrGeneration;
    }
    return UnknownShaderOutcome::disableHdrGeneration;
}

struct PipelineHeartbeatEvaluation
{
    bool currentGeneration{};
    bool currentFrame{};
    bool toneMapFamilyObserved{};
    bool lutObserved{};
    bool ordered{};
    enum class Outcome : std::uint8_t
    {
        ready,
        incomplete,
        staleGeneration,
        staleFrame
    } outcome{Outcome::incomplete};
    bool ready{};
};

enum class PipelineHeartbeatAction : std::uint8_t
{
    renderHdr,
    waitInSdr,
    disableGeneration
};

enum class TemporalSceneAction : std::uint8_t
{
    ignoreUnqualifiedBranch,
    acceptProxy,
    rejectMissingProxy
};

enum class LateGameUiAction : std::uint8_t
{
    passThrough,
    captureSeparately
};

struct LateGameUiFrame
{
    bool hdrPathRequested{};
    bool referenceHealthy{};
    bool gameplayReady{};
    bool productionOperatorApplied{};
    bool qualifiedSceneSelected{};
    bool exactSinglePresentationTarget{};

    [[nodiscard]] constexpr bool Ready() const noexcept
    {
        return hdrPathRequested && referenceHealthy && gameplayReady &&
            productionOperatorApplied && qualifiedSceneSelected && exactSinglePresentationTarget;
    }
};

struct LateGameUiBackdrop
{
    bool allowlistedBackdropShader{};
    bool exactIndexedQuad{};
    bool texture2dMaskSource{};
    bool depthDetached{};

    [[nodiscard]] constexpr bool Qualified() const noexcept
    {
        return allowlistedBackdropShader && exactIndexedQuad && texture2dMaskSource && depthDetached;
    }
};

[[nodiscard]] constexpr LateGameUiAction DecideLateGameUiAction(
    const LateGameUiFrame& frame,
    bool allowlistedUiShader,
    bool fullResolutionSdrUiSource) noexcept
{
    return frame.Ready() && allowlistedUiShader && fullResolutionSdrUiSource ?
        LateGameUiAction::captureSeparately :
        LateGameUiAction::passThrough;
}

[[nodiscard]] constexpr LateGameUiAction DecideLateGameUiBackdropAction(
    const LateGameUiFrame& frame, const LateGameUiBackdrop& backdrop) noexcept
{
    return frame.Ready() && backdrop.Qualified() ?
        LateGameUiAction::captureSeparately :
        LateGameUiAction::passThrough;
}

[[nodiscard]] constexpr PipelineHeartbeatAction DecidePipelineHeartbeatAction(
    const PipelineHeartbeatEvaluation& evaluation) noexcept
{
    switch (evaluation.outcome)
    {
    case PipelineHeartbeatEvaluation::Outcome::ready:
        return PipelineHeartbeatAction::renderHdr;
    case PipelineHeartbeatEvaluation::Outcome::incomplete:
        return PipelineHeartbeatAction::waitInSdr;
    case PipelineHeartbeatEvaluation::Outcome::staleGeneration:
    case PipelineHeartbeatEvaluation::Outcome::staleFrame:
        return PipelineHeartbeatAction::disableGeneration;
    }
    return PipelineHeartbeatAction::disableGeneration;
}

struct TemporalSceneEvidence
{
    bool qualifiedSceneSelected{};
    bool consumesSelectedScene{};
    bool presentationTarget{};
    bool exactProxyReady{};

    [[nodiscard]] constexpr bool Qualified() const noexcept
    {
        return qualifiedSceneSelected && consumesSelectedScene && presentationTarget;
    }
};

[[nodiscard]] constexpr TemporalSceneAction DecideTemporalSceneAction(
    const PipelineHeartbeatEvaluation& evaluation,
    const TemporalSceneEvidence& evidence) noexcept
{
    if (!evaluation.ready ||
        evaluation.outcome != PipelineHeartbeatEvaluation::Outcome::ready)
    {
        return TemporalSceneAction::ignoreUnqualifiedBranch;
    }
    if (!evidence.Qualified()) return TemporalSceneAction::ignoreUnqualifiedBranch;
    return evidence.exactProxyReady ? TemporalSceneAction::acceptProxy
                                   : TemporalSceneAction::rejectMissingProxy;
}

class PipelineHeartbeat
{
public:
    void Reset(std::uint64_t generation) noexcept;
    void BeginFrame(std::uint64_t generation, std::uint64_t frame) noexcept;
    void Observe(RenoDxShaderPass pass) noexcept;
    [[nodiscard]] PipelineHeartbeatEvaluation Evaluate(
        std::uint64_t generation,
        std::uint64_t frame) const noexcept;

private:
    std::uint64_t generation_{};
    std::uint64_t frame_{};
    bool toneMapFamilyObserved_{};
    bool lutObserved_{};
    bool orderedPairObserved_{};
};

[[nodiscard]] constexpr std::uint32_t RenoDxShaderBit(
    RenoDxShaderPass pass) noexcept
{
    return std::uint32_t{1} << static_cast<std::uint8_t>(pass);
}

}  // namespace fo4hdr
