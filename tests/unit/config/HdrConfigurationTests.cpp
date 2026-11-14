// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"
#include "SettingsPersistence.hpp"
#include "TestHarness.hpp"
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fo4hdr::tests {
namespace {

using enum ConfigurationRepairKind;
using RepairCounts = decltype(ConfigurationRepairs{}.counts);

[[nodiscard]] PluginConfigurationResult ParseWith(std::string_view extra)
{
    return ParsePluginConfiguration(MinimalProductionConfiguration() + std::string(extra));
}

// The repairs extra text adds to those of the minimal configuration.
[[nodiscard]] std::int64_t ExtraRepairs(std::string_view extra, ConfigurationRepairKind kind)
{
    const auto baseline = RepairCount(ParsePluginConfiguration(MinimalProductionConfiguration()), kind);
    return static_cast<std::int64_t>(RepairCount(ParseWith(extra), kind)) - baseline;
}

[[nodiscard]] std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

// Replaces the first occurrence; a missing pattern fails the check.
void Replace(std::string& text, std::string_view from, std::string_view to)
{
    const auto position = text.find(from);
    FO4HDR_CHECK(position != std::string::npos);
    if (position != std::string::npos) text.replace(position, from.size(), to);
}

}  // namespace

// The INI the plugin writes when none exists is the expected default, byte for byte.
void TestExpectedDefaultConfiguration()
{
    const auto expected = ReadText("config/FO4Isotope.ini");
    const auto generated = ParsePluginConfiguration("");
    FO4HDR_CHECK(generated.valid && generated.rewriteNeeded);
    if (generated.text != expected)
    {
        // Review this render, then copy it over the expected file; never edit that by hand.
        std::filesystem::create_directories("build/expected-config");
        std::ofstream("build/expected-config/FO4Isotope.ini", std::ios::binary) << generated.text;
    }
    FO4HDR_CHECK(generated.text == expected);
    FO4HDR_CHECK(expected.starts_with("# Isotope settings."));
    const auto reparsed = ParsePluginConfiguration(expected);
    FO4HDR_CHECK(reparsed.valid && !reparsed.rewriteNeeded);
    FO4HDR_CHECK(reparsed.repairs.counts == RepairCounts{});
    FO4HDR_CHECK(reparsed.stored == generated.stored);
    FO4HDR_CHECK(reparsed.value.enabled && reparsed.value.productionRequested);
    FO4HDR_CHECK(reparsed.value.weatherGradeResponse == WeatherGradeResponseSettings{});
    FO4HDR_CHECK(reparsed.value.productionAlgorithm ==
        ProductionToneMapperAlgorithm::gt7);
    FO4HDR_CHECK(reparsed.value.toneMapperParameters ==
        ToneMapperParameters{});
    FO4HDR_CHECK(reparsed.value.toneMapperParameters.agxContrast == 2.7);
    FO4HDR_CHECK(reparsed.value.colorGradingMethod == ColorGradingMethod::rgbRatio);
    FO4HDR_CHECK(reparsed.value.colorGradingCurve == ColorGradingCurve::gamma22);
    FO4HDR_CHECK(reparsed.value.exposureEv == 0.0);
    FO4HDR_CHECK(reparsed.value.displayPeakNits == 1000.0);
    FO4HDR_CHECK(reparsed.value.uiPaperWhiteNits == 203.0);
    FO4HDR_CHECK(reparsed.value.videoWhiteNits == 203.0);
    FO4HDR_CHECK(reparsed.value.contrast == 0.0);
    FO4HDR_CHECK(reparsed.value.blacks == 0.0);
    FO4HDR_CHECK(reparsed.value.highlights == 0.0);
    FO4HDR_CHECK(reparsed.value.shadows == 0.0);
    FO4HDR_CHECK(reparsed.value.saturationPercent == 100.0);
    FO4HDR_CHECK(reparsed.value.highlightSaturationPercent == 100.0);
    FO4HDR_CHECK(reparsed.value.temperature == 0.0);
    FO4HDR_CHECK(reparsed.value.tint == 0.0);
    FO4HDR_CHECK(!reparsed.value.selectiveColor.enabled);
    FO4HDR_CHECK(reparsed.value.selectiveColor.amountPercent == 100.0);
    FO4HDR_CHECK(!IsSelectiveColorEffective(reparsed.value.selectiveColor));
    FO4HDR_CHECK(
        reparsed.value.vanillaColorGradingStrengthPercent == 100.0);
    FO4HDR_CHECK(reparsed.value.fallbackGradeLuminancePercent == 67.0);
    FO4HDR_CHECK(reparsed.value.fallbackGradeColorPercent == 67.0);
    FO4HDR_CHECK(reparsed.value.bloomStrengthPercent == 50.0);
    FO4HDR_CHECK(reparsed.value.interfaceSettings.hdrEnabled);
    FO4HDR_CHECK(!reparsed.value.developerMode);
    FO4HDR_CHECK(!reparsed.value.traceRenderPipeline);
    FO4HDR_CHECK(!reparsed.value.measurePerformance);
    FO4HDR_CHECK(!reparsed.value.developerHdr10Reference);
    FO4HDR_CHECK(!reparsed.value.forceHdrSwapChainFailure);
    FO4HDR_CHECK(reparsed.value.sdrEnabled);
    FO4HDR_CHECK(reparsed.value.sdrSettings == DefaultSdrSettings());
    // The parser still accepts the former names; the expected file must not use them.
    FO4HDR_CHECK(HasIniLine(expected, "ColorGradingMethod=RgbRatio"));
    for (const auto* former : {"Radial", "NativeReference", "HdrColorResponse",
             "VanillaGradeLuminanceStrength", "VanillaGradeColorStrength", "[Interface]"})
        FO4HDR_CHECK(!expected.contains(former));
}

// The INI of the last release upgrades to the expected default, keeping its values.
void TestReleasedConfigurationUpgrade()
{
    const auto released = ReadText("tests/unit/config/ReleasedFO4Isotope.ini");
    const auto upgraded = ParsePluginConfiguration(released);
    FO4HDR_CHECK(upgraded.valid && upgraded.rewriteNeeded);
    FO4HDR_CHECK(upgraded.repairs.counts == RepairCounts{});
    FO4HDR_CHECK(upgraded.text == ReadText("config/FO4Isotope.ini"));

    auto changed = released;
    Replace(changed, "DisplayPeakNits=1000", "DisplayPeakNits=1400");
    Replace(changed, "Algorithm=GT7", "Algorithm=AgX");
    Replace(changed, "ShoulderStart=0.28", "ShoulderStart=0.3");
    const auto upgradedChanged = ParsePluginConfiguration(changed);
    FO4HDR_CHECK(upgradedChanged.valid);
    FO4HDR_CHECK(upgradedChanged.repairs.counts == RepairCounts{});
    FO4HDR_CHECK(upgradedChanged.value.displayPeakNits == 1400.0);
    FO4HDR_CHECK(upgradedChanged.value.productionAlgorithm == ProductionToneMapperAlgorithm::agX);
    FO4HDR_CHECK(upgradedChanged.value.sdrSettings.toneMapperParameters.gt7ShoulderStart == 0.3);
    FO4HDR_CHECK(HasIniLine(upgradedChanged.text, "DisplayPeakNits=1400"));
    FO4HDR_CHECK(HasIniLine(upgradedChanged.text, "Algorithm=AgX"));
}

// The SDR-only switch parses in its SDR section; the HDR section drops it.
void CheckSdrOnlySwitches()
{
    const auto switches = ParseWith(
        "[SdrToneMapper]\nAutomaticWeatherResponse=0\n[ToneMapper]\nAutomaticWeatherResponse=1\n");
    FO4HDR_CHECK(switches.valid);
    FO4HDR_CHECK(!switches.value.sdrSettings.sdrOperatorWeatherResponse);
    FO4HDR_CHECK(!switches.value.sdrOperatorWeatherResponse);
    FO4HDR_CHECK(ExtraRepairs(
        "[SdrToneMapper]\nAutomaticWeatherResponse=0\n[ToneMapper]\nAutomaticWeatherResponse=1\n",
        dropped) == 1);
}

void TestSdrProfileConfiguration()
{
    // A file without SDR keys requests SDR mode with the vanilla-neutral profile.
    const auto minimal = ParsePluginConfiguration(MinimalProductionConfiguration());
    FO4HDR_CHECK(minimal.valid && minimal.value.sdrEnabled);
    FO4HDR_CHECK(minimal.value.sdrSettings == DefaultSdrSettings());
    // A failed load falls back to default construction, which stays inert.
    FO4HDR_CHECK(!PluginConfiguration{}.sdrEnabled);

    constexpr std::string_view sdrKeys =
        "[General]\nSdrEnabled=0\n"
        "[SdrImage]\nExposureEV=0.5\nSaturation=120\nHighlightSaturation=60\nColorGradingMethod=Unknown\n"
        "[SdrEffects]\nBloomStrength=40\nFallbackGradeColor=20\n"
        "[SdrSelectiveColor]\nEnabled=1\nRedHue=5\n"
        "[SdrWeatherLutResponse]\nEnabled=1\nColorGain=0.5\n"
        "[SdrToneMapper]\nAlgorithm=Vanilla\n"
        "[SdrAgX]\nContrast=2.5\nShoulderPower=1.8\n";
    const auto parsed = ParseWith(sdrKeys);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(!parsed.value.sdrEnabled);
    auto expected = DefaultSdrSettings();
    expected.toneMapperParameters.agxContrast = 2.5;
    expected.exposureEv = 0.5;
    expected.saturationPercent = 120.0;
    expected.highlightSaturationPercent = 60.0;
    expected.bloomStrengthPercent = 40.0;
    expected.fallbackGradeColorPercent = 20.0;
    expected.selectiveColor.enabled = true;
    expected.selectiveColor.regions[0].hueDegrees = 5.0;
    expected.weatherGradeResponse.enabled = true;
    expected.weatherGradeResponse.color.gain = 0.5;
    FO4HDR_CHECK(parsed.value.sdrSettings == expected);
    // HDR-only names in an Sdr section are dropped.
    FO4HDR_CHECK(ExtraRepairs(sdrKeys, dropped) == 2);
    // SDR keys never reach the HDR settings or the production request.
    FO4HDR_CHECK(static_cast<const HdrSettings&>(parsed.value) ==
        static_cast<const HdrSettings&>(minimal.value));
    FO4HDR_CHECK(parsed.value.productionRequested == minimal.value.productionRequested);
}

// A repeated key keeps its last value, and SDR numbers clamp to the SDR ranges.
void TestSdrProfileRanges()
{
    const auto sdr = [](std::string_view extra) { return ParseWith(extra).value.sdrSettings; };
    FO4HDR_CHECK(sdr("[Image]\nContrast=1\n[SdrImage]\nContrast=2\n").contrast == 2.0);
    FO4HDR_CHECK(sdr("[SdrImage]\nContrast=1\n[SdrImage]\nContrast=2\n").contrast == 2.0);
    FO4HDR_CHECK(sdr("[SdrImage]\nExposureEV=4.5\n").exposureEv == kMaximumSceneExposureEv);
    FO4HDR_CHECK(sdr("[SdrEffects]\nBloomStrength=100.01\n").bloomStrengthPercent == 100.0);
    FO4HDR_CHECK(sdr("[SdrSelectiveColor]\nRedSaturation=151\n").selectiveColor.regions[0]
        .saturationPercent == kMaximumSelectiveColorSaturationPercent);
    FO4HDR_CHECK(!sdr("[SdrWeatherLutResponse]\nEnabled=2\n").weatherGradeResponse.enabled);
    FO4HDR_CHECK(ExtraRepairs("[SdrWeatherLutResponse]\nEnabled=2\n", defaulted) == 1);
    FO4HDR_CHECK(!ParseWith("[General]\nSdrEnabled=off\n").value.sdrEnabled);
    FO4HDR_CHECK(sdr("[SdrAgX]\nContrast=3.1\n").toneMapperParameters.agxContrast ==
        kMaximumAgxContrast);
    // At the SDR white AgX can only lower middle gray.
    FO4HDR_CHECK(sdr("[SdrAgX]\nMiddleGrayOffsetEV=0.1\n").toneMapperParameters.agxMiddleGrayEv ==
        kMaximumSdrAgxMiddleGrayEv);
    FO4HDR_CHECK(sdr("[SdrAgX]\nMiddleGrayOffsetEV=-0.5\n").toneMapperParameters.agxMiddleGrayEv ==
        -0.5);
    // SDR GT7's shoulder may start where its toe ends, below the HDR minimum.
    FO4HDR_CHECK(sdr("[SdrGT7]\nShoulderStart=0.216\n").toneMapperParameters.gt7ShoulderStart ==
        kMinimumSdrGt7ShoulderStart);
    FO4HDR_CHECK(sdr("[SdrGT7]\nShoulderStart=0.215\n").toneMapperParameters.gt7ShoulderStart ==
        kMinimumSdrGt7ShoulderStart);
    FO4HDR_CHECK(ExtraRepairs("[SdrGT7]\nShoulderStart=0.215\n", clamped) == 1);
    FO4HDR_CHECK(ParseWith("[GT7]\nShoulderStart=0.216\n").value.toneMapperParameters
        .gt7ShoulderStart == kMinimumGt7ShoulderStart);
}

void TestSdrProfileAlgorithms()
{
    CheckSdrOnlySwitches();
    const auto sdr = [](std::string_view extra) { return ParseWith(extra).value.sdrSettings; };
    // SDR offers the vanilla curve plus each operator once it is selectable.
    for (const auto [name, algorithm] : {std::pair{"ACES2", ProductionToneMapperAlgorithm::aces2},
             std::pair{"AgX", ProductionToneMapperAlgorithm::agX},
             std::pair{"GT7", ProductionToneMapperAlgorithm::gt7}})
        FO4HDR_CHECK(sdr("[SdrToneMapper]\nAlgorithm=" + std::string(name) + "\n")
            .productionAlgorithm == (IsSelectableSdrToneMapper(algorithm) ? algorithm :
                ProductionToneMapperAlgorithm::native));
    // The HDR profile never takes the vanilla curve.
    auto hdrVanilla = MinimalProductionConfiguration();
    Replace(hdrVanilla, "Algorithm=ACES2", "Algorithm=Vanilla");
    const auto vanilla = ParsePluginConfiguration(hdrVanilla);
    FO4HDR_CHECK(vanilla.valid);
    FO4HDR_CHECK(vanilla.value.productionAlgorithm == HdrSettings{}.productionAlgorithm);
    FO4HDR_CHECK(RepairCount(vanilla, defaulted) ==
        RepairCount(ParsePluginConfiguration(MinimalProductionConfiguration()), defaulted) + 1);
}

void TestProductionConfiguration()
{
    auto text = MinimalProductionConfiguration();
    const auto production = ParsePluginConfiguration(text);
    FO4HDR_CHECK(production.valid);
    FO4HDR_CHECK(production.value.enabled && production.value.productionRequested);
    FO4HDR_CHECK(!production.value.developerMode);

    Replace(text, "HdrEnabled=1", "HdrEnabled=0");
    const auto disabled = ParsePluginConfiguration(text);
    FO4HDR_CHECK(disabled.valid);
    FO4HDR_CHECK(!disabled.value.enabled && !disabled.value.productionRequested);

    Replace(text, "HdrEnabled=0", "HdrEnabled=1");
    Replace(text, "DeveloperMode=0", "DeveloperMode=1");
    Replace(text, "DeveloperHdr10Reference=0", "DeveloperHdr10Reference=1");
    const auto diagnostic = ParsePluginConfiguration(text);
    FO4HDR_CHECK(diagnostic.valid);
    FO4HDR_CHECK(diagnostic.value.enabled && diagnostic.value.developerHdr10Reference);
    FO4HDR_CHECK(!diagnostic.value.productionRequested);
}

void TestDefaultRoundTrip(const PluginConfigurationResult& parsed)
{
    auto sourceContrastSettings = static_cast<HdrSettings>(parsed.value);
    sourceContrastSettings.toneMapperParameters.agxContrast = 2.4;
    const auto sourceContrastSaved = RewriteHdrSettingsIni(
        MinimalProductionConfiguration(), {sourceContrastSettings}, {});
    FO4HDR_CHECK(HasIniLine(IniSection(sourceContrastSaved, "AgX"), "Contrast=2.4"));
    const auto sourceContrastReloaded =
        ParsePluginConfiguration(sourceContrastSaved);
    FO4HDR_CHECK(sourceContrastReloaded.valid);
    FO4HDR_CHECK(
        sourceContrastReloaded.value.toneMapperParameters.agxContrast == 2.4);
    FO4HDR_CHECK(parsed.value.colorGradingMethod == ColorGradingMethod::rgbRatio);
    for (const auto method : {ColorGradingMethod::rgbRatio,
             ColorGradingMethod::affine})
    {
        auto settings = static_cast<HdrSettings>(parsed.value);
        settings.colorGradingMethod = method;
        const auto saved = RewriteHdrSettingsIni(
            MinimalProductionConfiguration(), {settings}, {});
        const auto loaded = ParsePluginConfiguration(saved);
        FO4HDR_CHECK(loaded.valid);
        FO4HDR_CHECK(loaded.value.colorGradingMethod == method);
        FO4HDR_CHECK(RewriteHdrSettingsIni(saved, {settings}, {}) == saved);
        settings.colorGradingMethod = HdrSettings{}.colorGradingMethod;
        const auto reset = ParsePluginConfiguration(
            RewriteHdrSettingsIni(saved, {settings}, {}));
        FO4HDR_CHECK(reset.valid);
        FO4HDR_CHECK(reset.value.colorGradingMethod == ColorGradingMethod::rgbRatio);
    }
}

void TestGradingCurveRoundTrip(const PluginConfigurationResult& parsed)
{
    FO4HDR_CHECK(parsed.value.colorGradingCurve == ColorGradingCurve::gamma22);
    for (const auto curve : {ColorGradingCurve::gamma22, ColorGradingCurve::srgb})
    {
        auto settings = static_cast<HdrSettings>(parsed.value);
        settings.colorGradingCurve = curve;
        const auto saved = RewriteHdrSettingsIni(
            MinimalProductionConfiguration(), {settings}, {});
        FO4HDR_CHECK(HasIniLine(saved, "ColorGradingCurve=" + std::string(ToString(curve))));
        const auto loaded = ParsePluginConfiguration(saved);
        FO4HDR_CHECK(loaded.valid);
        FO4HDR_CHECK(loaded.value.colorGradingCurve == curve);
        FO4HDR_CHECK(RewriteHdrSettingsIni(saved, {settings}, {}) == saved);
    }
}

void TestMinimalConfiguration()
{
    auto parsed = ParsePluginConfiguration(
        MinimalProductionConfiguration());
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.toneMapperParameters ==
        ToneMapperParameters{});
    FO4HDR_CHECK(parsed.value.toneMapperParameters.agxContrast == 2.7);
    TestDefaultRoundTrip(parsed);
    TestGradingCurveRoundTrip(parsed);
    FO4HDR_CHECK(parsed.value.videoWhiteNits == 203.0);
    FO4HDR_CHECK(parsed.value.contrast == 0.0);
    FO4HDR_CHECK(parsed.value.blacks == 0.0);
    FO4HDR_CHECK(parsed.value.highlights == 0.0);
    FO4HDR_CHECK(parsed.value.shadows == 0.0);
    FO4HDR_CHECK(parsed.value.saturationPercent == 100.0);
    FO4HDR_CHECK(parsed.value.highlightSaturationPercent == 100.0);
    FO4HDR_CHECK(parsed.value.temperature == 0.0);
    FO4HDR_CHECK(parsed.value.tint == 0.0);
    FO4HDR_CHECK(
        parsed.value.vanillaColorGradingStrengthPercent == 100.0);
    FO4HDR_CHECK(parsed.value.fallbackGradeLuminancePercent == 67.0);
    FO4HDR_CHECK(parsed.value.fallbackGradeColorPercent == 67.0);
    FO4HDR_CHECK(parsed.value.bloomStrengthPercent == 50.0);
    // Absent keys are added with their defaults.
    FO4HDR_CHECK(RepairCount(parsed, added) > 0 && RepairCount(parsed, defaulted) == 0);
}

