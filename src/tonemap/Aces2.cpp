// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the ACES Project.
// Upstream: aces-aswf/aces-core@ee380496a201fd008841ff9399b29241bcd27a3a/lib/Lib.Academy.OutputTransform.ctl
// Upstream: aces-aswf/aces-output@90cadee61547c006ba8fad320ffacc171095bfad/d65/rec2100/Output.Academy.Rec2100-D65_1000nit_in_Rec2100-D65_ST2084.ctl
//
// Adapted from the ACES 2.0 forward Output Transform at:
// aces-aswf/aces-core@ee380496a201fd008841ff9399b29241bcd27a3a
//   lib/Lib.Academy.Utilities.ctl
//   lib/Lib.Academy.ColorSpaces.ctl
//   lib/Lib.Academy.Tonescale.ctl
//   lib/Lib.Academy.OutputTransform.ctl
// and the Rec.2100/D65 1000-nit parameterization at:
// aces-aswf/aces-output@90cadee61547c006ba8fad320ffacc171095bfad
//   d65/rec2100/Output.Academy.Rec2100-D65_1000nit_in_Rec2100-D65_ST2084.ctl
//
// Modified by Fallout4HDR: parameterize the Rec.2100/D65 limiting gamut for
// 400-2000 nit peaks and adapt dimensionless BT.709/D65 scene values to
// AP0/D60 without moving the neutral gray anchor. Return absolute display-
// linear BT.2020 before display encoding. Preserve normalized upper-hull hue
// interpolation and the complete bounded interval search. Academy terms are
// retained in THIRD_PARTY_NOTICES.md.

#include "Fallout4HDR/tonemap/Aces2.hpp"
#include "aces2/Aces2Model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <new>

