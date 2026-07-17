// SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneLayers.hlsli"

Texture2D<float4> ScenePixels : register(t0);
Texture2D<float4> BackdropPixels : register(t1);
Texture2D<float4> HudPixels : register(t2);
RWTexture2D<float4> PresentTarget : register(u0);

cbuffer PresentFrame : register(b0)
{
    uint2 FrameSize;
    float DisplayPeakNits;
    uint SceneDomain;
    float UiWhiteNits;
    float3 Reserved;
};

[numthreads(8, 8, 1)]
void PrepareBackdrop(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= FrameSize)) return;
    PresentTarget[pixel.xy] = Fo4HdrPrepareAuthoredLayer(ScenePixels.Load(int3(pixel.xy, 0)), true);
}

[numthreads(8, 8, 1)]
void PrepareHud(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= FrameSize)) return;
    PresentTarget[pixel.xy] = Fo4HdrPrepareAuthoredLayer(ScenePixels.Load(int3(pixel.xy, 0)), false);
}

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= FrameSize)) return;
    // Domain 0 retains the 203-nit scene white; domain 1 is BT.2020 relative to UI white.
    const float3 scene = ScenePixels.Load(int3(pixel.xy, 0)).rgb;
    float3 light = SceneDomain == 1u ? scene * UiWhiteNits : Fo4HdrBt709ToBt2020Nits(scene, 203.0f);
    light = Fo4HdrBlendLinearLayer(light, BackdropPixels.Load(int3(pixel.xy, 0)), UiWhiteNits);
    light = Fo4HdrBlendLinearLayer(light, HudPixels.Load(int3(pixel.xy, 0)), UiWhiteNits);
    const float3 contained = Fo4HdrBoundLight(light, DisplayPeakNits);
    // Encode only at the terminal UAV; scene and layer storage remain linear FP16.
    PresentTarget[pixel.xy] = float4(Fo4HdrEncodeAbsoluteLight(contained), 1.0f);
}
