// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../../shaders/fallout4/GradeTransfer.hlsli"

Texture2D<float4> SceneInputs : register(t0);
RWTexture2D<float4> GradeOutputs : register(u0);
cbuffer GradeTransferCase : register(b0)
{
    uint2 CaseSize;
    float Strength;
    float Padding;
};

[numthreads(8, 1, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= CaseSize)) return;
    const float3 scene = SceneInputs.Load(int3(pixel.xy, 0)).rgb;
    const float3 result = Fo4HdrTransferRadialGrade(scene, float3(0.25f, 0.5f, 0.75f), Strength);
    GradeOutputs[pixel.xy] = float4(result, 1.0f);
}
