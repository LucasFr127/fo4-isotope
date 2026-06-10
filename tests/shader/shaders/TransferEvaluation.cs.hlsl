// SPDX-License-Identifier: GPL-3.0-or-later

Texture2D<float4> LightValues : register(t0);
RWTexture2D<float4> DisplayCodes : register(u0);
cbuffer TransferCase : register(b0)
{
    uint2 CaseSize;
    float DisplayPeakNits;
    float Padding;
};

float BoundLight(float nits)
{
    return nits > 0.0f ? min(nits, DisplayPeakNits) : 0.0f;
}

float EncodePq(float nits)
{
    // Inverse display EOTF, BT.2100 Table 4, with absolute luminance units.
    // https://www.itu.int/rec/R-REC-BT.2100-3-202502-I/en
    // Containment and the PQ constants make both bases nonnegative;
    // abs makes that domain explicit to FXC.
    const float power = pow(abs(nits / 10000.0f), 2610.0f / 16384.0f);
    const float ratio = (3424.0f / 4096.0f + power * (2413.0f / 128.0f)) /
        (1.0f + power * (2392.0f / 128.0f));
    return saturate(pow(abs(ratio), 2523.0f / 32.0f));
}

[numthreads(8, 1, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= CaseSize)) return;
    const float3 input = LightValues.Load(int3(pixel.xy, 0)).rgb;
    float3 light = 0.0f;
    if (DisplayPeakNits > 0.0f && DisplayPeakNits <= 10000.0f)
    {
        light = float3(BoundLight(input.r), BoundLight(input.g), BoundLight(input.b));
    }
    DisplayCodes[pixel.xy] = float4(EncodePq(light.r), EncodePq(light.g), EncodePq(light.b), 1.0f);
}
