// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Polyphony Digital Inc.
// Upstream: https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp sha256=94df7e7e310b9423ebb5aaaa29416f376ebbae66e0948fb259f8688314b29822
// Fallout4HDR modification: retain the official default ICtCp/HDR algorithm;
// expose bounded toe, shoulder-start, and color-blend controls only in the
// production adapter; add finite-input validation and the BT.709/203-nit to
// Rec.2020/100-nit adapter. The native BT.2020 result is the production output;
// the inverse-BT.709 fit remains a comparison oracle only.
// LUT baking may defer the final peak clamp until after interpolation.
// The complete Polyphony Digital MIT permission is in THIRD_PARTY_NOTICES.md.

#include "Fallout4HDR/tonemap/Gt7.hpp"

#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace fo4hdr {
namespace {

using FloatRgb = std::array<float, 3>;

constexpr float kReferenceLuminance = 100.0F;
constexpr float kGt7SdrPaperWhiteNits = 250.0F;
constexpr float kMaximumDisplayPeakNits = 10000.0F;
constexpr double kMaximumSceneValue = 65504.0;

[[nodiscard]] float SmoothStep(
    float value,
    float edge0,
    float edge1) noexcept
{
    const float position = (value - edge0) / (edge1 - edge0);
    if (value < edge0) return 0.0F;
    if (value > edge1) return 1.0F;
    return position * position * (3.0F - 2.0F * position);
}

[[nodiscard]] FloatRgb InverseEotfSt2084(const FloatRgb& color) noexcept
{
    constexpr float m1 = 0.1593017578125F;
    constexpr float m2 = 78.84375F;
    constexpr float c1 = 0.8359375F;
    constexpr float c2 = 18.8515625F;
    constexpr float c3 = 18.6875F;
    constexpr float pqMaximumNits = 10000.0F;

    FloatRgb result{};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const float physical = color[index] * kReferenceLuminance;
        const float normalized = physical / pqMaximumNits;
        const float powered = std::powf(normalized, m1);
        result[index] = std::exp2f(m2 *
            (std::log2f(c1 + c2 * powered) -
             std::log2f(1.0F + c3 * powered)));
    }
    return result;
}

[[nodiscard]] FloatRgb EotfSt2084(const FloatRgb& color) noexcept
{
    constexpr float m1 = 0.1593017578125F;
    constexpr float m2 = 78.84375F;
    constexpr float c1 = 0.8359375F;
    constexpr float c2 = 18.8515625F;
    constexpr float c3 = 18.6875F;
    constexpr float pqMaximumNits = 10000.0F;

    FloatRgb result{};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const float encoded = std::clamp(color[index], 0.0F, 1.0F);
        const float powered = std::powf(encoded, 1.0F / m2);
        float luminance = powered - c1;
        if (luminance < 0.0F) luminance = 0.0F;
        luminance /= c2 - c3 * powered;
        luminance = std::powf(luminance, 1.0F / m1);
        result[index] = (luminance * pqMaximumNits) / kReferenceLuminance;
    }
    return result;
}

[[nodiscard]] FloatRgb RgbToIctcp(const FloatRgb& color) noexcept
{
    const float l = (color[0] * 1688.0F + color[1] * 2146.0F +
        color[2] * 262.0F) / 4096.0F;
    const float m = (color[0] * 683.0F + color[1] * 2951.0F +
        color[2] * 462.0F) / 4096.0F;
    const float s = (color[0] * 99.0F + color[1] * 309.0F +
        color[2] * 3688.0F) / 4096.0F;

    const auto pq = InverseEotfSt2084({l, m, s});
    const float lPq = pq[0];
    const float mPq = pq[1];
    const float sPq = pq[2];
    return {
        (2048.0F * lPq + 2048.0F * mPq) / 4096.0F,
        (6610.0F * lPq - 13613.0F * mPq + 7003.0F * sPq) / 4096.0F,
        (17933.0F * lPq - 17390.0F * mPq - 543.0F * sPq) / 4096.0F
    };
}

[[nodiscard]] FloatRgb IctcpToRgb(const FloatRgb& color) noexcept
{
    const float l = color[0] + 0.00860904F * color[1] +
        0.11103F * color[2];
    const float m = color[0] - 0.00860904F * color[1] -
        0.11103F * color[2];
    const float s = color[0] + 0.560031F * color[1] -
        0.320627F * color[2];

    const auto linear = EotfSt2084({l, m, s});
    const float lLinear = linear[0];
    const float mLinear = linear[1];
    const float sLinear = linear[2];
    return {
        std::max(3.43661F * lLinear - 2.50645F * mLinear +
            0.0698454F * sLinear, 0.0F),
        std::max(-0.79133F * lLinear + 1.9836F * mLinear -
            0.192271F * sLinear, 0.0F),
        std::max(-0.0259499F * lLinear - 0.0989137F * mLinear +
            1.12486F * sLinear, 0.0F)
    };
}

