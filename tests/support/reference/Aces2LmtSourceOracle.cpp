// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright Contributors to the ACES Project
//
// Upstream: scottdyer/aces-look@dc79dd74554e73a9e1518ec5f00ae0c1725f1686/contrast/LMT_Test_OCIO_options.dctl
// Source SHA-256:
// b5b1022c46758c02edd4c3967375d6daa0cd022e78c85962004a2f628318711f.
// Modified by FO4Isotope: this test-only C++ port selects only
// J2M2_CURVE ("J curve 2 + M curve 2"), preserves the source float constants
// and evaluation order, and replaces DCTL vector/intrinsic syntax with C++.

#include "Aces2LmtSourceOracle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace fo4hdr::tests {
namespace {

struct Float3
{
    float x{};
    float y{};
    float z{};
};

constexpr std::array<float, 15> kJ2Knots{
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

constexpr std::array<float, 42> kJ2Coefficients{
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

constexpr std::array<float, 10> kM2Knots{
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

constexpr std::array<float, 27> kM2Coefficients{
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

template <std::size_t KnotCount, std::size_t CoefficientCount>
[[nodiscard]] float EvaluateOcioBSpline(
    float x,
    const std::array<float, KnotCount>& knots,
    const std::array<float, CoefficientCount>& coefficients) noexcept
{
    static_assert(CoefficientCount == (KnotCount - 1) * 3);
    constexpr int setCount = static_cast<int>(CoefficientCount / 3);
    const int knotCount = static_cast<int>(KnotCount);

    const float knotStart = knots[0];
    const float knotEnd = knots[knotCount - 1];
    if (x <= knotStart)
    {
        return (x - knotStart) * coefficients[setCount] +
            coefficients[setCount * 2];
    }
    if (x >= knotEnd)
    {
        const float t = knotEnd - knots[knotCount - 2];
        const float slope = 2.0F * coefficients[setCount - 1] * t +
            coefficients[setCount * 2 - 1];
        const float offset =
            (coefficients[setCount - 1] * t +
             coefficients[setCount * 2 - 1]) *
                t +
            coefficients[setCount * 3 - 1];
        return (x - knotEnd) * slope + offset;
    }

    int segment = 0;
    for (; segment < knotCount - 2; ++segment)
    {
        if (x < knots[segment + 1]) break;
    }
    const float t = x - knots[segment];
    return (coefficients[segment] * t +
            coefficients[setCount + segment]) *
            t +
        coefficients[setCount * 2 + segment];
}

[[nodiscard]] Float3 RgbToJMh(Float3 rgb) noexcept
{
    const float l = 0.445181072F * rgb.x + 0.34964928F * rgb.y -
        0.00112973817F * rgb.z;
    const float m = 0.123734139F * rgb.x + 0.613643706F * rgb.y +
        0.0563227981F * rgb.z;
    const float s = 0.0117006991F * rgb.x + 0.0280607957F * rgb.y +
        0.753939092F * rgb.z;

    const float lPrime =
        std::pow(std::fabs(l), 0.42F) * std::copysign(1.0F, l);
    const float mPrime =
        std::pow(std::fabs(m), 0.42F) * std::copysign(1.0F, m);
    const float sPrime =
        std::pow(std::fabs(s), 0.42F) * std::copysign(1.0F, s);

    const float lAdapted = lPrime / (27.13F + std::fabs(lPrime));
    const float mAdapted = mPrime / (27.13F + std::fabs(mPrime));
    const float sAdapted = sPrime / (27.13F + std::fabs(sPrime));

    const float achromatic = 20.25881F * lAdapted +
        10.129405F * mAdapted + 0.506470263F * sAdapted;
    const float opponentA = 15480.0F * lAdapted -
        16887.2734F * mAdapted + 1407.27271F * sAdapted;
    const float opponentB = 1720.0F * lAdapted +
        1720.0F * mAdapted - 3440.0F * sAdapted;

    float j = 0.0F;
    float colorfulness = 0.0F;
    float hue = 0.0F;
    if (achromatic > 0.0F)
    {
        j = 100.0F * std::pow(achromatic, 1.13705599F);
        colorfulness =
            std::sqrt(opponentA * opponentA + opponentB * opponentB);
        hue = opponentA == 0.0F
            ? 0.0F
            : std::atan2(opponentB, opponentA) * 57.2957795F;
        if (hue < 0.0F) hue += 360.0F;
    }
    return {j, colorfulness, hue};
}

[[nodiscard]] Float3 JMhToRgb(Float3 jmh) noexcept
{
    const float hueRadians = jmh.z * 0.0174532924F;
    const float cosineHue = std::cos(hueRadians);
    const float sineHue = std::sin(hueRadians);

    const float achromatic = std::pow(jmh.x * 0.01F, 0.879464149F);
    const float opponentA = jmh.y * cosineHue;
    const float opponentB = jmh.y * sineHue;

    float lAdapted = 0.0323680267F * achromatic +
        2.07657631e-05F * opponentA + 1.3260621e-05F * opponentB;
    float mAdapted = 0.0323680267F * achromatic -
        4.10250432e-05F * opponentA - 1.20174373e-05F * opponentB;
    float sAdapted = 0.0323680267F * achromatic -
        1.01296409e-05F * opponentA - 0.000290076074F * opponentB;

    lAdapted = std::clamp(lAdapted, -0.99F, 0.99F);
    mAdapted = std::clamp(mAdapted, -0.99F, 0.99F);
    sAdapted = std::clamp(sAdapted, -0.99F, 0.99F);

    const auto decompress = [](float adapted) noexcept {
        return std::copysign(1.0F, adapted) *
            std::pow(
                std::fabs(
                    27.13F * adapted / (1.0F - std::fabs(adapted))),
                2.38095236F);
    };
    const float lPrime = decompress(lAdapted);
    const float mPrime = decompress(mAdapted);
    const float sPrime = decompress(sAdapted);

    const float red = 2.66705418F * lPrime - 1.52505863F * mPrime +
        0.117925495F * sPrime;
    const float green = -0.535811901F * lPrime + 1.94158077F * mPrime -
        0.14584811F * sPrime;
    const float blue = -0.0214488022F * lPrime - 0.048595503F * mPrime +
        1.32996523F * sPrime;
    return {red, green, blue};
}

}  // namespace

// Independent source-precision oracle for the pinned candidate LMT's selected
// J2M2_CURVE branch. Input and output are dimensionless scene-linear
// ACES2065-1 (AP0/D60). Deliberately performs source-style float evaluation
// and does not apply the Academy Output Transform, display adaptation,
// containment, gamut conversion, or PQ encoding.
[[nodiscard]] LinearRgb ApplyAces2ContrastLmtSourceFloat(
    LinearRgb sceneLinearAp0) noexcept
{
    const Float3 rgb{
        static_cast<float>(sceneLinearAp0.red),
        static_cast<float>(sceneLinearAp0.green),
        static_cast<float>(sceneLinearAp0.blue)
    };
    const Float3 jmh = RgbToJMh(rgb);
    const float normalizedJ = jmh.x / 400.0F;
    const float modifiedJ =
        EvaluateOcioBSpline(normalizedJ, kJ2Knots, kJ2Coefficients);
    const float modifiedM = jmh.y * std::fmax(
        0.0F,
        EvaluateOcioBSpline(normalizedJ, kM2Knots, kM2Coefficients));
    const Float3 output =
        JMhToRgb({modifiedJ * 400.0F, modifiedM, jmh.z});
    return {
        static_cast<double>(output.x),
        static_cast<double>(output.y),
        static_cast<double>(output.z)
    };
}

}  // namespace fo4hdr::tests
