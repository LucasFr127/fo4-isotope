// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {

inline constexpr unsigned kProductionOperatorConstantSlot = 13;
inline constexpr unsigned kProductionOperatorLutSlot = 7;
inline constexpr unsigned kProductionOperatorSamplerSlot = 7;

struct alignas(16) OperatorConstants
{
    // Mode zero disables sampling; one, two and three select ACES2, AgX and GT7.
    std::uint32_t mode{};
    float displayPeakNits{};
    float uiWhiteNits{};
    std::uint32_t inputDomain{};
    std::array<float, 4> shaper{};
    float vanillaColorGradingStrength{1.0F};
    float contrastSlope{1.0F};
    float blacksStrength{};
    float shadowsEv{};
    float highlightsEv{};
    float saturationScale{1.0F};
    std::array<float, 2> padding{};
    std::array<float, 4> colorBalanceRow0{1.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> colorBalanceRow1{0.0F, 1.0F, 0.0F, 0.0F};
    std::array<float, 4> colorBalanceRow2{0.0F, 0.0F, 1.0F, 0.0F};

    std::uint32_t selectiveColorActive{};
    std::array<std::uint32_t, 3> selectiveColorPadding{};
    std::array<std::array<float, 4>, kSelectiveColorRegionCount> selectiveColorRegions{};
    std::array<float, 4> selectiveColorPlaneA{};
    std::array<float, 4> selectiveColorPlaneB{};

    // The cold caller validates derived values before packing the shader layout.
    void SetGrade(float strength, const HdrSettingsMath& image) noexcept
    {
        vanillaColorGradingStrength = strength;
        contrastSlope = static_cast<float>(image.contrastSlope);
        blacksStrength = static_cast<float>(image.blacksStrength);
        shadowsEv = static_cast<float>(image.shadowsEv);
        highlightsEv = static_cast<float>(image.highlightsEv);
        saturationScale = static_cast<float>(image.saturationScale);
        const auto& matrix = image.colorBalanceBt709;
        colorBalanceRow0 = {static_cast<float>(matrix[0]), static_cast<float>(matrix[1]),
            static_cast<float>(matrix[2]), 0.0F};
        colorBalanceRow1 = {static_cast<float>(matrix[3]), static_cast<float>(matrix[4]),
            static_cast<float>(matrix[5]), 0.0F};
        colorBalanceRow2 = {static_cast<float>(matrix[6]), static_cast<float>(matrix[7]),
            static_cast<float>(matrix[8]), 0.0F};
    }
};
static_assert(sizeof(OperatorConstants) == 272);
static_assert(offsetof(OperatorConstants, inputDomain) == 12);
static_assert(offsetof(OperatorConstants, shaper) == 16);
static_assert(offsetof(OperatorConstants, vanillaColorGradingStrength) == 32);
static_assert(offsetof(OperatorConstants, contrastSlope) == 36);
static_assert(offsetof(OperatorConstants, blacksStrength) == 40);
static_assert(offsetof(OperatorConstants, shadowsEv) == 44);
static_assert(offsetof(OperatorConstants, highlightsEv) == 48);
static_assert(offsetof(OperatorConstants, saturationScale) == 52);
static_assert(offsetof(OperatorConstants, colorBalanceRow0) == 64);
static_assert(offsetof(OperatorConstants, colorBalanceRow1) == 80);
static_assert(offsetof(OperatorConstants, colorBalanceRow2) == 96);

static_assert(offsetof(OperatorConstants, selectiveColorActive) == 112);
static_assert(offsetof(OperatorConstants, selectiveColorPadding) == 116);
static_assert(offsetof(OperatorConstants, selectiveColorRegions) == 128);
static_assert(offsetof(OperatorConstants, selectiveColorPlaneA) == 240);
static_assert(offsetof(OperatorConstants, selectiveColorPlaneB) == 256);

}  // namespace fo4hdr