// DeveloperMode and the Debug keys default to off when absent.
void TestOptionalDiagnosticKeys()
{
    const auto parsed = ParsePluginConfiguration(
        "[General]\nHdrEnabled=1\n[Output]\nDisplayPeakNits=1000\nUiPaperWhiteNits=203\n"
        "[ToneMapper]\nAlgorithm=AgX\n[Image]\nExposureEV=0\n");
    FO4HDR_CHECK(parsed.valid && parsed.value.productionRequested);
    FO4HDR_CHECK(!parsed.value.developerMode && !parsed.value.traceRenderPipeline);
    FO4HDR_CHECK(!parsed.value.measurePerformance && !parsed.value.developerHdr10Reference);
    FO4HDR_CHECK(!parsed.value.forceHdrSwapChainFailure);
}

// The former AgX name still loads; with both names the current one wins.
void TestFormerAgxColorResponseName()
{
    for (const auto* key : {"ColorResponse", "HdrColorResponse"})
    {
        const auto parsed = ParseWith(std::string("[AgX]\n") + key + "=0.4\n");
        FO4HDR_CHECK(parsed.valid && parsed.value.toneMapperParameters.agxHdrPurity == 0.4);
        FO4HDR_CHECK(HasIniLine(parsed.text, "ColorResponse=0.4"));
        FO4HDR_CHECK(!parsed.text.contains("HdrColorResponse"));
    }
    FO4HDR_CHECK(ExtraRepairs("[AgX]\nHdrColorResponse=0.4\n", renamed) == 1);
    constexpr std::string_view both = "[AgX]\nColorResponse=0.4\nHdrColorResponse=0.5\n";
    FO4HDR_CHECK(ParseWith(both).value.toneMapperParameters.agxHdrPurity == 0.4);
    FO4HDR_CHECK(ExtraRepairs(both, dropped) == 1);
}

