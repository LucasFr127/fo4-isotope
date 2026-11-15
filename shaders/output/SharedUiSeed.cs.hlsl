// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SharedUiTransfer.hlsli"

// Seeds a scene that already occupies the shared UI image in place. Each
// texel reads only itself, so no source copy is needed. The transform must
// stay texel-identical to SharedUiSeed.ps.hlsl, the copy fallback.
RWTexture2D<float4> Composition : register(u0);

cbuffer SharedUiSeedConstants : register(b0)
{
    float SceneWhite;
    float UiWhite;
    uint SeedMode;
    uint Reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    Composition.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float4 scene = Composition[id.xy];
    if (SeedMode == 1u)
    {
        Composition[id.xy] = float4(scene.rgb * pow(abs(SceneWhite / UiWhite), 1.0f / 2.2f), scene.a);
        return;
    }
    Composition[id.xy] = float4(Fo4HdrSceneToSharedUi(scene.rgb, SceneWhite, UiWhite), 1.0f);
}
