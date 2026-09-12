// SPDX-License-Identifier: MIT
// Copyright (c) 2023 Thatcher Freeman
// Copyright Fallout4HDR contributors
// Upstream: thatcherfreeman/utility-dctls@a495531de3e4704a0a6e0113daa69be303172ed0/Effects/Smooth Tetra Skin.dctl
// Adapted for Fallout4HDR: linear input only, fixed bounded hue centers,
// shared stabilized weights, CPU-prepared neutral planes, and HDR-domain
// finite/negative-channel containment. Cylindrical conversion in the upstream
// source is attributed there to Juan Pablo Zambrano.

#include "Fallout4HDR/output/SelectiveColor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace fo4hdr {
namespace {

constexpr double kTwoPi{2.0 * std::numbers::pi};
constexpr double kDegreesToRadians{kTwoPi / 360.0};
constexpr double kPolarSmoothness{5.0};
constexpr double kNeutralSmoothness{0.1};
constexpr double kMaximumFiniteFp16{65504.0};
constexpr std::array<double, kSelectiveColorRegionCount> kCenters{
    0.0,
    std::numbers::pi / 6.0,
    std::numbers::pi / 3.0,
    2.0 * std::numbers::pi / 3.0,
    std::numbers::pi,
    4.0 * std::numbers::pi / 3.0,
    5.0 * std::numbers::pi / 3.0};

[[nodiscard]] bool InRange(
    double value, double minimum, double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] double WrapRadians(double value) noexcept
{
    value = std::fmod(value, kTwoPi);
    return value < 0.0 ? value + kTwoPi : value;
}

[[nodiscard]] std::array<double, kSelectiveColorRegionCount> Weights(
    double hue) noexcept
{
    std::array<double, kSelectiveColorRegionCount> exponents{};
    double maximum{-std::numeric_limits<double>::infinity()};
    for (std::size_t index = 0; index < exponents.size(); ++index)
    {
        exponents[index] = kPolarSmoothness *
            std::cos(hue - kCenters[index]);
        maximum = std::max(maximum, exponents[index]);
    }
    double sum{};
    for (auto& value : exponents)
    {
        value = std::exp(value - maximum);
        sum += value;
    }
    if (!(sum > 0.0) || !std::isfinite(sum)) return {};
    for (auto& value : exponents) value /= sum;
    return exponents;
}

[[nodiscard]] std::array<double, 3> Unsmooth(
    double hue, double saturation,
    const SelectiveColorPrepared& prepared) noexcept
{
    const auto weights = Weights(hue);
    double hueAdjustment{};
    double saturationMultiplier{};
    double gainStops{};
    for (std::size_t index = 0; index < weights.size(); ++index)
    {
        hueAdjustment += weights[index] * prepared.regions[index][0];
        saturationMultiplier += weights[index] * prepared.regions[index][1];
        gainStops += weights[index] * prepared.regions[index][2];
    }
    const auto outputHue = hue + hueAdjustment;
    const auto outputSaturation = saturation * saturationMultiplier;
    return {std::cos(outputHue) * outputSaturation,
        std::sin(outputHue) * outputSaturation,
        std::clamp(saturation, 0.0, 2.0) * gainStops};
}

[[nodiscard]] bool Finite(SelectiveColorRgb value) noexcept
{
    return std::isfinite(value.red) && std::isfinite(value.green) &&
        std::isfinite(value.blue);
}

[[nodiscard]] bool ValidRegion(const SelectiveColorRegionSettings& region) noexcept
{
    return InRange(region.hueDegrees,
               kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees) &&
        InRange(region.saturationPercent,
            kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent) &&
        InRange(region.brightness,
            kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness);
}

struct NeutralPlaneFit
{
    double sumXx{};
    double sumXy{};
    double sumYy{};
    std::array<double, 3> sumXz{};
    std::array<double, 3> sumYz{};

