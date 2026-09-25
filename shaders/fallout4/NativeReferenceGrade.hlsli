// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#ifndef FO4HDR_NATIVE_REFERENCE_GRADE_HLSLI
#define FO4HDR_NATIVE_REFERENCE_GRADE_HLSLI

#include "../tonemap/OperatorLut.hlsli"

Texture3D<float4> PreparedLook : register(t3);

float Largest(float3 rgb) { return max(rgb.x, max(rgb.y, rgb.z)); }
static const float3 GradeLuma = float3(0.212500006f, 0.715399981f, 0.0720999986f);

float3 CleanScene(float3 rgb)
{
    [unroll] for (uint channel = 0; channel < 3; ++channel)
        rgb[channel] = Fo4HdrSanitizeOperatorInput(rgb[channel], 0u);
    return rgb;
}

float3 BoundScene(float3 rgb)
{
    // Contract negative channels toward neutral at constant luminance instead
    // of clipping their hue correction independently.
    const float luminance = dot(rgb, GradeLuma);
    if (luminance <= 0.0f) return 0.0f;
    const float minimum = min(rgb.r, min(rgb.g, rgb.b));
    if (minimum < 0.0f)
        rgb = luminance + (rgb - luminance) * (luminance / (luminance - minimum));
    rgb = max(rgb, 0.0f);
    // Uniform scaling can round its maximum one FP32 ULP above the FP16 limit.
    return min(rgb * min(1.0f, 65504.0f / max(Largest(rgb), 65504.0f)), 65504.0f);
}

float NativeCurve(float value)
{
    // Factoring the native rational curve avoids cancellation at black.
    return value * (0.14f * value + 1.0f / 60.0f) /
        (value * (0.15f * value + 0.5f) + 0.06f);
}

float3 NativeDecode(float3 codes)
{
    return codes <= 0.04045f ? codes / 12.92f : pow(max((codes + 0.055f) / 1.055f, 0.0f), 2.4f);
}

float3 NativeEncode(float3 value)
{
    return value <= 0.0031308f ? value * 12.92f :
        1.055f * pow(max(value, 0.0f), 1.0f / 2.4f) - 0.055f;
}

float3 NativeCoordinates(float3 scene)
{
    // Keep the native per-channel Hable/gamma lookup below a one-stop safety
    // margin. Above it, reduce only lookup exposure with a C1 shoulder tending
    // to native white (5.6); the retained HDR scene never enters this shoulder.
    const float peak = Largest(scene);
    const float scale = peak > 2.8f ? (5.6f - 7.84f / peak) / peak : 1.0f;
    const float3 probe = scene * scale;
    const float3 reference = float3(NativeCurve(2.0f * probe.r),
        NativeCurve(2.0f * probe.g), NativeCurve(2.0f * probe.b)) / NativeCurve(11.2f);
    return pow(saturate(reference), 1.0f / 2.2f);
}

float3 NativeLerp(float3 a, float3 b, float3 fraction)
{
    return mad(fraction, b - a, a);
}

float3 NativeTrilinear(Texture3D<float4> look, float3 codes)
{
    const float3 position = saturate(codes) * 15.0f;
    const int3 cell = min(int3(position), 14);
    const float3 fraction = position - float3(cell);
    // Fixed-function 3D filtering can shift an identity look by one fractional
    // step on hardware. Interpolate the original nodes in FP32 so that error
    // cannot become a false hue correction when transferred back to HDR.
    return NativeLerp(
        NativeLerp(NativeLerp(look.Load(int4(cell, 0)).rgb,
                  look.Load(int4(cell + int3(1, 0, 0), 0)).rgb, fraction.x),
             NativeLerp(look.Load(int4(cell + int3(0, 1, 0), 0)).rgb,
                  look.Load(int4(cell + int3(1, 1, 0), 0)).rgb, fraction.x), fraction.y),
        NativeLerp(NativeLerp(look.Load(int4(cell + int3(0, 0, 1), 0)).rgb,
                  look.Load(int4(cell + int3(1, 0, 1), 0)).rgb, fraction.x),
             NativeLerp(look.Load(int4(cell + int3(0, 1, 1), 0)).rgb,
                  look.Load(int4(cell + int3(1, 1, 1), 0)).rgb, fraction.x), fraction.y), fraction.z);
}

float3 NativeReference(float3 codes)
{
    // Match the floating-point identity nodes and interpolation used by the
    // native sampler. A continuous ideal coordinate is a different reference
    // by a few ULPs, which HDR extrapolation can otherwise turn into color.
    static const float ramp[16] = {0.0f, 1.0f/15.0f, 2.0f/15.0f, 3.0f/15.0f,
        4.0f/15.0f, 5.0f/15.0f, 6.0f/15.0f, 7.0f/15.0f,
        8.0f/15.0f, 9.0f/15.0f, 10.0f/15.0f, 11.0f/15.0f,
        12.0f/15.0f, 13.0f/15.0f, 14.0f/15.0f, 1.0f};
    const float3 position = saturate(codes) * 15.0f;
    const int3 cell = min(int3(position), 14);
    const float3 fraction = position - float3(cell);
    return NativeDecode(NativeLerp(float3(ramp[cell.x], ramp[cell.y], ramp[cell.z]),
        float3(ramp[cell.x+1], ramp[cell.y+1], ramp[cell.z+1]), fraction));
}

