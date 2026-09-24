// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Carlos Lopez Jr.
// UI-relative transport policy: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
// RenoDX portions retain the MIT permission in THIRD_PARTY_NOTICES.md.
#ifndef FO4HDR_SCENE_LAYERS
#define FO4HDR_SCENE_LAYERS

#include "DisplayMath.hlsli"
#ifdef FO4HDR_LAYER_ORACLES
#include "../tonemap/Gt7.hlsli"
#endif

Texture2D<float4> FrameSource : register(t0);
Texture2D<float4> CapturedMatte : register(t1);
Texture2D<float4> CapturedHud : register(t2);
Texture2D<float4> SettingsSurface : register(t3);

cbuffer FrameMapping : register(b0)
{
    uint2 FrameSize;
    uint FrameMode;
    float MenuWhite;
    float ReferenceWhite;
    float PeakWhite;
    uint HasMatte;
    uint HasHud;
    uint HasSettings;
    uint3 Reserved;
};

// IEC sRGB inverse transfer. The armor interval has a separate signed gamma contract.
float3 DecodeAuthoredCodes(float3 code)
{
    return code <= 0.04045f ? code / 12.92f :
        pow(max((code + 0.055f) / 1.055f, 0.0f), 2.4f);
}

float3 BlendAuthoredLayer(float3 behind, float4 layer, float white, bool matte)
{
    const float coverage = saturate(layer.a);
    if (matte && coverage <= 1.0e-6f) return behind;
    // The Fallout matte is already a completed code-domain contribution. HUD
    // and ImGui instead require decoding straight color before premultiplication.
    float3 contribution = 0.0f;
    if (matte)
        contribution = DecodeAuthoredCodes(max(layer.rgb, 0.0f));
    else if (coverage > 1.0e-6f)
        contribution = DecodeAuthoredCodes(max(layer.rgb, 0.0f) / coverage) * coverage;
    return Fo4HdrExpandPrimaries(contribution * white) + behind * (1.0f - coverage);
}

float3 EvaluateFrameLayers(uint2 pixel)
{
    uint sourceWidth, sourceHeight;
    FrameSource.GetDimensions(sourceWidth, sourceHeight);
    const uint2 sourceSize = max(uint2(sourceWidth, sourceHeight), 1u);
    const uint2 sourcePixel = min(pixel * sourceSize / max(FrameSize, 1u), sourceSize - 1u);
    float3 light = FrameSource.Load(int3(sourcePixel, 0)).rgb;
    const bool pattern = FrameMode >= 10u && FrameMode <= 13u;
    const bool absolute = FrameMode == 3u || FrameMode == 7u ||
        (FrameMode >= 10u && FrameMode <= 14u);

    switch (FrameMode)
    {
    case 1u:
        light = Fo4HdrExpandPrimaries(light);
        break;
    case 2u:
        light = Fo4HdrExpandPrimaries((16.0f * pixel.x / max(1.0f, float(FrameSize.x) - 1.0f)).xxx);
        break;
    case 3u:
        light *= MenuWhite;
        break;
#ifdef FO4HDR_LAYER_ORACLES
    case 7u:
        light = Fo4HdrGt7Bt2020AbsoluteNits(light, PeakWhite, ReferenceWhite);
        break;
    case 8u:
        light = Fo4HdrGt7OfficialRec2020(light, PeakWhite);
        break;
#endif
    case 10u:
        light = (floor(pixel.x / max(1.0f, float(FrameSize.x) - 1.0f) * 31.0f + 1.0e-6f) *
            (PeakWhite * 1.125f / 31.0f)).xxx;
        break;
    case 11u:
        {
            const float2 coordinate = (float2(pixel) + 0.5f) / max(float2(FrameSize), 1.0f);
            light = all(coordinate >= 0.2f) && all(coordinate <= 0.8f) ? MenuWhite.xxx : 0.0f;
        }
        break;
    case 12u:
        {
            static const float stops[10] = {0.0f, 0.001f, 0.005f, 0.01f, 0.02f,
                0.05f, 0.1f, 0.2f, 0.5f, 1.0f};
            light = stops[min(pixel.x * 10u / max(FrameSize.x, 1u), 9u)].xxx;
        }
        break;
    case 13u:
        {
            static const float3 patches[6] = {float3(1,0,0), float3(0,1,0),
                float3(0,0,1), float3(1,1,0), float3(0,1,1), float3(1,0,1)};
            light = patches[min(pixel.x * 6u / max(FrameSize.x, 1u), 5u)] * MenuWhite;
            if (pixel.y >= FrameSize.y / 2u) light = Fo4HdrExpandPrimaries(light);
        }
        break;
    case 14u:
        light *= MenuWhite;
        {
            const float largest = max(light.r, max(light.g, light.b));
            if (largest >= PeakWhite * (1.0f - 1.0e-5f)) light = float3(PeakWhite, 0, 0);
            else if (largest >= 0.9f * PeakWhite) light = float3(PeakWhite, PeakWhite, 0);
        }
        break;
    default:
        break;
    }

    if (FrameMode == 0u)
    {
        if (HasMatte != 0u)
        {
            const float4 layer = CapturedMatte.Load(int3(pixel, 0));
            const float coverage = saturate(layer.a);
            if (coverage > 1.0e-6f) light = layer.rgb + light * (1.0f - coverage);
        }
        if (HasHud != 0u)
        {
            const float4 layer = CapturedHud.Load(int3(pixel, 0));
            light = layer.rgb + light * (1.0f - saturate(layer.a));
        }
    }
    else
    {
        const float white = absolute ? MenuWhite : 1.0f;
        if (!pattern && HasMatte != 0u)
            light = BlendAuthoredLayer(light, CapturedMatte.Load(int3(pixel, 0)), white, true);
        if (!pattern && HasHud != 0u)
            light = BlendAuthoredLayer(light, CapturedHud.Load(int3(pixel, 0)), white, false);
        [branch] if (HasSettings != 0u)
            light = BlendAuthoredLayer(light, SettingsSurface.Load(int3(pixel, 0)), white, false);
    }
    // A shared exit avoids FXC's uninitialized-result warning when this is inlined.
    return light;
}

#endif
