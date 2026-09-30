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
// ACES2 alone blends a recomputed zero-toe shadow response into the scalar
// tone scale below internal 0.18 and uses a fixed-width gamut shoulder to avoid
// the reference reach/gamut crossing discontinuity.
// The official display-encoding/PQ stage is intentionally omitted because the
// renderer owns the single final ST 2084 boundary. Normalized upper-hull hue
// interpolation and a full bounded interval search correct the pinned CTL's
// hue interpolation/search issues. See THIRD_PARTY_NOTICES.md.

#include "Aces2Model.hpp"

#include <algorithm>
#include <cmath>

namespace fo4hdr::aces2 {

[[nodiscard]] Vec3 Multiply(Vec3 value, const Matrix& matrix) noexcept
{
    // CTL's mult_f3_f33 uses row-vector order. Keeping that order here is
    // essential because RGBtoXYZ_f33 stores each primary in a matrix row.
    return {
        value[0] * matrix[0][0] + value[1] * matrix[1][0] +
            value[2] * matrix[2][0],
        value[0] * matrix[0][1] + value[1] * matrix[1][1] +
            value[2] * matrix[2][1],
        value[0] * matrix[0][2] + value[1] * matrix[1][2] +
            value[2] * matrix[2][2]
    };
}

[[nodiscard]] Matrix Multiply(
    const Matrix& left,
    const Matrix& right) noexcept
{
    Matrix result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            for (std::size_t index = 0; index < 3; ++index)
                result[row][column] +=
                    left[row][index] * right[index][column];
    return result;
}

[[nodiscard]] Matrix Scale(double scale, Matrix matrix) noexcept
{
    for (auto& row : matrix)
        for (auto& value : row) value *= scale;
    return matrix;
}

[[nodiscard]] Matrix ScaleDiagonal(Matrix matrix, Vec3 scale) noexcept
{
    matrix[0][0] *= scale[0];
    matrix[1][1] *= scale[1];
    matrix[2][2] *= scale[2];
    return matrix;
}

[[nodiscard]] Matrix Invert(const Matrix& matrix) noexcept
{
    const double determinant =
        matrix[0][0] *
            (matrix[1][1] * matrix[2][2] -
             matrix[1][2] * matrix[2][1]) -
        matrix[0][1] *
            (matrix[1][0] * matrix[2][2] -
             matrix[1][2] * matrix[2][0]) +
        matrix[0][2] *
            (matrix[1][0] * matrix[2][1] -
             matrix[1][1] * matrix[2][0]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-20)
        return kIdentity;

    const double reciprocal = 1.0 / determinant;
    return {{
        {(matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) * reciprocal,
         (matrix[0][2] * matrix[2][1] - matrix[0][1] * matrix[2][2]) * reciprocal,
         (matrix[0][1] * matrix[1][2] - matrix[0][2] * matrix[1][1]) * reciprocal},
        {(matrix[1][2] * matrix[2][0] - matrix[1][0] * matrix[2][2]) * reciprocal,
         (matrix[0][0] * matrix[2][2] - matrix[0][2] * matrix[2][0]) * reciprocal,
         (matrix[0][2] * matrix[1][0] - matrix[0][0] * matrix[1][2]) * reciprocal},
        {(matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]) * reciprocal,
         (matrix[0][1] * matrix[2][0] - matrix[0][0] * matrix[2][1]) * reciprocal,
         (matrix[0][0] * matrix[1][1] - matrix[0][1] * matrix[1][0]) * reciprocal}
    }};
}

[[nodiscard]] Matrix RgbToXyz(
    const Chromaticities& chromaticities,
    double luminance) noexcept
{
    const double xWhite =
        chromaticities.white[0] * luminance / chromaticities.white[1];
    const double zWhite =
        (1.0 - chromaticities.white[0] - chromaticities.white[1]) *
        luminance / chromaticities.white[1];
    const double divisor =
        chromaticities.red[0] *
            (chromaticities.blue[1] - chromaticities.green[1]) +
        chromaticities.blue[0] *
            (chromaticities.green[1] - chromaticities.red[1]) +
        chromaticities.green[0] *
            (chromaticities.red[1] - chromaticities.blue[1]);

    const double redScale =
        (xWhite * (chromaticities.blue[1] - chromaticities.green[1]) -
         chromaticities.green[0] *
             (luminance * (chromaticities.blue[1] - 1.0) +
              chromaticities.blue[1] * (xWhite + zWhite)) +
         chromaticities.blue[0] *
             (luminance * (chromaticities.green[1] - 1.0) +
              chromaticities.green[1] * (xWhite + zWhite))) /
        divisor;
    const double greenScale =
        (xWhite * (chromaticities.red[1] - chromaticities.blue[1]) +
         chromaticities.red[0] *
             (luminance * (chromaticities.blue[1] - 1.0) +
              chromaticities.blue[1] * (xWhite + zWhite)) -
         chromaticities.blue[0] *
             (luminance * (chromaticities.red[1] - 1.0) +
              chromaticities.red[1] * (xWhite + zWhite))) /
        divisor;
    const double blueScale =
        (xWhite * (chromaticities.green[1] - chromaticities.red[1]) -
         chromaticities.red[0] *
             (luminance * (chromaticities.green[1] - 1.0) +
              chromaticities.green[1] * (xWhite + zWhite)) +
         chromaticities.green[0] *
             (luminance * (chromaticities.red[1] - 1.0) +
              chromaticities.red[1] * (xWhite + zWhite))) /
        divisor;

    return {{
        {redScale * chromaticities.red[0],
         redScale * chromaticities.red[1],
         redScale * (1.0 - chromaticities.red[0] -
                     chromaticities.red[1])},
        {greenScale * chromaticities.green[0],
         greenScale * chromaticities.green[1],
         greenScale * (1.0 - chromaticities.green[0] -
                       chromaticities.green[1])},
        {blueScale * chromaticities.blue[0],
         blueScale * chromaticities.blue[1],
         blueScale * (1.0 - chromaticities.blue[0] -
                      chromaticities.blue[1])}
    }};
}

