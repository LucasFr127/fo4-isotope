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
// Modified by Fallout4HDR: parameterize the same Rec.2100 limiting gamut for
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

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace fo4hdr::aces2 {

using Vec2 = std::array<double, 2>;
using Vec3 = std::array<double, 3>;
using Matrix = std::array<Vec3, 3>;

struct Chromaticities
{
    Vec2 red;
    Vec2 green;
    Vec2 blue;
    Vec2 white;
};

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kHueLimit = 360.0;
constexpr std::size_t kTableSize = 360;
constexpr std::size_t kTotalTableSize = kTableSize + 2;
constexpr std::size_t kBaseIndex = 1;
constexpr std::size_t kCuspCornerCount = 6;
constexpr std::size_t kTotalCornerCount = kCuspCornerCount + 2;
constexpr std::size_t kMaxSortedCorners = 2 * kCuspCornerCount;

constexpr double kReferenceLuminance = 100.0;
constexpr double kAdaptingLuminance = 100.0;
constexpr double kBackgroundLuminance = 20.0;
constexpr Vec3 kSurround{0.9, 0.59, 0.9};
constexpr double kJScale = 100.0;
constexpr double kCamNlOffset = 0.2713 * kReferenceLuminance;
constexpr double kCamNlScale = 4.0 * kReferenceLuminance;
inline const double kModelGamma =
    kSurround[1] * (1.48 + std::sqrt(kBackgroundLuminance /
                                     kReferenceLuminance));

constexpr double kChromaCompress = 2.4;
constexpr double kChromaCompressFactor = 3.3;
constexpr double kChromaExpand = 1.3;
constexpr double kChromaExpandFactor = 0.69;
constexpr double kChromaExpandThreshold = 0.5;

constexpr double kSmoothCusps = 0.12;
constexpr double kSmoothM = 0.27;
constexpr double kCuspMidBlend = 1.3;
constexpr double kFocusGainBlend = 0.3;
constexpr double kFocusDistance = 1.35;
constexpr double kFocusDistanceScaling = 1.75;
constexpr double kCompressionThreshold = 0.75;

constexpr double kReachCuspTolerance = 1.0e-3;
constexpr double kDisplayCuspTolerance = 1.0e-7;
constexpr double kGammaMinimum = 0.0;
constexpr double kGammaMaximum = 5.0;
constexpr double kGammaSearchStep = 0.4;
constexpr double kGammaAccuracy = 1.0e-5;

constexpr Matrix kIdentity{{
    {1.0, 0.0, 0.0},
    {0.0, 1.0, 0.0},
    {0.0, 0.0, 1.0}
}};

constexpr Chromaticities kAp0{
    {0.73470, 0.26530},
    {0.00000, 1.00000},
    {0.00010, -0.07700},
    {0.32168, 0.33767}
};
constexpr Chromaticities kAp1{
    {0.713, 0.293},
    {0.165, 0.830},
    {0.128, 0.044},
    {0.32168, 0.33767}
};
constexpr Chromaticities kBt709{
    {0.640, 0.330},
    {0.300, 0.600},
    {0.150, 0.060},
    {0.3127, 0.3290}
};
constexpr Chromaticities kBt2020{
    {0.7080, 0.2920},
    {0.1700, 0.7970},
    {0.1310, 0.0460},
    {0.3127, 0.3290}
};
constexpr Chromaticities kCam16{
    {0.8336, 0.1735},
    {2.3854, -1.4659},
    {0.087, -0.125},
    {0.333, 0.333}
};
constexpr Matrix kBradford{{
    {0.89510, -0.75020, 0.03890},
    {0.26640, 1.71350, -0.06850},
    {-0.16140, 0.03670, 1.02960}
}};


struct ToneScaleParams
{
    double nR{};
    double g{};
    double t1{};
    double ct{};
    double s2{};
    double m2{};
    double forwardLimit{};
    double logPeak{};
};

struct JMhParams
{
    Matrix rgbToCam16{};
    Matrix cam16ToRgb{};
    Matrix coneToAab{};
    Matrix aabToCone{};
    double fLn{};
    double cz{};
    double invCz{};
    double awJ{};
    double invAwJ{};
};

struct OdtParams
{
    double peakLuminance{};
    JMhParams input{};
    JMhParams reach{};
    JMhParams limit{};
    ToneScaleParams toneScale{};
    double limitJMax{};
    double modelGammaInverse{};
    std::array<double, kTotalTableSize> reachM{};
    double saturation{};
    double saturationThreshold{};
    double compression{};
    double chromaCompressionScale{};
    double midJ{};
    double focusDistance{};
    double lowerHullGammaInverse{};
    std::array<double, kTotalTableSize> hues{};
    std::array<Vec3, kTotalTableSize> gamutCusps{};
    std::array<double, kTotalTableSize> upperHullGamma{};
};


struct CompressionAxis
{
    double focusJ;
    double limitJ;
    double slopeGain;
};

struct CompressionRay
{
    double jAxisIntersect;
    double slope;
};

struct GamutHull
{
    Vec2 cusp;
    double jMax;
    double upperGammaInverse;
    double lowerGammaInverse;
};

[[nodiscard]] Vec3 Multiply(Vec3 value, const Matrix& matrix) noexcept;

[[nodiscard]] Matrix Multiply(
    const Matrix& left,
    const Matrix& right) noexcept;

[[nodiscard]] Matrix Invert(const Matrix& matrix) noexcept;

[[nodiscard]] Matrix RgbToXyz(
    const Chromaticities& chromaticities,
    double luminance = 1.0) noexcept;

[[nodiscard]] Matrix CalculateRgbToRgb(
    const Chromaticities& source,
    const Chromaticities& destination) noexcept;

[[nodiscard]] std::size_t HuePosition(double hue) noexcept;

[[nodiscard]] double BaseHue(std::size_t position) noexcept;

[[nodiscard]] double JToAchromatic(double j, const JMhParams& params) noexcept;

[[nodiscard]] double JToY(double j, const JMhParams& params) noexcept;

[[nodiscard]] double YToJ(double y, const JMhParams& params) noexcept;

[[nodiscard]] Vec3 RgbToAab(Vec3 rgb, const JMhParams& params) noexcept;

[[nodiscard]] Vec3 RgbToJMh(Vec3 rgb, const JMhParams& params) noexcept;

[[nodiscard]] Vec3 JMhToRgb(Vec3 jmh, const JMhParams& params) noexcept;

[[nodiscard]] JMhParams InitializeJMh(
    const Chromaticities& primaries) noexcept;

[[nodiscard]] ToneScaleParams InitializeToneScale(
    double peak, double toe = 0.04) noexcept;

[[nodiscard]] double SolveJIntersect(
    Vec2 jm,
    const CompressionAxis& axis) noexcept;

[[nodiscard]] double CompressionVectorSlope(
    double intersectJ,
    const CompressionAxis& axis) noexcept;

[[nodiscard]] double FindBoundaryIntersectionM(
    const GamutHull& hull,
    CompressionRay ray,
    double cuspIntersectJ) noexcept;

[[nodiscard]] double FocusGain(
    double j,
    double analyticalThreshold,
    const OdtParams& params) noexcept;

[[nodiscard]] OdtParams InitializeOdt(double peak) noexcept;

}  // namespace fo4hdr::aces2
