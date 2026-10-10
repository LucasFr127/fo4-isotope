// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SharedUiTransfer.hlsli"

Texture2D<float4> Background : register(t0);

float4 ReadBackground(int2 pixel)
{
    return Background.Load(int3(clamp(pixel, int2(0, 0), int2(Width - 1, Height - 1)), 0));
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 pixel = int2(position.xy);
    if (Operation == 1u)
    {
        const int2 source = pixel * 2;
        return (ReadBackground(source) + ReadBackground(source + int2(1, 0)) +
            ReadBackground(source + int2(0, 1)) + ReadBackground(source + int2(1, 1))) * 0.25f;
    }
    if (Operation == 2u || Operation == 3u)
    {
        const int2 direction = Operation == 2u ? int2(1, 0) : int2(0, 1);
        return ReadBackground(pixel - direction) * 0.25f + ReadBackground(pixel) * 0.5f +
            ReadBackground(pixel + direction) * 0.25f;
    }
    if (any(position.xy < Rectangle.xy) || any(position.xy >= Rectangle.zw)) discard;
    if (Operation == 4u)
        return float4(Background.Load(int3(uint2(pixel) / 2u, 0)).rgb * 0.8f + Color.rgb * 0.2f, Color.a);
    if (Operation == 5u)
        return ReadBackground(pixel) * Color;
    return Color;
}

void mainTemporal(float4 position : SV_Position,
    out float4 history : SV_Target0, out float4 publicImage : SV_Target1)
{
    if (any(position.xy < Rectangle.xy) || any(position.xy >= Rectangle.zw)) discard;
    history = publicImage = ReadBackground(int2(position.xy));
}
