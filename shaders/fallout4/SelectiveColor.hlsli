// SPDX-License-Identifier: MIT
// Copyright (c) 2023 Thatcher Freeman
// Copyright Fallout4HDR contributors
// Upstream: thatcherfreeman/utility-dctls@a495531de3e4704a0a6e0113daa69be303172ed0/Effects/Smooth Tetra Skin.dctl
// Linear-only bounded adaptation for Fallout4HDR. The cylindrical conversion
// is attributed by the upstream source to Juan Pablo Zambrano.
#ifndef FO4HDR_SELECTIVE_COLOR_HLSLI
#define FO4HDR_SELECTIVE_COLOR_HLSLI

static const float kFo4HdrSelectiveNeutralWidth = 0.1f;
static const float kFo4HdrSelectiveMaximumFp16 = 65504.0f;
static const float2 kFo4HdrSelectiveCenters[7] = {
    float2(1.0f, 0.0f),
    float2(0.8660254038f, 0.5f),
    float2(0.5f, 0.8660254038f),
    float2(-0.5f, 0.8660254038f),
    float2(-1.0f, 0.0f),
    float2(-0.5f, -0.8660254038f),
    float2(0.5f, -0.8660254038f)
};

float4 Fo4HdrSelectiveRegion(uint index)
{
    if (index == 0u) return Fo4HdrSelectiveRed;
    if (index == 1u) return Fo4HdrSelectiveOrange;
    if (index == 2u) return Fo4HdrSelectiveYellow;
    if (index == 3u) return Fo4HdrSelectiveGreen;
    if (index == 4u) return Fo4HdrSelectiveCyan;
    if (index == 5u) return Fo4HdrSelectiveBlue;
    return Fo4HdrSelectiveMagenta;
}

float3 Fo4HdrSelectiveAdjustment(float2 hueDirection, float saturation)
{
    float exponents[7];
    float maximumExponent = -3.402823466e+38f;
    [unroll]
    for (uint index = 0u; index < 7u; ++index)
    {
        exponents[index] = 5.0f * dot(hueDirection,
            kFo4HdrSelectiveCenters[index]);
        maximumExponent = max(maximumExponent, exponents[index]);
    }
    float weightSum = 0.0f;
    float3 adjustment = 0.0f;
    [unroll]
    for (uint regionIndex = 0u; regionIndex < 7u; ++regionIndex)
    {
        const float weight = exp(exponents[regionIndex] - maximumExponent);
        const float4 region = Fo4HdrSelectiveRegion(regionIndex);
        adjustment += weight * region.xyz;
        weightSum += weight;
    }
    adjustment /= weightSum;

    // Validated hue limits bound this weighted angle to +/-10 degrees.
    // Small-angle series avoid SM5 sincos approximation error being amplified
    // when a nearly zero RGB channel is reconstructed from opposing terms.
    const float angleSquared = adjustment.x * adjustment.x;
    const float sine = adjustment.x * (1.0f + angleSquared *
        (-1.0f / 6.0f + angleSquared *
            (1.0f / 120.0f - angleSquared / 5040.0f)));
    const float cosine = 1.0f + angleSquared *
        (-1.0f / 2.0f + angleSquared *
            (1.0f / 24.0f - angleSquared / 720.0f));
    const float2 outputDirection = float2(
        hueDirection.x * cosine - hueDirection.y * sine,
        hueDirection.y * cosine + hueDirection.x * sine);
    const float outputSaturation = saturation * adjustment.y;
    return float3(outputDirection * outputSaturation,
        clamp(saturation, 0.0f, 2.0f) * adjustment.z);
}

float3 Fo4HdrApplySelectiveColor(float3 inputColor)
{
    if (Fo4HdrSelectiveColorActive == 0u ||
        (inputColor.r == inputColor.g && inputColor.g == inputColor.b))
        return inputColor;

    const float inputScale = max(inputColor.r,
        max(inputColor.g, inputColor.b));
    if (!(inputScale > 0.0f) || inputScale != inputScale)
        return inputColor;
    const float3 normalized = inputColor / inputScale;
    const float mean = (normalized.r + normalized.g + normalized.b) / 3.0f;
    if (!(mean > 0.0f) || mean != mean) return inputColor;
    const float a = normalized.r - 0.5f * normalized.g -
        0.5f * normalized.b;
    const float b = 0.8660254038f * (normalized.g - normalized.b);
    const float2 chroma = float2(a, b) / mean;
    const float chromaLength = length(chroma);
    if (!(chromaLength > 0.0f)) return inputColor;
    const float2 hueDirection = chroma / chromaLength;
    const float saturation = chromaLength / 3.0f;

    const float3 unsmooth = Fo4HdrSelectiveAdjustment(
        hueDirection, saturation);
    const float2 inputAb = hueDirection * saturation;
    const float3 plane = Fo4HdrSelectivePlaneA.xyz * inputAb.x +
        Fo4HdrSelectivePlaneB.xyz * inputAb.y;
    const float bump = exp2(-saturation * saturation /
        (kFo4HdrSelectiveNeutralWidth * kFo4HdrSelectiveNeutralWidth));
    const float3 outputAbz = lerp(unsmooth, plane, bump);
    const float outputMean = mean * exp2(outputAbz.z);
    // outputAbz.xy already contains direction times saturation. Retain it
    // directly to avoid a normalize/rescale round trip near zero RGB channels.
    float3 result = float3(
        outputMean * (1.0f + 2.0f * outputAbz.x),
        outputMean * (1.0f - outputAbz.x + 1.7320508076f * outputAbz.y),
        outputMean * (1.0f - outputAbz.x - 1.7320508076f * outputAbz.y));
    if (any(result != result) || any(abs(result) > 3.402823466e+38f))
        return inputColor;
    const float minimum = min(result.r, min(result.g, result.b));
    if (minimum < 0.0f)
    {
        const float safeMean = (result.r + result.g + result.b) / 3.0f;
        const float denominator = safeMean - minimum;
        if (!(safeMean > 0.0f) || !(denominator > 0.0f))
            return inputColor;
        // Algebraically the same neutral-axis contraction; the minimum
        // channel becomes exact zero without subtracting nearly equal terms.
        result = (result - minimum.xxx) * (safeMean / denominator);
    }
    result *= inputScale;
    if (any(result != result) || any(abs(result) > 3.402823466e+38f))
        return inputColor;
    const float maximum = max(result.r, max(result.g, result.b));
    if (maximum > kFo4HdrSelectiveMaximumFp16)
        result *= kFo4HdrSelectiveMaximumFp16 / maximum;
    return max(result, 0.0f);
}

#endif
