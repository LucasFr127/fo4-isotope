// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#ifndef FO4HDR_SHARED_UI_TRANSFER_INCLUDED
#define FO4HDR_SHARED_UI_TRANSFER_INCLUDED

#include "DisplayMath.hlsli"

float3 Fo4HdrEncodeSignedSrgb(float3 value)
{
    const float3 magnitude = abs(value);
    const float3 encoded = magnitude <= 0.0031308f ? 12.92f * magnitude :
        1.055f * pow(magnitude, 1.0f / 2.4f) - 0.055f;
    return value < 0.0f ? -encoded : encoded;
}

float3 Fo4HdrDecodeSignedSrgb(float3 value)
{
    const float3 magnitude = abs(value);
    const float3 decoded = magnitude <= 0.04045f ? magnitude / 12.92f :
        pow((magnitude + 0.055f) / 1.055f, 2.4f);
    return value < 0.0f ? -decoded : decoded;
}

float3 Fo4HdrSceneToSharedUi(float3 scene, float sceneWhite, float uiWhite)
{
    return Fo4HdrEncodeSignedSrgb(Fo4HdrReducePrimaries(scene) * (sceneWhite / uiWhite));
}

#endif