// The former Fallback Grade names still load in both profiles; with both names
// the current one wins.
void TestFormerFallbackGradeNames()
{
    constexpr std::string_view formerNames =
        "[Effects]\nVanillaGradeLuminanceStrength=40\nVanillaGradeColorStrength=30\n"
        "[SdrEffects]\nVanillaGradeLuminanceStrength=20\nVanillaGradeColorStrength=10\n";
    const auto former = ParseWith(formerNames);
    FO4HDR_CHECK(former.valid);
    FO4HDR_CHECK(former.value.fallbackGradeLuminancePercent == 40.0);
    FO4HDR_CHECK(former.value.fallbackGradeColorPercent == 30.0);
    FO4HDR_CHECK(former.value.sdrSettings.fallbackGradeLuminancePercent == 20.0);
    FO4HDR_CHECK(former.value.sdrSettings.fallbackGradeColorPercent == 10.0);
    FO4HDR_CHECK(ExtraRepairs(formerNames, renamed) == 4);
    FO4HDR_CHECK(!former.text.contains("VanillaGrade"));
    for (const auto* both : {"FallbackGradeLuminance=40\nVanillaGradeLuminanceStrength=50\n",
             "VanillaGradeColorStrength=50\nFallbackGradeColor=40\n"})
    {
        const auto text = std::string("[Effects]\n") + both;
        const auto parsed = ParseWith(text);
        FO4HDR_CHECK(parsed.value.fallbackGradeLuminancePercent +
            parsed.value.fallbackGradeColorPercent == 40.0 + 67.0);
        FO4HDR_CHECK(ExtraRepairs(text, dropped) == 1);
    }
}

