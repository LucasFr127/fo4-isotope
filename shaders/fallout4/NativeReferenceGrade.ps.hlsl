// SPDX-License-Identifier: GPL-3.0-or-later
#include "GradeTransfer.hlsli"
#include "NativeReferenceGrade.hlsli"
#include "ImageControls.hlsli"
#include "SelectiveColor.hlsli"

Texture2D<float4> SceneTexture : register(t0);
SamplerState SceneSampler : register(s0);

void main(
    float4 position : SV_POSITION0,
    float2 texcoord : TEXCOORD0,
    out float4 output : SV_Target0)
{
    const float4 source = SceneTexture.Sample(SceneSampler, texcoord);
    const float3 scene = Fo4HdrGradeCleanScene(source.rgb);
    float3 result = Fo4HdrNativeReferenceGrade(scene, VanillaColorGradingStrength);
    result = Fo4HdrApplyImageControls(result);
    result = Fo4HdrApplySelectiveColor(result);
    [branch]
    if (OperatorMode != 0u)
        result = Fo4HdrSampleOperatorLut(result) / OperatorUiWhiteNits;
    output = float4(result, source.a);
}
