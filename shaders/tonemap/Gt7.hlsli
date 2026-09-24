// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Polyphony Digital Inc.
// Upstream: https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp sha256=94df7e7e310b9423ebb5aaaa29416f376ebbae66e0948fb259f8688314b29822
// Fallout4HDR modification: translate the official default ICtCp/HDR color
// volume mapper to SM5 and adapt BT.709 scene units to its Rec.2020 framebuffer
// convention. The native BT.2020 path is the analytical GPU oracle.
// The complete Polyphony Digital MIT permission is in THIRD_PARTY_NOTICES.md.

#ifndef FO4HDR_GT7_INCLUDED
#define FO4HDR_GT7_INCLUDED

static const float kFo4HdrGt7ReferenceLuminance = 100.0f;
// The official HDR curve's fixed SDR-reference lower bound is 250.0 nits.

static const float3x3 kFo4HdrGt7Bt709ToBt2020 =
{
    {0.6274039149284363f, 0.3292830288410187f, 0.04331306740641594f},
    {0.06909728795289993f, 0.9195404052734375f, 0.01136231515556574f},
    {0.01639143936336040f, 0.08801330626010895f, 0.8955952525138855f}
};

float Fo4HdrGt7SmoothStep(float value, float edge0, float edge1)
{
    const float position = (value - edge0) / (edge1 - edge0);
    if (value < edge0) return 0.0f;
    if (value > edge1) return 1.0f;
    return position * position * (3.0f - 2.0f * position);
}

float3 Fo4HdrGt7InverseEotf(float3 framebufferValue)
{
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    precise float3 physical =
        max(framebufferValue, 0.0f) * kFo4HdrGt7ReferenceLuminance;
    precise float3 normalized = physical / 10000.0f;
    precise float3 powered = pow(abs(normalized), m1.xxx);
    precise float3 numerator = c1 + c2 * powered;
    precise float3 denominator = 1.0f + c3 * powered;
    // The official exp2(m2 * (log2(a) - log2(b))) expression loses
    // significant digits on SM5 transcendental units when a and b are close.
    // Its mathematically identical ratio-power form keeps ICtCp chroma stable.
    precise float3 ratio = numerator / denominator;
    precise float3 perceptual = pow(abs(ratio), m2.xxx);
    return perceptual;
}

float3 Fo4HdrGt7Eotf(float3 encoded)
{
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    precise float3 powered = pow(saturate(encoded), (1.0f / m2).xxx);
    precise float3 numerator = max(powered - c1, 0.0f);
    precise float3 denominator = c2 - c3 * powered;
    precise float3 luminance = numerator / denominator;
    luminance = pow(abs(luminance), (1.0f / m1).xxx);
    return (luminance * 10000.0f) / kFo4HdrGt7ReferenceLuminance;
}

float3 Fo4HdrGt7RgbToIctcp(float3 color)
{
    precise float3 lms = float3(
        (color.r * 1688.0f + color.g * 2146.0f + color.b * 262.0f) /
            4096.0f,
        (color.r * 683.0f + color.g * 2951.0f + color.b * 462.0f) /
            4096.0f,
        (color.r * 99.0f + color.g * 309.0f + color.b * 3688.0f) /
            4096.0f);
    precise float3 perceptual = Fo4HdrGt7InverseEotf(lms);
    // Each chroma row sums to zero. Subtracting the common LMS component
    // before applying the large coefficients avoids catastrophic cancellation.
    precise float lMinusM = perceptual.x - perceptual.y;
    precise float sMinusM = perceptual.z - perceptual.y;
    precise float3 result = float3(
        (perceptual.x + perceptual.y) * 0.5f,
        (6610.0f * lMinusM + 7003.0f * sMinusM) / 4096.0f,
        (17933.0f * lMinusM - 543.0f * sMinusM) / 4096.0f);
    return result;
}

