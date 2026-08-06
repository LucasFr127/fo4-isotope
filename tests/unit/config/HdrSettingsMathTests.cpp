// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace {

using fo4hdr::HdrSettings;
using Error = fo4hdr::HdrSettingsValidationError;

auto SettingBits(const HdrSettings& value)
{
    return std::array{std::bit_cast<std::uint64_t>(value.displayPeakNits),
        std::bit_cast<std::uint64_t>(value.uiPaperWhiteNits),
        std::bit_cast<std::uint64_t>(value.videoWhiteNits),
        std::bit_cast<std::uint64_t>(value.vanillaColorGradingStrengthPercent),
        std::bit_cast<std::uint64_t>(value.contrast), std::bit_cast<std::uint64_t>(value.blacks),
        std::bit_cast<std::uint64_t>(value.highlights), std::bit_cast<std::uint64_t>(value.shadows),
        std::bit_cast<std::uint64_t>(value.saturationPercent), std::bit_cast<std::uint64_t>(value.temperature),
        std::bit_cast<std::uint64_t>(value.tint)};
}

void TestSettingsDefaults()
{
    const HdrSettings defaults{};
    FO4HDR_CHECK(defaults.displayPeakNits == 1000.0);
    FO4HDR_CHECK(defaults.uiPaperWhiteNits == 203.0);
    FO4HDR_CHECK(defaults.videoWhiteNits == 203.0);
    FO4HDR_CHECK(defaults.vanillaColorGradingStrengthPercent == 100.0);
    FO4HDR_CHECK(fo4hdr::kSceneReferenceWhiteNits == 203.0);
    FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(defaults) == Error::none);
}

void TestSettingsRanges()
{
    struct Range
    {
        double HdrSettings::* member;
        double minimum;
        double maximum;
        Error error;
    };
    constexpr std::array ranges{
        Range{&HdrSettings::displayPeakNits, 400.0, 2000.0, Error::displayPeakOutOfRange},
        Range{&HdrSettings::uiPaperWhiteNits, 100.0, 300.0, Error::uiPaperWhiteOutOfRange},
        Range{&HdrSettings::videoWhiteNits, 100.0, 300.0, Error::videoWhiteOutOfRange},
        Range{&HdrSettings::vanillaColorGradingStrengthPercent, 0.0, 100.0, Error::vanillaColorGradingOutOfRange},
        Range{&HdrSettings::contrast, -100.0, 100.0, Error::contrastOutOfRange},
        Range{&HdrSettings::blacks, -100.0, 100.0, Error::blacksOutOfRange},
        Range{&HdrSettings::highlights, -100.0, 100.0, Error::highlightsOutOfRange},
        Range{&HdrSettings::shadows, -100.0, 100.0, Error::shadowsOutOfRange},
        Range{&HdrSettings::saturationPercent, 0.0, 200.0, Error::saturationOutOfRange},
        Range{&HdrSettings::temperature, -100.0, 100.0, Error::temperatureOutOfRange},
        Range{&HdrSettings::tint, -100.0, 100.0, Error::tintOutOfRange}};
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto& range : ranges)
    {
        HdrSettings settings{};
        for (const auto endpoint : {range.minimum, range.maximum})
        {
            settings.*range.member = endpoint;
            FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(settings) == Error::none);
        }
        for (const auto invalid : {std::nextafter(range.minimum, -infinity),
                 std::nextafter(range.maximum, infinity), nan, infinity, -infinity})
        {
            settings.*range.member = invalid;
            const auto before = SettingBits(settings);
            FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(settings) == range.error);
            FO4HDR_CHECK(SettingBits(settings) == before);
        }
    }
}

void TestIndependentWhites()
{
    HdrSettings settings{};
    settings.uiPaperWhiteNits = 300.0;
    FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(settings) == Error::none);
    FO4HDR_CHECK(settings.videoWhiteNits == 203.0);
    FO4HDR_CHECK(settings.displayPeakNits == 1000.0);
    settings = {};
    settings.videoWhiteNits = 100.0;
    FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(settings) == Error::none);
    FO4HDR_CHECK(settings.uiPaperWhiteNits == 203.0);
    FO4HDR_CHECK(settings.displayPeakNits == 1000.0);
}