[[nodiscard]] Vec3 XyToXyz(Vec2 xy) noexcept
{
    constexpr double luminance = 1.0;
    return {
        xy[0] * luminance / std::max(xy[1], 1.0e-10),
        luminance,
        (1.0 - xy[0] - xy[1]) * luminance /
            std::max(xy[1], 1.0e-10)
    };
}

[[nodiscard]] Matrix CalculateCat(
    Vec2 sourceWhite,
    Vec2 destinationWhite) noexcept
{
    const Vec3 sourceCone = Multiply(XyToXyz(sourceWhite), kBradford);
    const Vec3 destinationCone = Multiply(XyToXyz(destinationWhite), kBradford);
    Matrix vonKries = kIdentity;
    vonKries[0][0] = destinationCone[0] / sourceCone[0];
    vonKries[1][1] = destinationCone[1] / sourceCone[1];
    vonKries[2][2] = destinationCone[2] / sourceCone[2];
    return Multiply(kBradford, Multiply(vonKries, Invert(kBradford)));
}

[[nodiscard]] Matrix CalculateRgbToRgb(
    const Chromaticities& source,
    const Chromaticities& destination) noexcept
{
    return Multiply(
        RgbToXyz(source),
        Multiply(CalculateCat(source.white, destination.white),
                 Invert(RgbToXyz(destination))));
}

[[nodiscard]] double CopySignCtl(double magnitude, double sign) noexcept
{
    if (sign == 0.0) return 0.0;
    return std::copysign(std::abs(magnitude), sign);
}

[[nodiscard]] double WrapHue(double hue) noexcept
{
    double wrapped = std::fmod(hue, kHueLimit);
    if (wrapped < 0.0) wrapped += kHueLimit;
    return wrapped;
}

[[nodiscard]] std::size_t HuePosition(double hue) noexcept
{
    return static_cast<std::size_t>(
        WrapHue(hue) / kHueLimit * static_cast<double>(kTableSize));
}

[[nodiscard]] double BaseHue(std::size_t position) noexcept
{
    return static_cast<double>(position) * kHueLimit /
           static_cast<double>(kTableSize);
}

[[nodiscard]] double ConeCompressionForward(double value) noexcept
{
    const double powered = std::pow(std::abs(value), 0.42);
    const double compressed = powered / (kCamNlOffset + powered);
    return CopySignCtl(compressed, value);
}

[[nodiscard]] double ConeCompressionInverse(double value) noexcept
{
    const double limited = std::min(std::abs(value), 0.99);
    const double powered = kCamNlOffset * limited / (1.0 - limited);
    return CopySignCtl(std::pow(powered, 1.0 / 0.42), value);
}

[[nodiscard]] double AchromaticToJ(double achromatic, const JMhParams& params) noexcept
{
    return kJScale * std::pow(achromatic, params.cz);
}

[[nodiscard]] double JToAchromatic(double j, const JMhParams& params) noexcept
{
    return std::pow(j / kJScale, params.invCz);
}

[[nodiscard]] double JToY(double j, const JMhParams& params) noexcept
{
    const double achromatic = JToAchromatic(std::abs(j), params);
    const double response = params.awJ * achromatic;
    return ConeCompressionInverse(response) / params.fLn;
}

[[nodiscard]] double YToJ(double y, const JMhParams& params) noexcept
{
    const double response =
        ConeCompressionForward(std::abs(y) * params.fLn);
    return CopySignCtl(
        AchromaticToJ(response * params.invAwJ, params), y);
}

