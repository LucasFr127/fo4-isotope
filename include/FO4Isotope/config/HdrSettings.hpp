// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once


#include "FO4Isotope/output/SelectiveColor.hpp"
#include "FO4Isotope/output/WeatherGradeResponse.hpp"

#include <algorithm>
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
    gt7
};

enum class ColorGradingMethod : std::uint8_t
{
    radial,
    nativeReference
};

inline constexpr double kMinimumSceneExposureEv{-4.0};
inline constexpr double kMaximumSceneExposureEv{4.0};
inline constexpr double kDefaultProductionSceneExposureEv{0.0};
inline constexpr double kMinimumAcesLookStrength{0.0};
inline constexpr double kMaximumAcesLookStrength{1.0};
inline constexpr double kMinimumAgxMiddleGrayEv{-0.5};
inline constexpr double kMaximumAgxMiddleGrayEv{0.5};
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
inline constexpr double kImageMiddleGrayPivot{0.18};
inline constexpr double kBlacksKneeLuminance{0.02};
inline constexpr double kMaximumBlacksStrength{0.75};
inline constexpr double kShadowMaskFullBelow{0.02};
inline constexpr double kShadowMaskZeroAt{0.18};
inline constexpr double kHighlightMaskZeroBelow{1.0};
inline constexpr double kHighlightMaskFullAt{4.0};
inline constexpr double kMaximumSelectiveExposureEv{2.0};
inline constexpr std::uint32_t kDefaultSettingsToggleVirtualKey{0x48U};
inline constexpr std::uint32_t kSettingsHotkeyControl{1U};
inline constexpr std::uint32_t kSettingsHotkeyAlt{2U};
inline constexpr std::uint32_t kSettingsHotkeyShift{4U};

struct HdrSettingsHotkey
{
    std::uint32_t virtualKey{kDefaultSettingsToggleVirtualKey};
    std::uint32_t modifiers{kSettingsHotkeyAlt};
    bool operator==(const HdrSettingsHotkey&) const = default;
};

[[nodiscard]] constexpr bool IsSupportedHdrSettingsHotkey(
    HdrSettingsHotkey key) noexcept
{
    if ((key.modifiers & ~7U) != 0) return false;
    const auto vk = key.virtualKey;
    constexpr std::array<std::array<std::uint32_t, 2>, 10> ranges{{
        {0x30U, 0x39U}, {0x41U, 0x5AU}, {0x60U, 0x87U},
        {0x08U, 0x09U}, {0x0DU, 0x0DU}, {0x20U, 0x28U},
        {0x2DU, 0x2EU}, {0xBAU, 0xC0U}, {0xDBU, 0xDFU},
        {0xE2U, 0xE2U}}};
    const auto supported = std::ranges::any_of(ranges, [vk](const auto& range) {
        return vk >= range[0] && vk <= range[1];
    });
    if (!supported) return false;
    // Leave operating-system task switching, window closing and secure
    // attention chords available. Escape is reserved for capture cancellation.
    constexpr std::array reservedAltKeys{0x09U, 0x73U, 0x20U, 0x0DU};
    if ((key.modifiers & kSettingsHotkeyAlt) != 0 &&
        std::ranges::find(reservedAltKeys, vk) != reservedAltKeys.end())
        return false;
    if ((key.modifiers & (kSettingsHotkeyControl | kSettingsHotkeyAlt)) ==
            (kSettingsHotkeyControl | kSettingsHotkeyAlt) && vk == 0x2EU)
        return false;
    return true;
}

[[nodiscard]] bool ParseHdrSettingsHotkey(
    std::string_view text, HdrSettingsHotkey& key) noexcept;
[[nodiscard]] std::array<char, 64> FormatHdrSettingsHotkey(
    HdrSettingsHotkey key) noexcept;
inline constexpr double kMinimumInterfaceScale{1.0};
inline constexpr double kMaximumInterfaceScale{2.0};
inline constexpr double kDefaultInterfaceScale{1.25};
inline constexpr double kMinimumSettingsWindowWidth{520.0};
inline constexpr double kMaximumSettingsWindowWidth{1200.0};
inline constexpr double kDefaultSettingsWindowWidth{700.0};
inline constexpr double kMinimumSettingsWindowHeight{420.0};
inline constexpr double kMaximumSettingsWindowHeight{1000.0};
inline constexpr double kDefaultSettingsWindowHeight{680.0};

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
    double temperature{};
    double tint{};
    SelectiveColorSettings selectiveColor{};
    ColorGradingMethod colorGradingMethod{ColorGradingMethod::radial};
    double vanillaColorGradingStrengthPercent{100.0};
    double vanillaGradeLuminancePercent{100.0};
    double vanillaGradeColorPercent{100.0};
    WeatherGradeResponseSettings weatherGradeResponse{};
    double bloomStrengthPercent{100.0};
    bool operator==(const HdrSettings&) const = default;
};

// Live interface policy is intentionally separate from HdrSettings. Window
// geometry, scale and hotkey changes do not publish shader constants, rebuild
// an operator LUT, or invalidate temporal history.
struct HdrInterfaceSettings
{
    double scale{kDefaultInterfaceScale};
    double windowWidth{kDefaultSettingsWindowWidth};
    double windowHeight{kDefaultSettingsWindowHeight};
    // Physical client-area pixels; -1/-1 means the initial centered position.
    double windowX{-1.0};
    double windowY{-1.0};
    HdrSettingsHotkey toggleKey{};
    bool operator==(const HdrInterfaceSettings&) const = default;
};

enum class HdrSettingsValidationError : std::uint8_t
{
    none,
    unsupportedAlgorithm,
    unsupportedColorGradingMethod,
    displayPeakOutOfRange,
    uiPaperWhiteOutOfRange,
    videoWhiteOutOfRange,
    exposureOutOfRange,
    contrastOutOfRange,
    blacksOutOfRange,
    highlightsOutOfRange,
    shadowsOutOfRange,
    saturationOutOfRange,
    temperatureOutOfRange,
    tintOutOfRange,
    toneMapperParametersOutOfRange,
    selectiveColorOutOfRange,
    vanillaColorGradingOutOfRange,
    weatherGradeResponseInvalid,
    bloomStrengthOutOfRange
};

enum class HdrInterfaceSettingsValidationError : std::uint8_t
{
    none,
    scaleOutOfRange,
    windowWidthOutOfRange,
    windowHeightOutOfRange,
    windowPositionOutOfRange,
    unsupportedToggleKey
};

struct HdrSettingsMath
{
    double sceneExposureMultiplier{1.0};
    double contrastSlope{1.0};
    double blacksStrength{};
    double highlightsEv{};
    double shadowsEv{};
    double saturationScale{1.0};
    double vanillaColorGradingBlend{1.0};
    double vanillaGradeLuminanceRemoval{};
    double vanillaGradeColorRemoval{};
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
[[nodiscard]] bool ValidateToneMapperParameters(
    ProductionToneMapperAlgorithm algorithm,
    const ToneMapperParameters& parameters) noexcept;
[[nodiscard]] HdrSettingsMath DeriveHdrSettingsMath(
    const HdrSettings& settings) noexcept;
[[nodiscard]] HdrInterfaceSettingsValidationError
ValidateHdrInterfaceSettings(
    const HdrInterfaceSettings& settings) noexcept;

}  // namespace fo4hdr
