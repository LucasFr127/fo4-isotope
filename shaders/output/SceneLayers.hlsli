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

float3 Fo4HdrBlendLinearLayer(float3 behind, float4 layer, float uiWhiteNits)
{
    // Transport is normalized premultiplied BT.709; composition uses FP32 display light.
    return Fo4HdrExpandPrimaries(layer.rgb) * uiWhiteNits + behind * (1.0f - saturate(layer.a));
}

#endif
