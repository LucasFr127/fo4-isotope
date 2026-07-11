// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/tonemap/Aces2.hpp"
#include "Fallout4HDR/tonemap/Agx.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace fo4hdr::tests {
namespace {

void CheckSourcePrecision(LinearRgb actual, LinearRgb expected)
{
    // This budget is absolute nits; relative scaling would allow about two
    // nits of error at a 1000-nit output.
    FO4HDR_CHECK(std::abs(actual.red - expected.red) <= 0.002);
    FO4HDR_CHECK(std::abs(actual.green - expected.green) <= 0.002);
    FO4HDR_CHECK(std::abs(actual.blue - expected.blue) <= 0.002);
}

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

void TestAces2ReferenceSamples()
{
    struct Reference
    {
        double peak;
        LinearRgb input;
        LinearRgb output;
    };
    // Retained independent OCIO 2.4.2 values cover neutral, primary and high
    // finite samples. The two blue endpoints preserve the source discontinuity.
    constexpr std::array references{
        Reference{400.0, {0.18, 0.18, 0.18},
            {12.757924199104, 12.757939100266, 12.757928669453}},
        Reference{400.0, {64.0, 22.4, 0.0},
            {399.656343460083, 361.354207992554, 290.797209739685}},
        Reference{400.0, {65504.0, 0.0, 0.0}, {400.0, 400.0, 400.0}},
        Reference{1000.0, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
        Reference{1000.0, {0.18, 0.18, 0.18},
            {14.511531591415, 14.511562883854, 14.511547982693}},
        Reference{1000.0, {0.97784, 0.97784, 0.97784},
            {104.156661033630, 104.156637191772, 104.156625270844}},
        Reference{1000.0, {8.0, 0.0, 0.0},
            {468.879699707031, 84.470617771149, 30.910950899124}},
        Reference{1000.0, {0.0, 8.0, 0.0},
            {292.978191375732, 543.024587631226, 158.105611801147}},
        Reference{1000.0, {0.0, 0.0, 8.0},
            {30.586963891983, 0.572490692139, 756.818771362305}},
        Reference{1000.0, {2.03125, 0.6875, 0.53125},
            {145.051610469818, 89.921796321869, 72.836506366730}},
        Reference{1000.0, {64.0, 22.4, 0.0},
            {957.717037200928, 812.109851837158, 561.223602294922}},
        Reference{1000.0, {65504.0, 0.0, 0.0}, {1000.0, 1000.0, 1000.0}},
        Reference{2000.0, {0.18, 0.18, 0.18},
            {15.746709704399, 15.746742486954, 15.746729075909}},
        Reference{2000.0, {64.0, 22.4, 0.0},
            {1768.410110473633, 1413.102912902832, 840.102100372314}},
        Reference{2000.0, {65504.0, 0.0, 0.0}, {2000.0, 2000.0, 2000.0}},
        Reference{2000.0, {0.0, 0.0, 21.33}, {88.756132125854, 0.0, 2000.0}},
        Reference{2000.0, {0.0, 0.0, 21.34}, {106.985712051392, 30.772495269775, 2000.0}}
    };
    for (double peak : {400.0, 1000.0, 2000.0})
    {
        const auto transform = CreateAces2Transform(peak);
        FO4HDR_CHECK(transform);
        if (!transform) continue;
        for (const auto& reference : references)
        {
            if (reference.peak != peak) continue;
            const auto output = MapAces2ToBt2020Nits(*transform, reference.input);
            CheckSourcePrecision(output, reference.output);
            CheckContained(output, reference.peak);
        }
    }
}

void TestAces2ContainedInputs()
{
    const double maximum = std::numeric_limits<double>::max();
    for (double peak : {400.0, 1000.0, 2000.0})
    {
        const auto transform = CreateAces2Transform(peak);
        FO4HDR_CHECK(transform);
        if (!transform) continue;
        CheckSameRgb(MapAces2ToBt2020Nits(*transform, {}), {});
        for (const auto input : std::array{
                 LinearRgb{0.18, 0.18, 0.18}, LinearRgb{1.0, 0.0, 0.0},
                 LinearRgb{0.0, 1.0, 0.0}, LinearRgb{0.0, 0.0, 1.0},
                 LinearRgb{4.0, 16.0, 2.0}, LinearRgb{maximum, maximum, maximum},
                 LinearRgb{maximum, 0.0, 0.18}})
            CheckContained(MapAces2ToBt2020Nits(*transform, input), peak);
    }
}

void TestAces2InputPolicy()
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto transform = CreateAces2Transform(1000.0);
    FO4HDR_CHECK(transform);
    if (!transform) return;
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        std::array channels{1.0, 0.25, 4.0};
        channels[channel] = 0.0;
        const auto expected = MapAces2ToBt2020Nits(
            {channels[0], channels[1], channels[2]}, 1000.0);
        for (double bad : {-1.0, nan, infinity, -infinity})
        {
            channels[channel] = bad;
            CheckSameRgb(MapAces2ToBt2020Nits(
                {channels[0], channels[1], channels[2]}, 1000.0), expected);
            CheckSameRgb(MapAces2ToBt2020Nits(*transform,
                {channels[0], channels[1], channels[2]}), expected);
        }
    }
    for (double peak : {0.0, 399.999, 2000.001, nan, infinity, -infinity})
    {
        CheckSameRgb(MapAces2ToBt2020Nits({1.0, 0.25, 4.0}, peak), {});
        FO4HDR_CHECK(!CreateAces2Transform(peak));
    }
}

