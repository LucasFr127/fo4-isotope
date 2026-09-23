// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../../shaders/tonemap/OperatorLut.hlsli"
#include "../../../shaders/fallout4/LiveSettingsConstants.hlsli"

Texture3D<float4> OperatorCoefficients : register(t7);
SamplerState OperatorSampler : register(s7);

Texture2D<float4> SceneValues : register(t0);
RWTexture2D<float4> MappedNits : register(u0);

cbuffer ReferenceDimensions : register(b0)
{
    uint2 CaseSize;
    uint2 Reserved;
};

[numthreads(8, 1, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= CaseSize)) return;
    const float3 scene = SceneValues.Load(int3(pixel.xy, 0)).rgb;
    MappedNits[pixel.xy] = float4(Fo4HdrSampleOperatorLut(OperatorCoefficients, OperatorSampler, scene,
        Fo4HdrProductionMode, Fo4HdrOperatorShaperAngleGain, Fo4HdrOperatorShaperBias,
        Fo4HdrDisplayPeakNits, Fo4HdrOperatorShaperParameters), 1.0f);
}
