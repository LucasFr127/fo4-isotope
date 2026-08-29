// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/tonemap/Aces2Lmt.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace fo4hdr {
namespace {

using Vec3 = std::array<double, 3>;

[[nodiscard]] bool IsFinite(Vec3 value) noexcept
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
        std::isfinite(value[2]);
}

// SPDX-SnippetBegin
// SPDX-License-Identifier: Apache-2.0
// SPDX-SnippetCopyrightText: Copyright Contributors to the ACES Project
// SPDX-SnippetComment: Adapted from scottdyer/aces-look@dc79dd74554e73a9e1518ec5f00ae0c1725f1686/contrast/LMT_Test_OCIO_options.dctl, selecting only J2M2_CURVE ("J curve 2 + M curve 2"). Modified by Fallout4HDR: translated the dependency-free DCTL float transform to C++ double while retaining the source float constants and spline extrapolation; corrected hue on the vertical opponent axis; replaced DCTL vector/intrinsic syntax with project value types and standard math; added explicit nonfinite-input/output rejection; and omitted UI selection plus every unselected LMT curve. See THIRD_PARTY_NOTICES.md.
// Upstream file SHA-256: b5b1022c46758c02edd4c3967375d6daa0cd022e78c85962004a2f628318711f.

// Each literal is first rounded as the source DCTL's float literal and then
// promoted to double. Evaluation itself uses double so LUT generation does not
// add a second float-rounding step at every operation.
constexpr std::array<double, 15> kJ2Knots{
    0.0F,
    0.0330908746F,
    0.0634082332F,
    0.0868174285F,
    0.1123465F,
    0.141559377F,
    0.168578103F,
    0.212038472F,
    0.252095997F,
    0.328614175F,
    0.405946195F,
    0.496465236F,
    0.554657578F,
    0.711169541F,
    0.875658214F
};

constexpr std::array<double, 42> kJ2Coefficients{
    0.583150923F,
    2.73948884F,
    4.06192493F,
    0.650720537F,
    0.231780261F,
    3.648947F,
    0.510046005F,
    3.10289025F,
    0.716681123F,
    0.354450703F,
    -1.21799815F,
    9.68025112F,
    1.06305385F,
    0.220671952F,
    0.982156217F,
    1.02075016F,
    1.1868583F,
    1.37703109F,
    1.41025567F,
    1.42379761F,
    1.6209774F,
    1.66531098F,
    1.91389918F,
    2.02357745F,
    2.07839823F,
    1.85789418F,
    2.98452711F,
    3.3172884F,
    0.0F,
    0.0331389606F,
    0.0666033924F,
    0.0966126919F,
    0.132191107F,
    0.173586532F,
    0.214719504F,
    0.286131173F,
    0.357818305F,
    0.508462548F,
    0.667069614F,
    0.845224321F,
    0.986120224F,
    1.47927499F
};

constexpr std::array<double, 10> kM2Knots{
    0.0F,
    0.0251623373F,
    0.0500000007F,
    0.100000009F,
    0.150000006F,
    0.285999984F,
    0.319999993F,
    0.558181763F,
    0.75F,
    1.0F
};

constexpr std::array<double, 27> kM2Coefficients{
    0.602912724F,
    2.98396826F,
    2.38094521F,
    -1.90475237F,
    0.905670822F,
    -33.2700272F,
    -0.5602054F,
    -1.00038815F,
    0.0F,
    2.44047642F,
    2.4708178F,
    2.6190474F,
    2.85714197F,
    2.66666675F,
    2.91300917F,
    0.650646806F,
    0.383785367F,
    0.0F,
    1.0F,
    1.06178975F,
    1.125F,
    1.26190472F,
    1.39999998F,
    1.77941787F,
    1.84000003F,
    1.96319151F,
    2.0F
};

constexpr double kJNormalization = 400.0F;
constexpr double kLmsExponent = 0.42F;
constexpr double kCamOffset = 27.13F;
constexpr double kJExponent = 1.13705599F;
constexpr double kInverseJExponent = 0.879464149F;
constexpr double kInverseLmsExponent = 2.38095236F;
constexpr double kRadiansToDegrees = 57.2957795F;
constexpr double kDegreesToRadians = 0.0174532924F;
constexpr double kAdaptedConeLimit = 0.99F;

template <std::size_t KnotCount, std::size_t CoefficientCount>
[[nodiscard]] double EvaluateOcioBSpline(
    double x,
    const std::array<double, KnotCount>& knots,
    const std::array<double, CoefficientCount>& coefficients) noexcept
{
    static_assert(KnotCount >= 2);
    static_assert(CoefficientCount == (KnotCount - 1) * 3);
    constexpr std::size_t setCount = CoefficientCount / 3;

    const double knotStart = knots.front();
    const double knotEnd = knots.back();
    if (x <= knotStart)
        return (x - knotStart) * coefficients[setCount] +
            coefficients[setCount * 2];

    if (x >= knotEnd)
    {
        const double t = knotEnd - knots[KnotCount - 2];
        const double slope = 2.0 * coefficients[setCount - 1] * t +
            coefficients[setCount * 2 - 1];
        const double offset =
            (coefficients[setCount - 1] * t +
             coefficients[setCount * 2 - 1]) *
                t +
            coefficients[setCount * 3 - 1];
        return (x - knotEnd) * slope + offset;
    }

    std::size_t segment = 0;
    while (segment < KnotCount - 2 && x >= knots[segment + 1])
        ++segment;
    const double t = x - knots[segment];
    return (coefficients[segment] * t +
            coefficients[setCount + segment]) *
            t +
        coefficients[setCount * 2 + segment];
}