float3 Fo4HdrGt7IctcpToRgb(float3 color)
{
    precise float3 perceptual = float3(
        color.x + 0.00860904f * color.y + 0.11103f * color.z,
        color.x - 0.00860904f * color.y - 0.11103f * color.z,
        color.x + 0.560031f * color.y - 0.320627f * color.z);
    precise float3 linearLms = Fo4HdrGt7Eotf(perceptual);
    precise float3 result = float3(
        3.43661f * linearLms.x - 2.50645f * linearLms.y +
            0.0698454f * linearLms.z,
        -0.79133f * linearLms.x + 1.9836f * linearLms.y -
            0.192271f * linearLms.z,
        -0.0259499f * linearLms.x - 0.0989137f * linearLms.y +
            1.12486f * linearLms.z);
    return max(result, 0.0f);
}

float Fo4HdrGt7Curve(float value, float framebufferPeak)
{
    const float alpha = 0.25f;
    const float midPoint = 0.538f;
    const float linearSection = 0.444f;
    const float toeStrength = 1.280f;
    const float curvature = (linearSection - 1.0f) / (alpha - 1.0f);
    const float shoulderA = framebufferPeak * linearSection +
        framebufferPeak * curvature;
    const float shoulderB = -framebufferPeak * curvature *
        exp(linearSection / curvature);
    const float shoulderC = -1.0f / (curvature * framebufferPeak);
    if (value < 0.0f) return 0.0f;

    const float linearWeight = Fo4HdrGt7SmoothStep(value, 0.0f, midPoint);
    const float shoulder = shoulderA + shoulderB * exp(value * shoulderC);
    if (value < linearSection * framebufferPeak)
    {
        const float toe = midPoint *
            pow(abs(value / midPoint), toeStrength);
        return (1.0f - linearWeight) * toe + linearWeight * value;
    }
    return shoulder;
}

float3 Fo4HdrGt7OfficialRec2020(
    float3 rec2020Framebuffer,
    float displayPeakNits)
{
    const float3 input = min(max(rec2020Framebuffer, 0.0f), 65504.0f);
    const float framebufferPeak =
        displayPeakNits / kFo4HdrGt7ReferenceLuminance;
    const float targetIntensity =
        Fo4HdrGt7RgbToIctcp(framebufferPeak.xxx).x;
    precise float3 original = Fo4HdrGt7RgbToIctcp(input);
    precise float3 skewed = float3(
        Fo4HdrGt7Curve(input.r, framebufferPeak),
        Fo4HdrGt7Curve(input.g, framebufferPeak),
        Fo4HdrGt7Curve(input.b, framebufferPeak));
    precise float3 skewedUcs = Fo4HdrGt7RgbToIctcp(skewed);
    const float fadeStart = 0.98f;
    const float fadeEnd = 1.16f;
    const float chromaScale = 1.0f - Fo4HdrGt7SmoothStep(
        original.x / targetIntensity, fadeStart, fadeEnd);
    precise float3 scaled = Fo4HdrGt7IctcpToRgb(float3(
        skewedUcs.x,
        original.y * chromaScale,
        original.z * chromaScale));
    const float blendRatio = 0.6f;
    precise float3 blended =
        (1.0f - blendRatio) * skewed + blendRatio * scaled;
    return min(blended, framebufferPeak.xxx);
}

float3 Fo4HdrGt7Bt2020AbsoluteNits(
    float3 sceneLinear,
    float displayPeakNits,
    float sceneReferenceWhiteNits)
{
    const float3 sanitized = min(max(sceneLinear, 0.0f), 65504.0f);
    precise float3 sceneRec2020 = mul(kFo4HdrGt7Bt709ToBt2020, sanitized);
    precise float3 framebuffer = sceneRec2020 *
        (sceneReferenceWhiteNits / kFo4HdrGt7ReferenceLuminance);
    precise float3 mapped = Fo4HdrGt7OfficialRec2020(
        framebuffer, displayPeakNits);
    precise float3 absoluteRec2020 =
        mapped * kFo4HdrGt7ReferenceLuminance;
    return min(max(absoluteRec2020, 0.0f), displayPeakNits.xxx);
}

#endif
