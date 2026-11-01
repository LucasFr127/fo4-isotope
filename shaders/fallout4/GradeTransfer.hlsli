// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#ifndef FO4HDR_GRADE_TRANSFER
#define FO4HDR_GRADE_TRANSFER

#include "../tonemap/OperatorLut.hlsli"

float Fo4HdrLargestGradeChannel(float3 rgb) { return max(rgb.x, max(rgb.y, rgb.z)); }

// Display curve used to read native LUT codes. Lookups stay gamma-2.2 encoded.
static const uint Fo4HdrGradeCurveGamma22 = 0u;
static const uint Fo4HdrGradeCurveSrgb = 1u;
static const float3 Fo4HdrGradeLuma = float3(0.212500006f, 0.715399981f, 0.0720999986f);

float3 Fo4HdrGradeDecode(float3 codes, uint curve)
{
    if (curve == Fo4HdrGradeCurveSrgb)
        return codes <= 0.04045f ? codes / 12.92f : pow(max((codes + 0.055f) / 1.055f, 0.0f), 2.4f);
    return pow(max(codes, 0.0f), 2.2f);
}

float3 Fo4HdrGradeEncode(float3 value, uint curve)
{
    if (curve == Fo4HdrGradeCurveSrgb)
        return value <= 0.0031308f ? value * 12.92f :
            1.055f * pow(max(value, 0.0f), 1.0f / 2.4f) - 0.055f;
    return pow(max(value, 0.0f), 1.0f / 2.2f);
}

float3 Fo4HdrGradeCleanScene(float3 rgb)
{
    [unroll] for (uint channel = 0; channel < 3; ++channel)
        rgb[channel] = Fo4HdrSanitizeOperatorInput(rgb[channel], 0u);
    return rgb;
}

