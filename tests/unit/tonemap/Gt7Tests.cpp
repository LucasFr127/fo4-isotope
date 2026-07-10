// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

// Keep the independent sample intact, including its demonstration entry point.
#define main Fo4HdrGt7OfficialSampleMain
#include "../../../third_party/gt7/upstream/gt7_tone_mapping.cpp"
#undef main

namespace fo4hdr::tests {
namespace {

constexpr std::array<LinearRgb, 12> referenceInputs{{
    {0.5, 1.23, 0.75}, {12.3, 34.3, 56.9}, {1504.7, 64.51, 0.5},
    {}, {0.18, 0.18, 0.18}, {1.0, 1.0, 1.0}, {8.0, 0.0, 0.0},
    {0.0, 8.0, 16.0}, {0.0, 0.60791015625, 54.21875},
    {0.0, 2.6875, 28.734375}, {65504.0, 65504.0, 65504.0},
    {65504.0, 1.0, 0.001}
}};
constexpr std::array referencePeaks{250.0, 400.0, 430.0, 1000.0,
    2000.0, 4000.0, 10000.0};

std::array<float, 3> OfficialReference(std::array<float, 3> input, double peak)
{
    GT7ToneMapping reference;
    reference.initializeAsHDR(static_cast<float>(peak));
    std::array<float, 3> output{};
    reference.applyToneMapping(input.data(), output.data());
    return output;
}

void CheckSourceChannel(float actual, float expected, double peak)
{
    FO4HDR_CHECK(std::isfinite(actual));
    FO4HDR_CHECK(std::isfinite(expected));
    FO4HDR_CHECK(actual >= 0.0F);
    FO4HDR_CHECK(actual <= static_cast<float>(peak) / 100.0F);
    if (!std::isfinite(actual)) return;
    if (!std::isfinite(expected)) return;
    const auto actualBits = std::bit_cast<std::uint32_t>(actual);
    const auto expectedBits = std::bit_cast<std::uint32_t>(expected);
    const auto distance = actualBits > expectedBits ?
        actualBits - expectedBits : expectedBits - actualBits;
    FO4HDR_CHECK(distance <= 2U);
}

void CheckSourceRgb(LinearRgb actual, const std::array<float, 3>& expected,
    double peak)
{
    CheckSourceChannel(static_cast<float>(actual.red), expected[0], peak);
    CheckSourceChannel(static_cast<float>(actual.green), expected[1], peak);
    CheckSourceChannel(static_cast<float>(actual.blue), expected[2], peak);
}

void CheckSceneReference(LinearRgb scene, double peak)
{
    const auto rec2020 = ConvertBt709ToBt2020(scene);
    constexpr float sceneScale = 203.0F / 100.0F;
    const auto expected = OfficialReference({
        static_cast<float>(rec2020.red) * sceneScale,
        static_cast<float>(rec2020.green) * sceneScale,
        static_cast<float>(rec2020.blue) * sceneScale}, peak);
    const auto actual = MapGt7ToBt2020Nits(scene, peak);
    CheckSourceRgb({actual.red / 100.0, actual.green / 100.0,
        actual.blue / 100.0}, expected, peak);
}

void TestOfficialSourceParity()
{
    for (double peak : referencePeaks)
    {
        for (const auto input : referenceInputs)
        {
            const auto expected = OfficialReference({static_cast<float>(input.red),
                static_cast<float>(input.green), static_cast<float>(input.blue)}, peak);
            CheckSourceRgb(MapGt7ReferenceRec2020(input, peak), expected, peak);
            CheckSceneReference(input, peak);
        }
    }
    // These zero-red inputs preserve legitimate positive inverse-ICtCp cross-talk.
    FO4HDR_CHECK(MapGt7ReferenceRec2020(referenceInputs[8], 400.0).red > 1.0e-5);
    FO4HDR_CHECK(MapGt7ReferenceRec2020(referenceInputs[9], 400.0).red > 1.0e-5);
}

void CheckSameRgb(LinearRgb actual, LinearRgb expected)
{
    FO4HDR_CHECK(actual.red == expected.red);
    FO4HDR_CHECK(actual.green == expected.green);
    FO4HDR_CHECK(actual.blue == expected.blue);
}

void CheckInputPolicy(double bad, double replacement)
{
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        std::array actual{0.18, 1.0, 4.0};
        auto expected = actual;
        actual[channel] = bad;
        expected[channel] = replacement;
        const LinearRgb input{actual[0], actual[1], actual[2]};
        const LinearRgb sanitized{expected[0], expected[1], expected[2]};
        CheckSameRgb(MapGt7ReferenceRec2020(input, 1000.0),
            MapGt7ReferenceRec2020(sanitized, 1000.0));
        CheckSameRgb(MapGt7ToBt2020Nits(input, 1000.0),
            MapGt7ToBt2020Nits(sanitized, 1000.0));
    }
}

void TestInputAndPeakPolicy()
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (double invalid : {nan, infinity, -infinity, -1.0, 0.0, 249.0, 10001.0})
    {
        CheckSameRgb(MapGt7ReferenceRec2020({1.0, 1.0, 1.0}, invalid), {});
        CheckSameRgb(MapGt7ToBt2020Nits({1.0, 1.0, 1.0}, invalid), {});
    }
    for (double value : {nan, -infinity, -1.0}) CheckInputPolicy(value, 0.0);
    for (double value : {infinity, std::numeric_limits<double>::max(), 65505.0})
        CheckInputPolicy(value, 65504.0);
}

}  // namespace

void RunGt7Tests()
{
    TestOfficialSourceParity();
    TestInputAndPeakPolicy();
}

}  // namespace fo4hdr::tests