void TestAces2PeakReuse()
{
    struct PeakReference
    {
        double peak;
        LinearRgb expected;
    };
    constexpr std::array references{
        PeakReference{400.0, {12.757924199104, 12.757939100266, 12.757928669453}},
        PeakReference{1000.0, {14.511531591415, 14.511562883854, 14.511547982693}},
        PeakReference{2000.0, {15.746709704399, 15.746742486954, 15.746729075909}}
    };
    constexpr LinearRgb sample{0.18, 0.18, 0.18};
    const std::array transforms{CreateAces2Transform(400.0),
        CreateAces2Transform(1000.0), CreateAces2Transform(2000.0)};
    for (const auto& transform : transforms)
    {
        FO4HDR_CHECK(transform);
        if (!transform) return;
    }
    std::array<LinearRgb, references.size()> previous{};
    for (std::size_t i = 0; i < references.size(); ++i)
    {
        previous[i] = MapAces2ToBt2020Nits(*transforms[i], sample);
        CheckSameRgb(MapAces2ToBt2020Nits(sample, references[i].peak), previous[i]);
        CheckSourcePrecision(previous[i], references[i].expected);
        CheckSameRgb(MapAces2ToBt2020Nits(*transforms[i], sample), previous[i]);
        CheckSameRgb(MapAces2ToBt2020Nits(sample, 0.0), {});
        CheckSameRgb(MapAces2ToBt2020Nits(sample, references[i].peak), previous[i]);
        CheckSameRgb(MapAces2ToBt2020Nits(*transforms[i], sample), previous[i]);
    }
    for (std::size_t i = 0; i < references.size(); ++i)
    {
        const auto output = MapAces2ToBt2020Nits(*transforms[i], sample);
        CheckSourcePrecision(output, references[i].expected);
        CheckSameRgb(output, previous[i]);
    }
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

void RunToneMappingTests()
{
    TestAces2ReferenceSamples();
    TestAces2ContainedInputs();
    TestAces2InputPolicy();
    TestAces2PeakReuse();
    TestAgxNeutralReferences();
    TestAgxContainedInputs();
    TestAgxInputPolicy();
    TestAgxContextReuse();
}

}  // namespace fo4hdr::tests
