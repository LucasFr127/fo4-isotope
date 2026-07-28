// SPDX-License-Identifier: GPL-3.0-or-later
Texture2D<float> Fo4HdrMeasuredExposure : register(t0);
RWTexture2D<float2> Fo4HdrExposureSnapshot : register(u0);
cbuffer Fo4HdrGameExposureConstants : register(b2)
{
    float4 Fo4HdrUnusedGameConstants;
    float4 Fo4HdrExposureBounds;
}

bool Fo4HdrCaptureFinite(float value)
{
    // Integer exponent checks retain rejection under optimized SM5 compilation.
    return (asuint(value) & 0x7f800000u) != 0x7f800000u;
}

[numthreads(1, 1, 1)]
void main()
{
    const float measured = Fo4HdrMeasuredExposure.Load(int3(0, 0, 0));
    const float3 bounds = Fo4HdrExposureBounds.xyz;
    float2 snapshot = 0.0f;
    if (Fo4HdrCaptureFinite(measured) && measured >= 0.0f &&
        all((asuint(bounds) & 0x7f800000u) != 0x7f800000u) &&
        bounds.y > 0.0f && bounds.x >= bounds.y && bounds.z > 0.0f)
    {
        const float gain = clamp(bounds.z / (0.00100000005f + measured), bounds.y, bounds.x);
        if (Fo4HdrCaptureFinite(gain) && gain > 0.0f)
            snapshot = float2(gain, measured);
    }
    // Invalid pairs never replace either part of the native exposure calculation.
    Fo4HdrExposureSnapshot[uint2(0, 0)] = snapshot;
}
