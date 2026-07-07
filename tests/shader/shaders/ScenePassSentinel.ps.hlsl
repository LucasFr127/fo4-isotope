// SPDX-License-Identifier: GPL-3.0-or-later

interface IColor
{
    float4 Shade();
};

class SentinelColor : IColor
{
    float4 value;
    float4 Shade() { return value; }
};

cbuffer SentinelConstants : register(b0)
{
    SentinelColor sentinel;
};

IColor selectedColor;

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    return selectedColor.Shade();
}

float4 plainMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(0.0625, 0.125, 0.25, 0.5);
}

RWTexture2D<float4> SentinelOutput : register(u0);

[numthreads(1, 1, 1)]
void computeMain(uint3 pixel : SV_DispatchThreadID)
{
    SentinelOutput[pixel.xy] = selectedColor.Shade();
}