struct Gt7Curve
{
    float peakIntensity{};
    float midPoint{};
    float linearSection{};
    float toeStrength{};
    float shoulderA{};
    float shoulderB{};
    float shoulderC{};

    explicit Gt7Curve(
        float framebufferPeak,
        const ToneMapperParameters& parameters = {}) noexcept
        : peakIntensity(framebufferPeak),
          midPoint(0.538F),
          linearSection(static_cast<float>(parameters.gt7ShoulderStart)),
          toeStrength(static_cast<float>(parameters.gt7ToeStrength))
    {
        constexpr float alpha = 0.25F;
        const float curvature = (linearSection - 1.0F) / (alpha - 1.0F);
        shoulderA = peakIntensity * linearSection +
            peakIntensity * curvature;
        shoulderB = -peakIntensity * curvature *
            std::expf(linearSection / curvature);
        shoulderC = -1.0F / (curvature * peakIntensity);
    }

    [[nodiscard]] FloatRgb Evaluate(const FloatRgb& input) const noexcept
    {
        FloatRgb result{};
        for (std::size_t index = 0; index < result.size(); ++index)
        {
            const float value = input[index];
            if (value < 0.0F) continue;

            const float linearWeight = SmoothStep(value, 0.0F, midPoint);
            const float toeWeight = 1.0F - linearWeight;
            const float shoulder = shoulderA +
                shoulderB * std::expf(value * shoulderC);
            if (value < linearSection * peakIntensity)
            {
                const float toe = midPoint *
                    std::powf(value / midPoint, toeStrength);
                result[index] = toeWeight * toe + linearWeight * value;
            }
            else result[index] = shoulder;
        }
        return result;
    }
};

struct Gt7ColorVolume
{
    Gt7ColorVolume(Gt7Curve preparedCurve, float colorBlend) noexcept :
        curve(preparedCurve),
        targetIntensity(RgbToIctcp({curve.peakIntensity,
            curve.peakIntensity, curve.peakIntensity})[0]),
        blendRatio(colorBlend)
    {}

    Gt7Curve curve;
    float targetIntensity;
    float blendRatio;

    [[nodiscard]] FloatRgb Apply(
        const FloatRgb& input, bool containOutput) const noexcept;
};

FloatRgb Gt7ColorVolume::Apply(
    const FloatRgb& input, bool containOutput) const noexcept
{
    constexpr float fadeStart = 0.98F;
    constexpr float fadeEnd = 1.16F;

    const FloatRgb skewed = curve.Evaluate(input);
    if (blendRatio == 0.0F)
    {
        if (!containOutput) return skewed;
        return {std::min(skewed[0], curve.peakIntensity),
            std::min(skewed[1], curve.peakIntensity),
            std::min(skewed[2], curve.peakIntensity)};
    }
    const FloatRgb original = RgbToIctcp(input);
    const FloatRgb skewedUcs = RgbToIctcp(skewed);
    const float chromaScale = 1.0F - SmoothStep(
        original[0] / targetIntensity, fadeStart, fadeEnd);
    const FloatRgb scaledUcs{
        skewedUcs[0],
        original[1] * chromaScale,
        original[2] * chromaScale
    };
    const FloatRgb scaled = IctcpToRgb(scaledUcs);

    FloatRgb result{};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const float blended = (1.0F - blendRatio) * skewed[index] +
            blendRatio * scaled[index];
        result[index] = containOutput ?
            std::min(blended, curve.peakIntensity) : blended;
    }
    return result;
}

[[nodiscard]] FloatRgb ApplyOfficialHdr(
    const FloatRgb& input, float displayPeakNits) noexcept
{
    const float framebufferPeak = displayPeakNits / kReferenceLuminance;
    const Gt7Curve curve(framebufferPeak);
    return Gt7ColorVolume{curve, 0.6F}.Apply(input, true);
}

[[nodiscard]] LinearRgb SanitizeScene(LinearRgb value) noexcept
{
    std::array channels{value.red, value.green, value.blue};
    for (double& channel : channels)
    {
        if (std::isnan(channel) || channel <= 0.0) channel = 0.0;
        else channel = std::min(channel, kMaximumSceneValue);
    }
    return {channels[0], channels[1], channels[2]};
}

[[nodiscard]] FloatRgb SceneToGt7Framebuffer(LinearRgb sanitized) noexcept
{
    const auto sceneRec2020 = ConvertBt709ToBt2020(sanitized);
    constexpr float sceneScale =
        static_cast<float>(kSceneReferenceWhiteNits) /
        kReferenceLuminance;
    return {
        static_cast<float>(sceneRec2020.red) * sceneScale,
        static_cast<float>(sceneRec2020.green) * sceneScale,
        static_cast<float>(sceneRec2020.blue) * sceneScale
    };
}

