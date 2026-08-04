// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
    std::array<float, 3> padding{};
};
static_assert(sizeof(OperatorConstants) == 48);
static_assert(offsetof(OperatorConstants, inputDomain) == 12);
static_assert(offsetof(OperatorConstants, shaper) == 16);
static_assert(offsetof(OperatorConstants, vanillaColorGradingStrength) == 32);

}  // namespace fo4hdr
