// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once


#include "FO4Isotope/output/SelectiveColor.hpp"
#include "FO4Isotope/output/WeatherGradeResponse.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <string_view>

namespace fo4hdr {

inline constexpr std::size_t kMaximumLutGradePathLength{259};

[[nodiscard]] bool NormalizeLutGradePath(
    std::string_view path,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept;

[[nodiscard]] bool NormalizeImageSpaceGradeKey(
    std::string_view key,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept;

enum class ProductionToneMapperAlgorithm : std::uint8_t
{
    aces2,
    agX,
    gt7,
    // SDR profile only: the game's own tone curve.
    native
};

// SDR profile choices: the game's curve and each operator whose SDR
// configuration qualified.
[[nodiscard]] constexpr bool IsSelectableSdrToneMapper(
    ProductionToneMapperAlgorithm algorithm) noexcept
{
    using enum ProductionToneMapperAlgorithm;
    return algorithm == native || algorithm == aces2 || algorithm == agX || algorithm == gt7;
}

enum class ColorGradingMethod : std::uint8_t
{
    rgbRatio,
    affine
};

// Display curve both grading methods use to read native LUT codes.
enum class ColorGradingCurve : std::uint8_t
{
    gamma22,
    srgb
};

inline constexpr double kMinimumSceneExposureEv{-4.0};
inline constexpr double kMaximumSceneExposureEv{4.0};
inline constexpr double kDefaultProductionSceneExposureEv{0.0};
inline constexpr double kMinimumAcesLookStrength{0.0};
inline constexpr double kMaximumAcesLookStrength{1.0};
inline constexpr double kMinimumAgxMiddleGrayEv{-0.5};
inline constexpr double kMaximumAgxMiddleGrayEv{0.5};
// At the SDR white AgX can only lower middle gray: raising it leaves the
// darkening sigmoid without a valid toe.
inline constexpr double kMaximumSdrAgxMiddleGrayEv{0.0};
inline constexpr double kMinimumAgxContrast{2.2};
inline constexpr double kMaximumAgxContrast{3.0};
inline constexpr double kMinimumAgxHdrPurity{0.25};
inline constexpr double kMaximumAgxHdrPurity{0.6};
inline constexpr double kMinimumAgxHdrShoulderPower{1.75};
inline constexpr double kMaximumAgxHdrShoulderPower{2.25};
inline constexpr double kMinimumGt7ToeStrength{1.0};
inline constexpr double kMaximumGt7ToeStrength{1.5};
inline constexpr double kMinimumGt7ShoulderStart{0.35};
inline constexpr double kMaximumGt7ShoulderStart{0.60};
// In SDR, GT7's shoulder may start where its toe ends: the toe's 0.538 extent is
// 21.52% of the 250-nit SDR white, rounded up to the saved 0.001 precision.
inline constexpr double kMinimumSdrGt7ShoulderStart{0.216};
inline constexpr double kMinimumGt7ColorBlend{0.0};
inline constexpr double kMaximumGt7ColorBlend{1.0};
inline constexpr double kMinimumImageAdjustment{-100.0};
inline constexpr double kMaximumImageAdjustment{100.0};
inline constexpr double kMinimumSaturationPercent{0.0};
inline constexpr double kMaximumSaturationPercent{200.0};
inline constexpr double kMinimumVanillaColorGradingPercent{0.0};
inline constexpr double kMaximumVanillaColorGradingPercent{100.0};
inline constexpr double kMinimumBloomStrengthPercent{0.0};
inline constexpr double kMaximumBloomStrengthPercent{100.0};
inline constexpr double kDefaultUiPaperWhiteNits{203.0};
inline constexpr double kDefaultVideoWhiteNits{203.0};
inline constexpr double kMaximumSelectiveExposureEv{2.0};

// A settings publication is safe when every qualified draw for the current
// frame used one thread and that thread is also presenting the frame. Frames
// without a qualified draw do not establish an incompatibility.
[[nodiscard]] constexpr bool LiveSettingsFrameThreadsCompatible(
    std::uint32_t qualifiedDrawThread,
    bool qualifiedDrawThreadConflict,
    std::uint32_t presentThread) noexcept
{
    return !qualifiedDrawThreadConflict &&
        (qualifiedDrawThread == 0 ||
         qualifiedDrawThread == presentThread);
}

// Plain persisted/live values only. D3D resources and publication state do not
// belong here.
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
    bool operator==(const ToneMapperParameters&) const = default;
};

// SDR operator defaults, tuned in game separately from the HDR defaults.
inline constexpr ToneMapperParameters kDefaultSdrToneMapperParameters{
    .acesLookStrength = 0.44,
    .agxMiddleGrayEv = -0.39,
    .gt7ShoulderStart = 0.28};

[[nodiscard]] constexpr ToneMapperParameters ActiveToneMapperParameters(
    ProductionToneMapperAlgorithm algorithm,
    const ToneMapperParameters& parameters) noexcept
{
    ToneMapperParameters active{};
    switch (algorithm)
    {
    case ProductionToneMapperAlgorithm::aces2:
        active.acesLookStrength = parameters.acesLookStrength;
        break;
    case ProductionToneMapperAlgorithm::agX:
        active.agxMiddleGrayEv = parameters.agxMiddleGrayEv;
        active.agxContrast = parameters.agxContrast;
        active.agxHdrPurity = parameters.agxHdrPurity;
        active.agxHdrShoulderPower = parameters.agxHdrShoulderPower;
        break;
    case ProductionToneMapperAlgorithm::gt7:
        active.gt7ToeStrength = parameters.gt7ToeStrength;
        active.gt7ShoulderStart = parameters.gt7ShoulderStart;
        active.gt7ColorBlend = parameters.gt7ColorBlend;
        break;
    default:
        break;
    }
    return active;
}

struct HdrSettings
{
    ProductionToneMapperAlgorithm productionAlgorithm{
        ProductionToneMapperAlgorithm::gt7};
    ToneMapperParameters toneMapperParameters{};
    double displayPeakNits{1000.0};
    double uiPaperWhiteNits{kDefaultUiPaperWhiteNits};
    double videoWhiteNits{kDefaultVideoWhiteNits};
    double exposureEv{kDefaultProductionSceneExposureEv};
    double contrast{};
    double blacks{};
    double highlights{};
    double shadows{};
    double saturationPercent{100.0};
    // Saturation of bright highlights; it shares Saturation's range.
    double highlightSaturationPercent{100.0};
    double temperature{};
    double tint{};
    SelectiveColorSettings selectiveColor{};
    ColorGradingMethod colorGradingMethod{ColorGradingMethod::rgbRatio};
    ColorGradingCurve colorGradingCurve{ColorGradingCurve::gamma22};
    double vanillaColorGradingStrengthPercent{100.0};
    double fallbackGradeLuminancePercent{67.0};
    double fallbackGradeColorPercent{67.0};
    WeatherGradeResponseSettings weatherGradeResponse{};
    double bloomStrengthPercent{50.0};
    // SDR profile only; the HDR profile keeps it off. It turns the weather
    // response on while an operator replaces the SDR curve.
    bool sdrOperatorWeatherResponse{};
    bool operator==(const HdrSettings&) const = default;
};

// The selected SDR operator turns the weather response on regardless of Enabled.
[[nodiscard]] constexpr bool OperatorForcesWeatherResponse(const HdrSettings& settings) noexcept
{
    return settings.sdrOperatorWeatherResponse &&
        settings.productionAlgorithm != ProductionToneMapperAlgorithm::native;
}

// The weather response the renderer applies. The operator switch leaves the saved
// Enabled value alone, so Native keeps following it.
[[nodiscard]] constexpr WeatherGradeResponseSettings EffectiveWeatherGradeResponse(
    const HdrSettings& settings) noexcept
{
    auto response = settings.weatherGradeResponse;
    response.enabled = response.enabled || OperatorForcesWeatherResponse(settings);
    return response;
}

// Interface policy is intentionally separate from HdrSettings: it publishes no
// shader constants, rebuilds no operator LUT and keeps temporal history.
struct HdrInterfaceSettings
{
    // [General] HdrEnabled; it applies at the next game start.
    bool hdrEnabled{true};
    bool operator==(const HdrInterfaceSettings&) const = default;
};

enum class HdrSettingsValidationError : std::uint8_t
{
    none,
    unsupportedAlgorithm,
    unsupportedColorGradingMethod,
    unsupportedColorGradingCurve,
    displayPeakOutOfRange,
    uiPaperWhiteOutOfRange,
    videoWhiteOutOfRange,
    exposureOutOfRange,
    contrastOutOfRange,
    blacksOutOfRange,
    highlightsOutOfRange,
    shadowsOutOfRange,
    saturationOutOfRange,
    highlightSaturationOutOfRange,
    temperatureOutOfRange,
    tintOutOfRange,
    toneMapperParametersOutOfRange,
    selectiveColorOutOfRange,
    vanillaColorGradingOutOfRange,
    weatherGradeResponseInvalid,
    bloomStrengthOutOfRange
};

struct HdrSettingsMath
{
    double sceneExposureMultiplier{1.0};
    double contrastSlope{1.0};
    double blacksStrength{};
    double highlightsEv{};
    double shadowsEv{};
    double saturationScale{1.0};
    double highlightSaturationScale{1.0};
    double vanillaColorGradingBlend{1.0};
    double fallbackGradeLuminanceRemoval{};
    double fallbackGradeColorRemoval{};
    double bloomMultiplier{1.0};
    // Row-major linear BT.709 RGB transform. Neutral temperature/tint is the
    // exact identity matrix.
    std::array<double, 9> colorBalanceBt709{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0};
};

[[nodiscard]] HdrSettingsValidationError ValidateHdrSettings(
    const HdrSettings& settings) noexcept;
// The SDR profile follows the HDR rules but chooses among the SDR tone curves.
[[nodiscard]] HdrSettingsValidationError ValidateProfileSettings(
    const HdrSettings& settings, bool sdrProfile) noexcept;
[[nodiscard]] bool ValidateToneMapperParameters(
    ProductionToneMapperAlgorithm algorithm,
    const ToneMapperParameters& parameters) noexcept;
// The HDR ranges with the SDR AgX Middle Gray and GT7 Shoulder Start limits.
[[nodiscard]] bool ValidateSdrToneMapperParameters(
    ProductionToneMapperAlgorithm algorithm,
    const ToneMapperParameters& parameters) noexcept;
[[nodiscard]] HdrSettingsMath DeriveHdrSettingsMath(
    const HdrSettings& settings) noexcept;
// SDR profile: vanilla-neutral defaults for the creative controls, the native
// tone curve and kDefaultSdrToneMapperParameters. Its output and grading-method
// fields keep HDR defaults and are unused.
[[nodiscard]] HdrSettings DefaultSdrSettings() noexcept;
// A neutral SDR pass reproduces vanilla, so its original shader draws.
[[nodiscard]] bool SdrSceneControlsNeutral(const HdrSettings& settings) noexcept;
[[nodiscard]] bool SdrGradeControlsNeutral(const HdrSettings& settings) noexcept;

// Both persisted creative profiles; Interface settings stay shared.
struct HdrSettingsProfiles
{
    HdrSettings hdr{};
    HdrSettings sdr{DefaultSdrSettings()};
    bool operator==(const HdrSettingsProfiles&) const = default;
};

}  // namespace fo4hdr