    void AddSample(double hue, const SelectiveColorPrepared& prepared) noexcept
    {
        const auto x = std::cos(hue);
        const auto y = std::sin(hue);
        const auto output = Unsmooth(hue, 1.0, prepared);
        sumXx += x * x;
        sumXy += x * y;
        sumYy += y * y;
        for (std::size_t lane = 0; lane < 3; ++lane) {
            sumXz[lane] += x * output[lane];
            sumYz[lane] += y * output[lane];
        }
    }

    [[nodiscard]] bool WriteTo(SelectiveColorPrepared& prepared) const noexcept
    {
        const auto denominator = sumXx * sumYy - sumXy * sumXy;
        if (!(std::abs(denominator) > 1.0e-12) ||
            !std::isfinite(denominator))
            return false;
        for (std::size_t lane = 0; lane < 3; ++lane) {
            prepared.planeA[lane] =
                (sumYy * sumXz[lane] - sumXy * sumYz[lane]) / denominator;
            prepared.planeB[lane] =
                (sumXx * sumYz[lane] - sumXy * sumXz[lane]) / denominator;
            if (!std::isfinite(prepared.planeA[lane]) ||
                !std::isfinite(prepared.planeB[lane]))
                return false;
        }
        return true;
    }
};

[[nodiscard]] bool HasProcessableChroma(SelectiveColorRgb value) noexcept
{
    return Finite(value) && std::min({value.red, value.green, value.blue}) >= 0.0 &&
        (value.red != value.green || value.green != value.blue);
}

struct CylindricalColor
{
    double mean{};
    double hue{};
    double saturation{};
};

[[nodiscard]] bool ToCylindrical(
    SelectiveColorRgb normalized, CylindricalColor& color) noexcept
{
    color.mean = (normalized.red + normalized.green + normalized.blue) / 3.0;
    if (!(color.mean > 0.0) || !std::isfinite(color.mean)) return false;
    const auto a = normalized.red - 0.5 * normalized.green -
        0.5 * normalized.blue;
    const auto b = std::sqrt(3.0) * 0.5 *
        (normalized.green - normalized.blue);
    color.hue = WrapRadians(std::atan2(b / color.mean, a / color.mean));
    color.saturation = std::hypot(a / color.mean, b / color.mean) / 3.0;
    return color.saturation != 0.0;
}

[[nodiscard]] SelectiveColorRgb AdjustCylindrical(
    const CylindricalColor& color, const SelectiveColorPrepared& prepared) noexcept
{
    const auto unsmooth = Unsmooth(color.hue, color.saturation, prepared);
    const auto x = std::cos(color.hue) * color.saturation;
    const auto y = std::sin(color.hue) * color.saturation;
    std::array<double, 3> plane{};
    for (std::size_t lane = 0; lane < 3; ++lane)
        plane[lane] = prepared.planeA[lane] * x +
            prepared.planeB[lane] * y;
    const auto bump = std::exp2(-color.saturation * color.saturation /
        (kNeutralSmoothness * kNeutralSmoothness));
    std::array<double, 3> outputAbz{};
    for (std::size_t lane = 0; lane < 3; ++lane)
        outputAbz[lane] = std::lerp(unsmooth[lane], plane[lane], bump);
    const auto outputMean = color.mean * std::exp2(outputAbz[2]);
    // The Cartesian chroma already contains direction times saturation.
    // Avoid reconstructing polar coordinates before forming small RGB values.
    return {
        outputMean * (1.0 + 2.0 * outputAbz[0]),
        outputMean * (1.0 - outputAbz[0] + std::sqrt(3.0) * outputAbz[1]),
        outputMean * (1.0 - outputAbz[0] - std::sqrt(3.0) * outputAbz[1])};
}

[[nodiscard]] bool ContractNegativeChannels(SelectiveColorRgb& result) noexcept
{
    const auto minimum = std::min({result.red, result.green, result.blue});
    if (minimum >= 0.0) return true;
    const auto safeMean = (result.red + result.green + result.blue) / 3.0;
    const auto denominator = safeMean - minimum;
    if (!(safeMean > 0.0)) return false;
    if (!(denominator > 0.0) || !std::isfinite(denominator)) return false;
    const auto contraction = safeMean / denominator;
    // Equivalent contraction with an exact zero at the minimum channel.
    result.red = (result.red - minimum) * contraction;
    result.green = (result.green - minimum) * contraction;
    result.blue = (result.blue - minimum) * contraction;
    return true;
}

[[nodiscard]] SelectiveColorRgb RestoreScaleAndContain(
    SelectiveColorRgb result, double scale, SelectiveColorRgb input) noexcept
{
    if (!Finite(result)) return input;
    if (!ContractNegativeChannels(result)) return input;
    result.red *= scale;
    result.green *= scale;
    result.blue *= scale;
    if (!Finite(result)) return input;
    const auto maximum = std::max({result.red, result.green, result.blue});
    if (maximum > kMaximumFiniteFp16) {
        // Aim one binary64 step below the FP16 ceiling so multiplication
        // roundoff cannot leave the largest channel infinitesimally above it.
        const auto containment =
            std::nextafter(kMaximumFiniteFp16, 0.0) / maximum;
        result.red *= containment;
        result.green *= containment;
        result.blue *= containment;
    }
    return result;
}

}  // namespace

