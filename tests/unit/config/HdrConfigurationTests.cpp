// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "SettingsPersistence.hpp"
#include "TestHarness.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace fo4hdr::tests {

void TestShippedConfiguration()
{
    const auto shipped = LoadPluginConfiguration(
        std::filesystem::path{"config/FO4Isotope.ini"});
    FO4HDR_CHECK(shipped.valid);
    FO4HDR_CHECK(shipped.value.enabled && shipped.value.productionRequested);
    FO4HDR_CHECK(shipped.value.weatherGradeResponse == WeatherGradeResponseSettings{});
    FO4HDR_CHECK(shipped.value.productionAlgorithm ==
        ProductionToneMapperAlgorithm::gt7);
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
    FO4HDR_CHECK(shipped.value.vanillaGradeLuminancePercent == 67.0);
    FO4HDR_CHECK(shipped.value.vanillaGradeColorPercent == 67.0);
    FO4HDR_CHECK(shipped.value.bloomStrengthPercent == 50.0);
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

void TestProductionConfiguration()
{
    auto text = MinimalProductionConfiguration();
    const auto production = ParsePluginConfiguration(text);
    FO4HDR_CHECK(production.valid);
    FO4HDR_CHECK(production.value.enabled && production.value.productionRequested);
    FO4HDR_CHECK(!production.value.developerMode);

    const auto hdr = text.find("HdrEnabled=1");
    text.replace(hdr, 12, "HdrEnabled=0");
    const auto disabled = ParsePluginConfiguration(text);
    FO4HDR_CHECK(disabled.valid);
    FO4HDR_CHECK(!disabled.value.enabled && !disabled.value.productionRequested);

    text.replace(hdr, 12, "HdrEnabled=1");
    const auto developer = text.find("DeveloperMode=0");
    text.replace(developer, 15, "DeveloperMode=1");
    const auto reference = text.find("DeveloperHdr10Reference=0");
    text.replace(reference, 25, "DeveloperHdr10Reference=1");
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
        MinimalProductionConfiguration(), sourceContrastSettings, {});
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
            MinimalProductionConfiguration(), settings, {});
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

void TestMinimalConfiguration()
{
    auto parsed = ParsePluginConfiguration(
        MinimalProductionConfiguration());
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.toneMapperParameters ==
        ToneMapperParameters{});
    FO4HDR_CHECK(parsed.value.toneMapperParameters.agxContrast == 2.7);
    TestDefaultRoundTrip(parsed);
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
    FO4HDR_CHECK(parsed.value.vanillaGradeLuminancePercent == 67.0);
    FO4HDR_CHECK(parsed.value.vanillaGradeColorPercent == 67.0);
    FO4HDR_CHECK(parsed.value.bloomStrengthPercent == 50.0);
    FO4HDR_CHECK(parsed.value.interfaceSettings.toggleKey.virtualKey ==
        kDefaultSettingsToggleVirtualKey);
    FO4HDR_CHECK(parsed.value.interfaceSettings.toggleKey.modifiers ==
        kSettingsHotkeyAlt);
    FO4HDR_CHECK(parsed.value.interfaceSettings.scale == 1.25);
    FO4HDR_CHECK(parsed.value.interfaceSettings.windowWidth == 700.0);
    FO4HDR_CHECK(parsed.value.interfaceSettings.windowHeight == 680.0);
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
            MinimalProductionConfiguration(), settings, {});
        const auto loaded = ParsePluginConfiguration(saved);
        FO4HDR_CHECK(loaded.valid);
        FO4HDR_CHECK(loaded.value.toneMapperParameters == parameters);
        FO4HDR_CHECK(RewriteHdrSettingsIni(saved, settings, {}) == saved);
    }
}

