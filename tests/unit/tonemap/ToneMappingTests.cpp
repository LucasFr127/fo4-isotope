// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/tonemap/Aces2.hpp"
#include "Fallout4HDR/tonemap/Aces2Lmt.hpp"
#include "config/SettingsPersistence.hpp"
#include "reference/Aces2LmtSourceOracle.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <algorithm>
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

void CheckLookSource(LinearRgb input)
{
    const auto source = ApplyAces2ContrastLmtSourceFloat(input);
    const auto actual = ApplyAces2ContrastLmt(input);
    // Source-float versus double AP0, distinct from the absolute-nit ODT budget.
    const double tolerance = 2.0e-4 * std::max({1.0,
        std::abs(source.red), std::abs(source.green), std::abs(source.blue)});
    FO4HDR_CHECK(std::abs(actual.red - source.red) <= tolerance);
    FO4HDR_CHECK(std::abs(actual.green - source.green) <= tolerance);
    FO4HDR_CHECK(std::abs(actual.blue - source.blue) <= tolerance);
}

void CheckLookSettings()
{
    PluginConfiguration settings;
    settings.productionAlgorithm = ProductionToneMapperAlgorithm::aces2Lmt;
    settings.toneMapperParameters.acesLookStrength = 0.5;
    FO4HDR_CHECK((fo4hdr::ValidateHdrSettings(settings) == fo4hdr::HdrSettingsValidationError::none));
    const auto saved = RewriteHdrSettingsIni("; retained\n", settings);
    const auto parsed = ParsePluginConfiguration(saved);
    FO4HDR_CHECK(parsed.valid == true);
    FO4HDR_CHECK(static_cast<const HdrSettings&>(parsed.value) == static_cast<const HdrSettings&>(settings));
    auto changed = settings.toneMapperParameters;
    changed.acesLookStrength = 1.0;
    FO4HDR_CHECK(ActiveToneMapperParameters(settings.productionAlgorithm, changed) !=
        ActiveToneMapperParameters(settings.productionAlgorithm, settings.toneMapperParameters));
    FO4HDR_CHECK(ActiveToneMapperParameters(ProductionToneMapperAlgorithm::gt7, changed) ==
        ActiveToneMapperParameters(ProductionToneMapperAlgorithm::gt7, settings.toneMapperParameters));
    settings.toneMapperParameters.acesLookStrength =
        std::numeric_limits<double>::quiet_NaN();
    FO4HDR_CHECK(!(fo4hdr::ValidateHdrSettings(settings) == fo4hdr::HdrSettingsValidationError::none));
}

void TestAces2ContrastLook()
{
    for (const auto input : std::array{LinearRgb{}, LinearRgb{0.18, 0.18, 0.18},
             LinearRgb{2.0, 0.7, 0.5}, LinearRgb{-0.01, 0.18, 1.0}})
        CheckLookSource(input);
    const auto transform = CreateAces2Transform(1000.0);
    FO4HDR_CHECK(transform);
    if (!transform) return;
    for (double strength : {0.0, 0.5, 1.0})
    {
        CheckSameRgb(MapAces2LmtToBt2020Nits(*transform, {}, strength), {});
        CheckContained(MapAces2LmtToBt2020Nits(*transform, {2.0, 0.7, 0.5}, strength), 1000.0);
    }
    // Strength zero retains the fixed shadow adaptation, rather than becoming
    // the direct Academy operator. Direct reference checks above remain intact.
    constexpr LinearRgb shadow{0.001, 0.001, 0.001};
    FO4HDR_CHECK(MapAces2LmtToBt2020Nits(*transform, shadow, 0.0).red >
        MapAces2ToBt2020Nits(*transform, shadow).red);
    CheckSameRgb(MapAces2LmtToBt2020Nits(*transform, shadow, -1.0), {});
    CheckLookSettings();
}


}  // namespace

void RunToneMappingTests()
{
    TestAces2ReferenceSamples();
    TestAces2ContrastLook();
    TestAces2ContainedInputs();
    TestAces2InputPolicy();
    TestAces2PeakReuse();
}

}  // namespace fo4hdr::tests
