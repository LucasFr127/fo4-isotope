// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@68d2ab15e6086ab04084278ee8cceb85b8a8023e/src/games/fallout4/12_luts_0x80802E60.ps_5_0.hlsl
// Modified Fallout4HDR adaptation: retain four weighted native LUT samples
// through a project-authored radial extension in linear scene units.
// See THIRD_PARTY_NOTICES.md.

#include "GradeTransfer.hlsli"
#include "../tonemap/OperatorLut.hlsli"
#include "ImageControls.hlsli"
#include "SelectiveColor.hlsli"

Texture2D<float4> SceneTexture : register(t0);
Texture3D<float4> Lut0 : register(t3);
Texture3D<float4> Lut1 : register(t4);
Texture3D<float4> Lut2 : register(t5);
Texture3D<float4> Lut3 : register(t6);

SamplerState SceneSampler : register(s0);
SamplerState Lut0Sampler : register(s3);
SamplerState Lut1Sampler : register(s4);
SamplerState Lut2Sampler : register(s5);
SamplerState Lut3Sampler : register(s6);

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
    // Exposure is already applied; this seam preserves fixed 203-nit scene units.
    const float3 scene = Fo4HdrGradeCleanScene(sceneSample.rgb);
    float3 result = scene;
    [branch]
    if (VanillaColorGradingStrength > 0.0f)
        result = Fo4HdrTransferRadialGrade(scene, Fo4HdrSampleWeightedGrade(scene),
            VanillaColorGradingStrength);
    float luminanceRemoval;
    float colorRemoval;
    Fo4HdrResolveVanillaGradeRemoval(cb2[1], Fo4HdrLutProfilesActive,
        VanillaGradeLuminanceRemoval, VanillaGradeColorRemoval,
        Fo4HdrLutProfileLuminanceRemoval, Fo4HdrLutProfileColorRemoval,
        luminanceRemoval, colorRemoval);
    result = Fo4HdrApplyVanillaGradeRemoval(scene, result,
        luminanceRemoval, colorRemoval);
    result = Fo4HdrApplyImageControls(result);
    result = Fo4HdrApplySelectiveColor(result);
    [branch]
    if (OperatorMode != 0u)
        result = Fo4HdrSampleOperatorLut(result) / OperatorUiWhiteNits;
    output = float4(result, sceneSample.a);
}