namespace fo4hdr {
using namespace aces2;
namespace aces2 {

[[nodiscard]] ToneScaleParams InitializeToneScale(
    double peak) noexcept
{
    constexpr double normalizedWhite = 100.0;
    constexpr double toe = 0.04;
    constexpr double contrast = 1.15;
    constexpr double grayAnchor = 0.18;
    constexpr double grayDisplay = 10.013;
    constexpr double grayChange = 0.14;
    constexpr double hitMinimum = 128.0;
    constexpr double hitMaximum = 896.0;
    const double hit = hitMinimum + (hitMaximum - hitMinimum) *
        (std::log(peak / normalizedWhite) /
         std::log(10000.0 / normalizedWhite));
    const double m0 = peak / normalizedWhite;
    const double m1 = 0.5 * (m0 + std::sqrt(m0 * (m0 + 4.0 * toe)));
    const double u = std::pow(
        (hit / m1) / ((hit / m1) + 1.0), contrast);
    const double m = m1 / u;
    const double whiteIndex = std::log(peak / 100.0) / std::log(2.0);
    const double ct = grayDisplay / normalizedWhite *
        (1.0 + whiteIndex * grayChange);
    const double gip = 0.5 * (ct + std::sqrt(ct * (ct + 4.0 * toe)));
    const double powered = std::pow(gip / m, 1.0 / contrast);
    const double gipp2 = -(m1 * powered) / (powered - 1.0);
    const double w2 = grayAnchor / gipp2;
    const double s2 = w2 * m1;
    const double u2 = std::pow(
        (hit / m1) / ((hit / m1) + w2), contrast);
    const double m2 = m1 / u2;
    return {
        .nR = normalizedWhite,
        .g = contrast,
        .t1 = toe,
        .ct = ct,
        .s2 = s2,
        .m2 = m2,
        .forwardLimit = 8.0 * hit,
        .logPeak = std::log10(peak / normalizedWhite)
    };
}

struct ChromaToe
{
    double limit;
    double strength;
    double smoothness;
};

[[nodiscard]] double Toe(double value, ChromaToe curve) noexcept
{
    if (value > curve.limit) return value;
    const double k2 = std::max(curve.smoothness, 0.001);
    const double k1 = std::sqrt(curve.strength * curve.strength + k2 * k2);
    const double k3 = (curve.limit + k1) / (curve.limit + k2);
    const double minusB = k3 * value - k1;
    const double minusC = k2 * k3 * value;
    return 0.5 *
           (minusB + std::sqrt(minusB * minusB + 4.0 * minusC));
}

[[nodiscard]] double ReachMFromTable(
    double hue,
    const std::array<double, kTotalTableSize>& table) noexcept
{
    const std::size_t base = HuePosition(hue);
    const double fraction = hue - static_cast<double>(base);
    const std::size_t low = base + kBaseIndex;
    return std::lerp(table[low], table[low + 1], fraction);
}

[[nodiscard]] Vec3 ToneMapAndCompress(
    Vec3 jmh,
    const OdtParams& params) noexcept
{
    const double linear = JToY(jmh[0], params.input) /
                          kReferenceLuminance;
    const ToneScaleParams& toneScale = params.toneScale;
    const double positive = std::max(0.0, linear);
    const double formation = toneScale.m2 * std::pow(
        positive / (linear + toneScale.s2), toneScale.g);
    const double flared = std::max(
        0.0, formation * formation / (formation + toneScale.t1));
    const double toneMappedY = flared * toneScale.nR;
    const double toneMappedJ = YToJ(toneMappedY, params.input);
    double compressedM = jmh[1];

    if (jmh[1] != 0.0 && jmh[0] > 0.0)
    {
        const double normalizedJ = toneMappedJ / params.limitJMax;
        const double shadowJ = std::max(0.0, 1.0 - normalizedJ);
        const double radians = jmh[2] * kPi / 180.0;
        const double a = std::cos(radians);
        const double b = std::sin(radians);
        const double cos2 = a * a - b * b;
        const double sin2 = 2.0 * a * b;
        const double cos3 = 4.0 * a * a * a - 3.0 * a;
        const double sin3 = 3.0 * b - 4.0 * b * b * b;
        const double normalization =
            (11.34072 * a + 16.46899 * cos2 + 7.88380 * cos3 +
             14.66441 * b - 6.37224 * sin2 + 9.19364 * sin3 +
             77.12896) * params.chromaCompressionScale;
        const double limit =
            std::pow(normalizedJ, params.modelGammaInverse) *
            ReachMFromTable(jmh[2], params.reachM) /
            normalization;

        compressedM = jmh[1] * std::pow(
            toneMappedJ / jmh[0], params.modelGammaInverse);
        compressedM /= normalization;
        compressedM = limit - Toe(
            limit - compressedM,
            {limit - 0.001,
             shadowJ * params.saturation,
             std::sqrt(normalizedJ * normalizedJ +
                       params.saturationThreshold)});
        compressedM = Toe(
            compressedM,
            {limit, normalizedJ * params.compression, shadowJ});
        compressedM *= normalization;
    }

    return {toneMappedJ, compressedM, jmh[2]};
}

[[nodiscard]] double SolveJIntersect(
    Vec2 jm,
    const CompressionAxis& axis) noexcept
{
    const double j = jm[0];
    const double scaledM = jm[1] / axis.slopeGain;
    const double a = scaledM / axis.focusJ;
    if (j < axis.focusJ)
    {
        const double b = 1.0 - scaledM;
        const double c = -j;
        const double root = std::sqrt(b * b - 4.0 * a * c);
        return -2.0 * c / (b + root);
    }

    const double b = -(1.0 + scaledM + axis.limitJ * a);
    const double c = axis.limitJ * scaledM + j;
    const double root = std::sqrt(b * b - 4.0 * a * c);
    return -2.0 * c / (b - root);
}

[[nodiscard]] double CompressionVectorSlope(
    double intersectJ,
    const CompressionAxis& axis) noexcept
{
    const double direction =
        intersectJ < axis.focusJ ? intersectJ : axis.limitJ - intersectJ;
    return direction * (intersectJ - axis.focusJ) / (axis.focusJ * axis.slopeGain);
}

struct BoundaryCurve
{
    Vec2 maximum;
    double inverseGamma;
    double intersectionJ;
};

[[nodiscard]] double EstimateBoundaryIntersectionM(
    CompressionRay ray,
    const BoundaryCurve& curve) noexcept
{
    const double normalizedJ = ray.jAxisIntersect / curve.intersectionJ;
    const double shiftedIntersection =
        curve.intersectionJ * std::pow(normalizedJ, curve.inverseGamma);
    return shiftedIntersection * curve.maximum[1] /
           (curve.maximum[0] - ray.slope * curve.maximum[1]);
}

[[nodiscard]] double FindBoundaryIntersectionM(
    const GamutHull& hull,
    GamutRay ray) noexcept
{
    const double lower = EstimateBoundaryIntersectionM(
        ray.compression,
        {hull.cusp, hull.lowerGammaInverse, ray.cuspIntersectJ});
    const double upper = EstimateBoundaryIntersectionM(
        {hull.jMax - ray.compression.jAxisIntersect, -ray.compression.slope},
        {{hull.jMax - hull.cusp[0], hull.cusp[1]},
         hull.upperGammaInverse, hull.jMax - ray.cuspIntersectJ});
    const double scale = kSmoothCusps * hull.cusp[1];
    const double h = std::max(scale - std::abs(lower - upper), 0.0) / scale;
    return std::min(lower, upper) - h * h * h * scale / 6.0;
}

[[nodiscard]] double FocusGain(
    FocusRegion region,
    const OdtParams& params) noexcept
{
    double gain = params.limitJMax * params.focusDistance;
    if (region.lightness > region.threshold)
    {
        double adjustment = std::log10(
            (params.limitJMax - region.threshold) /
            std::max(0.0001, params.limitJMax - region.lightness));
        adjustment = adjustment * adjustment + 1.0;
        gain *= adjustment;
    }
    return gain;
}

struct GamutChromaBounds
{
    double gamut;
    double reach;
};

[[nodiscard]] double RemapM(
    double m,
    GamutChromaBounds bounds) noexcept
{
    const double boundaryRatio = bounds.gamut / bounds.reach;
    const double proportion =
        std::max(boundaryRatio, kCompressionThreshold);
    const double threshold = proportion * bounds.gamut;
    if (m <= threshold || proportion >= 1.0) return m;

    const double mOffset = m - threshold;
    const double gamutOffset = bounds.gamut - threshold;
    const double reachOffset = bounds.reach - threshold;
    const double scale =
        reachOffset / ((reachOffset / gamutOffset) - 1.0);
    return threshold + scale * (mOffset / scale) /
                           (1.0 + mOffset / scale);
}

struct GamutSample
{
    Vec2 cusp;
    double upperGammaInverse;
};

[[nodiscard]] GamutSample SampleGamut(
    double hue,
    const OdtParams& params) noexcept
{
    // A full bounded search avoids the pinned CTL's mismatch between padded
    // table-size estimates and nominal hue positions. The narrowed range can
    // otherwise exclude the real interval and extrapolate an unrelated pair.
    int low = 0;
    int high = static_cast<int>(kBaseIndex + kTableSize);
    int position = (low + high) / 2;

    while (low + 1 < high)
    {
        if (hue > params.hues[static_cast<std::size_t>(position)])
            low = position;
        else
            high = position;
        position = (low + high) / 2;
    }
    const auto upperIndex = static_cast<std::size_t>(std::max(1, high));
    const Vec3& lower = params.gamutCusps[upperIndex - 1];
    const Vec3& upper = params.gamutCusps[upperIndex];
    // Cusp and upper-hull tables share the same nonuniform padded hue grid.
    const double weight = (hue - params.hues[upperIndex - 1]) /
        (params.hues[upperIndex] - params.hues[upperIndex - 1]);
    return {
        {std::lerp(lower[0], upper[0], weight),
         std::lerp(lower[1], upper[1], weight)},
        std::lerp(params.upperHullGamma[upperIndex - 1],
                  params.upperHullGamma[upperIndex], weight)
    };
}

[[nodiscard]] Vec3 CompressGamut(
    Vec3 jmh,
    const OdtParams& params) noexcept
{
    if (jmh[0] <= 0.0) return {0.0, 0.0, jmh[2]};
    if (jmh[1] < 0.0 || jmh[0] > params.limitJMax)
        return {jmh[0], 0.0, jmh[2]};

    const GamutSample sample = SampleGamut(jmh[2], params);
    const Vec2 cusp = sample.cusp;
    const double focusJ = std::lerp(
        cusp[0],
        params.midJ,
        std::min(1.0,
                 kCuspMidBlend - cusp[0] / params.limitJMax));
    const double analyticalThreshold =
        std::lerp(cusp[0], params.limitJMax, kFocusGainBlend);
    const double slopeGain = FocusGain(
        {jmh[0], analyticalThreshold}, params);
    const CompressionAxis axis{focusJ, params.limitJMax, slopeGain};
    const double sourceIntersectJ = SolveJIntersect(
        {jmh[0], jmh[1]}, axis);
    const double gamutSlope = CompressionVectorSlope(
        sourceIntersectJ, axis);
    const double cuspIntersectJ = SolveJIntersect(
        cusp, axis);
    const double gamutBoundaryM = FindBoundaryIntersectionM(
        {cusp, params.limitJMax, sample.upperGammaInverse, params.lowerHullGammaInverse},
        {{sourceIntersectJ, gamutSlope}, cuspIntersectJ});
    if (gamutBoundaryM <= 0.0)
        return {jmh[0], 0.0, jmh[2]};

    const double reachBoundaryM = EstimateBoundaryIntersectionM(
        {sourceIntersectJ, gamutSlope},
        {{params.limitJMax, ReachMFromTable(jmh[2], params.reachM)},
         params.modelGammaInverse, params.limitJMax});
    const GamutChromaBounds bounds{gamutBoundaryM, reachBoundaryM};
    const double remappedM = RemapM(jmh[1], bounds);
    const Vec3 result{
        sourceIntersectJ + remappedM * gamutSlope,
        remappedM,
        jmh[2]
    };
    return result;
}

}  // namespace

namespace {

struct PeakTransform
{
    explicit PeakTransform(double peak) noexcept
        : bt709ToAp0(CalculateRgbToRgb(kBt709, kAp0)),
          ap0ToAp1(Multiply(RgbToXyz(kAp0), Invert(RgbToXyz(kAp1)))),
          ap1ToAp0(Multiply(RgbToXyz(kAp1), Invert(RgbToXyz(kAp0)))),
          odt(InitializeOdt(peak))
    {
    }

