// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/12_luts_0x80802E60.ps_5_0.hlsl
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
// Adapted for FO4Isotope from that shader and
// src/shaders/tonemap.hlsl. Modification: retain Fallout 4's four weighted LUT
// look through a project-owned radial HDR extension, and insert the selected
// production operator exactly once before TAA. Apply a project-authored
// grade-strength blend and BT.709 image controls
// before that operator. ACES 2 LMT, AgX, and GT7 use a device-scoped
// immutable FP16 operator LUT generated outside the hot path. Every operator
// publishes absolute linear BT.2020 and uses UI-white-relative BT.2020
// transport so later native backbuffer UI may retain its own
// paper white while temporal history keeps fixed scene units.
// See THIRD_PARTY_NOTICES.md.
// Direct Academy ACES 2 remains an internal component of the composed LMT
// source response; only the three production operators are selectable.

#include "../tonemap/OperatorLut.hlsli"
#include "LiveSettingsConstants.hlsli"
#include "SelectiveColor.hlsli"
#include "ImageControls.hlsli"
#include "GradeTransfer.hlsli"

Texture2D<float4> SceneTexture : register(t0);
Texture3D<float4> Lut0 : register(t3);
Texture3D<float4> Lut1 : register(t4);
Texture3D<float4> Lut2 : register(t5);
Texture3D<float4> Lut3 : register(t6);
Texture3D<float4> OperatorLut : register(t7);

SamplerState SceneSampler : register(s0);
SamplerState Lut0Sampler : register(s3);
SamplerState Lut1Sampler : register(s4);
SamplerState Lut2Sampler : register(s5);
SamplerState Lut3Sampler : register(s6);
SamplerState OperatorLutSampler : register(s7);

cbuffer Fallout4LutConstants : register(b2)
{
    float4 cb2[2];
}

float3 Fo4HdrSampleWeightedGrade(float3 scene)
{
    const float3 coordinate = (15.0f * Fo4HdrRadialCoordinates(scene) + 0.5f) / 16.0f;
    const float4x3 samples = float4x3(
        Lut0.SampleLevel(Lut0Sampler, coordinate, 0).rgb,
        Lut1.SampleLevel(Lut1Sampler, coordinate, 0).rgb,
        Lut2.SampleLevel(Lut2Sampler, coordinate, 0).rgb,
        Lut3.SampleLevel(Lut3Sampler, coordinate, 0).rgb);
    return mul(cb2[1], samples);
}

void main(
    float4 position : SV_POSITION0,
    float2 texcoord : TEXCOORD0,
    out float4 output : SV_Target0)
{
    const float4 sceneSample = SceneTexture.Sample(SceneSampler, texcoord);
    // Scene exposure was already applied by the qualified tone-map-family
    // replacement, before Fallout 4's later native HUD draws. Keeping this
    // seam exposure-neutral prevents the LUT/operator from dimming those UI
    // pixels a second time. The radial guide bounds only the LUT lookup.
    const float3 hdrLinear = Fo4HdrGradeCleanScene(sceneSample.rgb);
    float3 result = hdrLinear;
    if (Fo4HdrVanillaGradeStrength > 0.0f)
    {
        result = Fo4HdrTransferRadialGrade(hdrLinear,
            Fo4HdrSampleWeightedGrade(hdrLinear), Fo4HdrVanillaGradeStrength);
    }
    float luminanceRemoval;
    float colorRemoval;
    Fo4HdrResolveVanillaGradeRemoval(cb2[1], Fo4HdrLutProfilesActive,
        Fo4HdrVanillaGradeLuminanceRemoval,
        Fo4HdrVanillaGradeColorRemoval,
        Fo4HdrLutProfileLuminanceRemoval,
        Fo4HdrLutProfileColorRemoval, luminanceRemoval, colorRemoval);
    result = Fo4HdrApplyVanillaGradeRemoval(
        hdrLinear, result, luminanceRemoval, colorRemoval);
    result = Fo4HdrApplyImageControls(result);
    result = Fo4HdrApplySelectiveColor(result);
    // Modes 1..3 are ACES2LMT, AgX, and GT7. They share the t7/s7 LUT seam.
    if (Fo4HdrProductionMode >= 1u && Fo4HdrProductionMode <= 3u)
    {
        const float3 absoluteBt2020Nits = Fo4HdrSampleOperatorLut(
            OperatorLut, OperatorLutSampler, result,
            Fo4HdrProductionMode,
            Fo4HdrOperatorShaperAngleGain,
            Fo4HdrOperatorShaperBias, Fo4HdrDisplayPeakNits,
            Fo4HdrOperatorShaperParameters);
        // Match RenoDX's UI-relative intermediate. The temporal replacements
        // locally undo this scale before using their fixed scene-white history.
        result = absoluteBt2020Nits / Fo4HdrUiTransportWhiteNits;
    }
    output = float4(result, sceneSample.a);
}
