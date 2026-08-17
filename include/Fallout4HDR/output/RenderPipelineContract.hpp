// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace fo4hdr {

enum class NativeUiShaderRole : std::uint8_t
{
    unknown,
    hudComposite,
    gammaBackdrop
};

struct NativeUiShaderFact
{
    NativeUiShaderRole role{};
    std::array<std::uint32_t, 4> dxbcHash{};
};

// Exact Fallout 4 1.10.163 pixel-shader identities. HUD input is straight sRGB
// R8 RGBA at presentation extent; backdrop is SDR code contribution plus coverage.
inline constexpr std::array<NativeUiShaderFact, 2> kNativeUiShaders{{
    {NativeUiShaderRole::hudComposite, {0x525eaea9u, 0xaf037d1bu, 0x3ca0d381u, 0x8531a8b3u}},
    {NativeUiShaderRole::gammaBackdrop, {0xb939134cu, 0xc0f28bacu, 0x1a7e28c1u, 0xf4aff2feu}}}};

[[nodiscard]] constexpr NativeUiShaderRole FindNativeUiShaderRole(
    const std::array<std::uint32_t, 4>& dxbcHash) noexcept
{
    for (const auto& fact : kNativeUiShaders)
        if (fact.dxbcHash == dxbcHash) return fact.role;
    return NativeUiShaderRole::unknown;
}

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
        if (descriptor.crc32 == crc32)
        {
            return &descriptor;
        }
    }
    return nullptr;
}

// Required and optional roles need independent qualification before an unknown lookup.
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

}  // namespace fo4hdr
