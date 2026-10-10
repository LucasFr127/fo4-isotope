// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#ifndef FO4HDR_TEST_SHARED_UI_TRANSFER_INCLUDED
#define FO4HDR_TEST_SHARED_UI_TRANSFER_INCLUDED

cbuffer SharedUiConstants : register(b0)
{
    uint Width;
    uint Height;
    uint Operation;
    float UiWhite;
    float SceneWhite;
    float3 Reserved;
    float4 Color;
    float4 Rectangle;
};

float SignedSrgbEncode(float value)
{
    const float magnitude = abs(value);
    const float encoded = magnitude <= 0.0031308f
        ? 12.92f * magnitude : 1.055f * pow(magnitude, 1.0f / 2.4f) - 0.055f;
    return value < 0.0f ? -encoded : encoded;
}

float SignedSrgbDecode(float value)
{
    const float magnitude = abs(value);
    const float decoded = magnitude <= 0.04045f
        ? magnitude / 12.92f : pow((magnitude + 0.055f) / 1.055f, 2.4f);
    return value < 0.0f ? -decoded : decoded;
}

float3 SceneToSharedUi(float3 scene)
{
    const float3x3 to709 = {
        1.660491002108435f, -0.587641138788550f, -0.072849863319884f,
        -0.124550474521591f, 1.132899897125960f, -0.008349422604369f,
        -0.018150763354905f, -0.100578898008007f, 1.118729661362912f
    };
    const float3 scene709 = mul(to709, scene) * (SceneWhite / UiWhite);
    return float3(SignedSrgbEncode(scene709.r), SignedSrgbEncode(scene709.g),
        SignedSrgbEncode(scene709.b));
}

#endif