bool ValidateSelectiveColorSettings(
    const SelectiveColorSettings& settings) noexcept
{
    if (!InRange(settings.amountPercent,
            kMinimumSelectiveColorAmountPercent,
            kMaximumSelectiveColorAmountPercent))
        return false;
    return std::all_of(settings.regions.begin(), settings.regions.end(), ValidRegion);
}

bool IsSelectiveColorEffective(
    const SelectiveColorSettings& settings) noexcept
{
    if (!settings.enabled || settings.amountPercent == 0.0) return false;
    return std::any_of(settings.regions.begin(), settings.regions.end(),
        [](const SelectiveColorRegionSettings& region) {
            return region.hueDegrees != 0.0 ||
            region.saturationPercent != 100.0 ||
            region.brightness != 0.0;
        });
}

bool PrepareSelectiveColor(
    const SelectiveColorSettings& settings,
    SelectiveColorPrepared& prepared) noexcept
{
    prepared = {};
    if (!ValidateSelectiveColorSettings(settings)) return false;
    const auto amount = settings.amountPercent / 100.0;
    for (std::size_t index = 0; index < settings.regions.size(); ++index)
    {
        const auto& source = settings.regions[index];
        prepared.regions[index] = {
            source.hueDegrees * kDegreesToRadians * amount,
            std::lerp(1.0, source.saturationPercent / 100.0, amount),
            source.brightness / 100.0 * amount};
    }
    prepared.effectiveActive = IsSelectiveColorEffective(settings);
    if (!prepared.effectiveActive) return true;

    NeutralPlaneFit fit;
    for (const auto hue : kCenters) fit.AddSample(hue, prepared);
    return fit.WriteTo(prepared);
}

SelectiveColorRgb ApplySelectiveColor(
    SelectiveColorRgb input,
    const SelectiveColorPrepared& prepared) noexcept
{
    if (!prepared.effectiveActive || !HasProcessableChroma(input))
        return input;
    const auto scale = std::max({input.red, input.green, input.blue});
    if (!(scale > 0.0) || !std::isfinite(scale)) return input;
    const SelectiveColorRgb normalized{
        input.red / scale, input.green / scale, input.blue / scale};
    CylindricalColor color;
    if (!ToCylindrical(normalized, color)) return input;
    return RestoreScaleAndContain(AdjustCylindrical(color, prepared), scale, input);
}

double EvaluateSelectiveColorHueShiftDegrees(
    double inputHueDegrees,
    const SelectiveColorPrepared& prepared) noexcept
{
    if (!prepared.effectiveActive || !std::isfinite(inputHueDegrees))
        return 0.0;
    const auto weights = Weights(inputHueDegrees * kDegreesToRadians);
    double shift{};
    for (std::size_t index = 0; index < weights.size(); ++index)
        shift += weights[index] * prepared.regions[index][0];
    return shift / kDegreesToRadians;
}

}  // namespace fo4hdr
