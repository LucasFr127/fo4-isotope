// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#ifndef FO4HDR_DISPLAY_MATH_INCLUDED
#define FO4HDR_DISPLAY_MATH_INCLUDED

// D65 conversion is inverse(RGB2020_to_XYZ) * RGB709_to_XYZ. Each primary
// column (x/y, 1, (1-x-y)/y) is scaled to the common white before inversion.
// https://www.itu.int/rec/R-REC-BT.709-6-201506-I/en
// https://www.itu.int/rec/R-REC-BT.2020-2-201510-I/en
float3 Fo4HdrExpandPrimaries(float3 light)
{
    const float3x3 transform = {
        0.627403895934699f, 0.329283038377884f, 0.043313065687417f,
        0.069097289358232f, 0.919540395075459f, 0.011362315566309f,
        0.016391438875150f, 0.088013307877226f, 0.895595253247624f
    };
    return mul(transform, light);
}

float3 Fo4HdrBoundLight(float3 light, float ceiling)
{
    if ((asuint(ceiling) & 0x7fffffffu) >= 0x7f800000u || ceiling <= 0.0f || ceiling > 10000.0f)
        return 0.0f;
    // Inspect IEEE payloads so optimizing finite arithmetic cannot discard containment.
    const float3 finiteOrInfinite = (asuint(light) & 0x7fffffffu) > 0x7f800000u ? 0.0f : light;
    return clamp(finiteOrInfinite, 0.0f, ceiling);
}

float3 Fo4HdrBoundSdrSource(float3 light)
{
    // Contain non-finite storage before gamut arithmetic can cancel infinities.
    const float3 contained = (asuint(light) & 0x7fffffffu) > 0x7f800000u ? 0.0f : light;
    return clamp(contained, -65504.0f, 65504.0f);
}

float3 Fo4HdrEncodeSdrLight(float3 nits, float white)
{
    if ((asuint(white) & 0x7fffffffu) >= 0x7f800000u || white <= 0.0f)
        return 0.0f;
    const float3x3 to709 = {
        1.660491002108435f, -0.587641138788550f, -0.072849863319884f,
        -0.124550474521591f, 1.132899897125960f, -0.008349422604369f,
        -0.018150763354905f, -0.100578898008007f, 1.118729661362912f
    };
    return pow(Fo4HdrBoundLight(mul(to709, nits / white), 1.0f), 1.0f / 2.2f);
}

// BT.2100 Table 4 inverse EOTF: input is display light, not camera light.
// https://www.itu.int/rec/R-REC-BT.2100-3-202502-I/en
float3 Fo4HdrEncodeAbsoluteLight(float3 nits)
{
    const float3 luminance = Fo4HdrBoundLight(nits, 10000.0f) * 0.0001f;
    const float3 power = pow(luminance, 2610.0f / 16384.0f);
    const float3 ratio = (3424.0f / 4096.0f + power * (2413.0f / 128.0f)) /
        (1.0f + power * (2392.0f / 128.0f));
    return saturate(pow(ratio, 2523.0f / 32.0f));
}

#endif
