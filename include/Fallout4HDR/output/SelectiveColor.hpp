// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <array>
#include <cstddef>

namespace fo4hdr {

inline constexpr std::size_t kSelectiveColorRegionCount{7};
inline constexpr double kMinimumSelectiveColorHueDegrees{-10.0};
inline constexpr double kMaximumSelectiveColorHueDegrees{10.0};
inline constexpr double kMinimumSelectiveColorSaturationPercent{50.0};
inline constexpr double kMaximumSelectiveColorSaturationPercent{150.0};
inline constexpr double kMinimumSelectiveColorBrightness{-100.0};
inline constexpr double kMaximumSelectiveColorBrightness{100.0};
inline constexpr double kMinimumSelectiveColorAmountPercent{0.0};
inline constexpr double kMaximumSelectiveColorAmountPercent{100.0};

struct SelectiveColorRegionSettings
{
    double hueDegrees{};
    double saturationPercent{100.0};
    double brightness{};
    bool operator==(const SelectiveColorRegionSettings&) const = default;
};

struct SelectiveColorSettings
{
    bool enabled{};
    double amountPercent{100.0};
    std::array<SelectiveColorRegionSettings,
        kSelectiveColorRegionCount> regions{};
    bool operator==(const SelectiveColorSettings&) const = default;
};

struct SelectiveColorRgb
{
    double red{};
    double green{};
    double blue{};
};

struct SelectiveColorPrepared
{
    bool effectiveActive{};
    // Per region: folded hue radians, saturation multiplier, gain stops.
    std::array<std::array<double, 3>,
        kSelectiveColorRegionCount> regions{};
    // Three simultaneous origin-constrained planes: output AB and gain.
    std::array<double, 3> planeA{};
    std::array<double, 3> planeB{};
};

[[nodiscard]] bool ValidateSelectiveColorSettings(
    const SelectiveColorSettings& settings) noexcept;
[[nodiscard]] bool IsSelectiveColorEffective(
    const SelectiveColorSettings& settings) noexcept;
[[nodiscard]] bool PrepareSelectiveColor(
    const SelectiveColorSettings& settings,
    SelectiveColorPrepared& prepared) noexcept;
[[nodiscard]] SelectiveColorRgb ApplySelectiveColor(
    SelectiveColorRgb input,
    const SelectiveColorPrepared& prepared) noexcept;
[[nodiscard]] double EvaluateSelectiveColorHueShiftDegrees(
    double inputHueDegrees,
    const SelectiveColorPrepared& prepared) noexcept;

}  // namespace fo4hdr
