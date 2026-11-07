// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    linearBt2020UiRelative,
    encodedSrgbBt709UiRelative,
    encodedGamma22Bt709UiRelative
};

struct WideGamutResourceReadiness
{
    bool replacementShaderSet{};
    bool presentationProxySet{};
    bool compositionSet{};
    bool outputSet{};

    [[nodiscard]] constexpr bool Complete() const noexcept
    {
        return replacementShaderSet && presentationProxySet &&
            compositionSet && outputSet;
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
    clearBlackAndDisable,
    convertPublicImage
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
    std::uint8_t texture2dInputMask{};
};

inline constexpr std::array<RenoDxShaderDescriptor, 15>
    kRenoDxFallout4Shaders{{
        {RenoDxShaderPass::toneMapA, 0x8024E8B5u, "tonemap-a",
         PipelinePassGroup::toneMapFamily,
         PipelinePassRequirement::required, 1, 0x0F},
        {RenoDxShaderPass::toneMapB, 0x676B8B5Du, "tonemap-b",
         PipelinePassGroup::toneMapFamily,
         PipelinePassRequirement::required, 1, 0x0F},
        {RenoDxShaderPass::toneMapNewA, 0x67685D89u, "tonemap-new-a",
         PipelinePassGroup::toneMapFamily,
         PipelinePassRequirement::required, 0, 0x0F},
        {RenoDxShaderPass::toneMapNewB, 0xFEBC673Cu, "tonemap-new-b",
         PipelinePassGroup::toneMapFamily,
         PipelinePassRequirement::required, 0, 0x0F},
        {RenoDxShaderPass::lut, 0x80802E60u, "lut",
         PipelinePassGroup::creativeGrade,
         PipelinePassRequirement::required, 0, 0x01},
        {RenoDxShaderPass::taa, 0x8CAC3BD9u, "taa",
         PipelinePassGroup::temporalAa,
         PipelinePassRequirement::optional, 0, 0x0F},
        {RenoDxShaderPass::taa2, 0x61CC29E6u, "taa-2",
         PipelinePassGroup::temporalAa,
         PipelinePassRequirement::optional, 0, 0x1F},
        {RenoDxShaderPass::fxaa, 0x4FB4DA20u, "fxaa",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0, 0x01},
        {RenoDxShaderPass::video, 0x0F2CC0D1u, "video",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0, 0x01},
        {RenoDxShaderPass::dofAuxiliary1, 0x7543CFBCu, "dof-auxiliary-1",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0, 0x01},
        {RenoDxShaderPass::dofAuxiliary2, 0x5E75012Au, "dof-auxiliary-2",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0, 0x01},
        {RenoDxShaderPass::dofAuxiliary3, 0xC7AD6297u, "dof-auxiliary-3",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0, 0x05},
        {RenoDxShaderPass::dofBlender, 0x8F009507u, "dof-blender",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0, 0x07},
        {RenoDxShaderPass::gameDof, 0x83660755u, "game-dof",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0, 0x07},
        {RenoDxShaderPass::vignette, 0xB74B05F4u, "vignette",
         PipelinePassGroup::postEffect,
         PipelinePassRequirement::optional, 0, 0x03}
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

// The reversible positive-HDR adapter keeps Fallout 4's temporal body in its
// original normalized gamma domain without selecting a display peak or
// production tone mapper. The shipping HLSL uses the same equations.
[[nodiscard]] double EncodeTemporalIntermediate(double linear) noexcept;
[[nodiscard]] double DecodeTemporalIntermediate(double encoded) noexcept;

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
    rejectMissingProxy,
    acceptInternalScene
};

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
    bool internalSceneReady{};

    [[nodiscard]] constexpr bool InputQualified() const noexcept
    {
        return qualifiedSceneSelected && consumesSelectedScene;
    }
};

[[nodiscard]] constexpr TemporalSceneAction DecideTemporalSceneAction(
    const PipelineHeartbeatEvaluation& evaluation,
    const TemporalSceneEvidence& evidence) noexcept
{
    if (DecidePipelineHeartbeatAction(evaluation) != PipelineHeartbeatAction::renderHdr)
    {
        return TemporalSceneAction::ignoreUnqualifiedBranch;
    }
    if (!evidence.InputQualified()) return TemporalSceneAction::ignoreUnqualifiedBranch;
    if (evidence.presentationTarget)
        return evidence.exactProxyReady ? TemporalSceneAction::acceptProxy
                                       : TemporalSceneAction::rejectMissingProxy;
    return evidence.internalSceneReady ? TemporalSceneAction::acceptInternalScene
                                       : TemporalSceneAction::ignoreUnqualifiedBranch;
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

// LUT, TAA and FXAA replacements wait for the frame's identified tone map, so a missed
// tone map leaves the game's own SDR chain intact. Once a tone-map replacement has fed
// the frame, that chain no longer holds the game's codes and the replacements stay.
[[nodiscard]] constexpr bool SceneReplacementAllowed(
    RenoDxShaderPass pass, const PipelineHeartbeatEvaluation& heartbeat) noexcept
{
    constexpr auto chain = RenoDxShaderBit(RenoDxShaderPass::lut) | RenoDxShaderBit(RenoDxShaderPass::taa) |
        RenoDxShaderBit(RenoDxShaderPass::taa2) | RenoDxShaderBit(RenoDxShaderPass::fxaa);
    if ((RenoDxShaderBit(pass) & chain) == 0) return true;
    const bool current = heartbeat.currentGeneration && heartbeat.currentFrame;
    return current && heartbeat.toneMapFamilyObserved;
}

// Engine image-space effect whose Render call scopes a pixel-shader bind,
// identifying a recompiled shader that matches no RenoDX identity.
enum class NativeImageSpacePass : std::uint8_t
{
    none,
    toneMapCinematic,
    toneMapCinematicFade,
    lut,
    temporalAa,
    temporalAaPipboy,
    temporalAaPowerArmorPipboy,
    bokehDofBlend,
    distantBlur,
    fxaa
};

// A scoped foreign shader joins only a whole stock set registered by the
// device: the LUT and exactly one tone-map family, with no member of the other.
// Returns that family's offset in the pass table below.
[[nodiscard]] constexpr std::optional<std::size_t> RegisteredStockFamily(std::uint32_t registeredPasses) noexcept
{
    using enum RenoDxShaderPass;
    constexpr auto original = RenoDxShaderBit(toneMapA) | RenoDxShaderBit(toneMapB) | RenoDxShaderBit(lut);
    constexpr auto updated = RenoDxShaderBit(toneMapNewA) | RenoDxShaderBit(toneMapNewB) | RenoDxShaderBit(lut);
    const auto stock = registeredPasses & (original | updated);
    if (stock == original) return 0;
    if (stock == updated) return 3;
    return std::nullopt;
}

// A temporal, depth-of-field or FXAA scope takes its effect's one pass, but only while the
// device registered that pass's stock shader, which fixes the replacement's register layout.
[[nodiscard]] constexpr std::optional<RenoDxShaderPass> ResolveEffectStockPass(
    NativeImageSpacePass pass, std::uint32_t registeredPasses) noexcept
{
    using enum RenoDxShaderPass;
    constexpr std::array<RenoDxShaderPass, 6> passes{taa2, taa2, taa, dofBlender, gameDof, fxaa};
    const auto index = static_cast<std::size_t>(pass) - static_cast<std::size_t>(NativeImageSpacePass::temporalAa);
    if (index >= passes.size() || (registeredPasses & RenoDxShaderBit(passes[index])) == 0) return std::nullopt;
    return passes[index];
}

[[nodiscard]] constexpr std::optional<RenoDxShaderPass> ResolveNativeImageSpacePass(
    NativeImageSpacePass pass, std::uint32_t registeredPasses) noexcept
{
    using enum RenoDxShaderPass;
    if (pass > NativeImageSpacePass::lut) return ResolveEffectStockPass(pass, registeredPasses);
    if (pass == NativeImageSpacePass::none) return std::nullopt;
    const auto family = RegisteredStockFamily(registeredPasses);
    if (!family) return std::nullopt;
    constexpr std::array<RenoDxShaderPass, 6> passes{toneMapA, toneMapB, lut, toneMapNewA, toneMapNewB, lut};
    return passes[*family + static_cast<std::size_t>(pass) - 1U];
}

}  // namespace fo4hdr