void TestToneMapperControlRanges()
{
    using Algorithm = fo4hdr::OperatorLutAlgorithm;
    using Parameters = fo4hdr::ToneMapperParameters;
    struct Range
    {
        double Parameters::* member;
        double minimum;
        double maximum;
        Algorithm algorithm;
    };
    constexpr std::array ranges{
        Range{&Parameters::agxMiddleGrayEv, -0.25, 0.25, Algorithm::agX},
        Range{&Parameters::agxContrast, 2.4, 3.0, Algorithm::agX},
        Range{&Parameters::agxHdrPurity, 0.35, 0.6, Algorithm::agX},
        Range{&Parameters::agxHdrShoulderPower, 1.9, 2.1, Algorithm::agX},
        Range{&Parameters::gt7ToeStrength, 1.0, 1.5, Algorithm::gt7},
        Range{&Parameters::gt7ShoulderStart, 0.35, 0.6, Algorithm::gt7},
        Range{&Parameters::gt7ColorBlend, 0.0, 0.8, Algorithm::gt7}};
    const auto infinity = std::numeric_limits<double>::infinity();
    for (const auto& range : ranges)
    {
        Parameters parameters;
        for (const auto endpoint : {range.minimum, range.maximum})
        {
            parameters.*range.member = endpoint;
            FO4HDR_CHECK(fo4hdr::ValidateToneMapperParameters(range.algorithm, parameters));
        }
        for (const auto invalid : {std::nextafter(range.minimum, -infinity),
                 std::nextafter(range.maximum, infinity), infinity,
                 std::numeric_limits<double>::quiet_NaN()})
        {
            parameters.*range.member = invalid;
            FO4HDR_CHECK(!fo4hdr::ValidateToneMapperParameters(range.algorithm, parameters));
            fo4hdr::PluginConfiguration configuration;
            configuration.display.toneMapperParameters = parameters;
            FO4HDR_CHECK(!fo4hdr::ValidatePluginConfiguration(configuration));
        }
    }
    FO4HDR_CHECK(!fo4hdr::ValidateToneMapperParameters(static_cast<Algorithm>(255), {}));
}

void TestToneMapperEffectiveParameters()
{
    using Algorithm = fo4hdr::OperatorLutAlgorithm;
    const fo4hdr::ToneMapperParameters defaults;
    auto parameters = defaults;
    parameters.agxContrast = 3.0;
    parameters.gt7ColorBlend = 0.8;
    FO4HDR_CHECK(fo4hdr::ActiveToneMapperParameters(Algorithm::aces2, parameters) == defaults);
    const auto agx = fo4hdr::ActiveToneMapperParameters(Algorithm::agX, parameters);
    FO4HDR_CHECK(agx.agxContrast == 3.0);
    FO4HDR_CHECK(agx.gt7ColorBlend == defaults.gt7ColorBlend);
    const auto gt7 = fo4hdr::ActiveToneMapperParameters(Algorithm::gt7, parameters);
    FO4HDR_CHECK(gt7.gt7ColorBlend == 0.8);
    FO4HDR_CHECK(gt7.agxContrast == defaults.agxContrast);
}

