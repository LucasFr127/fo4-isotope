// SPDX-License-Identifier: GPL-3.0-or-later
#include "DisplayMath.hlsli"

Texture2D<float4> ScenePixels : register(t0);
RWTexture2D<float4> PresentTarget : register(u0);

cbuffer PresentFrame : register(b0)
{
    uint2 FrameSize;
    float DisplayPeakNits;
    float Reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= FrameSize)) return;
    // Linear scene white is fixed at 203 nits; peak containment belongs to the display boundary.
    const float3 scene = ScenePixels.Load(int3(pixel.xy, 0)).rgb;
    const float3 light = Fo4HdrExpandPrimaries(scene) * 203.0f;
    const float3 contained = Fo4HdrBoundLight(light, DisplayPeakNits);
    // Encode only at the terminal UAV; scene storage remains linear FP16.
    PresentTarget[pixel.xy] = float4(Fo4HdrEncodeAbsoluteLight(contained), 1.0f);
}
