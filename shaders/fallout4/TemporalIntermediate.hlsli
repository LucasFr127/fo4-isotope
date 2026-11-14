// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/{taa_0x8CAC3BD9,taa2_0x61CC29E6}.ps_5_0.hlsl
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
// Adapted for FO4Isotope from RenoDX's normalized gamma-domain wrapper around
// Fallout 4's original temporal filter. Modification: invert/reapply
// UI-relative linear BT.2020 transport around that peak-relative code, and
// store the R8 history luma with a 203-nit knee bounded at the same peak.
// See THIRD_PARTY_NOTICES.md.
#ifndef FO4HDR_TEMPORAL_INTERMEDIATE_HLSLI
#define FO4HDR_TEMPORAL_INTERMEDIATE_HLSLI

#include "LiveSettingsConstants.hlsli"

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

// Display peak in 203-nit scene units. The operator output never exceeds it.
float Fo4HdrTemporalPeak()
{
    return max(Fo4HdrDisplayPeakNits / Fo4HdrSceneReferenceWhiteNits, 1.0f);
}

// The original filter blends and sharpens these codes the way the game blends
// gamma-encoded display codes, and a saturated code decodes to the peak.
float3 Fo4HdrEncodeTemporalInput(float3 sceneRelative)
{
    return pow(saturate(sceneRelative / Fo4HdrTemporalPeak()), 1.0f / 2.2f);
}

float3 Fo4HdrDecodeTemporalOutput(float3 temporalCode)
{
    return pow(saturate(temporalCode), 2.2f) * Fo4HdrTemporalPeak();
}

// Stored history luma uses a Reinhard knee at 203-nit scene white with its
// white point at the peak. It spends more of the 256 history steps on shadows
// and midtones than the peak-relative code does.
float Fo4HdrTemporalCodeToHistory(float temporalCode)
{
    const float peak = Fo4HdrTemporalPeak();
    const float light = pow(saturate(temporalCode), 2.2f);
    return pow(light * (peak + light) / (1.0f + light * peak), 1.0f / 2.2f);
}

float Fo4HdrHistoryToTemporalCode(float history)
{
    const float peak = Fo4HdrTemporalPeak();
    const float compressed = pow(saturate(history), 2.2f);
    // Cancellation-free positive root of
    // light^2 + peak * (1 - compressed) * light - compressed = 0.
    const float slope = peak * (1.0f - compressed);
    return pow(2.0f * compressed / (slope + sqrt(slope * slope + 4.0f * compressed)),
        1.0f / 2.2f);
}

#endif
