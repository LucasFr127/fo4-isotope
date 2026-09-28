// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Test-only observation of the actual guide and weighted texture samples.
// Reuse the shipping sampling stage, but stop before decode/grade transfer;
// the C++ oracle evaluates those stages independently from these readbacks.
#define main Fo4HdrUnusedCreativeGradeMain
#include "../../../shaders/fallout4/CreativeGradeLut.ps.hlsl"
#undef main

void main(float4 position : SV_POSITION0, float2 texcoord : TEXCOORD0,
    out float4 sampled : SV_Target0, out float4 guide : SV_Target1)
{
    const float3 source = Fo4HdrGradeCleanScene(SceneTexture.Sample(SceneSampler,texcoord).rgb);
    const float3 linearGuide = source * Fo4HdrRadialGuideScale(Fo4HdrLargestGradeChannel(source));
    sampled = float4(Fo4HdrSampleWeightedGrade(source),1.0f);
    guide = float4(linearGuide,1.0f);
}
