// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Carlos Lopez Jr.
// UI-relative transport policy: clshortfuse/renodx@68d2ab15e6086ab04084278ee8cceb85b8a8023e/src/shaders/draw.hlsl
// RenoDX portions retain the MIT permission in THIRD_PARTY_NOTICES.md.
// Authored-code decoding and layer composition are project-authored adaptations.
#ifndef FO4HDR_SCENE_LAYERS_INCLUDED
#define FO4HDR_SCENE_LAYERS_INCLUDED

#include "DisplayMath.hlsli"

float3 Fo4HdrDecodeAuthoredCodes(float3 code)
{
    return code <= 0.04045f ? code / 12.92f :
        pow(max((code + 0.055f) / 1.055f, 0.0f), 2.4f);
}

float4 Fo4HdrPrepareAuthoredLayer(float4 coded, bool backdrop)
{
    const float coverage = saturate(coded.a);
    if (coverage <= 1e-6f) return float4(0.0f, 0.0f, 0.0f, coverage);
    // Completed backdrop RGB is its authored contribution; HUD RGB carries coverage in code space.
    const float3 contribution = backdrop ? Fo4HdrDecodeAuthoredCodes(max(coded.rgb, 0.0f)) :
        Fo4HdrDecodeAuthoredCodes(max(coded.rgb, 0.0f) / coverage) * coverage;
    return float4(contribution, coverage);
}

float3 Fo4HdrBlendAuthoredLayer(float3 behind, float4 coded, float uiWhiteNits, bool backdrop)
{
    // Decode and premultiply in registers; stored layers retain their authored codes.
    const float4 layer = Fo4HdrPrepareAuthoredLayer(coded, backdrop);
    return Fo4HdrBt709ToBt2020Nits(layer.rgb, uiWhiteNits) + behind * (1.0f - saturate(layer.a));
}

float3 Fo4HdrCalibrationLight(uint mode, uint2 pixel, uint2 extent,
    float2 peakAndWhite, float3 sceneNits)
{
    const float peak = peakAndWhite.x;
    const float white = peakAndWhite.y;
    switch (mode)
    {
    case 10u:
        return (floor(pixel.x / max(1.0f, float(extent.x) - 1.0f) * 31.0f + 1.0e-6f) *
            (peak * 1.125f / 31.0f)).xxx;
    case 11u:
        {
            const float2 coordinate = (float2(pixel) + 0.5f) / max(float2(extent), 1.0f);
            return all(coordinate >= 0.2f) && all(coordinate <= 0.8f) ? white.xxx : 0.0f;
        }
    case 12u:
        {
            static const float stops[10] = {0.0f, 0.001f, 0.005f, 0.01f, 0.02f,
                0.05f, 0.1f, 0.2f, 0.5f, 1.0f};
            return stops[min(pixel.x * 10u / max(extent.x, 1u), 9u)].xxx;
        }
    case 13u:
        {
            static const float3 patches[6] = {float3(1,0,0), float3(0,1,0),
                float3(0,0,1), float3(1,1,0), float3(0,1,1), float3(1,0,1)};
            const float3 patch = patches[min(pixel.x * 6u / max(extent.x, 1u), 5u)] * white;
            return pixel.y >= extent.y / 2u ? Fo4HdrBt709ToBt2020Nits(patch, 1.0f) : patch;
        }
    case 14u:
        {
            const float largest = max(sceneNits.r, max(sceneNits.g, sceneNits.b));
            if (largest >= peak * (1.0f - 1.0e-5f)) return float3(peak, 0, 0);
            if (largest >= 0.9f * peak) return float3(peak, peak, 0);
            return sceneNits;
        }
    default:
        return sceneNits;
    }
}

#endif
