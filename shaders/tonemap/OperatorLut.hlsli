// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FALLOUT4HDR_OPERATOR_LUT_HLSLI
#define FALLOUT4HDR_OPERATOR_LUT_HLSLI

cbuffer Fallout4OperatorConstants : register(b13)
{
    uint OperatorMode;
    float OperatorDisplayPeakNits;
    float OperatorUiWhiteNits;
    uint OperatorInputDomain;
    float4 OperatorShaper;
    float VanillaColorGradingStrength;
    float Fo4HdrContrastSlope;
    float Fo4HdrBlacksStrength;
    float Fo4HdrShadowsEv;
    float Fo4HdrHighlightsEv;
    float Fo4HdrSaturation;
    float2 GradePadding;
    float4 Fo4HdrColorAdaptationRow0;
    float4 Fo4HdrColorAdaptationRow1;
    float4 Fo4HdrColorAdaptationRow2;
    uint Fo4HdrSelectiveColorActive;
    uint3 SelectiveColorPadding;
    float4 Fo4HdrSelectiveRed;
    float4 Fo4HdrSelectiveOrange;
    float4 Fo4HdrSelectiveYellow;
    float4 Fo4HdrSelectiveGreen;
    float4 Fo4HdrSelectiveCyan;
    float4 Fo4HdrSelectiveBlue;
    float4 Fo4HdrSelectiveMagenta;
    float3 Fo4HdrSelectivePlaneA;
    float VanillaGradeLuminanceRemoval;
    float3 Fo4HdrSelectivePlaneB;
    float VanillaGradeColorRemoval;
}

Texture3D<float4> OperatorLut : register(t7);
SamplerState OperatorSampler : register(s7);

// These dimensions must match the CPU lattice and coefficient texture.
static const float kFo4HdrAces2OperatorLutEdge = 128.0f;
static const float kFo4HdrAgxOperatorLutEdge = 92.0f;
static const float kFo4HdrGt7OperatorLutEdge = 64.0f;

float Fo4HdrSanitizeOperatorInput(float value, uint mode)
{
    // Bit tests retain the explicit NaN/Inf policy under FXC's optimized arithmetic.
    const uint magnitude = asuint(value) & 0x7fffffffu;
    if (magnitude > 0x7f800000u ||
        (mode == 1u &&
            magnitude == 0x7f800000u)) return 0.0f;
    return clamp(value, 0.0f, 65504.0f);
}