void TestToneMapperConfigurationRanges()
{
    for (const auto parameters : {
             ToneMapperParameters{0.0, -0.5, 2.2, 0.25, 1.75, 1.0, 0.35, 0.0},
             ToneMapperParameters{1.0, 0.5, 3.0, 0.6, 2.25, 1.5, 0.6, 1.0},
             ToneMapperParameters{0.5, 0.4, 2.3, 0.3, 2.2, 1.2, 0.5, 0.9}})
    {
        HdrSettings settings;
        settings.toneMapperParameters = parameters;
        const auto saved = RewriteHdrSettingsIni(
            MinimalProductionConfiguration(), {settings}, {});
        const auto loaded = ParsePluginConfiguration(saved);
        FO4HDR_CHECK(loaded.valid);
        FO4HDR_CHECK(loaded.value.toneMapperParameters == parameters);
        FO4HDR_CHECK(RewriteHdrSettingsIni(saved, {settings}, {}) == saved);
    }
}

void TestConfigurationEndpoints()
{
    for (const auto blacks : {-100.0, 100.0})
    {
        const auto endpoint = ParseWith("\n[Image]\nBlacks=" + std::to_string(blacks) + "\n");
        FO4HDR_CHECK(endpoint.valid);
        FO4HDR_CHECK(endpoint.value.blacks == blacks);
    }
    for (const auto percent : {"0", "200"})
    {
        const auto endpoint = ParseWith(std::string("\n[Image]\nHighlightSaturation=") + percent + "\n");
        FO4HDR_CHECK(endpoint.valid);
        FO4HDR_CHECK(endpoint.value.highlightSaturationPercent == std::stod(percent));
    }
    const auto oversaturated = ParseWith("\n[Image]\nHighlightSaturation=200.01\n");
    FO4HDR_CHECK(oversaturated.valid);
    FO4HDR_CHECK(oversaturated.value.highlightSaturationPercent == kMaximumSaturationPercent);
    FO4HDR_CHECK(ExtraRepairs("\n[Image]\nHighlightSaturation=200.01\n", clamped) == 1);
    for (const auto key : {"FallbackGradeLuminance", "FallbackGradeColor"})
        for (const auto endpointValue : {0.0, 100.0})
        {
            const auto endpoint = ParseWith("\n[Effects]\n" + std::string(key) + "=" +
                std::to_string(endpointValue) + "\n");
            FO4HDR_CHECK(endpoint.valid);
            FO4HDR_CHECK((std::string_view{key} == "FallbackGradeLuminance" ?
                    endpoint.value.fallbackGradeLuminancePercent :
                    endpoint.value.fallbackGradeColorPercent) == endpointValue);
        }

    constexpr std::string_view unknownKeys =
        "\n[Image]\nThirdPartyImageSetting=malformed\n"
        "MidtoneBrightness=malformed\n"
        "[Effects]\nThirdPartyEffectSetting=invalid\n"
        "[Interface]\nThirdPartyInterfaceSetting=nan\n";
    const auto unknown = ParseWith(unknownKeys);
    FO4HDR_CHECK(unknown.valid);
    FO4HDR_CHECK(unknown.value.blacks == 0.0);
    FO4HDR_CHECK(ExtraRepairs(unknownKeys, dropped) == 4);
    for (const auto value : {"Original", "PreserveHDRColor"})
    {
        const auto retired = ParseWith(std::string("\n[Image]\nGradeTransfer=") + value + "\n");
        FO4HDR_CHECK(retired.valid && !retired.text.contains("GradeTransfer"));
    }
}