[[nodiscard]] Vec3 RgbToAab(Vec3 rgb, const JMhParams& params) noexcept
{
    const Vec3 cone = Multiply(rgb, params.rgbToCam16);
    const Vec3 adapted{
        ConeCompressionForward(cone[0]),
        ConeCompressionForward(cone[1]),
        ConeCompressionForward(cone[2])
    };
    return Multiply(adapted, params.coneToAab);
}

[[nodiscard]] Vec3 AabToJMh(Vec3 aab, const JMhParams& params) noexcept
{
    if (aab[0] <= 0.0) return {};
    return {
        AchromaticToJ(aab[0], params),
        std::sqrt(aab[1] * aab[1] + aab[2] * aab[2]),
        WrapHue(std::atan2(aab[2], aab[1]) * 180.0 / kPi)
    };
}

[[nodiscard]] Vec3 RgbToJMh(Vec3 rgb, const JMhParams& params) noexcept
{
    return AabToJMh(RgbToAab(rgb, params), params);
}

[[nodiscard]] Vec3 JMhToRgb(Vec3 jmh, const JMhParams& params) noexcept
{
    const double radians = jmh[2] * kPi / 180.0;
    const Vec3 aab{
        JToAchromatic(jmh[0], params),
        jmh[1] * std::cos(radians),
        jmh[1] * std::sin(radians)
    };
    const Vec3 adapted = Multiply(aab, params.aabToCone);
    const Vec3 cone{
        ConeCompressionInverse(adapted[0]),
        ConeCompressionInverse(adapted[1]),
        ConeCompressionInverse(adapted[2])
    };
    return Multiply(cone, params.cam16ToRgb);
}

[[nodiscard]] JMhParams InitializeJMh(
    const Chromaticities& primaries) noexcept
{
    const Matrix matrix16 = Invert(RgbToXyz(kCam16));
    constexpr Matrix baseConeToAab{{
        {2.0, 1.0, 1.0 / 9.0},
        {1.0, -12.0 / 11.0, 1.0 / 9.0},
        {1.0 / 20.0, 1.0 / 11.0, -2.0 / 9.0}
    }};
    const Matrix rgbToXyz = RgbToXyz(primaries);
    const Vec3 xyzWhite = Multiply(
        Vec3{kReferenceLuminance, kReferenceLuminance,
             kReferenceLuminance},
        rgbToXyz);
    const Vec3 rgbWhite = Multiply(xyzWhite, matrix16);
    const double yWhite = xyzWhite[1];

    const double k = 1.0 / (5.0 * kAdaptingLuminance + 1.0);
    const double k4 = k * k * k * k;
    const double fL =
        0.2 * k4 * (5.0 * kAdaptingLuminance) +
        0.1 * std::pow(1.0 - k4, 2.0) *
            std::pow(5.0 * kAdaptingLuminance, 1.0 / 3.0);
    const double fLn = fL / kReferenceLuminance;
    const Vec3 adaptation{
        fLn * yWhite / rgbWhite[0],
        fLn * yWhite / rgbWhite[1],
        fLn * yWhite / rgbWhite[2]
    };
    const Vec3 adaptedWhite{
        ConeCompressionForward(adaptation[0] * rgbWhite[0]),
        ConeCompressionForward(adaptation[1] * rgbWhite[1]),
        ConeCompressionForward(adaptation[2] * rgbWhite[2])
    };

    const Matrix coneToAabBase = Scale(kCamNlScale, baseConeToAab);
    const double aw =
        coneToAabBase[0][0] * adaptedWhite[0] +
        coneToAabBase[1][0] * adaptedWhite[1] +
        coneToAabBase[2][0] * adaptedWhite[2];
    const double awJ = ConeCompressionForward(fL);

    const Matrix rgbToCam = Multiply(
        Multiply(rgbToXyz, matrix16),
        Scale(kReferenceLuminance, kIdentity));
    const Matrix rgbToCamAdapted = Multiply(
        rgbToCam,
        ScaleDiagonal(kIdentity, adaptation));

    Matrix coneToAab{};
    for (std::size_t row = 0; row < 3; ++row)
    {
        coneToAab[row][0] = coneToAabBase[row][0] / aw;
        coneToAab[row][1] =
            coneToAabBase[row][1] * 43.0 * kSurround[2];
        coneToAab[row][2] =
            coneToAabBase[row][2] * 43.0 * kSurround[2];
    }

    return {
        .rgbToCam16 = rgbToCamAdapted,
        .cam16ToRgb = Invert(rgbToCamAdapted),
        .coneToAab = coneToAab,
        .aabToCone = Invert(coneToAab),
        .fLn = fLn,
        .cz = kModelGamma,
        .invCz = 1.0 / kModelGamma,
        .awJ = awJ,
        .invAwJ = 1.0 / awJ
    };
}

}  // namespace fo4hdr::aces2
