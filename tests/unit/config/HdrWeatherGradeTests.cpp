// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "SettingsPersistence.hpp"
#include "TestHarness.hpp"
#include <array>
#include <string>
#include <string_view>

namespace fo4hdr::tests {

void TestGradePathNormalization()
{
    std::array<char, kMaximumLutGradePathLength + 1> normalized{};
    FO4HDR_CHECK(NormalizeLutGradePath(
        "Textures\\Effects/LUT_\xC3\x84.DDS", normalized));
    FO4HDR_CHECK(std::string_view(normalized.data()) ==
        "textures/effects/lut_\xC3\x84.dds");
    FO4HDR_CHECK(!NormalizeLutGradePath(" leading.dds", normalized));
    FO4HDR_CHECK(!NormalizeLutGradePath("trailing.dds ", normalized));
    FO4HDR_CHECK(!NormalizeLutGradePath("bad[path.dds", normalized));
    FO4HDR_CHECK(!NormalizeLutGradePath("bad]path.dds", normalized));
    FO4HDR_CHECK(!NormalizeLutGradePath("\xC0\xAF", normalized));
    FO4HDR_CHECK(!NormalizeLutGradePath("\xE2\x82", normalized));
    FO4HDR_CHECK(!NormalizeLutGradePath(
        std::string(kMaximumLutGradePathLength + 1, 'a'), normalized));

    FO4HDR_CHECK(NormalizeImageSpaceGradeKey(
        "Fallout4.ESM|00AbCd", normalized));
    FO4HDR_CHECK(std::string_view(normalized.data()) ==
        "fallout4.esm|00abcd");
    for (const auto invalid : {
             "fallout4.esm|000000",
             "fallout4.esm|00001",
             "fallout4.esm|1000000",
             "fallout4.txt|000001",
             "bad/name.esm|000001",
             "bad:name.esm|000001",
             "bad?.esm|000001",
             "fallout4.esm|00000g",
             "fallout4.esm|000001|000002",
             "fallout4.esm |000001"})
        FO4HDR_CHECK(!NormalizeImageSpaceGradeKey(invalid, normalized));
    FO4HDR_CHECK(!NormalizeImageSpaceGradeKey(
        "bad\xC0\xAF.esm|000001", normalized));
}

void TestNeutralWeatherGradeConfiguration()
{
    for (const auto reference : {"0", "1"})
    {
        const auto text = std::string{"[General]\nDeveloperMode=1\n"
            "[Debug]\nTraceRenderPipeline=1\nShowDeveloperOverlay=0\n"
            "ForceHdrSwapChainFailure=0\nDeveloperHdr10Reference="} + reference + "\n";
        const auto parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(parsed.valid);
        FO4HDR_CHECK(!parsed.value.productionRequested);
        FO4HDR_CHECK(!parsed.value.weatherGradeResponse.enabled);
    }
}

void TestWeatherGradeConfiguration()
{
    TestGradePathNormalization();
    TestNeutralWeatherGradeConfiguration();
    FO4HDR_CHECK(!PluginConfiguration{}.weatherGradeResponse.enabled);
    auto minimal = MinimalProductionConfiguration();
    auto parsed = ParsePluginConfiguration(minimal);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.weatherGradeResponse == WeatherGradeResponseSettings{});
    const auto legacy = "\n[Effects]\nGradeProfilesEnabled=0\n"
        "[LUT:test.dds]\nColor=25\n[IMGS:Fallout4.esm|000001]\nInherit=1\n";
    parsed = ParsePluginConfiguration(minimal + legacy);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.weatherGradeResponse == WeatherGradeResponseSettings{});
    parsed = ParsePluginConfiguration(minimal +
        "\n[WeatherLutResponse]\nEnabled=0\nLuminanceGain=0.25\n"
        "ColorSunlight=-17.123456789012345\n");
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(!parsed.value.weatherGradeResponse.enabled);
    FO4HDR_CHECK(parsed.value.weatherGradeResponse.luminance.gain == 0.25);
    FO4HDR_CHECK(parsed.value.weatherGradeResponse.color.coefficients[0] ==
        -17.123456789012345);
    const auto saved = RewriteHdrSettingsIni(minimal + legacy, parsed.value, {});
    const auto reloaded = ParsePluginConfiguration(saved);
    FO4HDR_CHECK(reloaded.valid);
    FO4HDR_CHECK(reloaded.value.weatherGradeResponse == parsed.value.weatherGradeResponse);
    FO4HDR_CHECK(saved.find("GradeProfilesEnabled") == std::string::npos);
    FO4HDR_CHECK(saved.find("[LUT:test.dds]\nColor=25\n") != std::string::npos);
    FO4HDR_CHECK(RewriteHdrSettingsIni(saved, parsed.value, {}) == saved);
    for (const auto invalid : {
             "Enabled=2", "ColorGain=-0.01", "LuminanceGain=2.01",
             "LuminanceBase=101", "ColorMinimum=70\nColorMaximum=60",
             "ColorSunlight=nan", "ColorColorChange=1001",
             "LuminanceIntercept=-1001", "Enabled=0\nEnabled=1"})
    {
        const auto rejected = ParsePluginConfiguration(
            minimal + "\n[WeatherLutResponse]\n" + invalid + "\n");
        FO4HDR_CHECK(!rejected.valid);
    }
}

}  // namespace fo4hdr::tests