void TestExtendedConfiguration()
{
    const auto parsed = ParsePluginConfiguration(
        ExtendedProductionConfiguration());
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.colorGradingMethod == ColorGradingMethod::affine);
    FO4HDR_CHECK(parsed.value.colorGradingCurve == ColorGradingCurve::srgb);
    FO4HDR_CHECK(parsed.value.toneMapperParameters.acesLookStrength == 0.25);
    FO4HDR_CHECK(parsed.value.toneMapperParameters.agxMiddleGrayEv == -0.25);
    FO4HDR_CHECK(parsed.value.toneMapperParameters.agxContrast == 2.9);
    FO4HDR_CHECK(parsed.value.toneMapperParameters.agxHdrPurity == 0.6);
    FO4HDR_CHECK(
        parsed.value.toneMapperParameters.agxHdrShoulderPower == 2.1);
    FO4HDR_CHECK(parsed.value.toneMapperParameters.gt7ToeStrength == 1.4);
    FO4HDR_CHECK(parsed.value.toneMapperParameters.gt7ShoulderStart == 0.55);
    FO4HDR_CHECK(parsed.value.toneMapperParameters.gt7ColorBlend == 0.25);
    FO4HDR_CHECK(!parsed.value.developerMode);
    FO4HDR_CHECK(parsed.value.videoWhiteNits == 250.0);
    FO4HDR_CHECK(parsed.value.contrast == -25.0);
    FO4HDR_CHECK(parsed.value.blacks == -40.0);
    FO4HDR_CHECK(parsed.value.highlights == 50.0);
    FO4HDR_CHECK(parsed.value.shadows == -75.0);
    FO4HDR_CHECK(parsed.value.saturationPercent == 125.0);
    FO4HDR_CHECK(parsed.value.highlightSaturationPercent == 40.0);
    FO4HDR_CHECK(parsed.value.temperature == 20.0);
    FO4HDR_CHECK(parsed.value.tint == -30.0);
    FO4HDR_CHECK(
        parsed.value.vanillaColorGradingStrengthPercent == 60.0);
    FO4HDR_CHECK(parsed.value.fallbackGradeLuminancePercent == 75.0);
    FO4HDR_CHECK(parsed.value.fallbackGradeColorPercent == 40.0);
    FO4HDR_CHECK(parsed.value.bloomStrengthPercent == 75.0);
}

