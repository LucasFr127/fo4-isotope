// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "config/SettingsPersistence.hpp"
#include "HdrSettingsTestSupport.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <string>

namespace fo4hdr::tests {
namespace {

void CheckParser()
{
    const auto original = ParsePluginConfiguration(kOriginal);
    FO4HDR_CHECK(original.valid == true);
    FO4HDR_CHECK(original.value.productionAlgorithm == ProductionToneMapperAlgorithm::agX);
    auto requested = original.value;
    requested.productionAlgorithm = ProductionToneMapperAlgorithm::gt7;
    requested.displayPeakNits = 1500.0;
    requested.uiPaperWhiteNits = 100.0;
    requested.videoWhiteNits = 300.0;
    requested.vanillaColorGradingStrengthPercent = 50.0;
    requested.exposureEv = 1;
    requested.bloomStrengthPercent = 50;
    requested.contrast = 25;
    requested.blacks = -20;
    requested.highlights = 30;
    requested.shadows = -40;
    requested.saturationPercent = 120;
    requested.temperature = 15;
    requested.tint = -10;
    const auto rewritten = RewriteHdrSettingsIni(kOriginal, requested);
    const auto parsed = ParsePluginConfiguration(rewritten);
    FO4HDR_CHECK(parsed.valid == true);
    FO4HDR_CHECK(static_cast<const HdrSettings&>(parsed.value) == static_cast<const HdrSettings&>(requested));
    const auto finalCr = RewriteHdrSettingsIni(
        std::string(kOriginal) + "Keep=hello\r", requested);
    FO4HDR_CHECK(finalCr.ends_with("Keep=hello\r"));
    FO4HDR_CHECK(ParsePluginConfiguration("[ToneMapper]\nAlgorithm=unknown\n").valid ==
        false);
    FO4HDR_CHECK(ParsePluginConfiguration(std::string(kOriginal) +
        "[Output]\nDisplayPeakNits=1000\n").valid == false);
    requested.uiPaperWhiteNits = 99.0;
    FO4HDR_CHECK(!(fo4hdr::ValidateHdrSettings(requested) == fo4hdr::HdrSettingsValidationError::none));
    FO4HDR_CHECK(RewriteHdrSettingsIni(kOriginal, requested) == kOriginal);
}

void CheckColdSelection()
{
    const auto original = ParsePluginConfiguration(kOriginal);
    FO4HDR_CHECK(!original.value.measurePerformance && original.value.enabled);
    const auto text = std::string(kOriginal) +
        "[General]\nEnabled=1\nDeveloperMode=1\n[Debug]\nTraceRenderPipeline=1\n"
        "MeasurePerformance=1\nDeveloperHdr10Reference=1\nShowDeveloperOverlay=1\n"
        "ForceHdrSwapChainFailure=1\nKeep=note\n";
    const auto parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(parsed.valid == true);
    FO4HDR_CHECK(parsed.value.measurePerformance && parsed.value.enabled && parsed.value.developerMode);
    FO4HDR_CHECK(parsed.value.traceRenderPipeline && parsed.value.developerHdr10Reference);
    FO4HDR_CHECK(parsed.value.showDeveloperOverlay && parsed.value.forceHdrSwapChainFailure);
    auto requested = parsed.value;
    requested.uiPaperWhiteNits = 250;
    const auto rewritten = RewriteHdrSettingsIni(text, requested);
    FO4HDR_CHECK(rewritten.find("MeasurePerformance=1") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("Keep=note") != std::string::npos);
    FO4HDR_CHECK(ParsePluginConfiguration(rewritten).value == requested);
    FO4HDR_CHECK(ParsePluginConfiguration(std::string(kOriginal) +
        "[Debug]\nMeasurePerformance=2\n").valid == false);
    FO4HDR_CHECK(!ParsePluginConfiguration(std::string(kOriginal) +
        "[Debug]\nMeasurePerformance=1\n").valid);
    FO4HDR_CHECK(!ParsePluginConfiguration(
        "[General]\nEnabled=0\nDeveloperMode=1\n[Debug]\nShowDeveloperOverlay=1\n").valid);
}

}  // namespace

void RunHdrConfigurationTests()
{
    CheckParser();
    CheckColdSelection();
}

}  // namespace fo4hdr::tests
