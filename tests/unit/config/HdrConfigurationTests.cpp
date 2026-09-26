// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "SettingsPersistence.hpp"
#include "TestHarness.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace fo4hdr::tests {

void TestShippedConfiguration()
{
    const auto shipped = LoadPluginConfiguration(
        std::filesystem::path{"config/Fallout4HDR.ini"});
    FO4HDR_CHECK(shipped.valid);
    FO4HDR_CHECK(shipped.profileIssues.empty());
    FO4HDR_CHECK(shipped.distributedProfiles.count > 0);
    FO4HDR_CHECK(shipped.value.productionAlgorithm ==
        ProductionToneMapperAlgorithm::aces2Lmt);
    FO4HDR_CHECK(shipped.value.toneMapperParameters ==
        ToneMapperParameters{});
    FO4HDR_CHECK(shipped.value.toneMapperParameters.agxContrast == 2.7);
    FO4HDR_CHECK(shipped.value.colorGradingMethod == ColorGradingMethod::radial);
    FO4HDR_CHECK(shipped.value.exposureEv == 0.0);
    FO4HDR_CHECK(shipped.value.displayPeakNits == 1000.0);
    FO4HDR_CHECK(shipped.value.uiPaperWhiteNits == 203.0);
    FO4HDR_CHECK(shipped.value.videoWhiteNits == 203.0);
    FO4HDR_CHECK(shipped.value.contrast == 0.0);
    FO4HDR_CHECK(shipped.value.blacks == 0.0);
    FO4HDR_CHECK(shipped.value.highlights == 0.0);
    FO4HDR_CHECK(shipped.value.shadows == 0.0);
    FO4HDR_CHECK(shipped.value.saturationPercent == 100.0);
    FO4HDR_CHECK(shipped.value.temperature == 0.0);
    FO4HDR_CHECK(shipped.value.tint == 0.0);
    FO4HDR_CHECK(!shipped.value.selectiveColor.enabled);
    FO4HDR_CHECK(shipped.value.selectiveColor.amountPercent == 100.0);
    FO4HDR_CHECK(!IsSelectiveColorEffective(shipped.value.selectiveColor));
    FO4HDR_CHECK(
        shipped.value.vanillaColorGradingStrengthPercent == 100.0);
    FO4HDR_CHECK(shipped.value.vanillaGradeLuminancePercent == 100.0);
    FO4HDR_CHECK(shipped.value.vanillaGradeColorPercent == 100.0);
    FO4HDR_CHECK(shipped.value.bloomStrengthPercent == 100.0);
    FO4HDR_CHECK(shipped.value.interfaceSettings.toggleKey ==
        HdrSettingsHotkey{});
    FO4HDR_CHECK(shipped.value.interfaceSettings.scale == 1.25);
    FO4HDR_CHECK(shipped.value.interfaceSettings.windowWidth == 700.0);
    FO4HDR_CHECK(shipped.value.interfaceSettings.windowHeight == 680.0);
    FO4HDR_CHECK(shipped.value.interfaceSettings.windowX == -1.0);
    FO4HDR_CHECK(shipped.value.interfaceSettings.windowY == -1.0);
    FO4HDR_CHECK(!shipped.value.developerMode);
    FO4HDR_CHECK(!shipped.value.traceRenderPipeline);
    FO4HDR_CHECK(!shipped.value.measurePerformance);
    FO4HDR_CHECK(!shipped.value.developerHdr10Reference);
    FO4HDR_CHECK(!shipped.value.showDeveloperOverlay);
    FO4HDR_CHECK(!shipped.value.forceHdrSwapChainFailure);
}

