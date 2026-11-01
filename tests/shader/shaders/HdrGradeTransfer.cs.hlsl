// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../../../shaders/fallout4/GradeTransfer.hlsli"

Texture2D<float4> Scene : register(t0);
Texture2D<float4> Black : register(t1);
Texture2D<float4> Grade : register(t2);
RWTexture2D<float4> Output : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // Scene alpha carries strength; Grade alpha selects the curve (1 = sRGB).
    const float4 scene = Scene.Load(int3(id.xy, 0));
    const float4 grade = Grade.Load(int3(id.xy, 0));
    float3 result;
    [branch] if (grade.a == 1.0f)
        result = Fo4HdrTransferRgbRatioGradeSrgb(scene.rgb, grade.rgb,
            Black.Load(int3(id.xy, 0)).rgb, scene.a);
    else
        result = Fo4HdrTransferRgbRatioGrade(scene.rgb, grade.rgb, scene.a);
    Output[id.xy] = float4(result, 1.0f);
}