void Fo4HdrGroupedOperatorCoefficient(
    float position, float logicalEdge, float textureEdge,
    out float2 coordinates, out float2 weights)
{
    const float slice = min(floor(position),
        logicalEdge - 2.0f);
    const float t = position - slice;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float oneMinusT = 1.0f - t;
    const float4 cubic = float4(
        oneMinusT * oneMinusT * oneMinusT / 6.0f,
        (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f,
        (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f,
        t3 / 6.0f);
    weights = float2(cubic.x + cubic.y, cubic.z + cubic.w);
    const float2 physical = float2(
        slice + cubic.y / weights.x,
        slice + 2.0f + cubic.w / weights.y);
    coordinates = (physical + 0.5f) / textureEdge;
}

float3 Fo4HdrSampleOperatorCoefficientLut(
    Texture3D<float4> lut, SamplerState samplerState, float3 shaped,
    float logicalEdge, float textureEdge)
{
    float2 redCoordinates, redWeights;
    float2 greenCoordinates, greenWeights;
    float2 blueCoordinates, blueWeights;
    Fo4HdrGroupedOperatorCoefficient(
        shaped.r * (logicalEdge - 1.0f), logicalEdge, textureEdge,
        redCoordinates, redWeights);
    Fo4HdrGroupedOperatorCoefficient(
        shaped.g * (logicalEdge - 1.0f), logicalEdge, textureEdge,
        greenCoordinates, greenWeights);
    Fo4HdrGroupedOperatorCoefficient(
        shaped.b * (logicalEdge - 1.0f), logicalEdge, textureEdge,
        blueCoordinates, blueWeights);
    float3 mapped = 0.0f;
    [unroll]
    for (int blueTap = 0; blueTap < 2; ++blueTap)
    {
        [unroll]
        for (int greenTap = 0; greenTap < 2; ++greenTap)
        {
            [unroll]
            for (int redTap = 0; redTap < 2; ++redTap)
                mapped += redWeights[redTap] * greenWeights[greenTap] *
                    blueWeights[blueTap] * lut.SampleLevel(samplerState,
                        float3(redCoordinates[redTap],
                            greenCoordinates[greenTap],
                            blueCoordinates[blueTap]), 0.0f).rgb;
        }
    }
    return mapped;
}

float Fo4HdrGamutScale(float limit, float knee)
{
    if (knee <= 0.0f) return saturate(limit);
    // min-minus-correction avoids cancelling a huge peak/dark-channel ratio.
    const float blend = saturate(1.0f - abs(limit - 1.0f) / knee);
    return saturate(min(limit, 1.0f) - 0.25f * knee * blend * blend);
}

// Match the CPU AgX display gamut after interpolation: preserve luminance while
// removing negative channels, then preserve RGB ratios at the display peak.
float3 Fo4HdrAgxDisplayGamut(float3 value, float peak)
{
    const float luminance = dot(value, float3(0.2627f, 0.6780f, 0.0593f));
    const float center = max(luminance, 0.0f);
    const float3 chroma = value - luminance;
    const float minimumChroma = min(chroma.r, min(chroma.g, chroma.b));
    const float lowerScale = minimumChroma < 0.0f ?
        Fo4HdrGamutScale(center / -minimumChroma, 0.05f) : 1.0f;
    value = center + lowerScale * chroma;
    const float maximum = max(value.r, max(value.g, value.b));
    const float minimum = min(value.r, min(value.g, value.b));
    if (maximum <= 0.0f) return 0.0f;
    const float knee = 0.05f * saturate((maximum - minimum) / maximum);
    return clamp(value * Fo4HdrGamutScale(peak / maximum, knee), 0.0f, peak);
}

// The full-FP16-range shaper matches the CPU lattice coordinates.
float3 Fo4HdrSampleOperatorLut(float3 sceneLinear)
{
    const float kInputOffset = 1.0f / 65536.0f;
    const float kEdge = OperatorMode == 1u ?
        kFo4HdrAces2OperatorLutEdge : (OperatorMode == 2u ?
            kFo4HdrAgxOperatorLutEdge : kFo4HdrGt7OperatorLutEdge);

    sceneLinear = float3(
        Fo4HdrSanitizeOperatorInput(sceneLinear.r, OperatorMode),
        Fo4HdrSanitizeOperatorInput(sceneLinear.g, OperatorMode),
        Fo4HdrSanitizeOperatorInput(sceneLinear.b, OperatorMode));
    // The exact black endpoint bypasses the coefficient spline, which
    // otherwise reconstructs black by cancelling quantized coefficients with
    // adapter-dependent residuals. Preserve only exact black, never a dark band
    // or an individual zero channel of a colored input.
    if (all(sceneLinear == 0.0f)) return 0.0f.xxx;
    float3 encoded = log2(sceneLinear + kInputOffset);
    const float center = OperatorShaper.z;
    const float width = OperatorMode == 3u ? 2.5f : 3.0f;
    const float3 shaped = saturate(atan((encoded - center) / width) *
        OperatorShaper.x + encoded * OperatorShaper.w + OperatorShaper.y);
    float3 mapped = 0.0f;
    [branch]
    if (OperatorMode >= 1u && OperatorMode <= 3u)
    {
        // Eight grouped trilinear reads reconstruct the padded coefficient
        // lattice for the complete selected operator.
        mapped = Fo4HdrSampleOperatorCoefficientLut(
            OperatorLut, OperatorSampler, shaped, kEdge,
            kEdge + 2.0f);
    }
    else
    {
        // Unknown modes fail closed; never alias another operator.
        mapped = 0.0f;
    }
    // The source operators' final display clamp belongs here, not at lattice
    // vertices: clipping first creates artificial interpolation knees.
    if (OperatorMode == 2u)
        return Fo4HdrAgxDisplayGamut(mapped, OperatorDisplayPeakNits);
    return clamp(mapped, 0.0f, OperatorDisplayPeakNits);
}

#endif