void Fo4HdrResolveVanillaGradeRemoval(float4 nativeWeights,
    uint profilesActive, float globalLuminanceRemoval,
    float globalColorRemoval, float4 profileLuminanceRemoval,
    float4 profileColorRemoval, out float luminanceRemoval,
    out float colorRemoval)
{
    luminanceRemoval = globalLuminanceRemoval;
    colorRemoval = globalColorRemoval;
    if (profilesActive == 0u) return;

    const uint4 magnitudes = asuint(nativeWeights) & 0x7fffffffu;
    if (any(magnitudes >= 0x7f800000u) || any(nativeWeights < 0.0f)) return;
    const float maximum = max(max(nativeWeights.x, nativeWeights.y),
        max(nativeWeights.z, nativeWeights.w));
    if (!(maximum > 0.0f)) return;

    // Keep both the scaled sum and its reciprocal normal. Dividing directly
    // by FLT_MAX can compile to a subnormal reciprocal that hardware flushes
    // to zero. The exact power-of-two scale puts maxima >= 1 near 2^64 while
    // preserving every contribution large enough to affect an FP32 sum.
    const uint maximumExponent = (asuint(maximum) >> 23u) & 0xffu;
    const float normalizationScale = maximum >= 1.0f
        ? asfloat((318u - maximumExponent) << 23u) : 1.0f;
    precise float4 scaled = nativeWeights * normalizationScale;
    precise float sum = dot(scaled, float4(1.0f, 1.0f, 1.0f, 1.0f));
    if (!(sum > 0.0f)) return;
    precise float4 normalized = scaled / sum;
    luminanceRemoval = dot(normalized, profileLuminanceRemoval);
    colorRemoval = dot(normalized, profileColorRemoval);
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

float Fo4HdrNativeCurve(float value)
{
    // Factoring the native rational curve avoids cancellation at black.
    return value * (0.14f * value + 1.0f / 60.0f) /
        (value * (0.15f * value + 0.5f) + 0.06f);
}

float Fo4HdrRgbRatioGuideScale(float peak)
{
    // Factor f(peak)/peak so the inverse scale remains finite at exact black.
    // Only lookup exposure is bounded at native white; scene RGB is retained.
    if (peak >= 5.6f) return 1.0f / peak;
    const float value = 2.0f * peak;
    return 2.0f * (0.14f * value + 1.0f / 60.0f) /
        ((value * (0.15f * value + 0.5f) + 0.06f) * Fo4HdrNativeCurve(11.2f));
}

float3 Fo4HdrRgbRatioCoordinates(float3 scene)
{
    return pow(saturate(scene * Fo4HdrRgbRatioGuideScale(Fo4HdrLargestGradeChannel(scene))),
        1.0f / 2.2f);
}

float3 Fo4HdrTransferRgbRatioGrade(float3 input, float3 weightedCodes, float strength)
{
    const float3 scene = Fo4HdrGradeCleanScene(input);
    const float amount = (asuint(strength) & 0x7fffffffu) > 0x7f800000u
        ? 0.0f : saturate(strength);
    if (!(amount > 0.0f)) return scene;

    // Match the lookup's gamma encoding, then restore its common RGB scale.
    // This ratio extension also scales authored black lift with scene peak.
    const float3 decoded = pow(Fo4HdrGradeCleanScene(weightedCodes), 2.2f);
    const float3 restored = decoded / Fo4HdrRgbRatioGuideScale(Fo4HdrLargestGradeChannel(scene));
    // Uniform scaling can round its maximum one FP32 ULP above the FP16 limit.
    const float3 endpoint = min(restored * min(1.0f,
        65504.0f / max(Fo4HdrLargestGradeChannel(restored), 65504.0f)), 65504.0f);
    return lerp(scene, endpoint, amount);
}

float3 Fo4HdrTransferRgbRatioGradeSrgb(float3 input, float3 weightedCodes,
    float3 blackCodes, float strength)
{
    const float3 scene = Fo4HdrGradeCleanScene(input);
    const float amount = (asuint(strength) & 0x7fffffffu) > 0x7f800000u
        ? 0.0f : saturate(strength);
    if (!(amount > 0.0f)) return scene;

    // The sRGB toe cannot cancel the gamma-2.2 lookup scale per channel: that
    // ratio collapses near black and erases lift and cross-channel grading.
    // Keep the gamma-2.2 response above the look's black, rescale it by one
    // luminance tonal ratio, and add the sRGB-decoded black separately.
    const float scale = Fo4HdrRgbRatioGuideScale(Fo4HdrLargestGradeChannel(scene));
    const float3 codes = Fo4HdrGradeCleanScene(weightedCodes);
    const float3 black = Fo4HdrGradeCleanScene(blackCodes);
    const float3 above = Fo4HdrGradeDecode(codes, Fo4HdrGradeCurveGamma22) -
        Fo4HdrGradeDecode(black, Fo4HdrGradeCurveGamma22);
    const float3 blackLight = Fo4HdrGradeDecode(black, Fo4HdrGradeCurveSrgb);
    const float response = dot(abs(above), Fo4HdrGradeLuma);
    const float reference = dot(Fo4HdrGradeDecode(
        Fo4HdrRgbRatioCoordinates(scene), Fo4HdrGradeCurveSrgb), Fo4HdrGradeLuma);
    // The above-black term vanishes as the scene approaches black, so exact
    // black keeps only the look's black even if its codes differ from texel 0.
    const float tonalRatio = response > 0.0f && reference > 0.0f
        ? dot(abs(Fo4HdrGradeDecode(codes, Fo4HdrGradeCurveSrgb) - blackLight), Fo4HdrGradeLuma) /
            response * (dot(scene * scale, Fo4HdrGradeLuma) / reference)
        : 0.0f;
    // A channel graded below the look's black could otherwise turn negative.
    const float3 restored = max(above * tonalRatio + blackLight, 0.0f) / scale;
    const float3 endpoint = min(restored * min(1.0f,
        65504.0f / max(Fo4HdrLargestGradeChannel(restored), 65504.0f)), 65504.0f);
    return lerp(scene, endpoint, amount);
}

#endif
