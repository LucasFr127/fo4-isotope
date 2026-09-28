// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/{taa_0x8CAC3BD9,taa2_0x61CC29E6}.ps_5_0.hlsl
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
// Adapted for FO4Isotope from RenoDX's normalized gamma-domain wrapper around
// Fallout 4's original temporal filter. Modification: use a reversible,
// peak-independent positive-HDR compression and invert/reapply UI-relative
// linear BT.2020 transport while retaining fixed 203-nit temporal history.
// See THIRD_PARTY_NOTICES.md.
#ifndef FO4HDR_TEMPORAL_INTERMEDIATE_HLSLI
#define FO4HDR_TEMPORAL_INTERMEDIATE_HLSLI

#include "LiveSettingsConstants.hlsli"

static const float kFo4HdrMaximumFiniteFp16 = 65504.0f;
static const float kFo4HdrMaximumTemporalCode =
    kFo4HdrMaximumFiniteFp16 / (1.0f + kFo4HdrMaximumFiniteFp16);

float3 Fo4HdrNormalizeTemporalInput(float3 uiRelativeBt2020)
{
    return uiRelativeBt2020 *
        (Fo4HdrUiTransportWhiteNits / Fo4HdrSceneReferenceWhiteNits);
}

float3 Fo4HdrRestoreUiTransport(float3 sceneRelativeBt2020)
{
    return sceneRelativeBt2020 *
        (Fo4HdrSceneReferenceWhiteNits / Fo4HdrUiTransportWhiteNits);
}

float3 Fo4HdrEncodeTemporalInput(float3 linearColor)
{
    const float3 positive = min(
        max(linearColor, 0.0f), kFo4HdrMaximumFiniteFp16);
    const float3 compressed = positive / (1.0f + positive);
    return pow(compressed, 1.0f / 2.2f);
}

float3 Fo4HdrDecodeTemporalOutput(float3 temporalColor)
{
    const float3 compressed = min(
        pow(saturate(temporalColor), 2.2f),
        kFo4HdrMaximumTemporalCode);
    return min(
        compressed / max(1.0f - compressed, 1.0f / 65505.0f),
        kFo4HdrMaximumFiniteFp16);
}

#endif