float3 NativeWeightedCodes(float3 codes)
{
    return NativeTrilinear(PreparedLook, codes);
}

float GradeConfidence()
{
    // Do not amplify a vanishing sampled chroma difference. The second probe
    // carries the full residual, so regularizing this split loses no affine color.
    return 1.0e-3f;
}

float NeutralPivot(float3 reference)
{
    // Leave a measurable two-sided chroma span even beside cube black/white.
    // This bounds only the auxiliary probe; the native guide stays unchanged.
    return clamp(dot(reference, GradeLuma), 0.002f, 0.998f);
}

float4 ChromaProbe(float3 H, float3 R, float epsilon, float pivot, out float along)
{
    const float y = dot(R, GradeLuma);
    const float3 r = R - y;
    const float3 difference = (H - dot(H, GradeLuma)) - r;
    along = dot(r, difference) / (dot(r, r) + epsilon * epsilon);
    const float3 residual = difference - along * r;
    const float magnitude = Largest(abs(residual));
    if (!(magnitude > 0.0f)) return float4(pivot.xxx, 0.0f);

    // Probe the missing target direction instead of inferring its response
    // from a rotated copy of the first chroma secant. Half the available
    // headroom keeps the probe inside the authored cube.
    const float3 direction = residual / magnitude;
    const float positive = max(Largest(direction), 1.0e-20f);
    const float negative = max(Largest(-direction), 1.0e-20f);
    const float headroom = min(1.0f, min((1.0f - pivot) / positive, pivot / negative));
    // Keep a measurable span around neutral without differentiating a tiny
    // sampled color difference. Cube boundaries still take precedence.
    const float radius = min(max(Largest(abs(r)), 1.0e-3f), 0.5f * headroom);
    return float4(pivot + radius * direction, magnitude / radius);
}

float3 NativeReferenceTransfer(float3 H, float3 R, float3 G, float3 N, float3 B,
    float3 P, float3 Q, float along, float across, float3 U)
{
    const float referenceY = dot(R, GradeLuma);
    if (referenceY <= 0.0f) return B;
    const float sceneY = dot(H, GradeLuma);
    const float pivotY = dot(U, GradeLuma);
    const float3 neutralResponse = N - B;
    const float gain = dot(neutralResponse, GradeLuma) / pivotY;
    const float3 neutralResidual = neutralResponse - gain * U;
    const float3 colorResidual = (G - B) - gain * R;
    const float3 probeResidual = (Q - B) - gain * P;
    // Factor the shared neutral-axis correction before rebuilding RGB.
    const float neutralScale = ((sceneY - referenceY) - along * referenceY -
        across * dot(P, GradeLuma)) / pivotY;
    return BoundScene(G + gain * (H - R) + neutralScale * neutralResidual +
        along * colorResidual + across * probeResidual);
}

float3 NativeNeutralCodes(float code)
{
    const float position = saturate(code) * 15.0f;
    const int cell = min(int(position), 14);
    const float t = position - float(cell);
    const float3 a = PreparedLook.Load(int4(16, cell, 0, 0)).rgb;
    const float3 b = PreparedLook.Load(int4(17, cell, 0, 0)).rgb;
    const float3 c = PreparedLook.Load(int4(18, cell, 0, 0)).rgb;
    const float3 d = PreparedLook.Load(int4(19, cell, 0, 0)).rgb;
    return mad(t, mad(t, mad(t, d, c), b), a);
}

float3 Fo4HdrNativeReferenceGrade(float3 input, float strength)
{
    const float3 scene = CleanScene(input);
    const float amount = (asuint(strength) & 0x7fffffffu) > 0x7f800000u
        ? 0.0f : saturate(strength);
    if (!(amount > 0.0f)) return scene;

    const float3 codes = NativeCoordinates(scene);
    // Both sides of the native look are decoded as sRGB. Keeping the decoded
    // ungraded reference makes an identity look transfer no tonal response.
    const float3 reference = NativeReference(codes);
    const float3 graded = CleanScene(NativeDecode(CleanScene(NativeWeightedCodes(codes))));
    const float3 black = PreparedLook.Load(int4(16, 15, 0, 0)).rgb;
    const float3 neutralCodes = NativeEncode(NeutralPivot(reference).xxx);
    const float3 neutralReference = NativeReference(neutralCodes);
    const float3 neutral = CleanScene(NativeDecode(CleanScene(NativeNeutralCodes(neutralCodes.x))));
    float along;
    const float4 probe = ChromaProbe(scene, reference, GradeConfidence(), neutralReference.x, along);
    const float3 probeCodes = NativeEncode(probe.rgb);
    const float3 probeReference = NativeReference(probeCodes);
    float3 second = neutral;
    [branch] if (probe.w > 0.0f)
        second = CleanScene(NativeDecode(CleanScene(NativeWeightedCodes(probeCodes))));
    const float3 endpoint = NativeReferenceTransfer(scene, reference, graded, neutral, black,
        probeReference, second, along, probe.w, neutralReference);
    // H + amount*(endpoint-H) loses a small constant look at large H.
    // Preserve the full-strength endpoint before allowing arithmetic fusion.
    [branch] if (amount >= 1.0f) return endpoint;
    return mad(amount, endpoint, (1.0f - amount) * scene);
}

#endif
