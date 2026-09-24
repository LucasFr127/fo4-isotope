// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../shaders/output/DisplayMath.hlsli"

Texture2D<float4> LightValues : register(t0);
RWTexture2D<float4> DisplayCodes : register(u0);
cbuffer TransferCase : register(b0)
{
    uint2 CaseSize;
    float NitScale;
    uint RawCodes;
};

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= CaseSize)) return;
    const float3 value = LightValues.Load(int3(pixel.xy, 0)).rgb;
    float3 code = RawCodes == 1u ? value :
        Fo4HdrEncodeAbsoluteLight(Fo4HdrBoundLight(value * NitScale, 10000.0f));
    if (RawCodes == 2u)
    {
        // Experimental static ordered thresholds, one common offset for RGB.
        // Test-only: final-output dithering cannot repair earlier LUT errors.
        uint rank = 0u;
        [unroll]
        for (uint bit = 0u; bit < 3u; ++bit)
        {
            const uint x = (pixel.x >> bit) & 1u;
            const uint y = (pixel.y >> bit) & 1u;
            rank |= ((x ^ y) * 2u + y) << (4u - 2u * bit);
        }
        const float offset = ((rank + 0.5f) / 64.0f - 0.5f) / 1023.0f;
        const float3 light = Fo4HdrBoundLight(value * NitScale, 10000.0f);
        code = light <= 0.0f ? 0.0f :
            (light >= 10000.0f ? 1.0f : saturate(code + offset));
    }
    DisplayCodes[pixel.xy] = float4(saturate(code), 1.0f);
}