void TestLegacyRoundTrip(const PluginConfigurationResult& parsed)
{
    auto sourceContrastSettings = static_cast<HdrSettings>(parsed.value);
    sourceContrastSettings.toneMapperParameters.agxContrast = 2.4;
    const auto sourceContrastSaved = RewriteHdrSettingsIni(
        LegacyProductionConfiguration(), sourceContrastSettings, {});
    FO4HDR_CHECK(sourceContrastSaved.find("Contrast=2.4\n") !=
        std::string::npos);
    const auto sourceContrastReloaded =
        ParsePluginConfiguration(sourceContrastSaved);
    FO4HDR_CHECK(sourceContrastReloaded.valid);
    FO4HDR_CHECK(
        sourceContrastReloaded.value.toneMapperParameters.agxContrast == 2.4);
    FO4HDR_CHECK(parsed.value.colorGradingMethod == ColorGradingMethod::radial);
    for (const auto method : {ColorGradingMethod::radial,
             ColorGradingMethod::nativeReference})
    {
        auto settings = static_cast<HdrSettings>(parsed.value);
        settings.colorGradingMethod = method;
        const auto saved = RewriteHdrSettingsIni(
            LegacyProductionConfiguration(), settings, {});
        const auto loaded = ParsePluginConfiguration(saved);
        FO4HDR_CHECK(loaded.valid);
        FO4HDR_CHECK(loaded.value.colorGradingMethod == method);
        FO4HDR_CHECK(RewriteHdrSettingsIni(saved, settings, {}) == saved);
        settings.colorGradingMethod = HdrSettings{}.colorGradingMethod;
        const auto reset = ParsePluginConfiguration(
            RewriteHdrSettingsIni(saved, settings, {}));
        FO4HDR_CHECK(reset.valid);
        FO4HDR_CHECK(reset.value.colorGradingMethod == ColorGradingMethod::radial);
    }
}

void TestLegacyConfiguration()
{
    auto parsed = ParsePluginConfiguration(
        LegacyProductionConfiguration());
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.toneMapperParameters ==
        ToneMapperParameters{});
    FO4HDR_CHECK(parsed.value.toneMapperParameters.agxContrast == 2.7);
    TestLegacyRoundTrip(parsed);
    FO4HDR_CHECK(parsed.value.videoWhiteNits == 203.0);
    FO4HDR_CHECK(parsed.value.contrast == 0.0);
    FO4HDR_CHECK(parsed.value.blacks == 0.0);
    FO4HDR_CHECK(parsed.value.highlights == 0.0);
    FO4HDR_CHECK(parsed.value.shadows == 0.0);
    FO4HDR_CHECK(parsed.value.saturationPercent == 100.0);
    FO4HDR_CHECK(parsed.value.temperature == 0.0);
    FO4HDR_CHECK(parsed.value.tint == 0.0);
    FO4HDR_CHECK(
        parsed.value.vanillaColorGradingStrengthPercent == 100.0);
    FO4HDR_CHECK(parsed.value.vanillaGradeLuminancePercent == 100.0);
    FO4HDR_CHECK(parsed.value.vanillaGradeColorPercent == 100.0);
    FO4HDR_CHECK(parsed.value.bloomStrengthPercent == 100.0);
    FO4HDR_CHECK(parsed.value.interfaceSettings.toggleKey.virtualKey ==
        kDefaultSettingsToggleVirtualKey);
    FO4HDR_CHECK(parsed.value.interfaceSettings.toggleKey.modifiers ==
        kSettingsHotkeyAlt);
    FO4HDR_CHECK(parsed.value.interfaceSettings.scale == 1.25);
    FO4HDR_CHECK(parsed.value.interfaceSettings.windowWidth == 700.0);
    FO4HDR_CHECK(parsed.value.interfaceSettings.windowHeight == 680.0);
}

