// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../shaders/tonemap/OperatorLut.hlsli"

// Test only: exercise the production lookup without creative grading, UI
// normalization, FP16 input quantization, or final PQ hiding sampler errors.
Texture2D<float4> SceneInputs : register(t0);
Texture3D<float4> OperatorLut : register(t7);
SamplerState OperatorSampler : register(s7);
RWTexture2D<float4> OperatorOutputs : register(u0);

cbuffer TestBatchConstants : register(b0)
{
    uint BatchWidth;
    uint SampleCount;
    uint ProductionMode;
    float ShaperAngleGain;
    float ShaperBias;
    float DisplayPeakNits;
    float2 ShaperParameters;
}

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint index = thread.x + thread.y * BatchWidth;
    if (thread.x >= BatchWidth || index >= SampleCount) return;
    const float4 input = SceneInputs.Load(int3(thread.xy, 0));
    OperatorOutputs[thread.xy] = float4(Fo4HdrSampleOperatorLut(
        OperatorLut, OperatorSampler, input.rgb, ProductionMode,
        ShaperAngleGain, ShaperBias, DisplayPeakNits, ShaperParameters), 1.0f);
}
