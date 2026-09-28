// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "SettingsPersistence.hpp"
#include "reference/SelectiveColorSourceOracle.hpp"
#include "TestHarness.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

namespace fo4hdr::tests {

void TestSelectiveColorPreparationAmounts(SelectiveColorPrepared& prepared)
{
    // Check the public settings-to-source-parameter contract independently of
    // the oracle below, which deliberately starts from prepared parameters.
    constexpr std::array amounts{0.0, 25.0, 50.0, 100.0};
    constexpr std::array rawRegions{
        SelectiveColorRegionSettings{-8.0, 60.0, -75.0},
        SelectiveColorRegionSettings{6.0, 140.0, 50.0}};
    for (const auto amountPercent : amounts)
    {
        SelectiveColorSettings folded{};
        folded.enabled = true;
        folded.amountPercent = amountPercent;
        folded.regions[0] = rawRegions[0];
        folded.regions[4] = rawRegions[1];
        FO4HDR_CHECK(PrepareSelectiveColor(folded, prepared));
        const auto amount = amountPercent / 100.0;
        for (const auto index : {std::size_t{0}, std::size_t{4}})
        {
            const auto& raw = folded.regions[index];
            FO4HDR_CHECK(test::Approx(prepared.regions[index][0],
                raw.hueDegrees * std::numbers::pi / 180.0 * amount,
                1.0e-14));
            FO4HDR_CHECK(test::Approx(prepared.regions[index][1],
                1.0 + (raw.saturationPercent / 100.0 - 1.0) * amount,
                1.0e-14));
            FO4HDR_CHECK(test::Approx(prepared.regions[index][2],
                raw.brightness / 100.0 * amount, 1.0e-14));
        }
        FO4HDR_CHECK(prepared.effectiveActive == (amountPercent != 0.0));
    }
}

void TestSelectiveColorBasicBehavior(SelectiveColorSettings& settings, SelectiveColorPrepared& prepared)
{
    const SelectiveColorRgb sample{0.8, 0.2, 0.1};
    // Restore the moderate settings used by the basic behavior checks.
    FO4HDR_CHECK(PrepareSelectiveColor(settings, prepared));
    const auto output = ApplySelectiveColor(sample, prepared);
    FO4HDR_CHECK(std::isfinite(output.red));
    FO4HDR_CHECK(std::isfinite(output.green));
    FO4HDR_CHECK(std::isfinite(output.blue));
    FO4HDR_CHECK(output.red >= 0.0);
    FO4HDR_CHECK(output.green >= 0.0);
    FO4HDR_CHECK(output.blue >= 0.0);
    const SelectiveColorRgb gray{0.18, 0.18, 0.18};
    const auto unchangedGray = ApplySelectiveColor(gray, prepared);
    FO4HDR_CHECK(unchangedGray.red == gray.red);
    FO4HDR_CHECK(unchangedGray.green == gray.green);
    FO4HDR_CHECK(unchangedGray.blue == gray.blue);
    const auto scaled = ApplySelectiveColor(
        {sample.red * 8.0, sample.green * 8.0, sample.blue * 8.0},
        prepared);
    FO4HDR_CHECK(test::Approx(scaled.red, output.red * 8.0, 2.0e-12));
    FO4HDR_CHECK(test::Approx(scaled.green, output.green * 8.0, 2.0e-12));
    FO4HDR_CHECK(test::Approx(scaled.blue, output.blue * 8.0, 2.0e-12));

    // A positive angular derivative prevents hue reversal at the +/-10-degree limits.
    for (std::size_t index = 0; index < settings.regions.size(); ++index)
        settings.regions[index].hueDegrees = (index & 1U) == 0U ? 10.0 : -10.0;
    FO4HDR_CHECK(PrepareSelectiveColor(settings, prepared));
    constexpr double step = 360.0 / 4096.0;
    for (unsigned index = 0; index < 4096; ++index)
    {
        const auto hue = index * step;
        const auto left = EvaluateSelectiveColorHueShiftDegrees(hue, prepared);
        const auto right = EvaluateSelectiveColorHueShiftDegrees(
            hue + step, prepared);
        FO4HDR_CHECK(1.0 + (right - left) / step >= 0.1270);
    }

    settings.amountPercent = 0.0;
    FO4HDR_CHECK(PrepareSelectiveColor(settings, prepared));
    FO4HDR_CHECK(!prepared.effectiveActive);
    settings.amountPercent = 100.0;
    settings.regions[0].hueDegrees = 10.01;
    FO4HDR_CHECK(!ValidateSelectiveColorSettings(settings));
}

void TestSelectiveColorPersistence()
{
    HdrSettings persisted{};
    persisted.selectiveColor.enabled = true;
    persisted.selectiveColor.amountPercent = 75.0;
    for (std::size_t index = 0; index < persisted.selectiveColor.regions.size(); ++index)
    {
        const auto value = static_cast<double>(index);
        persisted.selectiveColor.regions[index] = {
            value - 3.0, 110.0 + value, -30.0 + value};
    }
    const auto rewritten = RewriteHdrSettingsIni(
        ExtendedProductionConfiguration(), persisted,
        HdrInterfaceSettings{});
    const auto parsed = ParsePluginConfiguration(rewritten);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.selectiveColor == persisted.selectiveColor);
    FO4HDR_CHECK(rewritten.find("RedHue=-3\n") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("OrangeSaturation=111\n") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("MagentaBrightness=-24\n") != std::string::npos);
}

