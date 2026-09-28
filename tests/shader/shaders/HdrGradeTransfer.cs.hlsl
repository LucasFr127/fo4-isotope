// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../../../shaders/fallout4/GradeTransfer.hlsli"

Texture2D<float4> Scene : register(t0);
Texture2D<float4> Grade : register(t2);
RWTexture2D<float4> Output : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Output[id.xy] = float4(Fo4HdrTransferRadialGrade(
        Scene.Load(int3(id.xy, 0)).rgb,
        Grade.Load(int3(id.xy, 0)).rgb,
        Scene.Load(int3(id.xy, 0)).a), 1.0f);
}
