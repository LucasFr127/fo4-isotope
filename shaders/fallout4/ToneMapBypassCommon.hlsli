// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@68d2ab15e6086ab04084278ee8cceb85b8a8023e/src/games/fallout4/shared.h
// Modified Fallout4HDR adaptation of the Fallout 4 tone-map family:
// retain native exposure, bloom, raw mask and affine scene filtering while
// bypassing normalized range compression. See THIRD_PARTY_NOTICES.md.
#ifndef FO4HDR_TONEMAP_BYPASS_COMMON_HLSLI
#define FO4HDR_TONEMAP_BYPASS_COMMON_HLSLI

#ifndef FO4HDR_SCENE_TEXTURE
#error FO4HDR_SCENE_TEXTURE must identify the Fallout 4 scene texture
#endif

#ifndef FO4HDR_BLOOM_TEXTURE
#error FO4HDR_BLOOM_TEXTURE must identify the Fallout 4 bloom texture
#endif

#ifndef FO4HDR_BLOOM_UV
#error FO4HDR_BLOOM_UV must identify the Fallout 4 bloom coordinate scale
#endif

#ifndef FO4HDR_CB2_COUNT
#error FO4HDR_CB2_COUNT must match the selected Fallout 4 shader variant
#endif

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t3 : register(t3);

SamplerState s0_s : register(s0);
SamplerState s1_s : register(s1);
SamplerState s2_s : register(s2);
SamplerState s3_s : register(s3);

Texture2D<float2> Fo4HdrExposureSnapshot : register(t17);
cbuffer Fo4HdrDeveloperExposureSelector : register(b13)
{
    uint Fo4HdrExposureLockEnabled;
    uint3 Fo4HdrExposureLockReserved;
}

cbuffer Fallout4ImageSpaceConstants : register(b2)
{
    float4 cb2[FO4HDR_CB2_COUNT];
}

float3 Fo4HdrApplySceneFilter(float3 inputColor, float measuredExposure)
{
    const float luminance = dot(
        inputColor, float3(0.212500006f, 0.715399981f, 0.0720999986f));
    float4 filtered = float4(inputColor, 0.0f) - luminance.xxxx;
    filtered = cb2[2].xxxx * filtered + luminance.xxxx;
    const float4 channelShape = luminance.xxxx * cb2[3].xyzw - filtered;
    filtered = cb2[3].wwww * channelShape + filtered;
    filtered = cb2[2].wwww * filtered - measuredExposure.xxxx;
    filtered = cb2[2].zzzz * filtered + measuredExposure.xxxx;

#if FO4HDR_HAS_FINAL_AFFINE
    filtered += cb2[5].wwww * (cb2[5].xyzw - filtered);
#endif

    return filtered.rgb;
}

void main(
    float4 position : SV_POSITION0,
    float2 texcoord : TEXCOORD0,
    out float4 output : SV_Target0)
{
    const float3 scene = FO4HDR_SCENE_TEXTURE;
    const float depthMask = t3.Sample(s3_s, texcoord).w;

    // The native raw alpha tag bypasses exposure, bloom and scene filtering.
    if (abs(depthMask * 255.0f - 4.0f) < 0.25f)
    {
        output = float4(scene, 1.0f);
        return;
    }

    const float2 bloomUv = FO4HDR_BLOOM_UV;
    const float3 bloom = FO4HDR_BLOOM_TEXTURE;
    float measuredExposure = t2.Sample(s2_s, texcoord).x;
    float exposure = cb2[1].z / (0.00100000005f + measuredExposure);
    exposure = clamp(exposure, cb2[1].y, cb2[1].x);

    if (Fo4HdrExposureLockEnabled == 1u)
    {
        const float2 captured = Fo4HdrExposureSnapshot.Load(int3(0, 0, 0));
        const bool finitePair = all((asuint(captured) & 0x7f800000u) != 0x7f800000u);
        if (finitePair && captured.x > 0.0f && captured.y >= 0.0f)
        {
            exposure = captured.x;
            measuredExposure = captured.y;
        }
    }

    // Keep linear scene-relative values above one; white remains 203 nits.
    const float3 exposedScene = (scene + bloom) * exposure;
    float3 targetColor = Fo4HdrApplySceneFilter(exposedScene, measuredExposure);
    // A common scale contains finite affine results to FP16 without hue clipping.
    const float maxTargetChannel = max(abs(targetColor.r),
        max(abs(targetColor.g), abs(targetColor.b)));
    if (maxTargetChannel > 65504.0f)
        targetColor *= 65504.0f / maxTargetChannel;
    output = float4(targetColor, 1.0f);
}

#endif