bool CheckSelectiveColorParity(SelectiveColorRgb input, const SelectiveColorPrepared& prepared)
{
    const auto expected = smooth_tetra_oracle::Apply(input, prepared);
    const std::array channels{expected.red, expected.green, expected.blue};
    if (std::ranges::any_of(channels, [](double value) { return value < 0.0; }))
        return false;
    const auto actual = ApplySelectiveColor(input, prepared);
    FO4HDR_CHECK(test::Approx(actual.red, expected.red, 2.0e-5));
    FO4HDR_CHECK(test::Approx(actual.green, expected.green, 2.0e-5));
    FO4HDR_CHECK(test::Approx(actual.blue, expected.blue, 2.0e-5));
    return true;
}

void TestSelectiveColorSourceParity(SelectiveColorPrepared& prepared)
{
    // Compare the optimized/prepared production CPU implementation against a
    // test-only oracle that preserves the source's separate interpolation
    // loops and per-pixel plane fit. Keep domain-adapter cases separate.
    SelectiveColorSettings paritySettings{};
    paritySettings.enabled = true;
    for (std::size_t index = 0; index < paritySettings.regions.size(); ++index)
    {
        paritySettings.regions[index].hueDegrees =
            (static_cast<double>(index) - 3.0) * 1.25;
        paritySettings.regions[index].saturationPercent =
            94.0 + static_cast<double>(index) * 2.0;
        paritySettings.regions[index].brightness =
            (static_cast<double>(index) - 3.0) * 4.0;
    }
    FO4HDR_CHECK(PrepareSelectiveColor(paritySettings, prepared));
    constexpr std::array<double, 7> samples{
        0.01, 0.03, 0.07, 0.15, 0.32, 0.65, 1.0};
    std::size_t parityCount{};
    for (const auto red : samples)
        for (const auto green : samples)
            for (const auto blue : samples)
            {
                parityCount += CheckSelectiveColorParity({red, green, blue}, prepared);
            }
    FO4HDR_CHECK(parityCount >= 300);
}

