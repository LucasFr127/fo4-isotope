// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/shared.h
// Adapted for FO4Isotope from the pinned Fallout 4 tone-map family.
// Modification: retain Fallout 4 exposure, separate bloom, mask, and
// scene-filter stages while removing only normalized Uncharted 2/Hable range
// compression; apply a project-authored multiplier on that separate bloom
// contribution before addition. The SDR-mode variants instead keep the
// game's own curve, or an operator's SDR table in its place, and apply
// project scene controls before it.
// See THIRD_PARTY_NOTICES.md for the retained MIT permission text.
#ifndef FO4HDR_TONEMAP_BYPASS_COMMON_HLSLI
#define FO4HDR_TONEMAP_BYPASS_COMMON_HLSLI

#include "LiveSettingsConstants.hlsli"

#ifndef FO4HDR_SDR_SCENE_CONTROLS
#define FO4HDR_SDR_SCENE_CONTROLS 0
#endif

#if FO4HDR_SDR_SCENE_CONTROLS
#define FO4HDR_SDR_OPERATOR_OUTPUT 1
#include "../tonemap/OperatorLut.hlsli"
#include "ImageControls.hlsli"
#include "SelectiveColor.hlsli"
#endif

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
#if FO4HDR_SDR_SCENE_CONTROLS
// The selected operator's SDR table, bound only while b13 selects it.
Texture3D<float4> Fo4HdrOperatorLut : register(t7);
#else
// Borrowed only for the developer lock; normal b13 sets its activity bit to zero.
Texture2D<float2> Fo4HdrDeveloperExposureSnapshot : register(t17);
#endif

SamplerState s0_s : register(s0);
SamplerState s1_s : register(s1);
SamplerState s2_s : register(s2);
SamplerState s3_s : register(s3);
#if FO4HDR_SDR_SCENE_CONTROLS
SamplerState Fo4HdrOperatorSampler : register(s7);
#endif

cbuffer Fallout4ImageSpaceConstants : register(b2)
{
    float4 cb2[FO4HDR_CB2_COUNT];
}

float3 Fo4HdrApplySceneFilter(
    float3 inputColor,
    float measuredExposure)
{
    const float luminance = dot(
        inputColor,
        float3(0.212500006f, 0.715399981f, 0.0720999986f));
    float4 filtered = float4(inputColor, 0.0f) - luminance.xxxx;
    filtered = cb2[2].xxxx * filtered + luminance.xxxx;
    const float4 channelShape =
        luminance.xxxx * cb2[3].xyzw - filtered;
    filtered = cb2[3].wwww * channelShape + filtered;
    filtered = cb2[2].wwww * filtered - measuredExposure.xxxx;
    filtered =
        cb2[2].zzzz * filtered + measuredExposure.xxxx;

#if FO4HDR_HAS_FINAL_AFFINE
    filtered += cb2[5].wwww * (cb2[5].xyzw - filtered);
#endif

    return filtered.rgb;
}

#if FO4HDR_SDR_SCENE_CONTROLS
// The game's own curve: Uncharted 2 with exposure bias 2, white 11.2 and the
// ImageSpace toe numerator, in the original operation order.
float3 Fo4HdrApplyNativeToneCurve(float3 color)
{
    const float toeNumerator = cb2[1].w;
    const float3 doubled = color + color;
    const float3 numerator = doubled * (color * 0.30f + 0.05f) + toeNumerator * 0.20f;
    const float3 denominator = doubled * (color * 0.30f + 0.5f) + 0.06f;
    const float3 curved = numerator / denominator - toeNumerator * 3.333333f;
    const float white = (toeNumerator * 0.20f + 19.3759995f) * 0.0408563502f -
        toeNumerator * 3.333333f;
    return (1.0f / white) * curved;
}

// Mode 0 keeps the game's curve. Modes 1-3 read the selected operator's SDR
// table, which takes the same scene value without the curve's exposure bias.
float3 Fo4HdrApplySdrToneCurve(float3 color)
{
    float3 curved;
    [branch]
    if (Fo4HdrProductionMode == 0u)
        curved = Fo4HdrApplyNativeToneCurve(color);
    else
        curved = Fo4HdrSampleOperatorLut(Fo4HdrOperatorLut, Fo4HdrOperatorSampler, color,
            Fo4HdrProductionMode, Fo4HdrOperatorShaperAngleGain, Fo4HdrOperatorShaperBias,
            1.0f, Fo4HdrOperatorShaperParameters);
    return curved;
}
#endif

void main(
    float4 position : SV_POSITION0,
    float2 texcoord : TEXCOORD0,
    out float4 output : SV_Target0)
{
    const float3 scene = FO4HDR_SCENE_TEXTURE;
    const float depthMask = t3.Sample(s3_s, texcoord).w;

    // Fallout 4 uses the raw alpha tag 4/255 for a branch that bypasses
    // exposure, bloom, the display curve, and the scene filter.
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
#if FO4HDR_SDR_SCENE_CONTROLS
    // Scene controls act in scene-linear light before the curve; the
    // ImageSpace filter then runs on display-referred values, as in vanilla.
    float3 controlled = (scene + bloom * Fo4HdrBloomStrength) *
        (exposure * Fo4HdrSceneExposureMultiplier);
    controlled = Fo4HdrApplySelectiveColor(Fo4HdrApplyImageControls(controlled));
    output = float4(Fo4HdrApplySceneFilter(
        Fo4HdrApplySdrToneCurve(controlled), measuredExposure), 1.0f);
#else
    [branch]
    if (Fo4HdrDeveloperExposureLocked != 0u)
    {
        const float2 snapshot = Fo4HdrDeveloperExposureSnapshot.Load(int3(0, 0, 0));
        if (all((asuint(snapshot) & 0x7f800000u) != 0x7f800000u) &&
            snapshot.x > 0.0f && snapshot.y >= 0.0f)
        {
            exposure = snapshot.x;
            measuredExposure = snapshot.y;
        }
    }

    // Retain bloom and bounded exposure; the destructive normalized Hable curve is absent.
    const float3 exposedScene = (scene + bloom * Fo4HdrBloomStrength) * exposure;
    float3 targetColor =
        Fo4HdrApplySceneFilter(exposedScene, measuredExposure) *
            Fo4HdrSceneExposureMultiplier;
    // Fallout 4's later exposure, bloom, and affine color filter can exceed
    // FP16 even when the input scene is representable.
    // Bound the complete target value with one scalar to avoid hue clipping.
    const float maxTargetChannel = max(abs(targetColor.r),
        max(abs(targetColor.g), abs(targetColor.b)));
    if (maxTargetChannel > 65504.0f)
        targetColor *= 65504.0f / maxTargetChannel;
    output = float4(targetColor, 1.0f);
#endif
}

#endif
