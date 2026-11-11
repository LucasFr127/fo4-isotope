// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace fo4hdr {
namespace {

using Matrix3 = std::array<double, 9>;

constexpr Matrix3 kBt709ToXyz{
    0.4123907992659595, 0.35758433938387796, 0.1804807884018343,
    0.21263900587151036, 0.7151686787677559, 0.07219231536073371,
    0.01933081871559185, 0.11919477979462599, 0.9505321522496607};
constexpr Matrix3 kXyzToBt709{
    3.2409699419045226, -1.537383177570094, -0.4986107602930034,
    -0.9692436362808796, 1.8759675015077202, 0.04155505740717559,
    0.05563007969699366, -0.20397695888897652, 1.0569715142428786};
constexpr Matrix3 kBradford{
    0.8951, 0.2664, -0.1614,
    -0.7502, 1.7135, 0.0367,
    0.0389, -0.0685, 1.0296};
constexpr Matrix3 kBradfordInverse{
    0.9869929054667123, -0.14705425642099013, 0.15996265166373122,
    0.4323052697233945, 0.5183602715367776, 0.0492912282128556,
    -0.008528664575177328, 0.04004282165408487, 0.9684866957875502};
constexpr std::array<double, 3> kD65Xyz{
    0.9504559270516716, 1.0, 1.0890577507598784};

[[nodiscard]] constexpr Matrix3 Multiply(
    const Matrix3& left,
    const Matrix3& right) noexcept
{
    Matrix3 result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            for (std::size_t inner = 0; inner < 3; ++inner)
                result[row * 3 + column] +=
                    left[row * 3 + inner] * right[inner * 3 + column];
    return result;
}

[[nodiscard]] constexpr std::array<double, 3> Multiply(
    const Matrix3& matrix,
    const std::array<double, 3>& vector) noexcept
{
    return {
        matrix[0] * vector[0] + matrix[1] * vector[1] +
            matrix[2] * vector[2],
        matrix[3] * vector[0] + matrix[4] * vector[1] +
            matrix[5] * vector[2],
        matrix[6] * vector[0] + matrix[7] * vector[1] +
            matrix[8] * vector[2]};
}

[[nodiscard]] constexpr bool InRange(
    double value,
    double minimum,
    double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] std::array<double, 2> PlanckianXy(
    double kelvin) noexcept
{
    const auto inverse = 1000.0 / kelvin;
    const auto inverse2 = inverse * inverse;
    const auto inverse3 = inverse2 * inverse;
    double x{};
    if (kelvin <= 4000.0)
        x = -0.2661239 * inverse3 - 0.2343589 * inverse2 +
            0.8776956 * inverse + 0.179910;
    else
        x = -3.0258469 * inverse3 + 2.1070379 * inverse2 +
            0.2226347 * inverse + 0.240390;

    double y{};
    if (kelvin <= 2222.0)
        y = -1.1063814 * x * x * x - 1.34811020 * x * x +
            2.18555832 * x - 0.20219683;
    else if (kelvin <= 4000.0)
        y = -0.9549476 * x * x * x - 1.37418593 * x * x +
            2.09137015 * x - 0.16748867;
    else
        y = 3.0817580 * x * x * x - 5.87338670 * x * x +
            3.75112997 * x - 0.37001483;
    return {x, y};
}

[[nodiscard]] Matrix3 BuildColorBalanceMatrix(
    double temperature,
    double tint) noexcept
{
    if (temperature == 0.0 && tint == 0.0)
        return {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    constexpr double d65X = 0.3127;
    constexpr double d65Y = 0.3290;
    const auto normalizedTemperature = temperature / 100.0;
    const auto endpoint = PlanckianXy(
        normalizedTemperature >= 0.0 ? 4000.0 : 10000.0);
    const auto amount = std::abs(normalizedTemperature);
    const auto targetX = std::lerp(d65X, endpoint[0], amount);
    // Positive Tint is magenta and negative Tint is green. The bounded 0.02
    // xy displacement is deliberately modest and is applied to the target
    // white before Bradford adaptation, never as independent RGB gains.
    const auto targetY = std::clamp(
        std::lerp(d65Y, endpoint[1], amount) - tint * 0.0002,
        0.25,
        0.40);
    const std::array<double, 3> targetXyz{
        targetX / targetY,
        1.0,
        (1.0 - targetX - targetY) / targetY};
    const auto sourceCone = Multiply(kBradford, kD65Xyz);
    const auto targetCone = Multiply(kBradford, targetXyz);
    Matrix3 coneScale{
        targetCone[0] / sourceCone[0], 0.0, 0.0,
        0.0, targetCone[1] / sourceCone[1], 0.0,
        0.0, 0.0, targetCone[2] / sourceCone[2]};
    const auto adaptation = Multiply(
        kBradfordInverse,
        Multiply(coneScale, kBradford));
    return Multiply(kXyzToBt709, Multiply(adaptation, kBt709ToXyz));
}

}  // namespace

namespace {

bool ValidateAgxParameters(const ToneMapperParameters& parameters) noexcept
{
    return InRange(parameters.agxMiddleGrayEv,
                   kMinimumAgxMiddleGrayEv, kMaximumAgxMiddleGrayEv) &&
            InRange(parameters.agxContrast,
                kMinimumAgxContrast, kMaximumAgxContrast) &&
            InRange(parameters.agxHdrPurity,
                kMinimumAgxHdrPurity, kMaximumAgxHdrPurity) &&
            InRange(parameters.agxHdrShoulderPower,
                kMinimumAgxHdrShoulderPower,
                kMaximumAgxHdrShoulderPower);
}

bool ValidateGt7Parameters(const ToneMapperParameters& parameters,
    double minimumShoulderStart = kMinimumGt7ShoulderStart) noexcept
{
    return InRange(parameters.gt7ToeStrength,
                   kMinimumGt7ToeStrength, kMaximumGt7ToeStrength) &&
            InRange(parameters.gt7ShoulderStart,
                minimumShoulderStart, kMaximumGt7ShoulderStart) &&
            InRange(parameters.gt7ColorBlend,
                kMinimumGt7ColorBlend, kMaximumGt7ColorBlend);
}

}  // namespace

bool ValidateToneMapperParameters(
    ProductionToneMapperAlgorithm algorithm,
    const ToneMapperParameters& parameters) noexcept
{
    switch (algorithm)
    {
    case ProductionToneMapperAlgorithm::aces2:
        return InRange(parameters.acesLookStrength,
            kMinimumAcesLookStrength, kMaximumAcesLookStrength);
    case ProductionToneMapperAlgorithm::agX:
        return ValidateAgxParameters(parameters);
    case ProductionToneMapperAlgorithm::gt7:
        return ValidateGt7Parameters(parameters);
    default:
        return false;
    }
}

bool ValidateSdrToneMapperParameters(
    ProductionToneMapperAlgorithm algorithm,
    const ToneMapperParameters& parameters) noexcept
{
    if (algorithm == ProductionToneMapperAlgorithm::gt7)
        return ValidateGt7Parameters(parameters, kMinimumSdrGt7ShoulderStart);
    return ValidateToneMapperParameters(algorithm, parameters) &&
        (algorithm != ProductionToneMapperAlgorithm::agX ||
            parameters.agxMiddleGrayEv <= kMaximumSdrAgxMiddleGrayEv);
}

namespace {

HdrSettingsValidationError ValidateColorGrading(const HdrSettings& settings) noexcept
{
    constexpr std::array methods{ColorGradingMethod::rgbRatio, ColorGradingMethod::affine};
    if (std::ranges::find(methods, settings.colorGradingMethod) == methods.end())
        return HdrSettingsValidationError::unsupportedColorGradingMethod;
    constexpr std::array curves{ColorGradingCurve::gamma22, ColorGradingCurve::srgb};
    if (std::ranges::find(curves, settings.colorGradingCurve) == curves.end())
        return HdrSettingsValidationError::unsupportedColorGradingCurve;
    return HdrSettingsValidationError::none;
}

HdrSettingsValidationError ValidateToneMappers(
    const HdrSettings& settings, bool sdrProfile) noexcept
{
    constexpr std::array algorithms{ProductionToneMapperAlgorithm::aces2,
        ProductionToneMapperAlgorithm::agX, ProductionToneMapperAlgorithm::gt7};
    const auto selected = settings.productionAlgorithm;
    const auto supported = sdrProfile ? IsSelectableSdrToneMapper(selected) :
        std::ranges::find(algorithms, selected) != algorithms.end();
    if (!supported) return HdrSettingsValidationError::unsupportedAlgorithm;
    // Every operator's settings stay valid, including those not selected.
    const auto validParameters = [&settings, sdrProfile](auto algorithm) {
        const auto& parameters = settings.toneMapperParameters;
        return sdrProfile ? ValidateSdrToneMapperParameters(algorithm, parameters) :
            ValidateToneMapperParameters(algorithm, parameters);
    };
    if (!std::ranges::all_of(algorithms, validParameters))
        return HdrSettingsValidationError::toneMapperParametersOutOfRange;
    return ValidateColorGrading(settings);
}

struct HdrNumericRule
{
    double HdrSettings::* member;
    double minimum;
    double maximum;
    HdrSettingsValidationError error;
};

HdrSettingsValidationError ValidateNumericRules(
    const HdrSettings& settings, std::span<const HdrNumericRule> rules) noexcept
{
    for (const auto& rule : rules)
        if (!InRange(settings.*rule.member, rule.minimum, rule.maximum))
            return rule.error;
    return HdrSettingsValidationError::none;
}
HdrSettingsValidationError ValidateHdrLuminance(const HdrSettings& settings) noexcept
{
    constexpr HdrNumericRule rules[]{
        {&HdrSettings::displayPeakNits, kMinimumDisplayPeakNits, kMaximumDisplayPeakNits,
            HdrSettingsValidationError::displayPeakOutOfRange},
        {&HdrSettings::uiPaperWhiteNits, kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits,
            HdrSettingsValidationError::uiPaperWhiteOutOfRange},
        {&HdrSettings::videoWhiteNits, kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits,
            HdrSettingsValidationError::videoWhiteOutOfRange},
        {&HdrSettings::exposureEv, kMinimumSceneExposureEv, kMaximumSceneExposureEv,
            HdrSettingsValidationError::exposureOutOfRange},
    };
    return ValidateNumericRules(settings, rules);
}

HdrSettingsValidationError ValidateHdrImage(const HdrSettings& settings) noexcept
{
    constexpr HdrNumericRule rules[]{
        {&HdrSettings::contrast, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::contrastOutOfRange},
        {&HdrSettings::blacks, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::blacksOutOfRange},
        {&HdrSettings::highlights, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::highlightsOutOfRange},
        {&HdrSettings::shadows, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::shadowsOutOfRange},
        {&HdrSettings::saturationPercent, kMinimumSaturationPercent, kMaximumSaturationPercent,
            HdrSettingsValidationError::saturationOutOfRange},
        {&HdrSettings::highlightSaturationPercent, kMinimumSaturationPercent,
            kMaximumSaturationPercent, HdrSettingsValidationError::highlightSaturationOutOfRange},
        {&HdrSettings::temperature, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::temperatureOutOfRange},
        {&HdrSettings::tint, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::tintOutOfRange},
    };
    return ValidateNumericRules(settings, rules);
}

HdrSettingsValidationError ValidateHdrGrading(const HdrSettings& settings) noexcept
{
    if (!ValidateSelectiveColorSettings(settings.selectiveColor))
        return HdrSettingsValidationError::selectiveColorOutOfRange;
    if (!InRange(settings.vanillaColorGradingStrengthPercent,
            kMinimumVanillaColorGradingPercent,
            kMaximumVanillaColorGradingPercent))
        return HdrSettingsValidationError::vanillaColorGradingOutOfRange;
    if (!InRange(settings.fallbackGradeLuminancePercent,
            kMinimumVanillaColorGradingPercent,
            kMaximumVanillaColorGradingPercent) ||
        !InRange(settings.fallbackGradeColorPercent,
            kMinimumVanillaColorGradingPercent,
            kMaximumVanillaColorGradingPercent))
        return HdrSettingsValidationError::vanillaColorGradingOutOfRange;
    if (!ValidateWeatherGradeResponseSettings(settings.weatherGradeResponse))
        return HdrSettingsValidationError::weatherGradeResponseInvalid;
    if (!InRange(settings.bloomStrengthPercent,
            kMinimumBloomStrengthPercent, kMaximumBloomStrengthPercent))
        return HdrSettingsValidationError::bloomStrengthOutOfRange;
    return HdrSettingsValidationError::none;
}

}  // namespace

HdrSettingsValidationError ValidateHdrSettings(const HdrSettings& settings) noexcept
{
    return ValidateProfileSettings(settings, false);
}

HdrSettingsValidationError ValidateProfileSettings(
    const HdrSettings& settings, bool sdrProfile) noexcept
{
    if (const auto error = ValidateToneMappers(settings, sdrProfile);
        error != HdrSettingsValidationError::none) return error;
    if (const auto error = ValidateHdrLuminance(settings);
        error != HdrSettingsValidationError::none) return error;
    if (const auto error = ValidateHdrImage(settings);
        error != HdrSettingsValidationError::none) return error;
    if (const auto error = ValidateHdrGrading(settings);
        error != HdrSettingsValidationError::none) return error;
    return HdrSettingsValidationError::none;
}

HdrSettingsMath DeriveHdrSettingsMath(
    const HdrSettings& settings) noexcept
{
    HdrSettingsMath result{};
    // The math is profile-independent, and the curve cannot identify the
    // profile once SDR selects an operator; either profile's rules suffice.
    if (ValidateProfileSettings(settings, false) != HdrSettingsValidationError::none &&
        ValidateProfileSettings(settings, true) != HdrSettingsValidationError::none)
        return result;
    result.sceneExposureMultiplier = std::exp2(settings.exposureEv);
    // The full slider range maps to a reversible one-stop contrast slope.
    result.contrastSlope = std::exp2(settings.contrast / 100.0);
    result.blacksStrength = settings.blacks * 0.0075;
    result.highlightsEv =
        settings.highlights / 100.0 * kMaximumSelectiveExposureEv;
    result.shadowsEv =
        settings.shadows / 100.0 * kMaximumSelectiveExposureEv;
    result.saturationScale = settings.saturationPercent / 100.0;
    result.highlightSaturationScale = settings.highlightSaturationPercent / 100.0;
    result.vanillaColorGradingBlend =
        settings.vanillaColorGradingStrengthPercent / 100.0;
    result.fallbackGradeLuminanceRemoval =
        1.0 - settings.fallbackGradeLuminancePercent / 100.0;
    result.fallbackGradeColorRemoval =
        1.0 - settings.fallbackGradeColorPercent / 100.0;
    result.bloomMultiplier = settings.bloomStrengthPercent / 100.0;
    result.colorBalanceBt709 =
        BuildColorBalanceMatrix(settings.temperature, settings.tint);
    return result;
}

HdrSettings DefaultSdrSettings() noexcept
{
    HdrSettings settings{};
    settings.productionAlgorithm = ProductionToneMapperAlgorithm::native;
    settings.toneMapperParameters = kDefaultSdrToneMapperParameters;
    settings.fallbackGradeLuminancePercent = 100.0;
    settings.fallbackGradeColorPercent = 100.0;
    settings.weatherGradeResponse.enabled = false;
    settings.bloomStrengthPercent = 100.0;
    settings.sdrOperatorWeatherResponse = true;
    return settings;
}

bool SdrSceneControlsNeutral(const HdrSettings& settings) noexcept
{
    const std::array neutral{
        settings.productionAlgorithm == ProductionToneMapperAlgorithm::native,
        settings.exposureEv == 0.0,
        settings.bloomStrengthPercent == 100.0, settings.contrast == 0.0,
        settings.blacks == 0.0, settings.highlights == 0.0,
        settings.shadows == 0.0, settings.saturationPercent == 100.0,
        settings.highlightSaturationPercent == 100.0, settings.temperature == 0.0, settings.tint == 0.0,
        !IsSelectiveColorEffective(settings.selectiveColor)};
    return std::ranges::find(neutral, false) == neutral.end();
}

bool SdrGradeControlsNeutral(const HdrSettings& settings) noexcept
{
    return settings.vanillaColorGradingStrengthPercent == 100.0 &&
        settings.fallbackGradeLuminancePercent == 100.0 &&
        settings.fallbackGradeColorPercent == 100.0 &&
        !EffectiveWeatherGradeResponse(settings).enabled;
}

}  // namespace fo4hdr
