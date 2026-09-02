// SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneLayers.hlsli"

Texture2D<float4> ScenePixels : register(t0);
Texture2D<float4> BackdropPixels : register(t1);
Texture2D<float4> HudPixels : register(t2);
Texture2D<float4> SettingsPixels : register(t3);
RWTexture2D<float4> PresentTarget : register(u0);

cbuffer PresentFrame : register(b0)
{
    uint2 FrameSize;
    float DisplayPeakNits;
    uint SceneDomain;
    float UiWhiteNits;
    uint SettingsPresent;
    uint CalibrationMode;
    uint Reserved;
};

[numthreads(8, 8, 1)]
void CopySdrCodes(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= FrameSize)) return;
    // Preserve authored display codes through the handoff; no linearization or PQ.
    PresentTarget[pixel.xy] = float4(saturate(ScenePixels.Load(int3(pixel.xy, 0)).rgb), 1.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= FrameSize)) return;
    // Domain 0 retains the 203-nit scene white; domain 1 is BT.2020 relative to UI white.
    const float3 scene = ScenePixels.Load(int3(pixel.xy, 0)).rgb;
    float3 light = SceneDomain == 1u ? scene * UiWhiteNits : Fo4HdrBt709ToBt2020Nits(scene, 203.0f);
    light = Fo4HdrCalibrationLight(CalibrationMode, pixel.xy, FrameSize,
        float2(DisplayPeakNits, UiWhiteNits), light);
    if (CalibrationMode < 10u || CalibrationMode > 13u)
    {
        light = Fo4HdrBlendAuthoredLayer(light, BackdropPixels.Load(int3(pixel.xy, 0)), UiWhiteNits, true);
        light = Fo4HdrBlendAuthoredLayer(light, HudPixels.Load(int3(pixel.xy, 0)), UiWhiteNits, false);
    }
    if (SettingsPresent != 0u)
    {
        light = Fo4HdrBlendAuthoredLayer(light, SettingsPixels.Load(int3(pixel.xy, 0)), UiWhiteNits, false);
    }
    const float3 contained = Fo4HdrBoundLight(light, DisplayPeakNits);
    // Encode only at the terminal UAV, after all contributions are linear light.
    PresentTarget[pixel.xy] = float4(Fo4HdrEncodeAbsoluteLight(contained), 1.0f);
}
