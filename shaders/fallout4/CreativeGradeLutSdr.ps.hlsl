// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/12_luts_0x80802E60.ps_5_0.hlsl
// Modified FO4Isotope SDR-mode adaptation of RenoDX Fallout 4 shader
// 0x80802E60: keep the game's four weighted 16-cube lookup and display-code
// output, and apply the project grade strength and luminance/color removal in
// gamma-2.2 display light. See THIRD_PARTY_NOTICES.md.

#include "LiveSettingsConstants.hlsli"
#include "GradeTransfer.hlsli"

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

void main(
    float4 position : SV_POSITION0,
    float2 texcoord : TEXCOORD0,
    out float4 output : SV_Target0)
{
    const float4 sceneSample = SceneTexture.Sample(SceneSampler, texcoord);
    // The curve's display-referred output, bounded at white as the lookup is.
    const float3 display = saturate(sceneSample.rgb);
    const float3 coordinate = (15.0f * pow(display, 1.0f / 2.2f) + 0.5f) / 16.0f;
    const float4x3 samples = float4x3(
        Lut0.SampleLevel(Lut0Sampler, coordinate, 0).rgb,
        Lut1.SampleLevel(Lut1Sampler, coordinate, 0).rgb,
        Lut2.SampleLevel(Lut2Sampler, coordinate, 0).rgb,
        Lut3.SampleLevel(Lut3Sampler, coordinate, 0).rgb);
    const float3 codes = mul(cb2[1], samples);
    float luminanceRemoval;
    float colorRemoval;
    Fo4HdrResolveVanillaGradeRemoval(cb2[1], Fo4HdrLutProfilesActive,
        Fo4HdrFallbackGradeLuminanceRemoval,
        Fo4HdrFallbackGradeColorRemoval,
        Fo4HdrLutProfileLuminanceRemoval,
        Fo4HdrLutProfileColorRemoval, luminanceRemoval, colorRemoval);
    const float strength = (asuint(Fo4HdrVanillaGradeStrength) & 0x7fffffffu) > 0x7f800000u
        ? 0.0f : saturate(Fo4HdrVanillaGradeStrength);
    // Full strength without removal returns the game's codes exactly.
    float3 result = codes;
    [branch] if (strength < 1.0f || luminanceRemoval > 0.0f || colorRemoval > 0.0f)
    {
        const float3 graded = lerp(display, Fo4HdrGradeDecode(
            Fo4HdrGradeCleanScene(codes), Fo4HdrGradeCurveGamma22), strength);
        result = Fo4HdrGradeEncode(Fo4HdrApplyVanillaGradeRemoval(
            display, graded, luminanceRemoval, colorRemoval), Fo4HdrGradeCurveGamma22);
    }
    output = float4(result, sceneSample.a);
}
