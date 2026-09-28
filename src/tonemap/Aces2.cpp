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
// Modified by FO4Isotope: parameterize the same Rec.2100 limiting gamut for
// 400-2000 nits and adapt dimensionless scene-linear BT.709/D65 to AP0/D60
// without changing neutral scene values, and stop at absolute display-linear
// Rec.2020. The direct AP0 forward boundary supports the pinned Contrast LMT;
// the shared forward path retains the official AP1 formation clamp.
// ACES2LMT alone blends a recomputed zero-toe shadow response into the scalar
// tone scale below internal 0.18 and uses a fixed-width gamut shoulder to avoid
// the reference reach/gamut crossing discontinuity.
// The official display-encoding/PQ stage is intentionally omitted because the
// renderer owns the single final ST 2084 boundary. Normalized upper-hull hue
// interpolation and a full bounded interval search correct the pinned CTL's
// hue interpolation/search issues. See THIRD_PARTY_NOTICES.md.

#include "FO4Isotope/tonemap/Aces2.hpp"

#include "aces2/Aces2Model.hpp"

#include "FO4Isotope/tonemap/Aces2Lmt.hpp"

#include "FO4Isotope/output/Hdr10Color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>

namespace fo4hdr {
using namespace aces2;
namespace aces2 {

[[nodiscard]] double ToneScaleHit(double peak) noexcept
{
    constexpr double normalizedWhite = 100.0;
    constexpr double hitMinimum = 128.0;
    constexpr double hitMaximum = 896.0;
    return hitMinimum + (hitMaximum - hitMinimum) *
        (std::log(peak / normalizedWhite) /
         std::log(10000.0 / normalizedWhite));
}

[[nodiscard]] ToneScaleParams InitializeToneScale(
    double peak, double toe) noexcept
{
    constexpr double normalizedWhite = 100.0;
    constexpr double contrast = 1.15;
    constexpr double grayAnchor = 0.18;
    constexpr double grayDisplay = 10.013;
    constexpr double grayChange = 0.14;
    const double hit = ToneScaleHit(peak);
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

[[nodiscard]] double ToneScaleForward(
    double value,
    const ToneScaleParams& params) noexcept
{
    const double positive = std::max(0.0, value);
    const double formation = params.m2 * std::pow(
        positive / (value + params.s2), params.g);
    // The recomputed zero-toe response has this finite limit at exact black.
    if (params.t1 == 0.0) return formation * params.nR;
    const double flared =
        std::max(0.0, formation * formation / (formation + params.t1));
    return flared * params.nR;
}

[[nodiscard]] double Toe(
    double value,
    double limit,
    double k1Input,
    double k2Input) noexcept
{
    if (value > limit) return value;
    const double k2 = std::max(k2Input, 0.001);
    const double k1 = std::sqrt(k1Input * k1Input + k2 * k2);
    const double k3 = (limit + k1) / (limit + k2);
    const double minusB = k3 * value - k1;
    const double minusC = k2 * k3 * value;
    return 0.5 *
           (minusB + std::sqrt(minusB * minusB + 4.0 * minusC));
}

[[nodiscard]] double ChromaCompressionNorm(
    double hue,
    const OdtParams& params) noexcept
{
    const double radians = hue * kPi / 180.0;
    const double a = std::cos(radians);
    const double b = std::sin(radians);
    const double cos2 = a * a - b * b;
    const double sin2 = 2.0 * a * b;
    const double cos3 = 4.0 * a * a * a - 3.0 * a;
    const double sin3 = 3.0 * b - 4.0 * b * b * b;
    return (11.34072 * a + 16.46899 * cos2 + 7.88380 * cos3 +
            14.66441 * b - 6.37224 * sin2 + 9.19364 * sin3 +
            77.12896) *
           params.chromaCompressionScale;
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
    const OdtParams& params,
    const ToneScaleParams* shadowToneScale = nullptr) noexcept
{
    const double linear = JToY(jmh[0], params.input) /
                          kReferenceLuminance;
    double toneMappedY = ToneScaleForward(linear, params.toneScale);
    // This scalar is evaluated after the Look and AP1 formation clamp. Keep
    // the Academy response exact above the transition, then let its complete
    // chroma and gamut processing respond to the modified lightness.
    if (shadowToneScale != nullptr && linear < 0.18)
    {
        const double q = std::clamp(linear / 0.18, 0.0, 1.0);
        const double weight = 1.0 - q * q * (3.0 - 2.0 * q);
        toneMappedY += weight *
            (ToneScaleForward(linear, *shadowToneScale) - toneMappedY);
    }
    const double toneMappedJ = YToJ(toneMappedY, params.input);
    double compressedM = jmh[1];

    if (jmh[1] != 0.0 && jmh[0] > 0.0)
    {
        const double normalizedJ = toneMappedJ / params.limitJMax;
        const double shadowJ = std::max(0.0, 1.0 - normalizedJ);
        const double normalization = ChromaCompressionNorm(
            jmh[2], params);
        const double limit =
            std::pow(normalizedJ, params.modelGammaInverse) *
            ReachMFromTable(jmh[2], params.reachM) /
            normalization;

        compressedM = jmh[1] * std::pow(
            toneMappedJ / jmh[0], params.modelGammaInverse);
        compressedM /= normalization;
        compressedM = limit - Toe(
            limit - compressedM,
            limit - 0.001,
            shadowJ * params.saturation,
            std::sqrt(normalizedJ * normalizedJ +
                      params.saturationThreshold));
        compressedM = Toe(
            compressedM,
            limit,
            normalizedJ * params.compression,
            shadowJ);
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

[[nodiscard]] double SmoothMinimumScaled(
    double a,
    double b,
    double scaleReference) noexcept
{
    const double scale = kSmoothCusps * scaleReference;
    const double h = std::max(scale - std::abs(a - b), 0.0) / scale;
    return std::min(a, b) - h * h * h * scale / 6.0;
}

[[nodiscard]] double EstimateBoundaryIntersectionM(
    CompressionRay ray,
    double inverseGamma,
    Vec2 maximum,
    double jIntersectionReference) noexcept
{
    const double normalizedJ = ray.jAxisIntersect / jIntersectionReference;
    const double shiftedIntersection =
        jIntersectionReference * std::pow(normalizedJ, inverseGamma);
    return shiftedIntersection * maximum[1] /
           (maximum[0] - ray.slope * maximum[1]);
}

[[nodiscard]] double FindBoundaryIntersectionM(
    const GamutHull& hull,
    CompressionRay ray,
    double cuspIntersectJ) noexcept
{
    const double lower = EstimateBoundaryIntersectionM(
        ray,
        hull.lowerGammaInverse,
        hull.cusp,
        cuspIntersectJ);
    const double upper = EstimateBoundaryIntersectionM(
        {hull.jMax - ray.jAxisIntersect, -ray.slope},
        hull.upperGammaInverse,
        {hull.jMax - hull.cusp[0], hull.cusp[1]},
        hull.jMax - cuspIntersectJ);
    return SmoothMinimumScaled(lower, upper, hull.cusp[1]);
}

[[nodiscard]] double FocusGain(
    double j,
    double analyticalThreshold,
    const OdtParams& params) noexcept
{
    double gain = params.limitJMax * params.focusDistance;
    if (j > analyticalThreshold)
    {
        double adjustment = std::log10(
            (params.limitJMax - analyticalThreshold) /
            std::max(0.0001, params.limitJMax - j));
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

[[nodiscard]] double RemapProductionM(
    double m,
    GamutChromaBounds bounds) noexcept
{
    const double threshold = kCompressionThreshold * bounds.gamut;
    if (m <= threshold || bounds.reach <= bounds.gamut) return m;

    // A fixed shoulder width avoids the reference threshold collapsing to
    // the gamut boundary as reach approaches gamut. Its reciprocal scale
    // tends to zero there, so this rational curve joins identity continuously
    // while mapping reach to gamut and retaining unit slope at the shoulder.
    const double inverseScale = (bounds.reach - bounds.gamut) /
        ((bounds.reach - threshold) * (bounds.gamut - threshold));
    const double offset = m - threshold;
    return threshold + offset / (1.0 + offset * inverseScale);
}

[[nodiscard]] std::size_t LookupHueInterval(
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
    return static_cast<std::size_t>(std::max(1, high));
}

[[nodiscard]] Vec2 CuspFromTable(
    double hue,
    const std::array<Vec3, kTotalTableSize>& table) noexcept
{
    std::size_t low = 0;
    std::size_t high = kBaseIndex + kTableSize;
    std::size_t position = HuePosition(hue) + kBaseIndex;
    while (low + 1 < high)
    {
        if (hue > table[position][2]) low = position;
        else high = position;
        position = (low + high) / 2;
    }
    const Vec3& lower = table[high - 1];
    const Vec3& upper = table[high];
    const double weight =
        (hue - lower[2]) / (upper[2] - lower[2]);
    return {
        std::lerp(lower[0], upper[0], weight),
        std::lerp(lower[1], upper[1], weight)
    };
}

void RecordCompressionBoundary(
    Aces2CompressionBoundary* boundary,
    GamutChromaBounds bounds,
    double sourceM) noexcept
{
    if (boundary == nullptr) return;
    if (!std::isfinite(bounds.gamut)) return;
    if (!std::isfinite(bounds.reach) || bounds.reach <= 0.0) return;
    *boundary = {bounds.gamut - bounds.reach, sourceM - bounds.gamut, true};
}

[[nodiscard]] Vec3 CompressGamut(
    Vec3 jmh,
    const OdtParams& params,
    Aces2CompressionBoundary* boundary = nullptr,
    bool production = false) noexcept
{
    if (jmh[0] <= 0.0) return {0.0, 0.0, jmh[2]};
    if (jmh[1] < 0.0 || jmh[0] > params.limitJMax)
        return {jmh[0], 0.0, jmh[2]};

    const std::size_t high = LookupHueInterval(jmh[2], params);
    // Both tables use the same nonuniform hue grid, including padded wrap.
    const double weight = (jmh[2] - params.hues[high - 1]) /
        (params.hues[high] - params.hues[high - 1]);
    const Vec2 cusp = production ? Vec2{
        std::lerp(params.gamutCusps[high - 1][0], params.gamutCusps[high][0], weight),
        std::lerp(params.gamutCusps[high - 1][1], params.gamutCusps[high][1], weight)
    } : CuspFromTable(jmh[2], params.gamutCusps);
    const double upperGammaInverse = std::lerp(
        params.upperHullGamma[high - 1],
        params.upperHullGamma[high],
        weight);
    const double focusJ = std::lerp(
        cusp[0],
        params.midJ,
        std::min(1.0,
                 kCuspMidBlend - cusp[0] / params.limitJMax));
    const double analyticalThreshold =
        std::lerp(cusp[0], params.limitJMax, kFocusGainBlend);
    const double slopeGain = FocusGain(
        jmh[0], analyticalThreshold, params);
    const CompressionAxis axis{focusJ, params.limitJMax, slopeGain};
    const double sourceIntersectJ = SolveJIntersect(
        {jmh[0], jmh[1]}, axis);
    const double gamutSlope = CompressionVectorSlope(
        sourceIntersectJ, axis);
    const double cuspIntersectJ = SolveJIntersect(
        cusp, axis);
    const double gamutBoundaryM = FindBoundaryIntersectionM(
        {cusp, params.limitJMax, upperGammaInverse, params.lowerHullGammaInverse},
        {sourceIntersectJ, gamutSlope},
        cuspIntersectJ);
    if (gamutBoundaryM <= 0.0)
        return {jmh[0], 0.0, jmh[2]};

    const double reachBoundaryM = EstimateBoundaryIntersectionM(
        {sourceIntersectJ, gamutSlope},
        params.modelGammaInverse,
        {params.limitJMax, ReachMFromTable(jmh[2], params.reachM)},
        params.limitJMax);
    const GamutChromaBounds bounds{gamutBoundaryM, reachBoundaryM};
    RecordCompressionBoundary(boundary, bounds, jmh[1]);
    const double remappedM = production ?
        RemapProductionM(jmh[1], bounds) :
        RemapM(jmh[1], bounds);
    const Vec3 result{
        sourceIntersectJ + remappedM * gamutSlope,
        remappedM,
        jmh[2]
    };
    return result;
}

[[nodiscard]] double SanitizeScene(double value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0) return 0.0;
    return value;
}

[[nodiscard]] bool IsPeakValid(double peak) noexcept
{
    return std::isfinite(peak) &&
           peak >= kMinimumDisplayPeakNits &&
           peak <= kMaximumDisplayPeakNits;
}

[[nodiscard]] bool IsLookStrengthValid(double strength) noexcept
{
    return std::isfinite(strength) && strength >= 0.0 && strength <= 1.0;
}

}  // namespace

class Aces2Transform
{
public:
    explicit Aces2Transform(double peak) noexcept
        : bt709ToAp0(CalculateRgbToRgb(kBt709, kAp0)),
          ap0ToAp1(Multiply(RgbToXyz(kAp0), Invert(RgbToXyz(kAp1)))),
          ap1ToAp0(Multiply(RgbToXyz(kAp1), Invert(RgbToXyz(kAp0)))),
          odt(InitializeOdt(peak)),
          shadowToneScale(InitializeToneScale(peak, 0.0))
    {
    }

    Matrix bt709ToAp0;
    Matrix ap0ToAp1;
    Matrix ap1ToAp0;
    OdtParams odt;
    ToneScaleParams shadowToneScale;
};

void Aces2TransformDeleter::operator()(
    const Aces2Transform* transform) const noexcept
{
    delete transform;
}

Aces2TransformHandle CreateAces2Transform(double displayPeakNits) noexcept
{
    if (!IsPeakValid(displayPeakNits)) return {nullptr};
    return Aces2TransformHandle(
        new (std::nothrow) Aces2Transform(displayPeakNits));
}

namespace {

[[nodiscard]] Vec3 EvaluateAces2Ap1GamutInput(
    const Aces2Transform& transform,
    Vec3 ap1,
    const ToneScaleParams* shadowToneScale = nullptr) noexcept
{
    for (double& channel : ap1)
        channel = std::clamp(
            channel, 0.0, transform.odt.toneScale.forwardLimit);
    const Vec3 aces = Multiply(ap1, transform.ap1ToAp0);

    return ToneMapAndCompress(
        RgbToJMh(aces, transform.odt.input), transform.odt, shadowToneScale);
}

[[nodiscard]] Vec3 EvaluateAces2Ap0GamutInput(
    const Aces2Transform& transform,
    Vec3 aces,
    const ToneScaleParams* shadowToneScale = nullptr) noexcept
{
    return EvaluateAces2Ap1GamutInput(
        transform, Multiply(aces, transform.ap0ToAp1), shadowToneScale);
}

[[nodiscard]] Vec3 EvaluateAces2GamutInput(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709) noexcept
{
    const Vec3 scene{
        SanitizeScene(sceneLinearBt709.red),
        SanitizeScene(sceneLinearBt709.green),
        SanitizeScene(sceneLinearBt709.blue)
    };
    return EvaluateAces2Ap0GamutInput(
        transform, Multiply(scene, transform.bt709ToAp0));
}

[[nodiscard]] LinearRgb EvaluateAces2GamutOutputNits(
    const Aces2Transform& transform,
    Vec3 toneMapped,
    bool production = false) noexcept
{
    const Vec3 compressed = CompressGamut(toneMapped, transform.odt, nullptr, production);
    Vec3 output = JMhToRgb(compressed, transform.odt.limit);

    // Preserve the gamut fit's small boundary excursions while interpolating
    // the LUT. Containing individual lattice vertices biases boundary cells.
    for (double& channel : output)
    {
        channel *= kReferenceLuminance;
        if (!std::isfinite(channel)) channel = 0.0;
    }
    return {output[0], output[1], output[2]};
}

[[nodiscard]] Vec3 EvaluateAces2LmtGamutInput(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709,
    double lookStrength) noexcept
{
    const Vec3 scene{
        SanitizeScene(sceneLinearBt709.red),
        SanitizeScene(sceneLinearBt709.green),
        SanitizeScene(sceneLinearBt709.blue)
    };
    const Vec3 ap0 = Multiply(scene, transform.bt709ToAp0);
    Vec3 blended = ap0;
    if (lookStrength != 0.0)
    {
        const auto looked = ApplyAces2ContrastLmt(
            {ap0[0], ap0[1], ap0[2]});
        blended = lookStrength == 1.0 ?
            Vec3{looked.red, looked.green, looked.blue} : Vec3{
                std::lerp(ap0[0], looked.red, lookStrength),
                std::lerp(ap0[1], looked.green, lookStrength),
                std::lerp(ap0[2], looked.blue, lookStrength)};
    }
    return EvaluateAces2Ap0GamutInput(
        transform, blended, &transform.shadowToneScale);
}

}  // namespace

Aces2CompressionBoundary EvaluateAces2LmtCompressionBoundary(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709,
    double lookStrength) noexcept
{
    if (!IsLookStrengthValid(lookStrength)) return {};
    Aces2CompressionBoundary boundary{};
    static_cast<void>(CompressGamut(
        EvaluateAces2LmtGamutInput(transform, sceneLinearBt709, lookStrength),
        transform.odt, &boundary, true));
    return boundary;
}

LinearRgb EvaluateAces2LutNits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709) noexcept
{
    const Vec3 toneMapped = EvaluateAces2GamutInput(
        transform, sceneLinearBt709);
    return EvaluateAces2GamutOutputNits(transform, toneMapped);
}

LinearRgb EvaluateAces2LmtLutNits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709,
    double lookStrength) noexcept
{
    if (!IsLookStrengthValid(lookStrength)) return {};
    return EvaluateAces2GamutOutputNits(transform,
        EvaluateAces2LmtGamutInput(transform, sceneLinearBt709, lookStrength), true);
}

LinearRgb MapAces2LmtToBt2020Nits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709,
    double lookStrength) noexcept
{
    const auto output = EvaluateAces2LmtLutNits(
        transform, sceneLinearBt709, lookStrength);
    return {
        std::clamp(output.red, 0.0, transform.odt.peakLuminance),
        std::clamp(output.green, 0.0, transform.odt.peakLuminance),
        std::clamp(output.blue, 0.0, transform.odt.peakLuminance)
    };
}

LinearRgb EvaluateAces2Ap0LutNits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearAp0) noexcept
{
    Vec3 aces{
        sceneLinearAp0.red, sceneLinearAp0.green, sceneLinearAp0.blue};
    for (double& channel : aces)
        if (!std::isfinite(channel)) channel = 0.0;
    return EvaluateAces2GamutOutputNits(
        transform, EvaluateAces2Ap0GamutInput(transform, aces));
}

LinearRgb MapAces2ToBt2020Nits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709) noexcept
{
    const auto output = EvaluateAces2LutNits(
        transform, sceneLinearBt709);
    // The pinned preset contains display-linear RGB before encoding. LUT
    // sampling performs this same containment after interpolation instead.
    return {
        std::clamp(output.red, 0.0, transform.odt.peakLuminance),
        std::clamp(output.green, 0.0, transform.odt.peakLuminance),
        std::clamp(output.blue, 0.0, transform.odt.peakLuminance)
    };
}

LinearRgb MapAces2ToBt2020Nits(
    LinearRgb sceneLinearBt709,
    double displayPeakNits) noexcept
{
    struct Cache
    {
        double peak{};
        Aces2TransformHandle transform{};
    };
    thread_local Cache cache{};
    if (!IsPeakValid(displayPeakNits)) return {};
    if (!cache.transform || cache.peak != displayPeakNits)
    {
        cache.transform = CreateAces2Transform(displayPeakNits);
        cache.peak = displayPeakNits;
    }
    if (!cache.transform) return {};
    return MapAces2ToBt2020Nits(*cache.transform, sceneLinearBt709);
}

}  // namespace fo4hdr