void TestConfigurationEndpoints()
{
    for (const auto blacks : {-100.0, 100.0})
    {
        const auto endpoint = ParsePluginConfiguration(
            MinimalProductionConfiguration() +
            "\n[Image]\nBlacks=" + std::to_string(blacks) + "\n");
        FO4HDR_CHECK(endpoint.valid);
        FO4HDR_CHECK(endpoint.value.blacks == blacks);
    }
    for (const auto key : {"VanillaGradeLuminanceStrength", "VanillaGradeColorStrength"})
        for (const auto endpointValue : {0.0, 100.0})
        {
            const auto endpoint = ParsePluginConfiguration(
                MinimalProductionConfiguration() +
                "\n[Effects]\n" + key + "=" +
                std::to_string(endpointValue) + "\n");
            FO4HDR_CHECK(endpoint.valid);
            FO4HDR_CHECK((std::string_view{key} == "VanillaGradeLuminanceStrength" ?
                    endpoint.value.vanillaGradeLuminancePercent :
                    endpoint.value.vanillaGradeColorPercent) == endpointValue);
        }

    const auto unknown = ParsePluginConfiguration(
        MinimalProductionConfiguration() +
        "\n[Image]\nThirdPartyImageSetting=malformed\n"
        "MidtoneBrightness=malformed\n"
        "[Effects]\nThirdPartyEffectSetting=invalid\n"
        "[Interface]\nThirdPartyInterfaceSetting=nan\n");
    FO4HDR_CHECK(unknown.valid);
    FO4HDR_CHECK(unknown.value.blacks == 0.0);
    for (const auto value : {"Original", "PreserveHDRColor"})
        FO4HDR_CHECK(ParsePluginConfiguration(
            MinimalProductionConfiguration() +
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
    FO4HDR_CHECK(parsed.value.bloomStrengthPercent == 75.0);
    FO4HDR_CHECK(parsed.value.interfaceSettings.toggleKey.virtualKey == 0x7BU);
    FO4HDR_CHECK(parsed.value.interfaceSettings.toggleKey.modifiers == 0U);
    FO4HDR_CHECK(parsed.value.interfaceSettings.scale == 1.5);
    FO4HDR_CHECK(parsed.value.interfaceSettings.windowWidth == 900.0);
    FO4HDR_CHECK(parsed.value.interfaceSettings.windowHeight == 760.0);
}

void TestOptionalConfigurationValues()
{
    TestShippedConfiguration();
    TestProductionConfiguration();
    TestMinimalConfiguration();
    TestToneMapperConfigurationRanges();
    TestConfigurationEndpoints();
    TestExtendedConfiguration();
}

void TestConfigurationMethodErrors()
{
    for (const auto token : {"Independent", "Native Reference", "radial", "999"})
    {
        const auto parsed = ParsePluginConfiguration(
            MinimalProductionConfiguration() +
            "\n[Image]\nColorGradingMethod=" + token + "\n");
        FO4HDR_CHECK(!parsed.valid);
        FO4HDR_CHECK(parsed.error ==
            PluginConfigurationError::unsupportedColorGradingMethod);
        FO4HDR_CHECK(parsed.value.colorGradingMethod == ColorGradingMethod::radial);
        FO4HDR_CHECK(!parsed.value.productionRequested);
    }
    const auto duplicateMethod = ParsePluginConfiguration(
        MinimalProductionConfiguration() +
        "\n[Image]\nColorGradingMethod=Radial\n"
        "ColorGradingMethod=NativeReference\n");
    FO4HDR_CHECK(!duplicateMethod.valid);
    FO4HDR_CHECK(duplicateMethod.error == PluginConfigurationError::duplicateKey);
    const auto duplicateParameter = ParsePluginConfiguration(
        MinimalProductionConfiguration() +
        "\n[AgX]\nContrast=2.4\nContrast=2.5\n");
    FO4HDR_CHECK(!duplicateParameter.valid);
    FO4HDR_CHECK(duplicateParameter.error ==
        PluginConfigurationError::duplicateKey);
    const auto distinctContrastKeys = ParsePluginConfiguration(
        MinimalProductionConfiguration() +
        "\n[Image]\nContrast=-25\n[AgX]\nContrast=2.75\n");
    FO4HDR_CHECK(distinctContrastKeys.valid);
    FO4HDR_CHECK(distinctContrastKeys.value.contrast == -25.0);
    FO4HDR_CHECK(
        distinctContrastKeys.value.toneMapperParameters.agxContrast == 2.75);
}

void TestConfigurationOperatorErrors()
{
    const auto retiredLookSection = ParsePluginConfiguration(
        MinimalProductionConfiguration() +
        "\n[ACES2LMT]\nContrastLookStrength=0.25\n");
    FO4HDR_CHECK(retiredLookSection.valid);
    FO4HDR_CHECK(retiredLookSection.value.toneMapperParameters.acesLookStrength ==
        ToneMapperParameters{}.acesLookStrength);

    for (const auto invalidParameter : {
             "[ACES2]\nContrastLookStrength=-0.01\n",
             "[AgX]\nMiddleGrayOffsetEV=nan\n",
             "[AgX]\nMiddleGrayOffsetEV=0.51\n",
             "[AgX]\nContrast=2.19\n",
             "[AgX]\nContrast=3.51\n",
             "[AgX]\nHdrColorResponse=0.24\n",
             "[AgX]\nHdrColorResponse=1.01\n",
             "[AgX]\nShoulderPower=0.99\n",
             "[AgX]\nShoulderPower=2.26\n",
             "[GT7]\nToeStrength=2.01\n",
             "[GT7]\nShoulderStart=0.34\n",
             "[GT7]\nColorBlend=-0.01\n",
             "[GT7]\nColorBlend=1.01\n"})
    {
        const auto invalid = ParsePluginConfiguration(
            MinimalProductionConfiguration() + "\n" + invalidParameter);
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
             "VanillaGradeLuminanceStrength=-1",
             "VanillaGradeColorStrength=nan",
             "BloomStrength=-1",
             "BloomStrength=100.01",
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
    text += "\n[Effects]\nVanillaGradeColorStrength=50\n";
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
        text = MinimalProductionConfiguration() +
            "\n[Interface]\nToggleKey=" + toggle + "\n";
        FO4HDR_CHECK(ParsePluginConfiguration(text).valid);
    }
    for (const auto toggle : {"F0", "F25", "f10", "F10junk", "Alt",
             "Alt+", "Ctrl+Ctrl+H", "Win+H", "Alt+F4", "Alt+Tab",
             "Ctrl+Alt+Delete", "Escape", "Ctrl+Escape", "Alt+Enter"})
    {
        text = MinimalProductionConfiguration() +
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
