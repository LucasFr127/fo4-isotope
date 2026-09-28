// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/09_videos_0x0F2CC0D1.ps_5_0.hlsl
// Whole-file simplified adaptation for FO4Isotope. Modification: retain the
// Fallout 4 BGR sample order, decode the SDR sRGB signal to linear, and map
// SDR video white to the independently configured video white, convert linear
// BT.709 to BT.2020, and publish the UI-relative wide-gamut HDR intermediate.
// RenoDRT and its broader configurable closure are excluded.
// See THIRD_PARTY_NOTICES.md.

#include "../output/DisplayMath.hlsli"
#include "LiveSettingsConstants.hlsli"

SamplerState SampBase_s : register(s0);
Texture2D<float4> TexBase : register(t0);

float3 Fo4HdrSrgbDecode(float3 encodedColor)
{
    const float3 low = encodedColor / 12.92f;
    const float3 high = pow(
        max((encodedColor + 0.055f) / 1.055f, 0.0f),
        2.4f);
    return lerp(high, low, encodedColor <= 0.04045f);
}

void main(
    float4 position : SV_POSITION0,
    float2 texcoord : TEXCOORD0,
    out float4 output : SV_Target0)
{
    const float4 sampled = TexBase.Sample(SampBase_s, texcoord);
    const float3 linearBt709 = Fo4HdrSrgbDecode(saturate(sampled.zyx));
    const float uiRelativeScale =
        Fo4HdrVideoWhiteNits /
        max(Fo4HdrUiTransportWhiteNits, 0.001f);
    output = float4(
        Fo4HdrExpandPrimaries(linearBt709) * uiRelativeScale,
        sampled.a);
}
