// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "HdrSettingsTestSupport.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <string>

namespace fo4hdr::tests {
namespace {

void CheckParser()
{
    const auto original = ParsePluginConfiguration(kOriginal);
    FO4HDR_CHECK(original.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(original.value.algorithm == OperatorLutAlgorithm::agX);
    auto requested = original.value;
    requested.algorithm = OperatorLutAlgorithm::gt7;
    requested.display = {1500.0, 100.0, 300.0};
    requested.display.vanillaColorGradingStrengthPercent = 50.0;
    requested.display.exposureEv = 1;
    requested.display.bloomStrengthPercent = 50;
    requested.display.contrast = 25;
    requested.display.blacks = -20;
    requested.display.highlights = 30;
    requested.display.shadows = -40;
    requested.display.saturationPercent = 120;
    requested.display.temperature = 15;
    requested.display.tint = -10;
    const auto rewritten = RewritePluginConfigurationIni(kOriginal, requested);
    const auto parsed = ParsePluginConfiguration(rewritten);
    FO4HDR_CHECK(parsed.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(parsed.value == requested);
    const auto finalCr = RewritePluginConfigurationIni(
        std::string(kOriginal) + "Keep=hello\r", requested);
    FO4HDR_CHECK(finalCr.ends_with("Keep=hello\r"));
    FO4HDR_CHECK(ParsePluginConfiguration("[ToneMapper]\nAlgorithm=unknown\n").status ==
        PluginConfigurationStatus::invalid);
    FO4HDR_CHECK(ParsePluginConfiguration(std::string(kOriginal) +
        "[Output]\nDisplayPeakNits=1000\n").status == PluginConfigurationStatus::invalid);
    requested.display.uiPaperWhiteNits = 99.0;
    FO4HDR_CHECK(!ValidatePluginConfiguration(requested));
    FO4HDR_CHECK(RewritePluginConfigurationIni(kOriginal, requested) == kOriginal);
}

void CheckColdSelection()
{
    const auto original = ParsePluginConfiguration(kOriginal);
    FO4HDR_CHECK(!original.value.measurePerformance && !original.value.enabled);
    const auto text = std::string(kOriginal) +
        "[General]\nEnabled=1\nDeveloperMode=1\n[Debug]\nTraceRenderPipeline=1\n"
        "MeasurePerformance=1\nDeveloperHdr10Reference=1\nShowDeveloperOverlay=1\n"
        "ForceHdrSwapChainFailure=1\nKeep=note\n";
    const auto parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(parsed.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(parsed.value.measurePerformance && parsed.value.enabled && parsed.value.developerMode);
    FO4HDR_CHECK(parsed.value.traceRenderPipeline && parsed.value.developerHdr10Reference);
    FO4HDR_CHECK(parsed.value.showDeveloperOverlay && parsed.value.forceHdrSwapChainFailure);
    auto requested = parsed.value;
    requested.display.uiPaperWhiteNits = 250;
    const auto rewritten = RewritePluginConfigurationIni(text, requested);
    FO4HDR_CHECK(rewritten.find("MeasurePerformance=1") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("Keep=note") != std::string::npos);
    FO4HDR_CHECK(ParsePluginConfiguration(rewritten).value == requested);
    FO4HDR_CHECK(ParsePluginConfiguration(std::string(kOriginal) +
        "[Debug]\nMeasurePerformance=2\n").status == PluginConfigurationStatus::invalid);
    requested.developerMode = false;
    FO4HDR_CHECK(!ValidatePluginConfiguration(requested));
    requested = original.value;
    requested.developerMode = true;
    requested.showDeveloperOverlay = true;
    FO4HDR_CHECK(!ValidatePluginConfiguration(requested));
}

}  // namespace

void RunHdrConfigurationTests()
{
    CheckParser();
    CheckColdSelection();
}

}  // namespace fo4hdr::tests
