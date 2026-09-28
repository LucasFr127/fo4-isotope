// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Project-authored GPU handoff of qualified tone-map exposure. Zero signals
// an invalid snapshot, never an assumed adaptation exposure. The developer
// lock also retains the measured scalar used by Fallout 4's scene filter.
Texture2D<float4> Fo4HdrMeasuredExposure : register(t0);
RWTexture2D<float2> Fo4HdrExposureSnapshot : register(u0);
cbuffer Fo4HdrGameExposureConstants : register(b2)
{
    float4 Fo4HdrUnusedGameConstants;
    float4 Fo4HdrExposureBounds;
}
[numthreads(1, 1, 1)]
void main()
{
    uint width, height;
    Fo4HdrMeasuredExposure.GetDimensions(width, height);
    const float measured = Fo4HdrMeasuredExposure.Load(int3(0, 0, 0)).x;
    const float denominator = 0.00100000005f + measured;
    float exposure = 0.0f;
    // Integer exponent checks keep invalid input rejection intact under FXC
    // optimization (isfinite on a clamp bound can otherwise be simplified).
    const bool measuredFinite = (asuint(measured) & 0x7f800000u) != 0x7f800000u;
    const bool boundsFinite = all(
        (asuint(Fo4HdrExposureBounds.xyz) & 0x7f800000u) != 0x7f800000u);
    if (width == 1u && height == 1u && measuredFinite && measured >= 0.0f &&
        boundsFinite && denominator > 0.0f &&
        Fo4HdrExposureBounds.y > 0.0f &&
        Fo4HdrExposureBounds.x >= Fo4HdrExposureBounds.y &&
        Fo4HdrExposureBounds.z > 0.0f)
    {
        exposure = clamp(Fo4HdrExposureBounds.z / denominator,
            Fo4HdrExposureBounds.y, Fo4HdrExposureBounds.x);
        if (!isfinite(exposure) || exposure <= 0.0f) exposure = 0.0f;
    }
    Fo4HdrExposureSnapshot[uint2(0, 0)] =
        float2(exposure, exposure > 0.0f ? measured : 0.0f);
}
