// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/tonemap/Agx.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace fo4hdr::tests {
namespace {

void CheckContained(LinearRgb value, double peak)
{
    for (double channel : {value.red, value.green, value.blue})
    {
        FO4HDR_CHECK(std::isfinite(channel));
        FO4HDR_CHECK(channel >= 0.0 && channel <= peak);
    }
}

void CheckSameRgb(LinearRgb actual, LinearRgb expected)
{
    FO4HDR_CHECK(actual.red == expected.red);
    FO4HDR_CHECK(actual.green == expected.green);
    FO4HDR_CHECK(actual.blue == expected.blue);
}

void CheckAgxReference(LinearRgb actual, LinearRgb expected)
{
    FO4HDR_CHECK(std::abs(actual.red - expected.red) <= 1.0e-6);
    FO4HDR_CHECK(std::abs(actual.green - expected.green) <= 1.0e-6);
    FO4HDR_CHECK(std::abs(actual.blue - expected.blue) <= 1.0e-6);
}

void TestAgxNeutralReferences()
{
    struct Reference
    {
        double peak;
        int stops;
        LinearRgb output;
    };
    // Fixed contrast 2.7 values use an absolute 1e-6-nit budget.
    constexpr std::array references{
        Reference{418.0, -6, {0.18079879, 0.1807987891, 0.1807987868}},
        Reference{418.0, -4, {1.089122075, 1.089122071, 1.089122058}},
        Reference{418.0, -2, {6.271933741, 6.271933714, 6.271933639}},
        Reference{418.0, 0, {36.54000045, 36.54000032, 36.53999992}},
        Reference{418.0, 2, {139.9818959, 139.9818957, 139.9818949}},
        Reference{1000.0, -6, {0.1856417662, 0.1856417653, 0.185641763}},
        Reference{1000.0, -4, {1.094696211, 1.094696207, 1.094696194}},
        Reference{1000.0, -2, {6.273966936, 6.27396691, 6.273966835}},
        Reference{1000.0, 0, {36.54000045, 36.54000032, 36.53999992}},
        Reference{1000.0, 2, {192.6212656, 192.6212651, 192.6212637}}
    };
    for (double peak : {418.0, 1000.0})
    {
        const auto transform = CreateAgxTransform(peak);
        FO4HDR_CHECK(transform);
        if (!transform) continue;
        for (const auto& reference : references)
        {
            if (reference.peak != peak) continue;
            const auto input = 0.18 * std::exp2(reference.stops);
            const auto output = MapAgxToBt2020Nits(*transform, {input, input, input});
            CheckAgxReference(output, reference.output);
            CheckContained(output, reference.peak);
        }
    }
}

void TestAgxContainedInputs()
{
    constexpr std::array inputs{
        LinearRgb{}, LinearRgb{0.001, 0.001, 0.001}, LinearRgb{0.01, 0.01, 0.01},
        LinearRgb{0.18, 0.18, 0.18}, LinearRgb{1.0, 1.0, 1.0},
        LinearRgb{4.0, 4.0, 4.0}, LinearRgb{8.0, 0.0, 0.0},
        LinearRgb{0.0, 8.0, 0.0}, LinearRgb{0.0, 0.0, 8.0},
        LinearRgb{16.0, 2.0, 0.1}, LinearRgb{65504.0, 0.0, 0.0}
    };
    for (double peak : {400.0, 1000.0, 2000.0})
    {
        const auto transform = CreateAgxTransform(peak);
        FO4HDR_CHECK(transform);
        if (!transform) continue;
        for (const auto input : inputs)
            CheckContained(MapAgxToBt2020Nits(*transform, input), peak);
    }
}

void TestAgxInputPolicy()
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double maximum = std::numeric_limits<double>::max();
    const auto transform = CreateAgxTransform(1000.0);
    FO4HDR_CHECK(transform);
    if (!transform) return;
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        std::array channels{1.0, 0.25, 4.0};
        channels[channel] = 0.0;
        const auto zeroed = MapAgxToBt2020Nits({channels[0], channels[1], channels[2]}, 1000.0);
        for (double bad : {-1.0, nan, -infinity})
        {
            channels[channel] = bad;
            CheckSameRgb(MapAgxToBt2020Nits({channels[0], channels[1], channels[2]}, 1000.0), zeroed);
            CheckSameRgb(MapAgxToBt2020Nits(*transform, {channels[0], channels[1], channels[2]}), zeroed);
        }
        channels[channel] = 65504.0;
        const auto capped = MapAgxToBt2020Nits({channels[0], channels[1], channels[2]}, 1000.0);
        for (double high : {infinity, maximum, 65505.0})
        {
            channels[channel] = high;
            CheckSameRgb(MapAgxToBt2020Nits({channels[0], channels[1], channels[2]}, 1000.0), capped);
            CheckSameRgb(MapAgxToBt2020Nits(*transform, {channels[0], channels[1], channels[2]}), capped);
        }
    }
    for (double peak : {0.0, 399.0, 2001.0, nan, infinity, -infinity})
    {
        CheckSameRgb(MapAgxToBt2020Nits({1.0, 0.25, 4.0}, peak), {});
        FO4HDR_CHECK(!CreateAgxTransform(peak));
    }
}

void TestAgxContextReuse()
{
    const auto low = CreateAgxTransform(418.0);
    const auto high = CreateAgxTransform(1000.0);
    FO4HDR_CHECK(low);
    FO4HDR_CHECK(high);
    if (!low) return;
    if (!high) return;
    for (const auto input : std::array{LinearRgb{0.18, 0.18, 0.18},
             LinearRgb{0.72, 0.72, 0.72}, LinearRgb{16.0, 2.0, 0.1}})
    {
        const auto expectedLow = MapAgxToBt2020Nits(input, 418.0);
        const auto expectedHigh = MapAgxToBt2020Nits(input, 1000.0);
        CheckSameRgb(MapAgxToBt2020Nits(*low, input), expectedLow);
        CheckSameRgb(MapAgxToBt2020Nits(*high, input), expectedHigh);
        CheckSameRgb(MapAgxToBt2020Nits(*low, input), expectedLow);
    }
    constexpr LinearRgb highlight{0.72, 0.72, 0.72};
    FO4HDR_CHECK(MapAgxToBt2020Nits(*high, highlight).red >
        MapAgxToBt2020Nits(*low, highlight).red);
}

}  // namespace

void RunAgxTests()
{
    TestAgxNeutralReferences();
    TestAgxContainedInputs();
    TestAgxInputPolicy();
    TestAgxContextReuse();
}

}  // namespace fo4hdr::tests
