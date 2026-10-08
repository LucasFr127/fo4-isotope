// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SharedUiTransfer.hlsli"

Texture2D<float4> Scene : register(t0);

cbuffer SharedUiSeedConstants : register(b0)
{
    float SceneWhite;
    float UiWhite;
    uint SeedMode;
    uint Reserved;
};

float4 main(float4 position : SV_Position) : SV_Target0
{
    const float4 scene = Scene.Load(int3(uint2(position.xy), 0));
    if (SeedMode == 1u)
        return float4(scene.rgb * pow(abs(SceneWhite / UiWhite), 1.0f / 2.2f), scene.a);
    return float4(Fo4HdrSceneToSharedUi(scene.rgb, SceneWhite, UiWhite), 1.0f);
}