void TestToneMapperPersistence()
{
    constexpr auto original = "[Output]\nDisplayPeakNits=1000\nUiPaperWhiteNits=203\n"
        "VideoWhiteNits=203\n[ToneMapper]\nAlgorithm=ACES2\n[Other]\nKeep=unchanged\n";
    const auto parsed = fo4hdr::ParsePluginConfiguration(original);
    FO4HDR_CHECK(parsed.status == fo4hdr::PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(parsed.value.display.toneMapperParameters == fo4hdr::ToneMapperParameters{});
    FO4HDR_CHECK(parsed.value.display.vanillaColorGradingStrengthPercent == 100.0);
    FO4HDR_CHECK(parsed.value.display.contrast == 0);
    FO4HDR_CHECK(parsed.value.display.saturationPercent == 100);
    auto requested = parsed.value;
    requested.display.vanillaColorGradingStrengthPercent = 50.0;
    requested.display.contrast = 25;
    requested.display.blacks = -20;
    requested.display.highlights = 30;
    requested.display.shadows = -40;
    requested.display.saturationPercent = 120;
    requested.display.temperature = 15;
    requested.display.tint = -10;
    requested.display.toneMapperParameters.agxContrast = 3.0;
    requested.display.toneMapperParameters.gt7ToeStrength = 1.5;
    FO4HDR_CHECK(!(requested == parsed.value));
    const auto rewritten = fo4hdr::RewritePluginConfigurationIni(original, requested);
    const auto restored = fo4hdr::ParsePluginConfiguration(rewritten);
    FO4HDR_CHECK(restored.status == fo4hdr::PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(restored.value == requested);
    FO4HDR_CHECK(rewritten.find("Keep=unchanged") != std::string::npos);
    FO4HDR_CHECK(fo4hdr::ParsePluginConfiguration("[Effects]\nVanillaColorGradingStrength=101\n").status ==
        fo4hdr::PluginConfigurationStatus::invalid);
    FO4HDR_CHECK(fo4hdr::ParsePluginConfiguration(rewritten + "[Effects]\nVanillaColorGradingStrength=50\n").status ==
        fo4hdr::PluginConfigurationStatus::invalid);
    FO4HDR_CHECK(fo4hdr::ParsePluginConfiguration("[Image]\nShadows=101\n").status ==
        fo4hdr::PluginConfigurationStatus::invalid);
    FO4HDR_CHECK(fo4hdr::ParsePluginConfiguration(rewritten + "[Image]\nTint=0\n").status ==
        fo4hdr::PluginConfigurationStatus::invalid);
    FO4HDR_CHECK(fo4hdr::ParsePluginConfiguration(rewritten + "[AgX]\nContrast=2.7\n").status ==
        fo4hdr::PluginConfigurationStatus::invalid);
}

void TestNeutralImageMath()
{
    const auto math = fo4hdr::DeriveHdrSettingsMath({});
    FO4HDR_CHECK(math.contrastSlope == 1);
    FO4HDR_CHECK(math.blacksStrength == 0);
    FO4HDR_CHECK(math.highlightsEv == 0);
    FO4HDR_CHECK(math.shadowsEv == 0);
    FO4HDR_CHECK(math.saturationScale == 1);
    constexpr std::array<double, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    FO4HDR_CHECK(math.colorBalanceBt709 == identity);
    HdrSettings invalid;
    invalid.tint = 101;
    FO4HDR_CHECK(fo4hdr::DeriveHdrSettingsMath(invalid).colorBalanceBt709 == identity);
}

void TestAdjustedImageMath()
{
    HdrSettings settings;
    settings.contrast = 100;
    settings.blacks = 100;
    settings.highlights = 50;
    settings.shadows = -50;
    settings.saturationPercent = 0;
    settings.temperature = 20;
    settings.tint = -30;
    const auto math = fo4hdr::DeriveHdrSettingsMath(settings);
    FO4HDR_CHECK(math.contrastSlope == 2);
    FO4HDR_CHECK(math.blacksStrength == 0.75);
    FO4HDR_CHECK(math.highlightsEv == 1);
    FO4HDR_CHECK(math.shadowsEv == -1);
    FO4HDR_CHECK(math.saturationScale == 0);
    FO4HDR_CHECK(math.colorBalanceBt709 != fo4hdr::HdrSettingsMath{}.colorBalanceBt709);
    for (const auto value : math.colorBalanceBt709) FO4HDR_CHECK(std::isfinite(value));
}

void TestColorBalanceAnchor()
{
    HdrSettings settings;
    settings.temperature = 20;
    settings.tint = -30;
    const auto actual = fo4hdr::DeriveHdrSettingsMath(settings).colorBalanceBt709;
    constexpr std::array<double, 9> expected{
        1.0212627931544651, 0.027566926935249494, 0.0064129259361357938,
        -0.0012828855962777597, 0.99650280298776606, 0.0026329634878390767,
        -0.0031038820888138231, -0.011582569944983787, 0.87324218699067924};
    for (std::size_t index = 0; index < actual.size(); ++index)
        FO4HDR_CHECK(fo4hdr::test::Approx(actual[index], expected[index], 1.0e-12));
}

}  // namespace

namespace fo4hdr::tests {

void RunHdrSettingsMathTests()
{
    TestSettingsDefaults();
    TestSettingsRanges();
    TestIndependentWhites();
    TestToneMapperControlRanges();
    TestToneMapperEffectiveParameters();
    TestToneMapperPersistence();
    TestNeutralImageMath();
    TestAdjustedImageMath();
    TestColorBalanceAnchor();
}

}  // namespace fo4hdr::tests
