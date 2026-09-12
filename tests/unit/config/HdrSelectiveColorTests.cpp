// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"
#include "Fallout4HDR/output/SelectiveColor.hpp"
#include "reference/SelectiveColorSourceOracle.hpp"
#include "HdrSettingsTestSupport.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>

namespace fo4hdr::tests {
namespace {

void CheckSelectiveSame(SelectiveColorRgb actual, SelectiveColorRgb expected)
{
    FO4HDR_CHECK(actual.red == expected.red);
    FO4HDR_CHECK(actual.green == expected.green);
    FO4HDR_CHECK(actual.blue == expected.blue);
}

void CheckSelectiveReferenceChannel(double actual, double expected)
{
    FO4HDR_CHECK(std::isfinite(expected));
    FO4HDR_CHECK(expected >= 0.0);
    // Absolute source-parity allowance for the selected positive domain only.
    FO4HDR_CHECK(std::abs(actual - expected) <= 2.0e-5);
}

void CheckSelectiveSource(SelectiveColorRgb input, const SelectiveColorPrepared& prepared)
{
    const auto expected = smooth_tetra_oracle::Apply(input, prepared);
    const auto actual = ApplySelectiveColor(input, prepared);
    CheckSelectiveReferenceChannel(actual.red, expected.red);
    CheckSelectiveReferenceChannel(actual.green, expected.green);
    CheckSelectiveReferenceChannel(actual.blue, expected.blue);
}

void TestSelectiveColorMath()
{
    SelectiveColorSettings settings;
    SelectiveColorPrepared prepared;
    FO4HDR_CHECK(PrepareSelectiveColor(settings, prepared));
    FO4HDR_CHECK(!prepared.effectiveActive);
    constexpr SelectiveColorRgb identity{0.8, 0.2, 0.1};
    CheckSelectiveSame(ApplySelectiveColor(identity, prepared), identity);
    settings.enabled = true;
    FO4HDR_CHECK(PrepareSelectiveColor(settings, prepared));
    FO4HDR_CHECK(!prepared.effectiveActive);
    CheckSelectiveSame(ApplySelectiveColor(identity, prepared), identity);
    for (std::size_t index = 0; index < settings.regions.size(); ++index)
    {
        const auto position = static_cast<double>(index);
        settings.regions[index] = {(position - 3.0) * 1.25,
            94.0 + position * 2.0, (position - 3.0) * 4.0};
    }
    FO4HDR_CHECK(PrepareSelectiveColor(settings, prepared));
    FO4HDR_CHECK(prepared.effectiveActive);
    for (std::size_t index = 0; index < prepared.regions.size(); ++index)
    {
        const auto position = static_cast<double>(index);
        FO4HDR_CHECK(std::abs(prepared.regions[index][0] -
            (position - 3.0) * 1.25 * std::numbers::pi / 180.0) <= 1.0e-14);
        FO4HDR_CHECK(std::abs(prepared.regions[index][1] -
            (94.0 + position * 2.0) / 100.0) <= 1.0e-14);
        FO4HDR_CHECK(std::abs(prepared.regions[index][2] -
            (position - 3.0) * 4.0 / 100.0) <= 1.0e-14);
    }
    for (const auto sample : std::array{SelectiveColorRgb{0.8, 0.2, 0.1},
             SelectiveColorRgb{0.1, 0.8, 0.2}, SelectiveColorRgb{0.2, 0.1, 0.8},
             SelectiveColorRgb{0.18, 0.18, 0.18}, SelectiveColorRgb{0.6, 0.5, 0.4},
             SelectiveColorRgb{0.4, 0.6, 0.5}})
        CheckSelectiveSource(sample, prepared);
    settings.regions[0].hueDegrees = 10.01;
    FO4HDR_CHECK(!PrepareSelectiveColor(settings, prepared));
}

constexpr std::string_view kSelectiveIni{
    "[SelectiveColor]\nEnabled=1\nAmount=60\n"
    "RedHue=1\nRedSaturation=101\nRedBrightness=-3\n"
    "OrangeHue=2\nOrangeSaturation=102\nOrangeBrightness=-2\n"
    "YellowHue=3\nYellowSaturation=103\nYellowBrightness=-1\n"
    "GreenHue=4\nGreenSaturation=104\nGreenBrightness=0\n"
    "CyanHue=5\nCyanSaturation=105\nCyanBrightness=1\n"
    "BlueHue=6\nBlueSaturation=106\nBlueBrightness=2\n"
    "MagentaHue=7\nMagentaSaturation=107\nMagentaBrightness=3\n"};

SelectiveColorSettings ExpectedSelectiveSettings()
{
    SelectiveColorSettings value;
    value.enabled = true;
    value.amountPercent = 60;
    value.regions = {{{1, 101, -3}, {2, 102, -2}, {3, 103, -1},
        {4, 104, 0}, {5, 105, 1}, {6, 106, 2}, {7, 107, 3}}};
    return value;
}

void TestSelectiveValidation()
{
    HdrSettings settings;
    FO4HDR_CHECK(!settings.selectiveColor.enabled);
    FO4HDR_CHECK(ValidateHdrSettings(settings) == HdrSettingsValidationError::none);
    settings.selectiveColor.regions.back().hueDegrees = 10.01;
    FO4HDR_CHECK(ValidateHdrSettings(settings) == HdrSettingsValidationError::selectiveColorOutOfRange);
    settings.selectiveColor = ExpectedSelectiveSettings();
    FO4HDR_CHECK(ValidateHdrSettings(settings) == HdrSettingsValidationError::none);
    settings.selectiveColor.amountPercent = std::numeric_limits<double>::infinity();
    FO4HDR_CHECK(ValidateHdrSettings(settings) == HdrSettingsValidationError::selectiveColorOutOfRange);
}

void TestSelectiveIni()
{
    const auto old = ParsePluginConfiguration(kOriginal);
    FO4HDR_CHECK(old.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(old.value.display.selectiveColor == SelectiveColorSettings{});
    const auto original = std::string(kOriginal) + std::string(kSelectiveIni);
    const auto parsed = ParsePluginConfiguration(original);
    FO4HDR_CHECK(parsed.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(parsed.value.display.selectiveColor == ExpectedSelectiveSettings());
    auto changed = parsed.value;
    changed.display.selectiveColor.regions.back().brightness = 25;
    FO4HDR_CHECK(changed != parsed.value);
    const auto rewritten = RewritePluginConfigurationIni(original, changed);
    FO4HDR_CHECK(rewritten.find("Untouched = yes") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("peak comment") != std::string::npos);
    const auto roundTrip = ParsePluginConfiguration(rewritten);
    FO4HDR_CHECK(roundTrip.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(roundTrip.value == changed);
    FO4HDR_CHECK(ParsePluginConfiguration(original + "Enabled=0\n").status == PluginConfigurationStatus::invalid);
    FO4HDR_CHECK(ParsePluginConfiguration(std::string(kOriginal) +
        "[SelectiveColor]\nEnabled=true\n").status == PluginConfigurationStatus::invalid);
    FO4HDR_CHECK(ParsePluginConfiguration(std::string(kOriginal) +
        "[SelectiveColor]\nEnabled=0\nMagentaBrightness=100.01\n").status == PluginConfigurationStatus::invalid);
}

}  // namespace

void RunHdrSelectiveColorTests()
{
    TestSelectiveColorMath();
    TestSelectiveValidation();
    TestSelectiveIni();
}

}  // namespace fo4hdr::tests
