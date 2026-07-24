// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/tonemap/Agx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace fo4hdr {
namespace {

using Matrix = std::array<std::array<double, 3>, 3>;

constexpr double kMaximumSceneValue = 65504.0;

using SigmoidSegment = AgxTransform::SigmoidSegment;
using Sigmoid = AgxTransform::Sigmoid;

struct AgxState
{
    double hdrSdrRatio;
    double darkMaximumExponent;
    Sigmoid formationSigmoid;
    Sigmoid darkeningSigmoid;
    double hdrPurity;
};

[[nodiscard]] LinearRgb Multiply(
    const Matrix& matrix,
    LinearRgb color) noexcept
{
    return {
        matrix[0][0] * color.red + matrix[0][1] * color.green +
            matrix[0][2] * color.blue,
        matrix[1][0] * color.red + matrix[1][1] * color.green +
            matrix[1][2] * color.blue,
        matrix[2][0] * color.red + matrix[2][1] * color.green +
            matrix[2][2] * color.blue
    };
}

[[nodiscard]] LinearRgb SanitizeScene(LinearRgb value) noexcept
{
    // Log encoding requires a nonnegative bounded scene domain. NaN and
    // negative values use zero; positive infinity uses the FP16 maximum.
    std::array channels{value.red, value.green, value.blue};
    for (double& channel : channels)
    {
        if (std::isnan(channel) || channel <= 0.0) channel = 0.0;
        else if (std::isinf(channel)) channel = kMaximumSceneValue;
        else channel = std::min(channel, kMaximumSceneValue);
    }
    return {channels[0], channels[1], channels[2]};
}

[[nodiscard]] LinearRgb Clamp01(LinearRgb value) noexcept
{
    return {
        std::clamp(value.red, 0.0, 1.0),
        std::clamp(value.green, 0.0, 1.0),
        std::clamp(value.blue, 0.0, 1.0)
    };
}

// SPDX-SnippetBegin
// SPDX-License-Identifier: MIT
// SPDX-SnippetCopyrightText: Copyright (c) 2025 Akihiro Noguchi
// SPDX-SnippetComment: adapted constants and matrices from aki-null/CustomToneMappingURP@ba6c68b5deafe4896115500d12cf976169d402ff/Runtime/Baker/AgX/AgXConstants.cs; transcribed to dependency-free C++ double and row-major project storage. See THIRD_PARTY_NOTICES.md.

constexpr double kMidGrey = 0.18;
constexpr double kLog2Minimum = -10.0;
constexpr double kLog2Maximum = 6.5;
constexpr double kExponentToe = 1.5;
constexpr double kExponentShoulder = 1.5;
constexpr double kChromaMix = 0.4;
constexpr double kSmallestNormalFloat = 1.17549435e-38;

// Row-major transcription of the matrices passed to Unity.Mathematics and
// applied there with math.mul(matrix, rgb).
constexpr Matrix kInsetMatrix{{
    {{0.856627153315983, 0.095121240538159, 0.048251606145858}},
    {{0.137318972929847, 0.761241990602591, 0.101439036467562}},
    {{0.111898212999950, 0.076799418603190, 0.811302368396859}}
}};

constexpr Matrix kOutsetMatrix{{
    {{1.127100581814437, -0.110606643096603, -0.016493938717835}},
    {{-0.141329763498438, 1.157823702216272, -0.016493938717834}},
    {{-0.141329763498438, -0.110606643096603, 1.251936406595040}}
}};
// SPDX-SnippetEnd

struct Hsv
{
    double hue{};
    double saturation{};
    double value{};
};

// SPDX-SnippetBegin
// SPDX-License-Identifier: BSD-3-Clause
// SPDX-SnippetCopyrightText: Copyright 2013 Colour Developers
// SPDX-SnippetComment: adapted RGB/HSV and Log2 helpers from aki-null/CustomToneMappingURP@ba6c68b5deafe4896115500d12cf976169d402ff/Runtime/Baker/ColorUtility.cs, where they are attributed to Colour; translated from Unity C# float3 to dependency-free C++ double. See THIRD_PARTY_NOTICES.md.
[[nodiscard]] double SafeDivide(double numerator, double denominator) noexcept
{
    return denominator != 0.0 ? numerator / denominator : 0.0;
}

[[nodiscard]] Hsv RgbToHsv(LinearRgb rgb) noexcept
{
    const auto maximum = std::max({rgb.red, rgb.green, rgb.blue});
    const auto minimum = std::min({rgb.red, rgb.green, rgb.blue});
    const auto delta = maximum - minimum;

    const auto saturation = SafeDivide(delta, maximum);
    const auto deltaRed = SafeDivide(
        (maximum - rgb.red) / 6.0 + delta * 0.5, delta);
    const auto deltaGreen = SafeDivide(
        (maximum - rgb.green) / 6.0 + delta * 0.5, delta);
    const auto deltaBlue = SafeDivide(
        (maximum - rgb.blue) / 6.0 + delta * 0.5, delta);

    auto hue = deltaBlue - deltaGreen;
    if (maximum == rgb.green)
        hue = 1.0 / 3.0 + deltaRed - deltaBlue;
    if (maximum == rgb.blue)
        hue = 2.0 / 3.0 + deltaGreen - deltaRed;
    if (hue < 0.0) hue += 1.0;
    if (hue > 1.0) hue -= 1.0;
    if (delta == 0.0) hue = 0.0;

    return {hue, saturation, maximum};
}

[[nodiscard]] LinearRgb HsvToRgb(Hsv hsv) noexcept
{
    const auto hue = std::clamp(hsv.hue, 0.0, 1.0);
    const auto saturation = std::clamp(hsv.saturation, 0.0, 1.0);
    const auto value = std::clamp(hsv.value, 0.0, 1.0);
    const auto hue6 = (hue - std::floor(hue)) * 6.0;
    const auto sector = static_cast<int>(std::floor(hue6));
    const auto fraction = hue6 - std::floor(hue6);
    const auto low = value * (1.0 - saturation);
    const auto falling = value * (1.0 - saturation * fraction);
    const auto rising = value * (1.0 - saturation * (1.0 - fraction));

    switch (sector)
    {
    case 1: return {falling, value, low};
    case 2: return {low, value, rising};
    case 3: return {low, falling, value};
    case 4: return {rising, low, value};
    case 5: return {value, low, falling};
    default: return {value, rising, low};
    }
}

struct Log2Encoding
{
    double minimumExponent;
    double maximumExponent;
    double middleGrey;
};

[[nodiscard]] double Log2Encode(
    double linear,
    const Log2Encoding& encoding) noexcept
{
    const auto safeLinear = std::max(linear, kSmallestNormalFloat);
    return (std::log2(safeLinear / encoding.middleGrey) - encoding.minimumExponent) /
        (encoding.maximumExponent - encoding.minimumExponent);
}

[[nodiscard]] double Log2Decode(
    double encoded,
    const Log2Encoding& encoding) noexcept
{
    return encoding.middleGrey * std::exp2(
        encoded * (encoding.maximumExponent - encoding.minimumExponent) + encoding.minimumExponent);
}
// SPDX-SnippetEnd

// SPDX-SnippetBegin
// SPDX-License-Identifier: MIT
// SPDX-SnippetCopyrightText: Copyright (c) 2025 Akihiro Noguchi
// SPDX-SnippetComment: adapted the selected neutral AgX core from aki-null/CustomToneMappingURP@ba6c68b5deafe4896115500d12cf976169d402ff/Runtime/Baker/AgX/AgXToneMapping.cs and Runtime/Baker/AgX/Sigmoid.cs. Modifications: dependency-free C++ double; fixed EV 0, contrast 2.7, HDR purity 0.5, shoulder power 2, P3Limit=false and SdrMaxNits=203; scene-linear BT.709 to absolute display-linear BT.2020 nits; finite/range handling. See THIRD_PARTY_NOTICES.md.
struct CurvePoint
{
    double x;
    double y;
};

struct SigmoidShape
{
    double slope;
    double toePower;
    double shoulderPower;
};

struct SegmentShape
{
    double power;
    double slope;
};

[[nodiscard]] double SigmoidScale(
    CurvePoint limit,
    CurvePoint transition,
    SegmentShape shape) noexcept
{
    const auto termA =
        std::pow(shape.slope * (limit.x - transition.x), -shape.power);
    const auto termB = std::pow(
        shape.slope * (limit.x - transition.x) / (limit.y - transition.y),
        shape.power) - 1.0;
    return std::pow(termA * termB, -1.0 / shape.power);
}

[[nodiscard]] double ExponentialCurve(
    double value,
    const Sigmoid& sigmoid,
    SigmoidSegment segment) noexcept
{
    const auto coordinate = sigmoid.slope * (value - sigmoid.transitionX) / segment.scale;
    const auto normalized = coordinate /
        std::pow(1.0 + std::pow(coordinate, segment.power), 1.0 / segment.power);
    return segment.scale * normalized + sigmoid.transitionY;
}

[[nodiscard]] Sigmoid PrepareSigmoid(
    CurvePoint pivot,
    SigmoidShape shape) noexcept
{
    // CustomToneMappingURP fixes both transition lengths to zero and the
    // limiting coordinates to (0,0) and (1,1).
    const auto transitionToeX = pivot.x;
    const auto transitionToeY = pivot.y;
    const auto transitionShoulderX = transitionToeX;
    const auto transitionShoulderY = transitionToeY;

    const auto scaleToe = -SigmoidScale(
        {1.0, 1.0},
        {1.0 - transitionToeX, 1.0 - transitionToeY},
        {shape.toePower, shape.slope});
    const auto scaleShoulder = SigmoidScale(
        {1.0, 1.0},
        {transitionShoulderX, transitionShoulderY},
        {shape.shoulderPower, shape.slope});
    const auto intercept = transitionToeY - shape.slope * transitionToeX;

    return {
        transitionToeX,
        transitionToeY,
        {scaleToe, shape.toePower},
        {scaleShoulder, shape.shoulderPower},
        shape.slope,
        intercept
    };
}

[[nodiscard]] double EvaluateSigmoid(
    double value,
    const Sigmoid& sigmoid) noexcept
{
    if (value < sigmoid.transitionX)
        return ExponentialCurve(
            value,
            sigmoid,
            sigmoid.toe);
    if (value <= sigmoid.transitionX)
        return sigmoid.slope * value + sigmoid.intercept;
    return ExponentialCurve(
        value,
        sigmoid,
        sigmoid.shoulder);
}

struct HueBlend
{
    double first;
    double second;
    double amount;
};

[[nodiscard]] double LerpChromaticityAngle(HueBlend blend) noexcept
{
    const auto delta = blend.second - blend.first;
    if (delta > 0.5)
        blend.second -= 1.0;
    else if (delta < -0.5)
        blend.second += 1.0;

    const auto result = blend.first + blend.amount * (blend.second - blend.first);
    return result - std::floor(result);
}

[[nodiscard]] LinearRgb ApplySigmoid(
    LinearRgb value,
    const Sigmoid& sigmoid) noexcept
{
    const auto apply = [=](double channel) noexcept {
        return EvaluateSigmoid(channel, sigmoid);
    };
    return {apply(value.red), apply(value.green), apply(value.blue)};
}

[[nodiscard]] LinearRgb DarkenMiddleGrey(
    LinearRgb color,
    const AgxState& transform) noexcept
{
    const auto preHsv = RgbToHsv(color);
    constexpr double minimumExponent = -20.0;
    const Log2Encoding encoding{minimumExponent, transform.darkMaximumExponent, kMidGrey};

    const auto darken = [&](double channel) noexcept {
        const auto encoded = Log2Encode(
            channel, encoding);
        const auto sigmoid = EvaluateSigmoid(
            encoded, transform.darkeningSigmoid);
        return Log2Decode(
            sigmoid, encoding);
    };
    const LinearRgb darkenedLinear{
        darken(color.red), darken(color.green), darken(color.blue)};
    const auto postHsv = RgbToHsv(darkenedLinear);

    return HsvToRgb({
        LerpChromaticityAngle({
            preHsv.hue, postHsv.hue, transform.hdrPurity}),
        preHsv.saturation +
            transform.hdrPurity *
                (postHsv.saturation - preHsv.saturation),
        postHsv.value
    });
}

[[nodiscard]] LinearRgb ApplyNeutralAgx(
    LinearRgb rec2020,
    const AgxState& transform) noexcept
{
    auto formation = Multiply(kInsetMatrix, rec2020);
    const auto preFormationHsv = RgbToHsv(formation);

    constexpr Log2Encoding encoding{kLog2Minimum, kLog2Maximum, kMidGrey};
    formation = Clamp01({
        Log2Encode(formation.red, encoding),
        Log2Encode(formation.green, encoding),
        Log2Encode(formation.blue, encoding)
    });

    auto displayLinear = ApplySigmoid(
        formation, transform.formationSigmoid);
    displayLinear = {
        std::copysign(std::pow(std::abs(displayLinear.red), 2.4),
            displayLinear.red),
        std::copysign(std::pow(std::abs(displayLinear.green), 2.4),
            displayLinear.green),
        std::copysign(std::pow(std::abs(displayLinear.blue), 2.4),
            displayLinear.blue)
    };
    displayLinear = DarkenMiddleGrey(displayLinear, transform);

    const auto postFormationHsv = RgbToHsv(displayLinear);
    displayLinear = HsvToRgb({
        LerpChromaticityAngle({
            preFormationHsv.hue,
            postFormationHsv.hue,
            kChromaMix}),
        postFormationHsv.saturation,
        postFormationHsv.value
    });

    const auto outset = Multiply(kOutsetMatrix, displayLinear);
    return {
        outset.red * transform.hdrSdrRatio,
        outset.green * transform.hdrSdrRatio,
        outset.blue * transform.hdrSdrRatio
    };
}

// SPDX-SnippetEnd

constexpr double kBt2020RedLuminance = 0.2627;
constexpr double kBt2020GreenLuminance = 0.6780;
constexpr double kBt2020BlueLuminance = 0.0593;
constexpr double kGamutScaleKnee = 0.05;

struct ScaleBlend
{
    double first;
    double second;
    double knee;
};

[[nodiscard]] double SmoothMinimum(ScaleBlend scale) noexcept
{
    const auto blend = std::clamp(
        1.0 - std::abs(scale.first - scale.second) / scale.knee, 0.0, 1.0);
    return std::min(scale.first, scale.second) -
        0.25 * scale.knee * blend * blend;
}

[[nodiscard]] bool IsValidDisplayPeak(double peak) noexcept
{
    return std::isfinite(peak) && peak >= kMinimumDisplayPeakNits &&
        peak <= kMaximumDisplayPeakNits;
}

[[nodiscard]] bool IsFinite(LinearRgb value) noexcept
{
    return std::isfinite(value.red) && std::isfinite(value.green) &&
        std::isfinite(value.blue);
}

[[nodiscard]] LinearRgb ContainNegativeChroma(LinearRgb value) noexcept
{
    const auto luminance = kBt2020RedLuminance * value.red +
        kBt2020GreenLuminance * value.green +
        kBt2020BlueLuminance * value.blue;
    const auto center = std::max(luminance, 0.0);
    const LinearRgb chroma{
        value.red - luminance,
        value.green - luminance,
        value.blue - luminance
    };

    auto maximumScale = std::numeric_limits<double>::infinity();
    for (const auto channel : {chroma.red, chroma.green, chroma.blue})
    {
        if (channel < 0.0)
            maximumScale = std::min(maximumScale, center / -channel);
    }

    // Lower containment contracts chroma about BT.2020 luminance. Upper
    // containment then scales RGB uniformly, avoiding the unstable division
    // by peak-minus-luminance for saturated highlights near display peak.
    const auto lowerScale = std::isfinite(maximumScale) ?
        std::clamp(SmoothMinimum({1.0, std::max(maximumScale, 0.0),
            kGamutScaleKnee}), 0.0, 1.0) : 1.0;
    LinearRgb result{
        center + lowerScale * chroma.red,
        center + lowerScale * chroma.green,
        center + lowerScale * chroma.blue
    };
    return result;
}

[[nodiscard]] LinearRgb ContainHighlightChroma(
    LinearRgb result, double displayPeakNits) noexcept
{
    const auto maximum = std::max({result.red, result.green, result.blue});
    const auto minimum = std::min({result.red, result.green, result.blue});
    if (!(maximum > 0.0)) return {};

    const auto chromaFraction = std::clamp(
        (maximum - minimum) / maximum, 0.0, 1.0);
    const auto upperLimit = displayPeakNits / maximum;
    const auto upperKnee = kGamutScaleKnee * chromaFraction;
    const auto upperScale = upperKnee > 0.0 ?
        std::clamp(SmoothMinimum({1.0, upperLimit, upperKnee}), 0.0, 1.0) :
        std::clamp(upperLimit, 0.0, 1.0);
    return ContainBt2020Nits({
        upperScale * result.red,
        upperScale * result.green,
        upperScale * result.blue
    }, displayPeakNits);
}

[[nodiscard]] LinearRgb ContainDisplayGamut(
    LinearRgb value, double displayPeakNits) noexcept
{
    if (!IsFinite(value) || !IsValidDisplayPeak(displayPeakNits)) return {};
    return ContainHighlightChroma(ContainNegativeChroma(value), displayPeakNits);
}

[[nodiscard]] LinearRgb EvaluateSceneNits(
    const AgxState& transform,
    LinearRgb sceneLinear) noexcept
{
    const LinearRgb containedBt709 = SanitizeScene(sceneLinear);
    const auto rec2020 = ConvertBt709ToBt2020(containedBt709);
    const auto relativeDisplay = ApplyNeutralAgx(rec2020, transform);

    return {
        relativeDisplay.red * kSceneReferenceWhiteNits,
        relativeDisplay.green * kSceneReferenceWhiteNits,
        relativeDisplay.blue * kSceneReferenceWhiteNits
    };
}

[[nodiscard]] AgxState PrepareAgx(
    double displayPeakNits, const ToneMapperParameters& parameters = {}) noexcept
{
    constexpr double fulcrumInput =
        -kLog2Minimum / (kLog2Maximum - kLog2Minimum);
    const auto fulcrumOutput = std::pow(kMidGrey, 1.0 / 2.4);
    const auto hdrSdrRatio = displayPeakNits / kSceneReferenceWhiteNits;
    const auto hdrShoulderPower = std::pow(
        hdrSdrRatio, std::log10(parameters.agxHdrShoulderPower));
    const auto shoulderExponent = kExponentShoulder * hdrShoulderPower;
    constexpr double minimumExponent = -20.0;
    const auto maximumExponent = std::log2(1.0 / kMidGrey);
    const Log2Encoding encoding{minimumExponent, maximumExponent, kMidGrey};
    const auto originalMiddleGrey = Log2Encode(kMidGrey, encoding);
    const auto darkenedMiddleGrey = Log2Encode(
        kMidGrey * std::exp2(parameters.agxMiddleGrayEv) / hdrSdrRatio, encoding);

    return {
        hdrSdrRatio,
        maximumExponent,
        PrepareSigmoid({fulcrumInput, fulcrumOutput},
            {parameters.agxContrast, kExponentToe, shoulderExponent}),
        PrepareSigmoid({originalMiddleGrey, darkenedMiddleGrey},
            {1.000001, 3.0, 1.0}),
        parameters.agxHdrPurity
    };
}

bool ValidSigmoid(const Sigmoid& value) noexcept
{
    const std::array fields{value.transitionX, value.transitionY, value.toe.scale,
        value.toe.power, value.shoulder.scale, value.shoulder.power, value.slope,
        value.intercept};
    if (!std::ranges::all_of(fields, [](double field) { return std::isfinite(field); })) return false;
    // Dark-segment scales may be signed, but division by a zero scale is invalid.
    if (value.toe.scale == 0.0 || value.shoulder.scale == 0.0) return false;
    return value.toe.power > 0.0 && value.shoulder.power > 0.0;
}

}  // namespace

