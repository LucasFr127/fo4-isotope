// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@68d2ab15e6086ab04084278ee8cceb85b8a8023e/src/games/fallout4/{taa_0x8CAC3BD9,taa2_0x61CC29E6}.ps_5_0.hlsl
// Upstream: clshortfuse/renodx@68d2ab15e6086ab04084278ee8cceb85b8a8023e/src/shaders/draw.hlsl
// Adapted for Fallout4HDR from RenoDX's gamma-domain temporal wrapper.
// Modification: use reversible positive-HDR compression within the filter,
// preserving linear BT.709 scene transport with a fixed 203-nit reference.
// See THIRD_PARTY_NOTICES.md.
#ifndef FO4HDR_TEMPORAL_INTERMEDIATE_HLSLI
#define FO4HDR_TEMPORAL_INTERMEDIATE_HLSLI

static const float kFo4HdrMaximumFiniteFp16 = 65504.0f;
static const float kFo4HdrMaximumTemporalCode =
    kFo4HdrMaximumFiniteFp16 / (1.0f + kFo4HdrMaximumFiniteFp16);

float3 Fo4HdrEncodeTemporalInput(float3 linearColor)
{
    // Compression bounds only the filter's local codes, not the scene RGB.
    const float3 positive = min(
        max(linearColor, 0.0f), kFo4HdrMaximumFiniteFp16);
    const float3 compressed = positive / (1.0f + positive);
    return pow(compressed, 1.0f / 2.2f);
}

float3 Fo4HdrDecodeTemporalOutput(float3 temporalColor)
{
    // The finite code ceiling keeps the inverse denominator above zero.
    const float3 compressed = min(
        pow(saturate(temporalColor), 2.2f),
        kFo4HdrMaximumTemporalCode);
    return min(
        compressed / max(1.0f - compressed, 1.0f / 65505.0f),
        kFo4HdrMaximumFiniteFp16);
}

#endif
