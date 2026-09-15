// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/SelectiveColor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fo4hdr {

inline constexpr std::size_t kMaximumLutGradeProfiles{64};
inline constexpr std::size_t kMaximumLutGradePathLength{259};

enum class GradeProfileKind : std::uint8_t
{
    lutAsset,
    imageSpace
};

struct LutGradeProfile
{
    std::array<char, kMaximumLutGradePathLength + 1> path{};
    double luminancePercent{100.0};
    double colorPercent{100.0};
    bool overrideLuminance{};
    bool overrideColor{};
    GradeProfileKind kind{GradeProfileKind::lutAsset};
    [[nodiscard]] constexpr bool operator==(const LutGradeProfile&) const noexcept = default;
};

struct LutGradeProfiles
{
    std::array<LutGradeProfile, kMaximumLutGradeProfiles> entries{};
    std::size_t count{};
    bool enabled{true};
    [[nodiscard]] constexpr bool operator==(const LutGradeProfiles&) const noexcept = default;
};

struct ResolvedLutGrade
{
    double luminancePercent{100.0};
    double colorPercent{100.0};
    [[nodiscard]] constexpr bool operator==(const ResolvedLutGrade&) const noexcept = default;
};

[[nodiscard]] bool NormalizeLutGradePath(
    std::string_view path,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept;
[[nodiscard]] bool NormalizeImageSpaceGradeKey(
    std::string_view key,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept;
[[nodiscard]] const LutGradeProfile* FindLutGradeProfile(
    const LutGradeProfiles& profiles, std::string_view path) noexcept;
[[nodiscard]] const LutGradeProfile* FindImageSpaceGradeProfile(
    const LutGradeProfiles& profiles, std::string_view key) noexcept;
[[nodiscard]] ResolvedLutGrade ResolveLutGradeProfile(
    const LutGradeProfiles& profiles, std::string_view path,
    double globalLuminancePercent, double globalColorPercent) noexcept;
[[nodiscard]] ResolvedLutGrade ResolveImageSpaceGradeProfile(
    const LutGradeProfiles& profiles, std::string_view lutPath,
    std::string_view imageSpaceKey, ResolvedLutGrade global) noexcept;
[[nodiscard]] bool ValidateLutGradeProfiles(const LutGradeProfiles& profiles) noexcept;

inline constexpr double kDefaultUiPaperWhiteNits{203.0};
inline constexpr double kDefaultVideoWhiteNits{203.0};
inline constexpr double kMinimumSceneExposureEv{-4.0};
inline constexpr double kMaximumSceneExposureEv{4.0};
inline constexpr double kMinimumBloomStrengthPercent{0.0};
inline constexpr double kMaximumBloomStrengthPercent{200.0};
inline constexpr double kMinimumImageAdjustment{-100.0};
inline constexpr double kMaximumImageAdjustment{100.0};
inline constexpr double kMinimumSaturationPercent{0.0};
inline constexpr double kMaximumSaturationPercent{200.0};
inline constexpr double kMinimumVanillaColorGradingPercent{0.0};
inline constexpr double kMaximumVanillaColorGradingPercent{100.0};
inline constexpr double kMinimumAcesLookStrength{0.0};
inline constexpr double kMaximumAcesLookStrength{1.0};
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
    gt7,
    aces2Lmt
};

struct ToneMapperParameters
{
    double acesLookStrength{1.0};
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
    if (algorithm == OperatorLutAlgorithm::aces2Lmt)
        active.acesLookStrength = parameters.acesLookStrength;
    else if (algorithm == OperatorLutAlgorithm::agX)
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

enum class ColorGradingMethod : std::uint8_t
{
    radial,
    nativeReference
};

struct HdrSettings
{
    double displayPeakNits{1000.0};
    double uiPaperWhiteNits{kDefaultUiPaperWhiteNits};
    double videoWhiteNits{kDefaultVideoWhiteNits};
    ToneMapperParameters toneMapperParameters;
    double vanillaColorGradingStrengthPercent{100.0};
    double vanillaGradeLuminancePercent{100.0};
    double vanillaGradeColorPercent{100.0};
    ColorGradingMethod colorGradingMethod{ColorGradingMethod::radial};
    double contrast{};
    double blacks{};
    double highlights{};
    double shadows{};
    double saturationPercent{100.0};
    double temperature{};
    double tint{};
    double exposureEv{};
    double bloomStrengthPercent{100.0};
    SelectiveColorSettings selectiveColor{};
    LutGradeProfiles lutGradeProfiles{};
};

enum class HdrSettingsValidationError : std::uint8_t
{
    none,
    displayPeakOutOfRange,
    uiPaperWhiteOutOfRange,
    videoWhiteOutOfRange,
    vanillaColorGradingOutOfRange,
    unsupportedColorGradingMethod,
    contrastOutOfRange,
    blacksOutOfRange,
    highlightsOutOfRange,
    shadowsOutOfRange,
    saturationOutOfRange,
    temperatureOutOfRange,
    tintOutOfRange,
    exposureOutOfRange,
    bloomStrengthOutOfRange,
    selectiveColorOutOfRange,
    invalidLutGradeProfiles
};

[[nodiscard]] HdrSettingsValidationError ValidateHdrSettings(
    const HdrSettings& settings) noexcept;

struct HdrSettingsMath
{
    double contrastSlope{1.0};
    double blacksStrength{};
    double highlightsEv{};
    double shadowsEv{};
    double saturationScale{1.0};
    // Row-major linear BT.709 transform; neutral temperature/tint is exact identity.
    std::array<double, 9> colorBalanceBt709{
        1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    double sceneExposureMultiplier{1.0};
    double bloomMultiplier{1.0};
    double vanillaGradeLuminanceRemoval{};
    double vanillaGradeColorRemoval{};
};

[[nodiscard]] HdrSettingsMath DeriveHdrSettingsMath(const HdrSettings& settings) noexcept;

}  // namespace fo4hdr
