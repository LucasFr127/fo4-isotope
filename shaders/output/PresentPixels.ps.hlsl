// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SceneLayers.hlsli"

float4 main(float4 position : SV_Position) : SV_Target
{
    const uint2 pixel = uint2(position.xy);
    float3 value = EvaluateFrameLayers(pixel);
    if (FrameMode == 0u)
        value = saturate(value);
    else
    {
        if (FrameMode == 1u || FrameMode == 2u) value *= ReferenceWhite;
        // Compose in registers and encode directly into the presenting buffer:
        // no intermediate storage, rounding, or full-frame output copy.
        value = Fo4HdrEncodeAbsoluteLight(value);
    }
    return float4(value, 1.0f);
}