void TestConfigurationEndpoints()
{
    for (const auto blacks : {-100.0, 100.0})
    {
        const auto endpoint = ParsePluginConfiguration(
            LegacyProductionConfiguration() +
            "\n[Image]\nBlacks=" + std::to_string(blacks) + "\n");
        FO4HDR_CHECK(endpoint.valid);
        FO4HDR_CHECK(endpoint.value.blacks == blacks);
    }
    for (const auto key : {"VanillaGradeLuminance", "VanillaGradeColor"})
        for (const auto endpointValue : {0.0, 100.0})
        {
            const auto endpoint = ParsePluginConfiguration(
                LegacyProductionConfiguration() +
                "\n[Effects]\n" + key + "=" +
                std::to_string(endpointValue) + "\n");
            FO4HDR_CHECK(endpoint.valid);
            FO4HDR_CHECK((std::string_view{key} == "VanillaGradeLuminance" ?
                    endpoint.value.vanillaGradeLuminancePercent :
                    endpoint.value.vanillaGradeColorPercent) == endpointValue);
        }

    const auto unknown = ParsePluginConfiguration(
        LegacyProductionConfiguration() +
        "\n[Image]\nThirdPartyImageSetting=malformed\n"
        "MidtoneBrightness=malformed\n"
        "[Effects]\nThirdPartyEffectSetting=invalid\n"
        "[Interface]\nThirdPartyInterfaceSetting=nan\n");
    FO4HDR_CHECK(unknown.valid);
    FO4HDR_CHECK(unknown.value.blacks == 0.0);
    for (const auto value : {"Original", "PreserveHDRColor"})
        FO4HDR_CHECK(ParsePluginConfiguration(
            LegacyProductionConfiguration() +
            "\n[Image]\nGradeTransfer=" + value + "\n").valid);
}

void TestExtendedConfiguration()
{
    PluginConfigurationResult parsed;
    parsed = ParsePluginConfiguration(
        ExtendedProductionConfiguration());
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.colorGradingMethod == ColorGradingMethod::nativeReference);
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
    FO4HDR_CHECK(parsed.value.temperature == 20.0);
    FO4HDR_CHECK(parsed.value.tint == -30.0);
    FO4HDR_CHECK(
        parsed.value.vanillaColorGradingStrengthPercent == 60.0);
    FO4HDR_CHECK(parsed.value.vanillaGradeLuminancePercent == 75.0);
    FO4HDR_CHECK(parsed.value.vanillaGradeColorPercent == 40.0);
    FO4HDR_CHECK(parsed.value.bloomStrengthPercent == 175.0);
    FO4HDR_CHECK(parsed.value.interfaceSettings.toggleKey.virtualKey == 0x7BU);
    FO4HDR_CHECK(parsed.value.interfaceSettings.toggleKey.modifiers == 0U);
    FO4HDR_CHECK(parsed.value.interfaceSettings.scale == 1.5);
    FO4HDR_CHECK(parsed.value.interfaceSettings.windowWidth == 900.0);
    FO4HDR_CHECK(parsed.value.interfaceSettings.windowHeight == 760.0);
}

void TestOptionalConfigurationValues()
{
    TestShippedConfiguration();
    TestLegacyConfiguration();
    TestConfigurationEndpoints();
    TestExtendedConfiguration();
}

void TestConfigurationMethodErrors()
{
    for (const auto token : {"Independent", "Native Reference", "radial", "999"})
    {
        const auto parsed = ParsePluginConfiguration(
            LegacyProductionConfiguration() +
            "\n[Image]\nColorGradingMethod=" + token + "\n");
        FO4HDR_CHECK(!parsed.valid);
        FO4HDR_CHECK(parsed.error ==
            PluginConfigurationError::unsupportedColorGradingMethod);
        FO4HDR_CHECK(parsed.value.colorGradingMethod == ColorGradingMethod::radial);
        FO4HDR_CHECK(!parsed.value.productionRequested);
    }
    const auto duplicateMethod = ParsePluginConfiguration(
        LegacyProductionConfiguration() +
        "\n[Image]\nColorGradingMethod=Radial\n"
        "ColorGradingMethod=NativeReference\n");
    FO4HDR_CHECK(!duplicateMethod.valid);
    FO4HDR_CHECK(duplicateMethod.error == PluginConfigurationError::duplicateKey);
    const auto duplicateParameter = ParsePluginConfiguration(
        LegacyProductionConfiguration() +
        "\n[AgX]\nContrast=2.4\nContrast=2.5\n");
    FO4HDR_CHECK(!duplicateParameter.valid);
    FO4HDR_CHECK(duplicateParameter.error ==
        PluginConfigurationError::duplicateKey);
    const auto distinctContrastKeys = ParsePluginConfiguration(
        LegacyProductionConfiguration() +
        "\n[Image]\nContrast=-25\n[AgX]\nContrast=2.75\n");
    FO4HDR_CHECK(distinctContrastKeys.valid);
    FO4HDR_CHECK(distinctContrastKeys.value.contrast == -25.0);
    FO4HDR_CHECK(
        distinctContrastKeys.value.toneMapperParameters.agxContrast == 2.75);
}

