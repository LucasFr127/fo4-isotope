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

}  // namespace

void RunHdrConfigurationTests()
{
    CheckParser();
}

}  // namespace fo4hdr::tests
