// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SceneLayers.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    const uint2 pixel = uint2(position.xy);
    float3 value = EvaluateFrameLayers(pixel);
    if (FrameMode == 0u)
        value = saturate(value);
    else if (FrameMode == 4u || FrameMode == 5u)
        value = Fo4HdrEncodeSdrLight(value, MenuWhite);
    else
    {
        if (FrameMode == 1u || FrameMode == 2u) value *= ReferenceWhite;
        if (AssumedContent != 0u) value = Fo4HdrBoundLight(value, PeakWhite);
        value = Fo4HdrEncodeAbsoluteLight(value);
    }
    return float4(value, 1.0f);
}