[[nodiscard]] double SignedPower(double value, double exponent) noexcept
{
    return std::copysign(std::pow(std::abs(value), exponent), value);
}

[[nodiscard]] Vec3 Ap0ToJMh(Vec3 rgb) noexcept
{
    const double l =
        0.445181072F * rgb[0] + 0.34964928F * rgb[1] -
        0.00112973817F * rgb[2];
    const double m =
        0.123734139F * rgb[0] + 0.613643706F * rgb[1] +
        0.0563227981F * rgb[2];
    const double s =
        0.0117006991F * rgb[0] + 0.0280607957F * rgb[1] +
        0.753939092F * rgb[2];

    const double lPrime = SignedPower(l, kLmsExponent);
    const double mPrime = SignedPower(m, kLmsExponent);
    const double sPrime = SignedPower(s, kLmsExponent);
    const double lAdapted = lPrime / (kCamOffset + std::abs(lPrime));
    const double mAdapted = mPrime / (kCamOffset + std::abs(mPrime));
    const double sAdapted = sPrime / (kCamOffset + std::abs(sPrime));

    const double achromatic =
        20.25881F * lAdapted + 10.129405F * mAdapted +
        0.506470263F * sAdapted;
    const double opponentA =
        15480.0F * lAdapted - 16887.2734F * mAdapted +
        1407.27271F * sAdapted;
    const double opponentB =
        1720.0F * lAdapted + 1720.0F * mAdapted -
        3440.0F * sAdapted;

    if (!(achromatic > 0.0)) return {};

    // A zero horizontal opponent component still has a +/-90-degree hue
    // when the vertical component is nonzero. Only the origin is achromatic.
    double hue = opponentA == 0.0 && opponentB == 0.0
        ? 0.0
        : std::atan2(opponentB, opponentA) * kRadiansToDegrees;
    if (hue < 0.0) hue += 360.0;
    return {
        100.0F * std::pow(achromatic, kJExponent),
        std::sqrt(opponentA * opponentA + opponentB * opponentB),
        hue
    };
}

[[nodiscard]] Vec3 JMhToAp0(Vec3 jmh) noexcept
{
    const double hueRadians = jmh[2] * kDegreesToRadians;
    const double achromatic =
        std::pow(jmh[0] * 0.01F, kInverseJExponent);
    const double opponentA = jmh[1] * std::cos(hueRadians);
    const double opponentB = jmh[1] * std::sin(hueRadians);

    double lAdapted =
        0.0323680267F * achromatic + 2.07657631e-05F * opponentA +
        1.3260621e-05F * opponentB;
    double mAdapted =
        0.0323680267F * achromatic - 4.10250432e-05F * opponentA -
        1.20174373e-05F * opponentB;
    double sAdapted =
        0.0323680267F * achromatic - 1.01296409e-05F * opponentA -
        0.000290076074F * opponentB;

    lAdapted = std::clamp(
        lAdapted, -kAdaptedConeLimit, kAdaptedConeLimit);
    mAdapted = std::clamp(
        mAdapted, -kAdaptedConeLimit, kAdaptedConeLimit);
    sAdapted = std::clamp(
        sAdapted, -kAdaptedConeLimit, kAdaptedConeLimit);

    const auto decompress = [](double value) noexcept {
        const double magnitude = std::abs(value);
        return std::copysign(
            std::pow(
                std::abs(kCamOffset * value / (1.0 - magnitude)),
                kInverseLmsExponent),
            value);
    };
    const double lPrime = decompress(lAdapted);
    const double mPrime = decompress(mAdapted);
    const double sPrime = decompress(sAdapted);

    return {
        2.66705418F * lPrime - 1.52505863F * mPrime +
            0.117925495F * sPrime,
        -0.535811901F * lPrime + 1.94158077F * mPrime -
            0.14584811F * sPrime,
        -0.0214488022F * lPrime - 0.048595503F * mPrime +
            1.32996523F * sPrime
    };
}

[[nodiscard]] Vec3 ApplyJ2M2(Vec3 rgb) noexcept
{
    const Vec3 jmh = Ap0ToJMh(rgb);
    const double normalizedJ = jmh[0] / kJNormalization;
    const double modifiedJ = EvaluateOcioBSpline(
        normalizedJ, kJ2Knots, kJ2Coefficients);
    const double colorfulnessGain = std::max(
        0.0,
        EvaluateOcioBSpline(normalizedJ, kM2Knots, kM2Coefficients));
    return JMhToAp0({
        modifiedJ * kJNormalization,
        jmh[1] * colorfulnessGain,
        jmh[2]
    });
}

// SPDX-SnippetEnd

}  // namespace

LinearRgb ApplyAces2ContrastLmt(LinearRgb sceneLinearAp0) noexcept
{
    const Vec3 input{
        sceneLinearAp0.red,
        sceneLinearAp0.green,
        sceneLinearAp0.blue
    };
    if (!IsFinite(input)) return {};

    const Vec3 output = ApplyJ2M2(input);
    if (!IsFinite(output)) return {};
    return {output[0], output[1], output[2]};
}

}  // namespace fo4hdr
