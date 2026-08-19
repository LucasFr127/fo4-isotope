// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/color/gamma.hlsl
// Also src/shaders/color/rgb.hlsl and src/shaders/math/sign.hlsl at that pin.
// Completed-output peak scaling: src/shaders/draw.hlsl SwapChainPass at that pin.
// Adaptation: signed gamma 2.2 and BT.2020/BT.709 conversion around native
// encoded-domain composition, in-place SM5 FP16 UAV, b13 direction, and bounds.
// The forward primary conversion uses the standards-derived display math.
// See THIRD_PARTY_NOTICES.md for the retained MIT permission text.
#include "../output/DisplayMath.hlsli"

RWTexture2D<float4> Composition : register(u0);
cbuffer EncodedCompositionParameters : register(b13)
{
    uint Encode;
    float PeakUiRelative;
    uint2 Reserved;
};

// Product of RenoDX XYZ_TO_BT709_MAT and BT2020_TO_XYZ_MAT.
static const float3x3 kArmorBt2020ToBt709 =
{
    1.6604910020f, -0.5876411388f, -0.0728498633f,
    -0.1245504745f, 1.1328998971f, -0.0083494226f,
    -0.0181507634f, -0.1005788980f, 1.1187296614f
};

float3 SignedGamma(float3 value, float exponent)
{
    return asfloat((asuint(pow(abs(value), exponent)) & 0x7fffffffu)
        | (asuint(value) & 0x80000000u));
}

float ContainStorage(float value)
{
    // Emergency FP16 containment can lose channel ratios; alpha is untouched.
    if (isnan(value)) return 0.0f;
    return clamp(value, -65504.0f, 65504.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    Composition.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float4 value = Composition[id.xy];
    float3 transformed = Encode != 0
        ? SignedGamma(mul(kArmorBt2020ToBt709, value.rgb), 1.0f / 2.2f)
        : Fo4HdrExpandPrimaries(SignedGamma(value.rgb, 2.2f));
    if (Encode == 0 && PeakUiRelative > 0.0f)
    {
        // Scale completed RGB together: additive native codes may exceed one.
        const float maximum = max(max(transformed.r, transformed.g), transformed.b);
        transformed *= PeakUiRelative / max(PeakUiRelative, maximum);
    }
    value.rgb = float3(ContainStorage(transformed.r),
        ContainStorage(transformed.g), ContainStorage(transformed.b));
    Composition[id.xy] = value;
}
