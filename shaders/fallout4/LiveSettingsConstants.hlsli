// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#ifndef FO4HDR_LIVE_SETTINGS_CONSTANTS_HLSLI
#define FO4HDR_LIVE_SETTINGS_CONSTANTS_HLSLI

// Slot b13 is absent from the matched Fallout 4 shader family. This buffer
// starts with a fixed 32-byte operator prefix followed by Image/Effects controls.
// The native renderer owns validation,
// publication, and preservation/restoration of this complete 320-byte buffer.
cbuffer Fo4HdrToneMapConstants : register(b13)
{
    // c0: operator/transport prefix.
    uint Fo4HdrProductionMode;
    float Fo4HdrDisplayPeakNits;
    float Fo4HdrSceneReferenceWhiteNits;
    float Fo4HdrUiTransportWhiteNits;

    // c1: operator/transport prefix.
    float Fo4HdrSceneExposureMultiplier;
    float Fo4HdrVideoWhiteNits;
    float Fo4HdrOperatorShaperAngleGain;
    float Fo4HdrOperatorShaperBias;

    // c2: Effects and upper-luminance Image controls.
    float Fo4HdrBloomStrength;
    float Fo4HdrVanillaGradeStrength;
    float Fo4HdrContrastSlope;
    float Fo4HdrHighlightsEv;

    // c3: remaining Image controls and developer exposure-lock state.
    float Fo4HdrShadowsEv;
    float Fo4HdrSaturation;
    // Session-only developer snapshot. Normal/persisted settings keep this zero.
    uint Fo4HdrDeveloperExposureLocked;
    float Fo4HdrBlacksStrength;

    // c4-c6: row-major D65 chromatic-adaptation transform. The w lanes are
    // explicit padding so CPU/HLSL layout does not depend on matrix packing.
    float4 Fo4HdrColorAdaptationRow0;
    float4 Fo4HdrColorAdaptationRow1;
    float4 Fo4HdrColorAdaptationRow2;

    // c7: activity, reserved state and prepared shaper center/logarithmic gain.
    uint Fo4HdrSelectiveColorActive;
    uint Fo4HdrLiveSettingsReserved1;
    float2 Fo4HdrOperatorShaperParameters;

    // c8-c14: hue radians, saturation multiplier, gain stops, padding.
    float4 Fo4HdrSelectiveRed;
    float4 Fo4HdrSelectiveOrange;
    float4 Fo4HdrSelectiveYellow;
    float4 Fo4HdrSelectiveGreen;
    float4 Fo4HdrSelectiveCyan;
    float4 Fo4HdrSelectiveBlue;
    float4 Fo4HdrSelectiveMagenta;

    // c15-c16: precomputed origin-constrained neutral planes and grade-removal
    // amounts. The scalar lanes occupy the former plane padding.
    float3 Fo4HdrSelectivePlaneA;
    float Fo4HdrVanillaGradeLuminanceRemoval;
    float3 Fo4HdrSelectivePlaneB;
    float Fo4HdrVanillaGradeColorRemoval;

    // c17-c18: per-native-LUT grade-removal profiles in native slot order.
    float4 Fo4HdrLutProfileLuminanceRemoval;
    float4 Fo4HdrLutProfileColorRemoval;

    // c19: profile activity and explicit ABI padding.
    uint Fo4HdrLutProfilesActive;
    uint Fo4HdrLutProfilesReserved0;
    uint Fo4HdrLutProfilesReserved1;
    uint Fo4HdrLutProfilesReserved2;
}

#endif
