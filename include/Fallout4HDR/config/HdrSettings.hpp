// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once


#include "Fallout4HDR/output/SelectiveColor.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
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
    bool operator==(const LutGradeProfile&) const = default;
};

struct LutGradeProfiles
{
    std::array<LutGradeProfile, kMaximumLutGradeProfiles> entries{};
    std::size_t count{};
    bool enabled{true};
    bool operator==(const LutGradeProfiles&) const = default;
};

struct ResolvedLutGrade
{
    double luminancePercent{100.0};
    double colorPercent{100.0};
    bool operator==(const ResolvedLutGrade&) const = default;
};

struct LutGradeOverride
{
    double luminancePercent;
    bool overrideLuminance;
    double colorPercent;
    bool overrideColor;
};

[[nodiscard]] bool NormalizeLutGradePath(
    std::string_view path,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept;

[[nodiscard]] bool NormalizeImageSpaceGradeKey(
    std::string_view key,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept;

[[nodiscard]] const LutGradeProfile* FindLutGradeProfile(
    const LutGradeProfiles& profiles, std::string_view path) noexcept;

[[nodiscard]] LutGradeProfile* FindLutGradeProfile(
    LutGradeProfiles& profiles, std::string_view path) noexcept;

[[nodiscard]] const LutGradeProfile* FindImageSpaceGradeProfile(
    const LutGradeProfiles& profiles, std::string_view key) noexcept;

[[nodiscard]] LutGradeProfile* FindImageSpaceGradeProfile(
    LutGradeProfiles& profiles, std::string_view key) noexcept;

[[nodiscard]] bool SetLutGradeProfile(LutGradeProfiles& profiles,
    std::string_view path, LutGradeOverride value) noexcept;

[[nodiscard]] bool SetImageSpaceGradeProfile(LutGradeProfiles& profiles,
    std::string_view key, LutGradeOverride value) noexcept;

[[nodiscard]] bool AddLutGradeProfile(LutGradeProfiles& profiles,
    std::string_view path, LutGradeOverride value) noexcept;

[[nodiscard]] bool RemoveLutGradeProfile(LutGradeProfiles& profiles,
    std::string_view path) noexcept;

[[nodiscard]] bool RemoveImageSpaceGradeProfile(LutGradeProfiles& profiles,
    std::string_view key) noexcept;

[[nodiscard]] ResolvedLutGrade ResolveLutGradeProfile(
    const LutGradeProfiles& profiles, std::string_view path,
    double globalLuminancePercent, double globalColorPercent) noexcept;

[[nodiscard]] ResolvedLutGrade ResolveImageSpaceGradeProfile(
    const LutGradeProfiles& profiles, std::string_view lutPath,
    std::string_view imageSpaceKey, ResolvedLutGrade global) noexcept;

[[nodiscard]] bool ValidateLutGradeProfiles(const LutGradeProfiles& profiles) noexcept;


enum class ProductionToneMapperAlgorithm : std::uint8_t
{
    aces2Lmt,
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
inline constexpr double kMinimumImageAdjustment{-100.0};
inline constexpr double kMaximumImageAdjustment{100.0};
inline constexpr double kMinimumSaturationPercent{0.0};
inline constexpr double kMaximumSaturationPercent{200.0};
inline constexpr double kMinimumVanillaColorGradingPercent{0.0};
inline constexpr double kMaximumVanillaColorGradingPercent{100.0};
inline constexpr double kMinimumBloomStrengthPercent{0.0};
inline constexpr double kMaximumBloomStrengthPercent{200.0};
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
    case ProductionToneMapperAlgorithm::aces2Lmt:
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
        ProductionToneMapperAlgorithm::aces2Lmt};
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
    LutGradeProfiles lutGradeProfiles{};
    double bloomStrengthPercent{100.0};
    bool operator==(const HdrSettings&) const = default;
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
    lutGradeProfilesInvalid,
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

}  // namespace fo4hdr