[[nodiscard]] bool IsValidPeak(double peakNits) noexcept
{
    return std::isfinite(peakNits) &&
        peakNits >= kGt7SdrPaperWhiteNits &&
        peakNits <= kMaximumDisplayPeakNits;
}

[[nodiscard]] LinearRgb FitBt709DisplayGamut(
    LinearRgb color,
    double displayPeakNits) noexcept
{
    const double luminance = 0.2126 * color.red +
        0.7152 * color.green + 0.0722 * color.blue;
    if (!std::isfinite(luminance) || luminance <= 0.0) return {};
    if (luminance >= displayPeakNits)
        return {displayPeakNits, displayPeakNits, displayPeakNits};

    double chroma = 1.0;
    for (const double channel : {color.red, color.green, color.blue})
    {
        if (channel < 0.0)
            chroma = std::min(chroma, luminance / (luminance - channel));
        else if (channel > displayPeakNits)
            chroma = std::min(chroma,
                (displayPeakNits - luminance) / (channel - luminance));
    }
    std::array channels{color.red, color.green, color.blue};
    for (double& channel : channels)
        channel = std::clamp(
            luminance + chroma * (channel - luminance),
            0.0,
            displayPeakNits);
    return {channels[0], channels[1], channels[2]};
}

}  // namespace

class Gt7Transform
{
public:
    Gt7Transform(double peak, const ToneMapperParameters& parameters) noexcept
        : volume{Gt7Curve(static_cast<float>(peak) / kReferenceLuminance,
              parameters),
          static_cast<float>(parameters.gt7ColorBlend)}
    {}

    const Gt7ColorVolume volume;
};

void Gt7TransformDeleter::operator()(const Gt7Transform* transform) const noexcept
{
    delete transform;
}

Gt7TransformHandle CreateGt7Transform(
    double displayPeakNits, const ToneMapperParameters& parameters) noexcept
{
    if (!IsValidPeak(displayPeakNits) || !ValidateToneMapperParameters(
            ProductionToneMapperAlgorithm::gt7, parameters)) return {};
    return Gt7TransformHandle(new (std::nothrow)
        Gt7Transform(displayPeakNits, parameters));
}

LinearRgb MapGt7ReferenceRec2020(
    LinearRgb framebufferRec2020,
    double displayPeakNits) noexcept
{
    if (!IsValidPeak(displayPeakNits)) return {};

    const auto sanitized = SanitizeScene(framebufferRec2020);
    const FloatRgb input{
        static_cast<float>(sanitized.red),
        static_cast<float>(sanitized.green),
        static_cast<float>(sanitized.blue)
    };
    const auto result = ApplyOfficialHdr(
        input, static_cast<float>(displayPeakNits));
    return {result[0], result[1], result[2]};
}

LinearRgb EvaluateGt7LutNits(
    LinearRgb sceneLinear,
    double displayPeakNits,
    const ToneMapperParameters& parameters) noexcept
{
    if (!IsValidPeak(displayPeakNits) ||
        !ValidateToneMapperParameters(
            ProductionToneMapperAlgorithm::gt7, parameters))
        return {};

    return EvaluateGt7LutNits(Gt7Transform(displayPeakNits, parameters), sceneLinear);
}

LinearRgb EvaluateGt7LutNits(
    const Gt7Transform& transform, LinearRgb sceneLinear) noexcept
{
    const auto sanitized = SanitizeScene(sceneLinear);
    const auto input = SceneToGt7Framebuffer(sanitized);
    const auto mapped = transform.volume.Apply(input, false);
    return {
        static_cast<double>(mapped[0]) * kReferenceLuminance,
        static_cast<double>(mapped[1]) * kReferenceLuminance,
        static_cast<double>(mapped[2]) * kReferenceLuminance
    };
}

LinearRgb MapGt7ToBt2020Nits(
    LinearRgb sceneLinear,
    double displayPeakNits,
    const ToneMapperParameters& parameters) noexcept
{
    return ContainBt2020Nits(
        EvaluateGt7LutNits(sceneLinear, displayPeakNits, parameters),
        displayPeakNits);
}

LinearRgb MapGt7ToBt709NitsM12(
    LinearRgb sceneLinear,
    double displayPeakNits) noexcept
{
    const auto absoluteBt2020 = MapGt7ToBt2020Nits(
        sceneLinear, displayPeakNits);
    return FitBt709DisplayGamut(
        ConvertBt2020ToBt709ForComparison(absoluteBt2020),
        displayPeakNits);
}

}  // namespace fo4hdr
