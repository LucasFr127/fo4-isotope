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

float3 Fo4HdrTransferRadialGrade(float3 scene, float3 weightedCodes)
{
    // Decode the weighted codes once, then restore their common scene scale.
    const float3 decoded = pow(Fo4HdrGradeCleanScene(weightedCodes), 2.2f);
    const float3 restored = decoded / Fo4HdrRadialGuideScale(Fo4HdrLargestGradeChannel(scene));
    // Uniform scaling may round one FP32 ULP above the FP16 limit.
    return min(restored * min(1.0f,
        65504.0f / max(Fo4HdrLargestGradeChannel(restored), 65504.0f)), 65504.0f);
}

#endif
