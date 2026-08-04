// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fo4hdr {

inline constexpr double kDefaultUiPaperWhiteNits{203.0};
inline constexpr double kDefaultVideoWhiteNits{203.0};
inline constexpr double kMinimumVanillaColorGradingPercent{0.0};
inline constexpr double kMaximumVanillaColorGradingPercent{100.0};
inline constexpr double kMinimumAgxMiddleGrayEv{-0.25};
inline constexpr double kMaximumAgxMiddleGrayEv{0.25};
inline constexpr double kMinimumAgxContrast{2.4};
inline constexpr double kMaximumAgxContrast{3.0};
inline constexpr double kMinimumAgxHdrPurity{0.35};
inline constexpr double kMaximumAgxHdrPurity{0.6};
inline constexpr double kMinimumAgxHdrShoulderPower{1.9};
inline constexpr double kMaximumAgxHdrShoulderPower{2.1};
inline constexpr double kMinimumGt7ToeStrength{1.0};
inline constexpr double kMaximumGt7ToeStrength{1.5};
inline constexpr double kMinimumGt7ShoulderStart{0.35};
inline constexpr double kMaximumGt7ShoulderStart{0.60};
inline constexpr double kMinimumGt7ColorBlend{0.0};
inline constexpr double kMaximumGt7ColorBlend{0.8};

enum class OperatorLutAlgorithm : std::uint8_t
{
    aces2,
    agX,
    gt7
};

struct ToneMapperParameters
{
    double agxMiddleGrayEv{};
    double agxContrast{2.7};
    double agxHdrPurity{0.5};
    double agxHdrShoulderPower{2.0};
    double gt7ToeStrength{1.28};
    double gt7ShoulderStart{0.444};
    double gt7ColorBlend{0.6};
    [[nodiscard]] constexpr bool operator==(const ToneMapperParameters&) const noexcept = default;
};

// Inactive controls remain saved but cannot change the selected LUT content key.
[[nodiscard]] constexpr ToneMapperParameters ActiveToneMapperParameters(
    OperatorLutAlgorithm algorithm, const ToneMapperParameters& parameters) noexcept
{
    ToneMapperParameters active;
    if (algorithm == OperatorLutAlgorithm::agX)
    {
        active.agxMiddleGrayEv = parameters.agxMiddleGrayEv;
        active.agxContrast = parameters.agxContrast;
        active.agxHdrPurity = parameters.agxHdrPurity;
        active.agxHdrShoulderPower = parameters.agxHdrShoulderPower;
    }
    else if (algorithm == OperatorLutAlgorithm::gt7)
    {
        active.gt7ToeStrength = parameters.gt7ToeStrength;
        active.gt7ShoulderStart = parameters.gt7ShoulderStart;
        active.gt7ColorBlend = parameters.gt7ColorBlend;
    }
    return active;
}

[[nodiscard]] bool ValidateToneMapperParameters(
    OperatorLutAlgorithm algorithm, const ToneMapperParameters& parameters) noexcept;

struct HdrSettings
{
    double displayPeakNits{1000.0};
    double uiPaperWhiteNits{kDefaultUiPaperWhiteNits};
    double videoWhiteNits{kDefaultVideoWhiteNits};
    ToneMapperParameters toneMapperParameters;
    double vanillaColorGradingStrengthPercent{100.0};
};

enum class HdrSettingsValidationError : std::uint8_t
{
    none,
    displayPeakOutOfRange,
    uiPaperWhiteOutOfRange,
    videoWhiteOutOfRange,
    vanillaColorGradingOutOfRange
};

[[nodiscard]] HdrSettingsValidationError ValidateHdrSettings(
    const HdrSettings& settings) noexcept;

}  // namespace fo4hdr
