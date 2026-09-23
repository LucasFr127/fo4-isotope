// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/12_luts_0x80802E60.ps_5_0.hlsl
// Retains the origin and notices of the containing creative-grade adaptation.
// Modification: extract its project-authored image controls for both grade methods.
#ifndef FO4HDR_IMAGE_CONTROLS_HLSLI
#define FO4HDR_IMAGE_CONTROLS_HLSLI

#include "LiveSettingsConstants.hlsli"

static const float3 kBt709Luma =
    float3(0.212500006f, 0.715399981f, 0.0720999986f);
static const float kFo4HdrImagePivot = 0.18f;
static const float kFo4HdrMaximumFiniteFp16 = 65504.0f;

float Fo4HdrSanitizeImageChannel(float value)
{
    if (value != value)
    {
        return 0.0f;
    }
    if (abs(value) > kFo4HdrMaximumFiniteFp16)
    {
        return value < 0.0f ? 0.0f : kFo4HdrMaximumFiniteFp16;
    }
    return max(value, 0.0f);
}

float3 Fo4HdrSanitizeImage(float3 value)
{
    return float3(
        Fo4HdrSanitizeImageChannel(value.r),
        Fo4HdrSanitizeImageChannel(value.g),
        Fo4HdrSanitizeImageChannel(value.b));
}

float3 Fo4HdrScaleLuminance(float3 color, float scale)
{
    // A common scale preserves channel ratios until per-channel FP16
    // containment is needed; values are not clipped to the SDR [0,1] range.
    return Fo4HdrSanitizeImage(color * max(scale, 0.0f));
}

float3 Fo4HdrSetLuminance(float3 color, float targetLuminance)
{
    const float luminance = dot(color, kBt709Luma);
    if (luminance <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return Fo4HdrScaleLuminance(
        color, max(targetLuminance, 0.0f) / luminance);
}

float3 Fo4HdrApplyBlacks(float3 color)
{
    float3 result = color;
    if (Fo4HdrBlacksStrength != 0.0f)
    {
        const float luminance = dot(color, kBt709Luma);
        if (luminance < 0.02f)
        {
            const float toe = 1.0f - max(luminance, 0.0f) / 0.02f;
            result = Fo4HdrScaleLuminance(color,
                1.0f + Fo4HdrBlacksStrength * toe * toe);
        }
    }
    return result;
}

bool Fo4HdrOtherImageControlsAreNeutral()
{
    return Fo4HdrShadowsEv == 0.0f &&
        Fo4HdrHighlightsEv == 0.0f &&
        Fo4HdrContrastSlope == 1.0f &&
        Fo4HdrSaturation == 1.0f &&
        all(Fo4HdrColorAdaptationRow0 == float4(1.0f, 0.0f, 0.0f, 0.0f)) &&
        all(Fo4HdrColorAdaptationRow1 == float4(0.0f, 1.0f, 0.0f, 0.0f)) &&
        all(Fo4HdrColorAdaptationRow2 == float4(0.0f, 0.0f, 1.0f, 0.0f));
}

float3 Fo4HdrApplyImageControls(float3 color)
{
    float3 controlled = color;
    if (Fo4HdrOtherImageControlsAreNeutral())
    {
        // Keep default bypass and above-knee identity exact. Do not send a
        // Blacks-only edit through unrelated contrast/saturation arithmetic.
        controlled = Fo4HdrApplyBlacks(color);
    }
    else
    {
        float luminance = dot(color, kBt709Luma);
        const float shadowMask = 1.0f - smoothstep(
            0.02f, kFo4HdrImagePivot, luminance);
        color = Fo4HdrScaleLuminance(
            color, exp2(Fo4HdrShadowsEv * shadowMask));

        luminance = dot(color, kBt709Luma);
        const float highlightMask = smoothstep(1.0f, 4.0f, luminance);
        color = Fo4HdrScaleLuminance(
            color, exp2(Fo4HdrHighlightsEv * highlightMask));

        luminance = dot(color, kBt709Luma);
        if (luminance > 0.0f)
        {
            const float contrastedLuminance = kFo4HdrImagePivot * exp2(
                log2(max(luminance / kFo4HdrImagePivot, 1.0e-6f)) *
                    Fo4HdrContrastSlope);
            color = Fo4HdrSetLuminance(color, contrastedLuminance);
        }

        color = Fo4HdrApplyBlacks(color);
        luminance = dot(color, kBt709Luma);
        const float3 chroma = color - luminance.xxx;
        float boundedSaturation = Fo4HdrSaturation;
        if (boundedSaturation > 1.0f)
        {
            // Preserve luminance while preventing an oversaturation request from
            // driving a channel negative and relying on a later luminance-changing
            // clamp. Already gamut-bound primaries therefore remain stable.
            if (chroma.r < 0.0f)
                boundedSaturation = min(
                    boundedSaturation, luminance / max(-chroma.r, 1.0e-6f));
            if (chroma.g < 0.0f)
                boundedSaturation = min(
                    boundedSaturation, luminance / max(-chroma.g, 1.0e-6f));
            if (chroma.b < 0.0f)
                boundedSaturation = min(
                    boundedSaturation, luminance / max(-chroma.b, 1.0e-6f));
        }
        color = luminance.xxx + chroma * boundedSaturation;

        color = float3(
            dot(Fo4HdrColorAdaptationRow0.xyz, color),
            dot(Fo4HdrColorAdaptationRow1.xyz, color),
            dot(Fo4HdrColorAdaptationRow2.xyz, color));
        controlled = Fo4HdrSanitizeImage(color);
    }
    return controlled;
}

#endif
