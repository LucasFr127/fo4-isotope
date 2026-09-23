// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {

inline constexpr float kDeveloperReferenceNits = 203.0F;
inline constexpr float kVideoWhiteNits = 203.0F;
inline constexpr unsigned kProductionOperatorConstantSlot = 13;
inline constexpr unsigned kProductionOperatorLutSlot = 7;
inline constexpr unsigned kProductionOperatorSamplerSlot = 7;

struct alignas(16) OperatorConstants
{
    std::uint32_t mode{};
    float displayPeakNits{1000.0F};
    float sceneReferenceWhiteNits{kDeveloperReferenceNits};
    float uiTransportWhiteNits{kDeveloperReferenceNits};
    float sceneExposureMultiplier{1.0F};
    float videoWhiteNits{kVideoWhiteNits};
    float operatorShaperAngleGain{};
    float operatorShaperBias{};
    float bloomStrength{1.0F};
    float vanillaColorGradingStrength{1.0F};
    float contrastSlope{1.0F};
    float highlightsEv{};
    float shadowsEv{};
    float saturationScale{1.0F};
    std::uint32_t liveSettingsReserved{}; // Developer exposure lock in diagnostic copy only.
    float blacksStrength{};
    std::array<float, 4> colorBalanceRow0{1.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> colorBalanceRow1{0.0F, 1.0F, 0.0F, 0.0F};
    std::array<float, 4> colorBalanceRow2{0.0F, 0.0F, 1.0F, 0.0F};
    std::uint32_t selectiveColorActive{};
    std::uint32_t liveSettingsReserved1{};
    std::array<float, 2> operatorShaperParameters{};
    std::array<std::array<float, 4>, kSelectiveColorRegionCount>
        selectiveColorRegions{};
    // xyz remain the selective-color plane coefficients. The spare w lanes
    // carry independent vanilla-grade removal controls without changing ABI.
    std::array<float, 4> selectiveColorPlaneA{};
    std::array<float, 4> selectiveColorPlaneB{};
    std::array<float, 4> lutProfileLuminanceRemoval{};
    std::array<float, 4> lutProfileColorRemoval{};
    std::uint32_t lutProfilesActive{};
    std::array<std::uint32_t, 3> lutProfilePadding{};
};
static_assert(sizeof(OperatorConstants) == 320);
static_assert(offsetof(OperatorConstants, bloomStrength) == 32);
static_assert(offsetof(OperatorConstants, shadowsEv) == 48);
static_assert(offsetof(OperatorConstants, blacksStrength) == 60);
static_assert(offsetof(OperatorConstants, colorBalanceRow0) == 64);
static_assert(offsetof(OperatorConstants, colorBalanceRow1) == 80);
static_assert(offsetof(OperatorConstants, colorBalanceRow2) == 96);
static_assert(offsetof(OperatorConstants, selectiveColorActive) == 112);
static_assert(offsetof(OperatorConstants, liveSettingsReserved1) == 116);
static_assert(offsetof(OperatorConstants, operatorShaperParameters) == 120);
static_assert(offsetof(OperatorConstants, selectiveColorRegions) == 128);
static_assert(offsetof(OperatorConstants, selectiveColorPlaneA) == 240);
static_assert(offsetof(OperatorConstants, selectiveColorPlaneB) == 256);
static_assert(offsetof(OperatorConstants, selectiveColorPlaneA) +
    3 * sizeof(float) == 252);
static_assert(offsetof(OperatorConstants, selectiveColorPlaneB) +
    3 * sizeof(float) == 268);
static_assert(offsetof(OperatorConstants, lutProfileLuminanceRemoval) == 272);
static_assert(offsetof(OperatorConstants, lutProfileColorRemoval) == 288);
static_assert(offsetof(OperatorConstants, lutProfilesActive) == 304);


} // namespace fo4hdr
