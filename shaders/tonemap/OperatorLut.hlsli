// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#ifndef FO4ISOTOPE_OPERATOR_LUT_HLSLI
#define FO4ISOTOPE_OPERATOR_LUT_HLSLI

// SDR tables hold relative BT.709 with white at 1 instead of BT.2020 nits.
#ifndef FO4HDR_SDR_OPERATOR_OUTPUT
#define FO4HDR_SDR_OPERATOR_OUTPUT 0
#endif

// These dimensions and the ACES2 shaper width must match the CPU lattice and
// coefficient texture.
#if FO4HDR_SDR_OPERATOR_OUTPUT
static const float kFo4HdrAces2OperatorLutEdge = 58.0f;
static const float kFo4HdrAgxOperatorLutEdge = 49.0f;
static const float kFo4HdrGt7OperatorLutEdge = 53.0f;
static const float kFo4HdrAces2ShaperWidth = 3.0f;
#else
static const float kFo4HdrAces2OperatorLutEdge = 94.0f;
static const float kFo4HdrAgxOperatorLutEdge = 66.0f;
static const float kFo4HdrGt7OperatorLutEdge = 64.0f;
static const float kFo4HdrAces2ShaperWidth = 2.0f;
#endif


float Fo4HdrSanitizeOperatorInput(float value, uint productionMode)
{
    // Bit tests retain the explicit NaN/Inf policy under FXC's optimized arithmetic.
    const uint magnitude = asuint(value) & 0x7fffffffu;
    if (magnitude > 0x7f800000u ||
        (productionMode == 1u &&
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

// Match ApplyAgxDisplayGamut after interpolation: preserve luminance while
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

#if FO4HDR_SDR_OPERATOR_OUTPUT
// Match WhiteChromaScale: the knee'd chroma scale about the luminance center that
// brings the brightest channel to white.
float Fo4HdrSdrWhiteChromaScale(float center, float3 chroma)
{
    const float maximumChroma = max(chroma.r, max(chroma.g, chroma.b));
    return maximumChroma > 0.0f ?
        Fo4HdrGamutScale((1.0f - center) / maximumChroma, 0.05f) : 1.0f;
}

// Match ContainSdrOperatorOutput for GT7: the official per-channel clip at white
// in BT.2020, then BT.709 chroma contracted toward luminance with the 5% knee
// until it fits, keeping luminance below white. These are the CPU matrices.
float3 Fo4HdrSdrGt7Containment(float3 value)
{
    const float3x3 toBt2020 = {
        0.627403895934699f, 0.329283038377884f, 0.043313065687417f,
        0.069097289358232f, 0.919540395075459f, 0.011362315566309f,
        0.016391438875150f, 0.088013307877226f, 0.895595253247624f
    };
    const float3x3 toBt709 = {
        1.6604909457791666f, -0.5876410948776531f, -0.0728498646690886f,
        -0.12455046636659303f, 1.1328998802803918f, -0.008349422033247976f,
        -0.01815076427030725f, -0.10057889487266629f, 1.118729662272663f
    };
    const float3 clipped = mul(toBt709, saturate(mul(toBt2020, value)));
    const float luminance = dot(clipped, float3(0.2126f, 0.7152f, 0.0722f));
    if (luminance >= 1.0f) return 1.0f;
    const float center = max(luminance, 0.0f);
    const float3 chroma = clipped - luminance;
    const float minimumChroma = min(chroma.r, min(chroma.g, chroma.b));
    const float lowerScale = minimumChroma < 0.0f ?
        Fo4HdrGamutScale(center / -minimumChroma, 0.05f) : 1.0f;
    const float scale = min(lowerScale, Fo4HdrSdrWhiteChromaScale(center, chroma));
    return saturate(center + scale * chroma);
}

// Match ContainSdrOperatorOutput for AgX: each channel soft-clips at zero, with
// a knee 5% of BT.709 luminance wide, and scales back to that luminance; chroma
// then contracts toward it until the brightest channel fits.
float3 Fo4HdrSdrDisplayGamut(float3 value)
{
    const float3 weights = float3(0.2126f, 0.7152f, 0.0722f);
    const float luminance = dot(value, weights);
    if (luminance <= 0.0f) return 0.0f;
    if (luminance >= 1.0f) return 1.0f;
    const float knee = 0.05f * luminance;
    const float3 blend = saturate(1.0f - abs(value) / knee);
    const float3 lifted = max(value, 0.0f) + 0.25f * knee * blend * blend;
    const float3 chroma = lifted * (luminance / dot(lifted, weights)) - luminance;
    return saturate(luminance + Fo4HdrSdrWhiteChromaScale(luminance, chroma) * chroma);
}
#endif

// The full-FP16-range shaper matches the CPU lattice coordinates.
float3 Fo4HdrSampleOperatorLut(
    Texture3D<float4> operatorLut,
    SamplerState operatorSampler,
    float3 sceneLinear,
    uint productionMode,
    float shaperAngleGain,
    float shaperBias,
    float displayPeakNits, float2 shaperParameters)
{
    const float kInputOffset = 1.0f / 65536.0f;
    const float kEdge = productionMode == 1u ?
        kFo4HdrAces2OperatorLutEdge : (productionMode == 2u ?
            kFo4HdrAgxOperatorLutEdge : kFo4HdrGt7OperatorLutEdge);

    sceneLinear = float3(
        Fo4HdrSanitizeOperatorInput(sceneLinear.r, productionMode),
        Fo4HdrSanitizeOperatorInput(sceneLinear.g, productionMode),
        Fo4HdrSanitizeOperatorInput(sceneLinear.b, productionMode));
    // The production black endpoint bypasses the coefficient spline, which
    // otherwise reconstructs black by cancelling quantized coefficients with
    // adapter-dependent residuals. Preserve only exact black, never a dark band
    // or an individual zero channel of a colored input.
    if (all(sceneLinear == 0.0f)) return 0.0f.xxx;
    float3 encoded = log2(sceneLinear + kInputOffset);
    const float center = shaperParameters.x;
    const float width = productionMode == 3u ? 2.5f :
        (productionMode == 1u ? kFo4HdrAces2ShaperWidth : 3.0f);
    const float3 shaped = saturate(atan((encoded - center) / width) *
        shaperAngleGain + encoded * shaperParameters.y + shaperBias);
    float3 mapped = 0.0f;
    [branch]
    if (productionMode >= 1u && productionMode <= 3u)
    {
        // Eight grouped trilinear reads reconstruct the padded coefficient
        // lattice for the complete selected operator.
        mapped = Fo4HdrSampleOperatorCoefficientLut(
            operatorLut, operatorSampler, shaped, kEdge,
            kEdge + 2.0f);
    }
    else
    {
        // Unknown production modes fail closed; never alias another operator.
        mapped = 0.0f;
    }
    // The source operators' final display clamp belongs here, not at lattice
    // vertices: clipping first creates artificial interpolation knees.
#if FO4HDR_SDR_OPERATOR_OUTPUT
    // ACES2 clamps like its Rec.709 preset, and GT7 clips like its official sample.
    if (productionMode == 2u) return Fo4HdrSdrDisplayGamut(mapped);
    return productionMode == 3u ? Fo4HdrSdrGt7Containment(mapped) : saturate(mapped);
#else
    if (productionMode == 2u)
        return Fo4HdrAgxDisplayGamut(mapped, displayPeakNits);
    return clamp(mapped, 0.0f, displayPeakNits);
#endif
}

#endif
