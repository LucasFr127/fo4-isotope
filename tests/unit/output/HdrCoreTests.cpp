// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

void TestBt709ToBt2020()
{
    const auto neutral = fo4hdr::ConvertBt709ToBt2020({4.0, 4.0, 4.0});
    FO4HDR_CHECK(fo4hdr::test::Approx(neutral.red, 4.0, 1.0e-7));
    FO4HDR_CHECK(fo4hdr::test::Approx(neutral.green, 4.0, 1.0e-7));
    FO4HDR_CHECK(fo4hdr::test::Approx(neutral.blue, 4.0, 1.0e-7));

    const auto red = fo4hdr::ConvertBt709ToBt2020({1.0, 0.0, 0.0});
    FO4HDR_CHECK(fo4hdr::test::Approx(red.red, 0.6274039149284363));
    FO4HDR_CHECK(fo4hdr::test::Approx(red.green, 0.06909728795289993));
    FO4HDR_CHECK(fo4hdr::test::Approx(red.blue, 0.0163914393633604));

    const auto green = fo4hdr::ConvertBt709ToBt2020({0.0, 1.0, 0.0});
    FO4HDR_CHECK(fo4hdr::test::Approx(green.red, 0.3292830288410187));
    FO4HDR_CHECK(fo4hdr::test::Approx(green.green, 0.9195404052734375));
    FO4HDR_CHECK(fo4hdr::test::Approx(green.blue, 0.08801330626010895));

    const auto blue = fo4hdr::ConvertBt709ToBt2020({0.0, 0.0, 1.0});
    FO4HDR_CHECK(fo4hdr::test::Approx(blue.red, 0.04331306740641594));
    FO4HDR_CHECK(fo4hdr::test::Approx(blue.green, 0.01136231515556574));
    FO4HDR_CHECK(fo4hdr::test::Approx(blue.blue, 0.8955952525138855));

    const auto emissive = fo4hdr::ConvertBt709ToBt2020({8.0, 4.0, 0.0});
    FO4HDR_CHECK(emissive.red > 1.0);
    FO4HDR_CHECK(emissive.green > 1.0);
    FO4HDR_CHECK(emissive.blue > 0.0);

    for (const auto sample : {
             fo4hdr::LinearRgb{1.0, 0.0, 0.0}, fo4hdr::LinearRgb{0.0, 1.0, 0.0},
             fo4hdr::LinearRgb{0.0, 0.0, 1.0}, fo4hdr::LinearRgb{8.0, 4.0, 0.5},
             fo4hdr::LinearRgb{203.0, 203.0, 203.0}})
    {
        const auto roundTrip = fo4hdr::ConvertBt2020ToBt709ForComparison(
            fo4hdr::ConvertBt709ToBt2020(sample));
        FO4HDR_CHECK(fo4hdr::test::Approx(roundTrip.red, sample.red, 1.0e-12));
        FO4HDR_CHECK(fo4hdr::test::Approx(roundTrip.green, sample.green, 1.0e-12));
        FO4HDR_CHECK(fo4hdr::test::Approx(roundTrip.blue, sample.blue, 1.0e-12));
    }
}

void TestDisplayStandardsOracle()
{
    // Independent XYZ route from ITU primary coordinates and D65, without
    // rounding the combined RGB transform to the transform's binary32 coefficient precision.
    // https://www.itu.int/rec/R-REC-BT.709-6-201506-I/en
    // https://www.itu.int/rec/R-REC-BT.2020-2-201510-I/en
    using Matrix = std::array<std::array<double, 3>, 3>;
    constexpr Matrix narrowToXyz{{
        {0.4123907992659595, 0.3575843393838780, 0.1804807884018343},
        {0.2126390058715104, 0.7151686787677560, 0.0721923153607337},
        {0.0193308187155919, 0.1191947797946260, 0.9505321522496607}
    }};
    constexpr Matrix wideToXyz{{
        {0.6369580483012914, 0.1446169035862083, 0.1688809751641721},
        {0.2627002120112671, 0.6779980715188710, 0.0593017164698620},
        {0.0, 0.0280726930490874, 1.0609850577107910}
    }};
    for (const auto input : {
        fo4hdr::LinearRgb{1, 0, 0}, fo4hdr::LinearRgb{0, 1, 0}, fo4hdr::LinearRgb{0, 0, 1},
        fo4hdr::LinearRgb{203, 203, 203}, fo4hdr::LinearRgb{-3, 8, -0.25},
        fo4hdr::LinearRgb{65504, 0.0001, 32752}})
    {
        const auto output = fo4hdr::ConvertBt709ToBt2020(input);
        for (std::size_t row = 0; row < 3; ++row) {
            const auto& a = narrowToXyz[row];
            const auto& b = wideToXyz[row];
            const double expected = a[0] * input.red + a[1] * input.green + a[2] * input.blue;
            const double actual = b[0] * output.red + b[1] * output.green + b[2] * output.blue;
            const double scale = std::max({1.0, std::abs(input.red),
                std::abs(input.green), std::abs(input.blue)});
            FO4HDR_CHECK(std::abs(actual - expected) <= scale * 5.0e-8);
        }
    }

}

void TestSt2084ReferenceVectors()
{
    struct Reference
    {
        double nits;
        double encoded;
    };
    constexpr std::array<Reference, 12> references{{
        {0.0, 7.309559025783966e-7},
        {0.001, 0.006302377054571391},
        {0.01, 0.02148621379868528},
        {0.1, 0.06233686566269587},
        {1.0, 0.14994573210018022},
        {80.0, 0.4858567653886785},
        {100.0, 0.508078421517399},
        {203.0, 0.5806888810416109},
        {400.0, 0.6525785975630672},
        {1000.0, 0.751827096247041},
        {2000.0, 0.8274246448588193},
        {10000.0, 1.0}
    }};

    double previous = -1.0;
    for (const auto& reference : references)
    {
        const auto encoded = fo4hdr::EncodeSt2084Pq(reference.nits);
        FO4HDR_CHECK(fo4hdr::test::Approx(encoded, reference.encoded, 1.0e-12));
        FO4HDR_CHECK(encoded >= previous);
        FO4HDR_CHECK(encoded >= 0.0 && encoded <= 1.0);
        previous = encoded;
    }
}

void TestSt2084InputBounds()
{
    using fo4hdr::EncodeSt2084Pq;
    FO4HDR_CHECK(EncodeSt2084Pq(-1.0) == EncodeSt2084Pq(0.0));
    FO4HDR_CHECK(EncodeSt2084Pq(20000.0) == 1.0);
    FO4HDR_CHECK(EncodeSt2084Pq(std::numeric_limits<double>::infinity()) == 1.0);
    FO4HDR_CHECK(EncodeSt2084Pq(-std::numeric_limits<double>::infinity()) ==
        EncodeSt2084Pq(0.0));
    FO4HDR_CHECK(EncodeSt2084Pq(std::numeric_limits<double>::quiet_NaN()) ==
        EncodeSt2084Pq(0.0));
}

}  // namespace

int main()
{
    TestBt709ToBt2020();
    TestDisplayStandardsOracle();
    TestSt2084ReferenceVectors();
    TestSt2084InputBounds();
    return fo4hdr::test::failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