// Layout, case and spelling differences do not change the settings; the render
// puts every key back in its canonical form.
void TestConfigurationNormalization()
{
    const auto canonical = ParsePluginConfiguration("").text;
    auto lineFeeds = canonical;
    for (auto position = lineFeeds.find("\r\n"); position != std::string::npos;
         position = lineFeeds.find("\r\n", position))
        lineFeeds.erase(position, 1);
    const auto fromLineFeeds = ParsePluginConfiguration("\xEF\xBB\xBF" + lineFeeds);
    FO4HDR_CHECK(fromLineFeeds.valid && fromLineFeeds.rewriteNeeded);
    FO4HDR_CHECK(fromLineFeeds.text == canonical);
    FO4HDR_CHECK(fromLineFeeds.repairs.counts == RepairCounts{});
    const auto lower = ParsePluginConfiguration(
        "[output]\r\ndisplaypeaknits=1400\r\n[TONEMAPPER]\r\nalgorithm=agx\r\n");
    FO4HDR_CHECK(lower.value.displayPeakNits == 1400.0);
    FO4HDR_CHECK(lower.value.productionAlgorithm == ProductionToneMapperAlgorithm::agX);
    FO4HDR_CHECK(HasIniLine(lower.text, "DisplayPeakNits=1400"));
    FO4HDR_CHECK(HasIniLine(lower.text, "Algorithm=AgX"));

    // Keys before the first section, unknown keys and hand-written comments are dropped.
    auto edited = "Stray=1\r\n# my note\r\n" + canonical;
    Replace(edited, "[Output]\r\n", "[Output]\r\nThirdParty=kept\r\n");
    const auto cleaned = ParsePluginConfiguration(edited);
    FO4HDR_CHECK(cleaned.text == canonical);
    FO4HDR_CHECK(RepairCount(cleaned, dropped) == 2);
    FO4HDR_CHECK(cleaned.repairs.entries.size() == 2);
    FO4HDR_CHECK(!cleaned.repairs.entries.empty() && cleaned.repairs.entries[0].key == "Stray");

    // Switches take words; spaces and a leading plus sign parse.
    auto words = canonical;
    Replace(words, "SdrEnabled=1", "SdrEnabled = off");
    Replace(words, "DisplayPeakNits=1000", "DisplayPeakNits = +1200");
    const auto spelled = ParsePluginConfiguration(words);
    FO4HDR_CHECK(!spelled.value.sdrEnabled && spelled.value.displayPeakNits == 1200.0);
    FO4HDR_CHECK(spelled.repairs.counts == RepairCounts{});
    FO4HDR_CHECK(HasIniLine(spelled.text, "SdrEnabled=0"));
    // A plus sign before a minus sign does not parse.
    auto signs = canonical;
    Replace(signs, "ExposureEV=0\r\n", "ExposureEV=+-1\r\n");
    FO4HDR_CHECK(RepairCount(ParsePluginConfiguration(signs), defaulted) == 1);

    // A channel Maximum below its Minimum rises to it.
    auto channel = canonical;
    Replace(channel, "LuminanceMinimum=0", "LuminanceMinimum=60");
    Replace(channel, "LuminanceMaximum=100", "LuminanceMaximum=40");
    const auto raised = ParsePluginConfiguration(channel);
    FO4HDR_CHECK(raised.value.weatherGradeResponse.luminance.maximum == 60.0);
    FO4HDR_CHECK(RepairCount(raised, clamped) == 1);

    // A diagnostic switch without DeveloperMode keeps its value and has no effect.
    auto diagnostics = canonical;
    Replace(diagnostics, "TraceRenderPipeline=0", "TraceRenderPipeline=1");
    const auto inert = ParsePluginConfiguration(diagnostics);
    FO4HDR_CHECK(!inert.value.traceRenderPipeline && inert.stored.traceRenderPipeline);
    FO4HDR_CHECK(!inert.rewriteNeeded);
    Replace(diagnostics, "DeveloperMode=0", "DeveloperMode=1");
    FO4HDR_CHECK(ParsePluginConfiguration(diagnostics).value.traceRenderPipeline);

    // Numbers keep the 0.001 steps they are saved with.
    auto precise = canonical;
    Replace(precise, "ShoulderStart=0.444", "ShoulderStart=0.4444");
    FO4HDR_CHECK(ParsePluginConfiguration(precise).value.toneMapperParameters.gt7ShoulderStart ==
        0.444);
}

