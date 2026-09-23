// SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneLayers.hlsli"

RWTexture2D<float4> PresentTarget : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= FrameSize)) return;
    float3 value = EvaluateFrameLayers(pixel.xy);
    if (FrameMode == 0u)
        value = saturate(value);
    else
    {
        if (FrameMode == 1u || FrameMode == 2u) value *= ReferenceWhite;
        // Compose in registers and encode directly into the presenting buffer:
        // no intermediate storage, rounding, or full-frame output copy.
        value = Fo4HdrEncodeAbsoluteLight(value);
    }
    PresentTarget[pixel.xy] = float4(value, 1.0f);
}
