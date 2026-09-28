// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "LiveSettingsConstants.hlsli"
#include "ImageControls.hlsli"
#include "SelectiveColor.hlsli"
#include "GradeTransfer.hlsli"
#include "NativeReferenceGrade.hlsli"

Texture2D<float4> SceneTexture : register(t0);
Texture3D<float4> OperatorLut : register(t7);
SamplerState SceneSampler : register(s0);
SamplerState OperatorLutSampler : register(s7);

cbuffer Fallout4LutConstants : register(b2)
{
    float4 cb2[2];
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const float4 source = SceneTexture.Sample(SceneSampler, uv);
    const float3 hdrLinear = Fo4HdrGradeCleanScene(source.rgb);
    float3 result = Fo4HdrNativeReferenceGrade(
        hdrLinear, Fo4HdrVanillaGradeStrength);
    float luminanceRemoval;
    float colorRemoval;
    Fo4HdrResolveVanillaGradeRemoval(cb2[1], Fo4HdrLutProfilesActive,
        Fo4HdrVanillaGradeLuminanceRemoval,
        Fo4HdrVanillaGradeColorRemoval,
        Fo4HdrLutProfileLuminanceRemoval,
        Fo4HdrLutProfileColorRemoval, luminanceRemoval, colorRemoval);
    result = Fo4HdrApplyVanillaGradeRemoval(
        hdrLinear, result, luminanceRemoval, colorRemoval);
    result = Fo4HdrApplyImageControls(result);
    result = Fo4HdrApplySelectiveColor(result);
    // Grading retains scene-linear BT.709; the complete display operator owns
    // highlight convergence and the conversion to absolute linear BT.2020.
    if (Fo4HdrProductionMode >= 1u && Fo4HdrProductionMode <= 3u)
    {
        result = Fo4HdrSampleOperatorLut(OperatorLut, OperatorLutSampler,
            result, Fo4HdrProductionMode, Fo4HdrOperatorShaperAngleGain,
            Fo4HdrOperatorShaperBias, Fo4HdrDisplayPeakNits,
            Fo4HdrOperatorShaperParameters) /
            Fo4HdrUiTransportWhiteNits;
    }
    return float4(result, source.a);
}