// A missing INI is generated; one that exists but cannot be read is never rewritten.
void TestConfigurationFiles()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("FO4Isotope-config-files-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    FO4HDR_CHECK(std::filesystem::create_directory(root));
    const auto path = root / "FO4Isotope.ini";
    const auto missing = LoadPluginConfiguration(path);
    FO4HDR_CHECK(missing.valid && missing.fileMissing && missing.rewriteNeeded);
    FO4HDR_CHECK(WriteSettingsFile(path, missing.text));
    FO4HDR_CHECK(ReadText(path) == missing.text);
    const auto existing = LoadPluginConfiguration(path);
    FO4HDR_CHECK(existing.valid && !existing.fileMissing && !existing.rewriteNeeded);

    const auto locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FO4HDR_CHECK(locked != INVALID_HANDLE_VALUE);
    const auto unreadable = LoadPluginConfiguration(path);
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    FO4HDR_CHECK(!unreadable.valid && !unreadable.rewriteNeeded);
    FO4HDR_CHECK(unreadable.error == PluginConfigurationError::fileUnavailable);
    FO4HDR_CHECK(ToString(unreadable.error) == "file-unavailable");

    // A read-only INI keeps its bytes, and the failed write is reported.
    FO4HDR_CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
    FO4HDR_CHECK(!WriteSettingsFile(path, "[General]\r\nHdrEnabled=0\r\n"));
    FO4HDR_CHECK(ReadText(path) == missing.text);
    FO4HDR_CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
    std::error_code error;
    FO4HDR_CHECK(std::filesystem::remove_all(root, error) != 0 && !error);
}

void TestConfigurationValues()
{
    TestExpectedDefaultConfiguration();
    TestReleasedConfigurationUpgrade();
    TestSdrProfileConfiguration();
    TestSdrProfileRanges();
    TestSdrProfileAlgorithms();
    TestProductionConfiguration();
    TestMinimalConfiguration();
    TestFormerAgxColorResponseName();
    TestFormerFallbackGradeNames();
    TestOptionalDiagnosticKeys();
    TestToneMapperConfigurationRanges();
    TestConfigurationEndpoints();
    TestExtendedConfiguration();
    TestConfigurationNormalization();
    TestConfigurationFiles();
}

// Tokens match without regard to case; unknown ones take the default.
void TestConfigurationCurveRepairs()
{
    for (const auto [token, curve] : {std::pair{"SRGB", ColorGradingCurve::srgb},
             std::pair{"srgb", ColorGradingCurve::srgb},
             std::pair{"gamma22", ColorGradingCurve::gamma22}})
    {
        const auto parsed = ParseWith(std::string("\n[Image]\nColorGradingCurve=") + token + "\n");
        FO4HDR_CHECK(parsed.valid && parsed.value.colorGradingCurve == curve);
        FO4HDR_CHECK(HasIniLine(parsed.text, "ColorGradingCurve=" + std::string(ToString(curve))));
    }
    for (const auto token : {"Gamma 2.2", "2.2", ""})
    {
        const auto text = std::string("\n[Image]\nColorGradingCurve=") + token + "\n";
        const auto parsed = ParseWith(text);
        FO4HDR_CHECK(parsed.valid && parsed.value.colorGradingCurve == ColorGradingCurve::gamma22);
        FO4HDR_CHECK(parsed.value.productionRequested);
        FO4HDR_CHECK(ExtraRepairs(text, defaulted) == 1);
    }
    const auto repeatedCurve = ParseWith(
        "\n[Image]\nColorGradingCurve=Gamma22\nColorGradingCurve=sRGB\n");
    FO4HDR_CHECK(repeatedCurve.value.colorGradingCurve == ColorGradingCurve::srgb);
}

void TestConfigurationMethodRepairs()
{
    for (const auto token : {"Independent", "Native Reference", "RGB Ratio", "999"})
    {
        const auto text = std::string("\n[Image]\nColorGradingMethod=") + token + "\n";
        const auto parsed = ParseWith(text);
        FO4HDR_CHECK(parsed.valid);
        FO4HDR_CHECK(parsed.value.colorGradingMethod == ColorGradingMethod::rgbRatio);
        FO4HDR_CHECK(ExtraRepairs(text, defaulted) == 1);
    }
    // Former names, in any case, load and are renamed.
    for (const auto [token, method] : {std::pair{"radial", ColorGradingMethod::rgbRatio},
             std::pair{"NativeReference", ColorGradingMethod::affine}})
    {
        const auto text = std::string("\n[Image]\nColorGradingMethod=") + token + "\n";
        FO4HDR_CHECK(ParseWith(text).value.colorGradingMethod == method);
        FO4HDR_CHECK(ExtraRepairs(text, renamed) == 1);
    }
    FO4HDR_CHECK(ParseWith("\n[Image]\nColorGradingMethod=affine\n").value.colorGradingMethod ==
        ColorGradingMethod::affine);
    const auto repeatedMethod = ParseWith(
        "\n[Image]\nColorGradingMethod=RgbRatio\nColorGradingMethod=Affine\n");
    FO4HDR_CHECK(repeatedMethod.value.colorGradingMethod == ColorGradingMethod::affine);
    const auto repeatedParameter = ParseWith("\n[AgX]\nContrast=2.4\nContrast=2.5\n");
    FO4HDR_CHECK(repeatedParameter.value.toneMapperParameters.agxContrast == 2.5);
    const auto distinctContrastKeys = ParseWith(
        "\n[Image]\nContrast=-25\n[AgX]\nContrast=2.75\n");
    FO4HDR_CHECK(distinctContrastKeys.valid);
    FO4HDR_CHECK(distinctContrastKeys.value.contrast == -25.0);
    FO4HDR_CHECK(
        distinctContrastKeys.value.toneMapperParameters.agxContrast == 2.75);
}

