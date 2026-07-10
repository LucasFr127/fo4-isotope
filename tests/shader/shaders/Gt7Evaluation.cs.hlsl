// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../../shaders/tonemap/Gt7.hlsli"

Texture2D<float4> FramebufferValues : register(t0);
RWTexture2D<float4> MappedValues : register(u0);

cbuffer ReferenceCase : register(b0)
{
    uint2 CaseSize;
    float DisplayPeakNits;
    float Padding;
};

[numthreads(8, 1, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= CaseSize)) return;
    const float3 raw = FramebufferValues.Load(int3(pixel.xy, 0)).rgb;
    MappedValues[pixel.xy] = float4(Fo4HdrGt7OfficialRec2020(raw, DisplayPeakNits), 1.0);
}
