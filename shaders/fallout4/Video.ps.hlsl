// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@68d2ab15e6086ab04084278ee8cceb85b8a8023e/src/games/fallout4/09_videos_0x0F2CC0D1.ps_5_0.hlsl
// Adaptation: retain native BGR sample order and alpha, decode sRGB, and transport
// linear BT.2020 relative to UI white using the independently selected video white.
// See THIRD_PARTY_NOTICES.md.

#include "../output/SceneLayers.hlsli"

SamplerState SampBase_s : register(s0);
Texture2D<float4> TexBase : register(t0);

cbuffer VideoWhite : register(b0)
{
    float VideoWhiteScale;
    float3 Reserved;
};

void main(
    float4 position : SV_POSITION0,
    float2 texcoord : TEXCOORD0,
    out float4 output : SV_Target0)
{
    const float4 sampled = TexBase.Sample(SampBase_s, texcoord);
    const float3 linearBt709 = Fo4HdrDecodeAuthoredCodes(saturate(sampled.zyx));
    output = float4(Fo4HdrExpandPrimaries(linearBt709) * VideoWhiteScale, sampled.a);
}