// Out-of-range operator values clamp; text that does not parse takes the default.
void TestConfigurationOperatorRepairs()
{
    const auto retiredLookSection = ParseWith("\n[ACES2LMT]\nContrastLookStrength=0.25\n");
    FO4HDR_CHECK(retiredLookSection.valid);
    FO4HDR_CHECK(retiredLookSection.value.toneMapperParameters.acesLookStrength ==
        ToneMapperParameters{}.acesLookStrength);

    for (const auto invalidParameter : {
             "[ACES2]\nContrastLookStrength=-0.01\n",
             "[AgX]\nMiddleGrayOffsetEV=nan\n",
             "[AgX]\nMiddleGrayOffsetEV=0.51\n",
             "[AgX]\nContrast=2.19\n",
             "[AgX]\nContrast=3.51\n",
             "[AgX]\nColorResponse=0.24\n",
             "[AgX]\nColorResponse=1.01\n",
             "[AgX]\nHdrColorResponse=0.24\n",
             "[AgX]\nShoulderPower=0.99\n",
             "[AgX]\nShoulderPower=2.26\n",
             "[GT7]\nToeStrength=2.01\n",
             "[GT7]\nShoulderStart=0.34\n",
             "[GT7]\nColorBlend=-0.01\n",
             "[GT7]\nColorBlend=1.01\n"})
    {
        const auto text = std::string("\n") + invalidParameter;
        const auto repaired = ParseWith(text);
        FO4HDR_CHECK(repaired.valid);
        FO4HDR_CHECK(ValidateHdrSettings(repaired.value) == HdrSettingsValidationError::none);
        const auto notANumber = std::string_view{invalidParameter}.contains("nan");
        FO4HDR_CHECK(ExtraRepairs(text, notANumber ? defaulted : clamped) == 1);
    }
}

void TestConfigurationRangeRepairs()
{
    const auto extended = ParsePluginConfiguration(ExtendedProductionConfiguration());
    for (const auto keyValue : {
             "VideoWhiteNits=99",
             "Contrast=-101",
             "Blacks=101",
             "Blacks=nan",
             "Highlights=101",
             "Shadows=nan",
             "Saturation=201",
             "Temperature=-101",
             "Tint=101",
             "VanillaColorGradingStrength=101",
             "FallbackGradeLuminance=-1",
             "FallbackGradeColor=nan",
             "BloomStrength=-1",
             "BloomStrength=100.01",
             "DisplayPeakNits=1e70"})
    {
        auto text = ExtendedProductionConfiguration();
        const std::string_view replacement{keyValue};
        const auto position = text.find(replacement.substr(0, replacement.find('=')));
        FO4HDR_CHECK(position != std::string::npos);
        const auto end = text.find('\n', position);
        text.replace(position, end - position, keyValue);
        const auto parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(parsed.valid);
        FO4HDR_CHECK(ValidateHdrSettings(parsed.value) == HdrSettingsValidationError::none);
        const auto kind = replacement.ends_with("=nan") ? defaulted : clamped;
        FO4HDR_CHECK(RepairCount(parsed, kind) == RepairCount(extended, kind) + 1);
    }
}

void TestConfigurationRepeatedEffects()
{
    auto text = ExtendedProductionConfiguration();
    text += "\n[Effects]\nBloomStrength=100\n";
    FO4HDR_CHECK(ParsePluginConfiguration(text).value.bloomStrengthPercent == 100.0);

    text = ExtendedProductionConfiguration();
    text += "\n[Effects]\nFallbackGradeColor=50\n";
    FO4HDR_CHECK(ParsePluginConfiguration(text).value.fallbackGradeColorPercent == 50.0);
}

// The released settings-window keys load and are dropped.
void TestRetiredInterfaceKeys()
{
    constexpr std::string_view retired =
        "\n[Interface]\nToggleKey=Win+H\nScale=1.1\nWindowWidth=519\n"
        "WindowHeight=1001\nWindowX=-2\nWindowY=nan\n";
    const auto parsed = ParseWith(retired);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.interfaceSettings == HdrInterfaceSettings{});
    FO4HDR_CHECK(ExtraRepairs(retired, dropped) == 6);
    FO4HDR_CHECK(!parsed.text.contains("[Interface]"));
}

// Every key has a default; only an unreadable INI starts neutral SDR.
void TestFallbackConfiguration()
{
    const auto parsed = ParsePluginConfiguration("[General]\nDeveloperMode=1\n");
    FO4HDR_CHECK(parsed.valid && parsed.value.enabled && parsed.value.developerMode);
    FO4HDR_CHECK(parsed.value.productionRequested);
    const auto fallback = FallbackPluginConfiguration();
    FO4HDR_CHECK(fallback.sdrEnabled && fallback.configurationDefaults);
    FO4HDR_CHECK(!fallback.enabled && !fallback.productionRequested && !fallback.developerMode);
    FO4HDR_CHECK(!fallback.interfaceSettings.hdrEnabled);
    FO4HDR_CHECK(fallback.sdrSettings == DefaultSdrSettings());
}

void TestConfigurationRepairs()
{
    TestFallbackConfiguration();
    TestConfigurationMethodRepairs();
    TestConfigurationCurveRepairs();
    TestConfigurationOperatorRepairs();
    TestConfigurationRangeRepairs();
    TestConfigurationRepeatedEffects();
    TestRetiredInterfaceKeys();
}

}  // namespace fo4hdr::tests