void TestConfigurationOperatorErrors()
{
    for (const auto invalidParameter : {
             "[ACES2LMT]\nLookStrength=-0.01\n",
             "[AgX]\nMiddleGray=nan\n",
             "[AgX]\nMiddleGray=0.5\n",
             "[AgX]\nContrast=2.3\n",
             "[AgX]\nContrast=3.51\n",
             "[AgX]\nHdrPurity=0.25\n",
             "[AgX]\nHdrPurity=1.01\n",
             "[AgX]\nShoulderPower=0.99\n",
             "[AgX]\nShoulderPower=2.2\n",
             "[GT7]\nToeStrength=2.01\n",
             "[GT7]\nShoulderStart=0.34\n",
             "[GT7]\nColorBlend=-0.01\n",
             "[GT7]\nColorBlend=0.9\n"})
    {
        const auto invalid = ParsePluginConfiguration(
            LegacyProductionConfiguration() + "\n" + invalidParameter);
        FO4HDR_CHECK(!invalid.valid);
        FO4HDR_CHECK(invalid.error ==
            (std::string_view{invalidParameter}.find("nan") !=
                    std::string_view::npos ?
                PluginConfigurationError::invalidNumber :
                PluginConfigurationError::valueOutOfRange));
    }
}

void TestConfigurationRangeErrors()
{
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
             "VanillaGradeLuminance=-1",
             "VanillaGradeColor=nan",
             "BloomStrength=-1",
             "Scale=1.1",
             "WindowWidth=519",
             "WindowHeight=1001"})
    {
        auto text = ExtendedProductionConfiguration();
        const std::string_view replacement{keyValue};
        const auto position = text.find(replacement.substr(
            0, replacement.find('=')));
        FO4HDR_CHECK(position != std::string::npos);
        const auto end = text.find('\n', position);
        text.replace(position, end - position, keyValue);
        const auto parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(!parsed.valid);
        const auto replacementValue = replacement.substr(replacement.find('=') + 1U);
        FO4HDR_CHECK(parsed.error ==
            (replacementValue == "nan" || replacementValue == "inf" ?
                PluginConfigurationError::invalidNumber :
                PluginConfigurationError::valueOutOfRange));
    }
}

void TestConfigurationDuplicateEffects()
{
    auto text = ExtendedProductionConfiguration();
    text += "\n[Effects]\nBloomStrength=100\n";
    auto parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(parsed.error ==
        PluginConfigurationError::duplicateKey);

    text = ExtendedProductionConfiguration();
    text += "\n[Effects]\nVanillaGradeColor=50\n";
    parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(parsed.error == PluginConfigurationError::duplicateKey);
}

void TestConfigurationToggleKeys()
{
    std::string text;
    PluginConfigurationResult parsed;
    for (const auto toggle : {"F1", "F10", "F24", "Alt+H", "Space",
             "Ctrl+Shift+H", "Ctrl+Alt+Shift+Numpad0"})
    {
        text = LegacyProductionConfiguration() +
            "\n[Interface]\nToggleKey=" + toggle + "\n";
        FO4HDR_CHECK(ParsePluginConfiguration(text).valid);
    }
    for (const auto toggle : {"F0", "F25", "f10", "F10junk", "Alt",
             "Alt+", "Ctrl+Ctrl+H", "Win+H", "Alt+F4", "Alt+Tab",
             "Ctrl+Alt+Delete", "Escape", "Ctrl+Escape", "Alt+Enter"})
    {
        text = LegacyProductionConfiguration() +
            "\n[Interface]\nToggleKey=" + toggle + "\n";
        parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(!parsed.valid);
        FO4HDR_CHECK(parsed.error ==
            PluginConfigurationError::unsupportedToggleKey);
    }
}

void TestOptionalConfigurationFailures()
{
    TestConfigurationMethodErrors();
    TestConfigurationOperatorErrors();
    TestConfigurationRangeErrors();
    TestConfigurationDuplicateEffects();
    TestConfigurationToggleKeys();
}

}  // namespace fo4hdr::tests