    Matrix bt709ToAp0;
    Matrix ap0ToAp1;
    Matrix ap1ToAp0;
    OdtParams odt;
};

[[nodiscard]] LinearRgb EvaluateScene(const PeakTransform& transform, LinearRgb input) noexcept
{
    Vec3 scene{input.red, input.green, input.blue};
    for (double& channel : scene)
        if (!std::isfinite(channel) || channel <= 0.0) channel = 0.0;
    const Vec3 ap0 = Multiply(scene, transform.bt709ToAp0);
    Vec3 ap1 = Multiply(ap0, transform.ap0ToAp1);
    // Formation clamping belongs to AP1 before the Academy appearance model.
    for (double& channel : ap1)
        channel = std::isfinite(channel)
            ? std::clamp(channel, 0.0, transform.odt.toneScale.forwardLimit) : 0.0;
    const Vec3 aces = Multiply(ap1, transform.ap1ToAp0);
    const Vec3 toneMapped = ToneMapAndCompress(RgbToJMh(aces, transform.odt.input), transform.odt);
    const Vec3 compressed = CompressGamut(toneMapped, transform.odt);
    Vec3 output = JMhToRgb(compressed, transform.odt.limit);
    for (double& channel : output)
    {
        const double nits = channel * kReferenceLuminance;
        channel = std::isfinite(nits) ? std::clamp(nits, 0.0, transform.odt.peakLuminance) : 0.0;
    }
    return {output[0], output[1], output[2]};
}

}  // namespace

LinearRgb MapAces2ToBt2020Nits(LinearRgb sceneLinearBt709, double displayPeakNits) noexcept
{
    if (!std::isfinite(displayPeakNits)) return {};
    if (displayPeakNits < kMinimumDisplayPeakNits || displayPeakNits > kMaximumDisplayPeakNits)
        return {};
    struct Cache
    {
        double peak{};
        std::unique_ptr<PeakTransform> transform;
    };
    thread_local Cache cache;
    if (!cache.transform || cache.peak != displayPeakNits)
    {
        cache.transform.reset(new (std::nothrow) PeakTransform(displayPeakNits));
        cache.peak = displayPeakNits;
    }
    if (!cache.transform) return {};
    return EvaluateScene(*cache.transform, sceneLinearBt709);
}

}  // namespace fo4hdr