void TestSelectiveColorMixedParity(SelectiveColorPrepared& prepared)
{
    // Exercise mixed parameter families at every fixed center, representative
    // between-center hues, the polar wrap, and the neutral-axis approach.
    constexpr std::array mixedRegions{
        std::array{SelectiveColorRegionSettings{-9.5, 52.0, -80.0},
            SelectiveColorRegionSettings{7.25, 145.0, 60.0},
            SelectiveColorRegionSettings{-3.0, 80.0, 90.0},
            SelectiveColorRegionSettings{9.0, 130.0, -55.0},
            SelectiveColorRegionSettings{-7.0, 110.0, 25.0},
            SelectiveColorRegionSettings{4.5, 65.0, -35.0},
            SelectiveColorRegionSettings{1.75, 138.0, 70.0}},
        std::array{SelectiveColorRegionSettings{8.0, 148.0, 75.0},
            SelectiveColorRegionSettings{-6.5, 58.0, -65.0},
            SelectiveColorRegionSettings{2.0, 125.0, -30.0},
            SelectiveColorRegionSettings{-8.5, 72.0, 85.0},
            SelectiveColorRegionSettings{6.0, 135.0, -90.0},
            SelectiveColorRegionSettings{-1.5, 95.0, 40.0},
            SelectiveColorRegionSettings{9.5, 55.0, 20.0}}};
    constexpr std::array hueSamples{0.0, 15.0, 30.0, 45.0, 60.0,
        90.0, 120.0, 150.0, 180.0, 210.0, 240.0, 270.0, 300.0,
        330.0, 359.999};
    constexpr std::array saturationSamples{1.0e-7, 0.2};
    for (std::size_t settingIndex = 0;
         settingIndex < mixedRegions.size(); ++settingIndex)
    {
        SelectiveColorSettings mixed{};
        mixed.enabled = true;
        mixed.amountPercent = settingIndex == 0 ? 37.0 : 83.0;
        mixed.regions = mixedRegions[settingIndex];
        FO4HDR_CHECK(PrepareSelectiveColor(mixed, prepared));
        for (const auto hueDegrees : hueSamples)
            for (const auto saturation : saturationSamples)
            {
                const auto hue = hueDegrees * std::numbers::pi / 180.0;
                constexpr double mean{0.25};
                const auto radial = mean * saturation;
                const SelectiveColorRgb input{
                    mean + 2.0 * radial * std::cos(hue),
                    mean - radial * std::cos(hue) +
                        std::sqrt(3.0) * radial * std::sin(hue),
                    mean - radial * std::cos(hue) -
                        std::sqrt(3.0) * radial * std::sin(hue)};
                (void)CheckSelectiveColorParity(input, prepared);
                const auto actual = ApplySelectiveColor(input, prepared);
                FO4HDR_CHECK(std::isfinite(actual.red));
    FO4HDR_CHECK(actual.red >= 0.0);
                FO4HDR_CHECK(std::isfinite(actual.green));
    FO4HDR_CHECK(actual.green >= 0.0);
                FO4HDR_CHECK(std::isfinite(actual.blue));
    FO4HDR_CHECK(actual.blue >= 0.0);
            }
    }
}

void CheckSelectiveColorGuards(const SelectiveColorPrepared& candidate)
{
    constexpr std::array<SelectiveColorRgb, 5> stressColors{{
        {0.0, 0.0, 0.0},
        {0.18, 0.18, 0.18},
        {1.0, 0.0, 0.0},
        {0.000000059604644775390625, 0.0000000298023223876953125, 0.0},
        {65504.0, 32752.0, 1.0}}};

    for (const auto input : stressColors)
    {
        const auto result = ApplySelectiveColor(input, candidate);
        FO4HDR_CHECK(std::isfinite(result.red));
        FO4HDR_CHECK(std::isfinite(result.green));
        FO4HDR_CHECK(std::isfinite(result.blue));
        FO4HDR_CHECK(result.red >= 0.0);
        FO4HDR_CHECK(result.red <= 65504.0);
        FO4HDR_CHECK(result.green >= 0.0);
        FO4HDR_CHECK(result.green <= 65504.0);
        FO4HDR_CHECK(result.blue >= 0.0);
        FO4HDR_CHECK(result.blue <= 65504.0);
    }
}

