// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#define FO4HDR_LAYER_ORACLES
#include "../../../shaders/output/SceneLayers.hlsli"

RWTexture2D<float4> LayerValues : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (all(pixel.xy < FrameSize))
        LayerValues[pixel.xy] = float4(EvaluateFrameLayers(pixel.xy), 1.0f);
}