LinearRgb ApplyAgxDisplayGamut(
    LinearRgb displayLinearBt2020Nits, double displayPeakNits) noexcept
{
    return ContainDisplayGamut(displayLinearBt2020Nits, displayPeakNits);
}

std::optional<AgxTransform> CreateAgxTransform(
    double displayPeakNits, const ToneMapperParameters& parameters) noexcept
{
    if (!IsValidDisplayPeak(displayPeakNits)) return std::nullopt;
    if (!ValidateToneMapperParameters(OperatorLutAlgorithm::agX, parameters)) return std::nullopt;
    const auto prepared = PrepareAgx(displayPeakNits, parameters);
    if (!ValidSigmoid(prepared.formationSigmoid)) return std::nullopt;
    if (!ValidSigmoid(prepared.darkeningSigmoid)) return std::nullopt;
    AgxTransform transform;
    transform.displayPeakNits_ = displayPeakNits;
    transform.hdrSdrRatio_ = prepared.hdrSdrRatio;
    transform.darkMaximumExponent_ = prepared.darkMaximumExponent;
    transform.hdrPurity_ = prepared.hdrPurity;
    transform.formationSigmoid_ = prepared.formationSigmoid;
    transform.darkeningSigmoid_ = prepared.darkeningSigmoid;
    return transform;
}

LinearRgb EvaluateAgxLutNits(
    const AgxTransform& transform, LinearRgb sceneLinearBt709) noexcept
{
    const AgxState prepared{transform.hdrSdrRatio_, transform.darkMaximumExponent_,
        transform.formationSigmoid_, transform.darkeningSigmoid_, transform.hdrPurity_};
    return EvaluateSceneNits(prepared, sceneLinearBt709);
}

LinearRgb MapAgxToBt2020Nits(
    const AgxTransform& transform, LinearRgb sceneLinearBt709) noexcept
{
    return ApplyAgxDisplayGamut(
        EvaluateAgxLutNits(transform, sceneLinearBt709), transform.displayPeakNits_);
}

LinearRgb MapAgxToBt2020Nits(
    LinearRgb sceneLinearBt709, double displayPeakNits) noexcept
{
    if (!IsValidDisplayPeak(displayPeakNits)) return {};
    const auto transform = PrepareAgx(displayPeakNits);
    return ApplyAgxDisplayGamut(
        EvaluateSceneNits(transform, sceneLinearBt709), displayPeakNits);
}

}  // namespace fo4hdr