void TestSelectiveColorEndpointGuards(SelectiveColorPrepared& prepared)
{
    // Exhaust every seven-region endpoint pattern independently for each
    // parameter family. These are guarded-domain checks, deliberately
    // separate from the moderate source-parity corpus above.
    struct EndpointFamily
    {
        double SelectiveColorRegionSettings::* member;
        double minimum;
        double maximum;
    };
    constexpr EndpointFamily families[]{
        {&SelectiveColorRegionSettings::hueDegrees, -10.0, 10.0},
        {&SelectiveColorRegionSettings::saturationPercent, 50.0, 150.0},
        {&SelectiveColorRegionSettings::brightness, -100.0, 100.0}};
    for (unsigned bits = 0; bits < 128; ++bits)
    {
        for (const auto& family : families)
        {
            SelectiveColorSettings endpoints{};
            endpoints.enabled = true;
            for (std::size_t index = 0; index < endpoints.regions.size(); ++index)
                endpoints.regions[index].*family.member =
                    (bits & (1U << index)) != 0U ? family.maximum : family.minimum;
            FO4HDR_CHECK(PrepareSelectiveColor(endpoints, prepared));
            CheckSelectiveColorGuards(prepared);
        }
    }
}

void TestSelectiveColorInvalidInputs(SelectiveColorPrepared& prepared)
{
    SelectiveColorSettings invalid{};
    invalid.regions[0].brightness =
        std::numeric_limits<double>::infinity();
    FO4HDR_CHECK(!ValidateSelectiveColorSettings(invalid));
    invalid = {};
    invalid.amountPercent = std::numeric_limits<double>::quiet_NaN();
    FO4HDR_CHECK(!ValidateSelectiveColorSettings(invalid));
    constexpr std::array negativeInputs{
        SelectiveColorRgb{-0.1, 0.2, 0.3},
        SelectiveColorRgb{0.2, -0.1, 0.3},
        SelectiveColorRgb{0.2, 0.3, -0.1}};
    for (const auto input : negativeInputs)
    {
        const auto result = ApplySelectiveColor(input, prepared);
        FO4HDR_CHECK(result.red == input.red);
    FO4HDR_CHECK(result.green == input.green);
    FO4HDR_CHECK(result.blue == input.blue);
    }
    for (const auto value : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()})
    {
        const SelectiveColorRgb input{value, 0.2, 0.3};
        const auto result = ApplySelectiveColor(input, prepared);
        FO4HDR_CHECK((std::isnan(value) && std::isnan(result.red)) ||
            result.red == value);
        FO4HDR_CHECK(result.green == input.green);
    FO4HDR_CHECK(result.blue == input.blue);
    }}

void TestSelectiveColorMathAndPersistence()
{
    SelectiveColorSettings settings{};
    FO4HDR_CHECK(ValidateSelectiveColorSettings(settings));
    FO4HDR_CHECK(!IsSelectiveColorEffective(settings));
    SelectiveColorPrepared prepared{};
    FO4HDR_CHECK(PrepareSelectiveColor(settings, prepared));
    const SelectiveColorRgb sample{0.8, 0.2, 0.1};
    FO4HDR_CHECK(ApplySelectiveColor(sample, prepared).red == sample.red);

    settings.enabled = true;
    settings.regions[0].hueDegrees = 10.0;
    settings.regions[1].saturationPercent = 125.0;
    settings.regions[5].brightness = 50.0;
    FO4HDR_CHECK(IsSelectiveColorEffective(settings));
    FO4HDR_CHECK(PrepareSelectiveColor(settings, prepared));

    TestSelectiveColorPreparationAmounts(prepared);
    TestSelectiveColorBasicBehavior(settings, prepared);
    TestSelectiveColorPersistence();
    TestSelectiveColorSourceParity(prepared);
    TestSelectiveColorMixedParity(prepared);
    TestSelectiveColorEndpointGuards(prepared);
    TestSelectiveColorInvalidInputs(prepared);
}

}  // namespace fo4hdr::tests
