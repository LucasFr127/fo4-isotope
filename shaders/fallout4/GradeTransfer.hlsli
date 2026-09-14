// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FO4HDR_GRADE_TRANSFER
#define FO4HDR_GRADE_TRANSFER

float Fo4HdrLargestGradeChannel(float3 rgb) { return max(rgb.x, max(rgb.y, rgb.z)); }

float3 Fo4HdrGradeCleanScene(float3 rgb)
{
    // Bit tests keep the NaN policy explicit under optimized shader arithmetic.
    [unroll] for (uint channel = 0; channel < 3; ++channel)
    {
        const uint magnitude = asuint(rgb[channel]) & 0x7fffffffu;
        rgb[channel] = magnitude > 0x7f800000u
            ? 0.0f : clamp(rgb[channel], 0.0f, 65504.0f);
    }
    return rgb;
}

float3 Fo4HdrApplyVanillaGradeRemoval(float3 hdr, float3 graded,
    float luminanceRemoval, float colorRemoval)
{
    if (!(luminanceRemoval > 0.0f || colorRemoval > 0.0f)) return graded;
    if (luminanceRemoval >= 1.0f && colorRemoval >= 1.0f) return hdr;
    if (all(hdr == graded)) return hdr;

    static const float3 luminanceWeights =
        float3(0.212500006f, 0.715399981f, 0.0720999986f);
    const float hdrLuminance = dot(hdr, luminanceWeights);
    const float gradedLuminance = dot(graded, luminanceWeights);
    if (!(hdrLuminance > 0.0f || gradedLuminance > 0.0f)) return 0.0f;

    float3 hdrChromaticity = 1.0f;
    [branch] if (hdrLuminance > 0.0f)
        hdrChromaticity = hdr / hdrLuminance;
    float3 gradedChromaticity = hdrChromaticity;
    [branch] if (gradedLuminance > 0.0f)
        gradedChromaticity = graded / gradedLuminance;

    const float targetLuminance = lerp(hdrLuminance, gradedLuminance,
        1.0f - saturate(luminanceRemoval));
    const float3 targetChromaticity = lerp(hdrChromaticity,
        gradedChromaticity, 1.0f - saturate(colorRemoval));
    const float3 adjusted = targetLuminance * targetChromaticity;
    // Nondefault luma/chroma combinations can exceed the FP16 transport range.
    const float scale = min(1.0f, 65504.0f /
        max(Fo4HdrLargestGradeChannel(adjusted), 65504.0f));
    return min(adjusted * scale, 65504.0f);
}

float Fo4HdrRadialCurve(float value)
{
    // Factoring the native rational curve avoids cancellation at black.
    return value * (0.14f * value + 1.0f / 60.0f) /
        (value * (0.15f * value + 0.5f) + 0.06f);
}

float Fo4HdrRadialGuideScale(float peak)
{
    // The factored inverse remains finite at black; only lookup exposure is bounded.
    if (peak >= 5.6f) return 1.0f / peak;
    const float value = 2.0f * peak;
    return 2.0f * (0.14f * value + 1.0f / 60.0f) /
        ((value * (0.15f * value + 0.5f) + 0.06f) * Fo4HdrRadialCurve(11.2f));
}

float3 Fo4HdrRadialCoordinates(float3 scene)
{
    return pow(saturate(scene * Fo4HdrRadialGuideScale(Fo4HdrLargestGradeChannel(scene))),
        1.0f / 2.2f);
}

float3 Fo4HdrTransferRadialGrade(float3 input, float3 weightedCodes, float strength)
{
    const float3 scene = Fo4HdrGradeCleanScene(input);
    const uint magnitude = asuint(strength) & 0x7fffffffu;
    const float amount = magnitude > 0x7f800000u ? 0.0f : saturate(strength);
    if (amount <= 0.0f) return scene;
    // Decode the weighted codes once, then restore their common scene scale.
    const float3 decoded = pow(Fo4HdrGradeCleanScene(weightedCodes), 2.2f);
    const float3 restored = decoded / Fo4HdrRadialGuideScale(Fo4HdrLargestGradeChannel(scene));
    // Uniform scaling may round one FP32 ULP above the FP16 limit.
    const float3 graded = min(restored * min(1.0f,
        65504.0f / max(Fo4HdrLargestGradeChannel(restored), 65504.0f)), 65504.0f);
    if (amount >= 1.0f) return graded;
    return lerp(scene, graded, amount);
}

#endif
