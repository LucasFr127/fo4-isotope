// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"
#include "SettingsPersistence.hpp"
#include "TestHarness.hpp"
#include <Windows.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>

namespace fo4hdr::tests {

void CheckRewrittenOutput(const std::string& rewritten, const std::string& bomRewritten)
{
    FO4HDR_CHECK(bomRewritten.starts_with("\xEF\xBB\xBF[General]\n"));
    FO4HDR_CHECK(bomRewritten.find("\xEF\xBB\xBF", 3) ==
        std::string::npos);
    FO4HDR_CHECK(bomRewritten.find("[General]", 4) == std::string::npos);
    FO4HDR_CHECK(rewritten.find("# keep this comment\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("Unknown=kept\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("ThirdPartyEffect=kept\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("ThirdPartyNumber=2\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("DisplayPeakNits=430\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("UiPaperWhiteNits=180\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("VideoWhiteNits=220\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("ColorGradingMethod=Affine\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("ColorGradingCurve=sRGB\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[ToneMapper]\r\nAlgorithm=ACES2\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[ACES2]\r\nContrastLookStrength=0.35\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[AgX]\r\nMiddleGrayOffsetEV=-0.25\r\nContrast=2.8\r\n"
        "ColorResponse=0.55\r\nShoulderPower=2.05\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[GT7]\r\nToeStrength=1.4\r\nShoulderStart=0.52\r\n"
        "ColorBlend=0.3\r\n") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("[Image]\r\nExposureEV=0.25\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "ExposureEV=0.25\r\nContrast=-20\r\nBlacks=-40\r\n") !=
        std::string::npos);
}

void CheckRewrittenRoundTrip(const HdrSettings& settings, const HdrInterfaceSettings& interfaceSettings)
{
    const auto roundTripRewrite = RewriteHdrSettingsIni(
        MinimalProductionConfiguration(), {settings}, interfaceSettings);
    FO4HDR_CHECK(RewriteHdrSettingsIni(
        roundTripRewrite, {settings}, interfaceSettings) == roundTripRewrite);
    const auto reloadedRewrite =
        ParsePluginConfiguration(roundTripRewrite);
    FO4HDR_CHECK(reloadedRewrite.valid);
    FO4HDR_CHECK(reloadedRewrite.value.exposureEv == settings.exposureEv);
    FO4HDR_CHECK(reloadedRewrite.value.toneMapperParameters ==
        settings.toneMapperParameters);
    FO4HDR_CHECK(reloadedRewrite.value.colorGradingMethod == settings.colorGradingMethod);
    FO4HDR_CHECK(reloadedRewrite.value.colorGradingCurve == settings.colorGradingCurve);
    FO4HDR_CHECK(test::Approx(reloadedRewrite.value.sceneExposureMultiplier,
        std::exp2(settings.exposureEv), 1.0e-15));
    FO4HDR_CHECK(reloadedRewrite.value.blacks == settings.blacks);
    FO4HDR_CHECK(reloadedRewrite.value.fallbackGradeLuminancePercent ==
        settings.fallbackGradeLuminancePercent);
    FO4HDR_CHECK(reloadedRewrite.value.fallbackGradeColorPercent ==
        settings.fallbackGradeColorPercent);
    auto resetParameters = settings;
    resetParameters.toneMapperParameters = {};
    resetParameters.fallbackGradeLuminancePercent = HdrSettings{}.fallbackGradeLuminancePercent;
    resetParameters.fallbackGradeColorPercent = HdrSettings{}.fallbackGradeColorPercent;
    resetParameters.bloomStrengthPercent = HdrSettings{}.bloomStrengthPercent;
    const auto resetParametersReloaded = ParsePluginConfiguration(
        RewriteHdrSettingsIni(
            roundTripRewrite, {resetParameters}, interfaceSettings));
    FO4HDR_CHECK(resetParametersReloaded.valid);
    FO4HDR_CHECK(resetParametersReloaded.value.toneMapperParameters ==
        ToneMapperParameters{});
    FO4HDR_CHECK(
        resetParametersReloaded.value.fallbackGradeLuminancePercent == 67.0);
    FO4HDR_CHECK(
        resetParametersReloaded.value.fallbackGradeColorPercent == 67.0);
    FO4HDR_CHECK(resetParametersReloaded.value.bloomStrengthPercent == 50.0);
}

void CheckRewrittenEffects(const std::string& rewritten)
{
    FO4HDR_CHECK(rewritten.find("[Effects]\r\n") != std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "VanillaColorGradingStrength=100\r\n"
        "FallbackGradeLuminance=65\r\nFallbackGradeColor=35\r\n"
        "BloomStrength=75\r\n") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("BloomStrength=75\r\n") !=
        std::string::npos);
}

// A full save removes the released settings-window keys, and their section
// once it holds no other key.
void CheckRewrittenRetiredInterface(const HdrSettings& settings, const HdrInterfaceSettings& interfaceSettings)
{
    constexpr std::string_view released =
        "[Effects]\nBloomStrength=50\n\n[Interface]\n# Shortcut comment\nToggleKey=F8\n"
        "Scale=1.25\nWindowWidth=700\nWindowHeight=680\nWindowX=-1\nWindowY=-1\n\n"
        "[Debug]\nTraceRenderPipeline=0\n";
    const auto saved = RewriteHdrSettingsIni(released, {settings}, interfaceSettings);
    FO4HDR_CHECK(saved.find("BloomStrength=75\n\n[Debug]\nTraceRenderPipeline=0\n") !=
        std::string::npos);
    FO4HDR_CHECK(saved.find("[Interface]") == std::string::npos);
    FO4HDR_CHECK(saved.find("Shortcut comment") == std::string::npos);
    for (const auto* key : {"ToggleKey", "Scale", "WindowWidth", "WindowHeight", "WindowX", "WindowY"})
        FO4HDR_CHECK(saved.find(key) == std::string::npos);
    FO4HDR_CHECK(RewriteHdrSettingsIni(saved, {settings}, interfaceSettings) == saved);

    const auto extended = RewriteHdrSettingsIni(
        "[Interface]\nScale=1.25\nThirdPartyInterface=kept\n", {settings}, interfaceSettings);
    FO4HDR_CHECK(extended.starts_with("[Interface]\n"));
    FO4HDR_CHECK(extended.find("ThirdPartyInterface=kept\n") != std::string::npos);
    FO4HDR_CHECK(extended.find("Scale") == std::string::npos);
}

void CheckRewrittenMigration(const HdrSettings& settings, const HdrInterfaceSettings& interfaceSettings)
{
    constexpr std::string_view retiredGradeTransfer =
        "[ToneMapper]\nAlgorithm=ACES2\n"
        "[Image]\nGradeTransfer=PreserveHDRColor\nContrast=0\n";
    const auto migrated = RewriteHdrSettingsIni(
        retiredGradeTransfer, {settings}, interfaceSettings);
    FO4HDR_CHECK(migrated.find("GradeTransfer") == std::string::npos);
    FO4HDR_CHECK(migrated.find("ExposureEV=0.25") != std::string::npos);

    // A full save renames HdrColorResponse in place in both profiles and drops
    // the retired overlay switch.
    constexpr std::string_view formerNames =
        "[AgX]\nHdrColorResponse=0.4\n[SdrAgX]\nHdrColorResponse=0.4\n"
        "[Debug]\nShowDeveloperOverlay=0\n";
    const auto renamed = RewriteHdrSettingsIni(formerNames, {settings}, interfaceSettings);
    FO4HDR_CHECK(renamed.find("HdrColorResponse") == std::string::npos);
    FO4HDR_CHECK(renamed.find("ShowDeveloperOverlay") == std::string::npos);
    FO4HDR_CHECK(renamed.find("[AgX]\nColorResponse=0.55\n") != std::string::npos);
    FO4HDR_CHECK(renamed.find("[SdrAgX]\nColorResponse=") != std::string::npos);

    // A full save renames the former Fallback Grade keys in place in both profiles.
    constexpr std::string_view formerGrade =
        "[Effects]\nVanillaGradeLuminanceStrength=50\nVanillaGradeColorStrength=50\n"
        "[SdrEffects]\nVanillaGradeLuminanceStrength=50\nVanillaGradeColorStrength=50\n";
    const auto regraded = RewriteHdrSettingsIni(formerGrade, {settings}, interfaceSettings);
    FO4HDR_CHECK(regraded.find("VanillaGradeLuminanceStrength") == std::string::npos);
    FO4HDR_CHECK(regraded.find("VanillaGradeColorStrength") == std::string::npos);
    FO4HDR_CHECK(regraded.find(
        "[Effects]\nFallbackGradeLuminance=65\nFallbackGradeColor=35\n") != std::string::npos);
    FO4HDR_CHECK(regraded.find(
        "[SdrEffects]\nFallbackGradeLuminance=100\nFallbackGradeColor=100\n") != std::string::npos);

    // Former method names still load, and a full save writes the current names.
    for (const auto& [former, method, current] : {
             std::tuple{"Radial", ColorGradingMethod::rgbRatio, "RgbRatio"},
             std::tuple{"NativeReference", ColorGradingMethod::affine, "Affine"}})
    {
        const auto text = MinimalProductionConfiguration() +
            "[Image]\nColorGradingMethod=" + former + "\n";
        const auto loaded = ParsePluginConfiguration(text);
        FO4HDR_CHECK(loaded.valid);
        FO4HDR_CHECK(loaded.value.colorGradingMethod == method);
        const HdrSettings loadedSettings = loaded.value;
        const auto saved = RewriteHdrSettingsIni(text, {loadedSettings}, interfaceSettings);
        FO4HDR_CHECK(saved.find(std::string("ColorGradingMethod=") + current + "\n") !=
            std::string::npos);
        FO4HDR_CHECK(saved.find(former) == std::string::npos);
    }

    auto precise = settings;
    precise.exposureEv = 0.123456;
    const auto preciseSaved = RewriteHdrSettingsIni(
        MinimalProductionConfiguration(), {precise}, interfaceSettings);
    const auto preciseLoaded = ParsePluginConfiguration(preciseSaved);
    FO4HDR_CHECK(preciseLoaded.valid);
    FO4HDR_CHECK(preciseLoaded.value.exposureEv == 0.123);
}

// The settings menu saves the HDR request for the next game start.
void CheckRewrittenHdrRequest(const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings, std::string_view original)
{
    auto hdrOff = interfaceSettings;
    hdrOff.hdrEnabled = false;
    FO4HDR_CHECK(RewriteHdrSettingsIni(original, {settings}, hdrOff).find(
        "[General]\r\nHdrEnabled=0\r\nUnknown=kept\r\n") != std::string::npos);
    const auto reloaded = ParsePluginConfiguration(
        RewriteHdrSettingsIni(MinimalProductionConfiguration(), {settings}, hdrOff));
    FO4HDR_CHECK(reloaded.valid && !reloaded.value.enabled);
    FO4HDR_CHECK(!reloaded.value.interfaceSettings.hdrEnabled);
    // An INI from before the key gains it once it is on, inside its one [General]
    // section, where a Win32 profile reader finds it.
    auto hosted = interfaceSettings;
    hosted.hostedPresentation = true;
    const auto hostedText = RewriteHdrSettingsIni(MinimalProductionConfiguration(), {settings}, hosted);
    FO4HDR_CHECK(hostedText.find("DeveloperMode=0\nHostedPresentation=1\n[Output]") !=
        std::string::npos);
    FO4HDR_CHECK(hostedText.find("[General]", 1) == std::string::npos);
    FO4HDR_CHECK(RewriteHdrSettingsIni(original, {settings}, hosted).find(
        "Unknown=kept\r\nHostedPresentation=1\r\n") != std::string::npos);
    const auto hostedReloaded = ParsePluginConfiguration(hostedText);
    FO4HDR_CHECK(hostedReloaded.valid && hostedReloaded.value.hostedPresentation);
    FO4HDR_CHECK(hostedReloaded.value.interfaceSettings == hosted);
    hosted.hostedPresentation = false;
    FO4HDR_CHECK(RewriteHdrSettingsIni(hostedText, {settings}, hosted).find(
        "HostedPresentation=0\n") != std::string::npos);
}

void CheckSdrProfileRewrite()
{
    auto sdr = DefaultSdrSettings();
    sdr.exposureEv = -0.5;
    sdr.contrast = 15.0;
    sdr.highlightSaturationPercent = 70.0;
    sdr.bloomStrengthPercent = 60.0;
    sdr.selectiveColor.enabled = true;
    sdr.selectiveColor.regions[3].saturationPercent = 80.0;
    sdr.weatherGradeResponse.enabled = true;
    sdr.toneMapperParameters.agxContrast = 2.5;
    sdr.toneMapperParameters.gt7ShoulderStart = kMinimumSdrGt7ShoulderStart;
    const HdrSettingsProfiles profiles{.hdr = HdrSettings{}, .sdr = sdr};
    // Missing SDR sections are appended under their own names, without HDR-only keys.
    const auto saved = RewriteHdrSettingsIni(MinimalProductionConfiguration(), profiles, {});
    // A section runs through the newline that ends its last key.
    const auto section = [&saved](std::string_view header) {
        const auto begin = saved.find(header);
        if (begin == std::string::npos) return std::string{};
        const auto end = saved.find("\n[", begin);
        return saved.substr(begin, end == std::string::npos ? end : end + 1 - begin);
    };
    FO4HDR_CHECK(section("[SdrToneMapper]").find("Algorithm=Vanilla\n") != std::string::npos);
    FO4HDR_CHECK(section("[SdrAgX]").find("Contrast=2.5\n") != std::string::npos);
    FO4HDR_CHECK(section("[SdrAgX]").find("ShoulderPower") == std::string::npos);
    FO4HDR_CHECK(section("[SdrGT7]").find("ToeStrength=1.28\n") != std::string::npos);
    // The SDR-only key is written only to its SDR section.
    FO4HDR_CHECK(section("[SdrToneMapper]").find("AutomaticWeatherResponse=1\n") != std::string::npos);
    FO4HDR_CHECK(saved.find("AutomaticWeatherResponse=") == saved.rfind("AutomaticWeatherResponse="));
    // The SDR shoulder minimum survives the saved 0.001 precision.
    FO4HDR_CHECK(section("[SdrGT7]").find("ShoulderStart=0.216\n") != std::string::npos);
    FO4HDR_CHECK(saved.find("[SdrImage]\nExposureEV=-0.5\nContrast=15\n") != std::string::npos);
    FO4HDR_CHECK(section("[SdrImage]").find("HighlightSaturation=70\n") != std::string::npos);
    FO4HDR_CHECK(saved.find("[SdrEffects]\n") != std::string::npos);
    FO4HDR_CHECK(saved.find("BloomStrength=60\n") != std::string::npos);
    FO4HDR_CHECK(saved.find("[SdrSelectiveColor]\nEnabled=1\n") != std::string::npos);
    FO4HDR_CHECK(saved.find("[SdrWeatherLutResponse]\nEnabled=1\n") != std::string::npos);
    FO4HDR_CHECK(saved.find("ColorGradingCurve=") == saved.rfind("ColorGradingCurve="));
    FO4HDR_CHECK(RewriteHdrSettingsIni(saved, profiles, {}) == saved);
    const auto reloaded = ParsePluginConfiguration(saved);
    FO4HDR_CHECK(reloaded.valid);
    FO4HDR_CHECK(reloaded.value.sdrSettings == sdr);
    FO4HDR_CHECK(reloaded.value.exposureEv == 0.0);
    FO4HDR_CHECK(reloaded.value.bloomStrengthPercent == HdrSettings{}.bloomStrengthPercent);

    // An existing SDR section is rewritten in place; foreign and HDR-only names stay.
    constexpr std::string_view existing =
        "[SdrImage]\nExposureEV=1\nColorGradingMethod=Affine\nThirdParty=kept\n";
    FO4HDR_CHECK(RewriteHdrSettingsIni(existing, profiles, {}).find(
        "[SdrImage]\nExposureEV=-0.5\nColorGradingMethod=Affine\nThirdParty=kept\n") !=
        std::string::npos);
}

void TestSettingsPersistenceRewrite()
{
    HdrSettings settings{};
    const HdrInterfaceSettings interfaceSettings{};
    settings.productionAlgorithm = ProductionToneMapperAlgorithm::aces2;
    settings.toneMapperParameters = {
        .acesLookStrength = 0.35,
        .agxMiddleGrayEv = -0.25,
        .agxContrast = 2.8,
        .agxHdrPurity = 0.55,
        .agxHdrShoulderPower = 2.05,
        .gt7ToeStrength = 1.4,
        .gt7ShoulderStart = 0.52,
        .gt7ColorBlend = 0.3};
    settings.colorGradingMethod = ColorGradingMethod::affine;
    settings.colorGradingCurve = ColorGradingCurve::srgb;
    settings.displayPeakNits = 430.0;
    settings.uiPaperWhiteNits = 180.0;
    settings.videoWhiteNits = 220.0;
    settings.exposureEv = 0.25;
    settings.contrast = -20.0;
    settings.blacks = -40.0;
    settings.fallbackGradeLuminancePercent = 65.0;
    settings.fallbackGradeColorPercent = 35.0;
    settings.bloomStrengthPercent = 75.0;
    constexpr std::string_view original =
        "# keep this comment\r\n"
        "[General]\r\nHdrEnabled=1\r\nUnknown=kept\r\n"
        "[Output]\r\nDisplayPeakNits=1000\r\n"
        "UiPaperWhiteNits=203\r\n"
        "[Effects]\r\nThirdPartyNumber=2\r\n"
        "ThirdPartyEffect=kept\r\n";
    const auto rewritten = RewriteHdrSettingsIni(
        original, {settings}, interfaceSettings);
    const auto bomRewritten = RewriteHdrSettingsIni(
        std::string("\xEF\xBB\xBF") + MinimalProductionConfiguration(),
        {settings}, interfaceSettings);
    CheckRewrittenOutput(rewritten, bomRewritten);
    CheckRewrittenRoundTrip(settings, interfaceSettings);
    CheckRewrittenEffects(rewritten);
    CheckRewrittenRetiredInterface(settings, interfaceSettings);
    CheckRewrittenMigration(settings, interfaceSettings);
    CheckSdrProfileRewrite();
    CheckRewrittenHdrRequest(settings, interfaceSettings, original);
    settings.colorGradingMethod = static_cast<ColorGradingMethod>(0xFF);
    FO4HDR_CHECK(RewriteHdrSettingsIni(
        original, {settings}, interfaceSettings) == original);
    settings.colorGradingMethod = ColorGradingMethod::affine;
    settings.colorGradingCurve = static_cast<ColorGradingCurve>(0xFF);
    FO4HDR_CHECK(RewriteHdrSettingsIni(
        original, {settings}, interfaceSettings) == original);
    settings.colorGradingCurve = ColorGradingCurve::srgb;
    settings.displayPeakNits = 399.0;
    FO4HDR_CHECK(RewriteHdrSettingsIni(
        original, {settings}, interfaceSettings) == original);
}

SettingsPersistenceSnapshot WaitPersistence()
{
    for (unsigned attempt = 0; attempt < 2000; ++attempt)
    {
        const auto result = ReadSettingsPersistenceSnapshot();
        if (result.state != SettingsPersistenceState::saving) return result;
        Sleep(1);
    }
    return ReadSettingsPersistenceSnapshot();
}

struct PersistenceFixture
{
    std::filesystem::path root;
    std::filesystem::path path;
    std::filesystem::path profileDirectory;
    std::filesystem::path personalPath;
    HdrSettings settings;
    HdrInterfaceSettings interfaceSettings;
};
PersistenceFixture CreatePersistenceFixture()
{

    const auto root = std::filesystem::temp_directory_path() /
        ("FO4Isotope-settings-save-" +
         std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    FO4HDR_CHECK(std::filesystem::create_directory(root));
    const auto path = root / "FO4Isotope.ini";
    const auto profileDirectory = root / "FO4Isotope";
    FO4HDR_CHECK(std::filesystem::create_directory(profileDirectory));
    const auto personalPath = profileDirectory / "Personal.ini";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << MinimalProductionConfiguration()
               << "\n[Image]\nGradeTransfer=PreserveHDRColor\n"
               << "[Interface]\nToggleKey=F8\nScale=1.25\n"
               << "[LUT:stale.dds]\nColor=1\n";
    }
    {
        std::ofstream output(
            profileDirectory / "Z.ini", std::ios::binary | std::ios::trunc);
        output << "[LUT:textures/distributed.dds]\nColor=66\n";
    }
    {
        std::ofstream output(personalPath, std::ios::binary | std::ios::trunc);
        output << "user-owned sidecar\r\n[LUT:test.dds]\r\nColor=nan\r\n";
    }
    auto startup = LoadPluginConfiguration(path);
    FO4HDR_CHECK(startup.valid);
    FO4HDR_CHECK(InitializeSettingsPersistence(path));
    HdrSettings settings{};
    settings.displayPeakNits = 430.0;
    settings.exposureEv = 0.75;
    settings.blacks = 35.0;
    settings.fallbackGradeLuminancePercent = 60.0;
    settings.fallbackGradeColorPercent = 30.0;
    settings.weatherGradeResponse.luminance.base = 85.0;
    settings.weatherGradeResponse.color.gain = 0.75;
    settings.toneMapperParameters = {
        .acesLookStrength = 0.4,
        .agxMiddleGrayEv = 0.25,
        .agxContrast = 3.0,
        .agxHdrPurity = 0.55,
        .agxHdrShoulderPower = 2.1,
        .gt7ToeStrength = 1.5,
        .gt7ShoulderStart = 0.5,
        .gt7ColorBlend = 0.2};
    settings.colorGradingMethod = ColorGradingMethod::affine;
    settings.colorGradingCurve = ColorGradingCurve::srgb;
    return {root, path, profileDirectory, personalPath, settings, HdrInterfaceSettings{}};
}

void TestPersistedMainSettings(PersistenceFixture& fixture)
{
    auto& [root, path, profileDirectory, personalPath, settings, interfaceSettings] = fixture;
    const auto generation =
        RequestHdrSettingsSave({settings}, interfaceSettings);
    FO4HDR_CHECK(generation != 0);
    SettingsPersistenceSnapshot snapshot{};
    for (unsigned attempt = 0; attempt < 2000; ++attempt)
    {
        snapshot = ReadSettingsPersistenceSnapshot();
        if (snapshot.generation == generation &&
            snapshot.state != SettingsPersistenceState::saving)
            break;
        Sleep(1);
    }
    FO4HDR_CHECK(snapshot.generation == generation);
    FO4HDR_CHECK(snapshot.state == SettingsPersistenceState::saved);
    std::ifstream input(path, std::ios::binary);
    const std::string persisted(std::istreambuf_iterator<char>(input), {});
    FO4HDR_CHECK(persisted.find("DisplayPeakNits=430") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("[Image]\nExposureEV=0.75") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("Blacks=35") != std::string::npos);
    FO4HDR_CHECK(persisted.find("FallbackGradeLuminance=60") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("FallbackGradeColor=30") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("[LUT:stale.dds]\nColor=1\n") != std::string::npos);
    std::ifstream personalInput(personalPath, std::ios::binary);
    const std::string personalBytes(std::istreambuf_iterator<char>(personalInput), {});
    FO4HDR_CHECK(personalBytes == "user-owned sidecar\r\n[LUT:test.dds]\r\nColor=nan\r\n");
    std::ifstream distributedInput(profileDirectory / "Z.ini", std::ios::binary);
    const std::string distributedBytes(std::istreambuf_iterator<char>(distributedInput), {});
    FO4HDR_CHECK(distributedBytes == "[LUT:textures/distributed.dds]\nColor=66\n");
    FO4HDR_CHECK(persisted.find("[ACES2]\nContrastLookStrength=0.4") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("[AgX]\nMiddleGrayOffsetEV=0.25\nContrast=3") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find(
        "[GT7]\nToeStrength=1.5\nShoulderStart=0.5") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("GradeTransfer") == std::string::npos);
    FO4HDR_CHECK(persisted.find("[Interface]") == std::string::npos);
    const auto reloaded = LoadPluginConfiguration(path);
    FO4HDR_CHECK(reloaded.valid);
    FO4HDR_CHECK(!reloaded.value.developerMode);
    FO4HDR_CHECK(reloaded.value.blacks == settings.blacks);
    FO4HDR_CHECK(reloaded.value.fallbackGradeLuminancePercent ==
        settings.fallbackGradeLuminancePercent);
    FO4HDR_CHECK(reloaded.value.fallbackGradeColorPercent ==
        settings.fallbackGradeColorPercent);
    FO4HDR_CHECK(reloaded.value.weatherGradeResponse ==
        settings.weatherGradeResponse);
    FO4HDR_CHECK(reloaded.value.toneMapperParameters ==
        settings.toneMapperParameters);
    FO4HDR_CHECK(reloaded.value.colorGradingMethod == settings.colorGradingMethod);
    FO4HDR_CHECK(persisted.find("ColorGradingCurve=sRGB\n") != std::string::npos);
    FO4HDR_CHECK(reloaded.value.colorGradingCurve == settings.colorGradingCurve);
    input.close();
}

void TestSettingsAutosaveDebounce(PersistenceFixture& fixture)
{
    SettingsAutosave autosave;
    autosave.Initialize({fixture.settings}, fixture.interfaceSettings);
    auto settings = fixture.settings;
    const auto interfaceSettings = fixture.interfaceSettings;
    FO4HDR_CHECK(!autosave.Dirty({settings}, interfaceSettings));
    auto hdrRequest = interfaceSettings;
    hdrRequest.hdrEnabled = !hdrRequest.hdrEnabled;
    FO4HDR_CHECK(autosave.Dirty({settings}, hdrRequest));
    const auto generation = ReadSettingsPersistenceSnapshot().generation;
    settings.exposureEv = 1.25;
    autosave.Update({settings}, interfaceSettings, {100, true, true});
    FO4HDR_CHECK(autosave.Dirty({settings}, interfaceSettings));
    autosave.Update({settings}, interfaceSettings, {599, true, true});
    FO4HDR_CHECK(ReadSettingsPersistenceSnapshot().generation == generation);
    settings.contrast = 5.0;
    autosave.Update({settings}, interfaceSettings, {600, true, true});
    autosave.Update({settings}, interfaceSettings, {1099, true, true});
    FO4HDR_CHECK(ReadSettingsPersistenceSnapshot().generation == generation);
    autosave.Update({settings}, interfaceSettings, {1100, false, false});
    FO4HDR_CHECK(ReadSettingsPersistenceSnapshot().generation == generation);
    autosave.Update({settings}, interfaceSettings, {1101, true, false});
    FO4HDR_CHECK(!autosave.RequestFailed());
    FO4HDR_CHECK(WaitPersistence().state == SettingsPersistenceState::saved);
    settings.exposureEv = 1.5;
    autosave.Update({settings}, interfaceSettings, {1102, false, false});
    FO4HDR_CHECK(autosave.Dirty({settings}, interfaceSettings));
    const auto reloaded = LoadPluginConfiguration(fixture.path);
    FO4HDR_CHECK(reloaded.valid);
    FO4HDR_CHECK(reloaded.value.exposureEv == 1.25);
    FO4HDR_CHECK(reloaded.value.contrast == 5.0);
    autosave.Update({settings}, interfaceSettings, {1103, true, false});
    FO4HDR_CHECK(WaitPersistence().state == SettingsPersistenceState::saved);
    autosave.Update({settings}, interfaceSettings, {1104, true, false});
    FO4HDR_CHECK(!autosave.Dirty({settings}, interfaceSettings));
    FO4HDR_CHECK(LoadPluginConfiguration(fixture.path).value.exposureEv == 1.5);
}

void TestSettingsAutosaveRetry(PersistenceFixture& fixture)
{
    const auto startup = LoadPluginConfiguration(fixture.path);
    FO4HDR_CHECK(startup.valid);
    auto settings = fixture.settings;
    settings.exposureEv = startup.value.exposureEv;
    const auto interfaceSettings = startup.value.interfaceSettings;
    SettingsAutosave autosave;
    autosave.Initialize({settings}, interfaceSettings);
    const auto locked = CreateFileW(fixture.path.c_str(), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FO4HDR_CHECK(locked != INVALID_HANDLE_VALUE);
    if (locked == INVALID_HANDLE_VALUE) return;
    autosave.Update({fixture.settings}, fixture.interfaceSettings, {2000, true, false});
    const auto failed = WaitPersistence();
    CloseHandle(locked);
    FO4HDR_CHECK(failed.state == SettingsPersistenceState::failed);
    FO4HDR_CHECK(autosave.Dirty({fixture.settings}, fixture.interfaceSettings));
    autosave.Update({fixture.settings}, fixture.interfaceSettings, {3999, true, false});
    FO4HDR_CHECK(ReadSettingsPersistenceSnapshot().generation == failed.generation);
    autosave.Update({fixture.settings}, fixture.interfaceSettings, {4000, true, false});
    FO4HDR_CHECK(WaitPersistence().state == SettingsPersistenceState::saved);
    autosave.Update({fixture.settings}, fixture.interfaceSettings, {4001, true, false});
    FO4HDR_CHECK(!autosave.Dirty({fixture.settings}, fixture.interfaceSettings));
    FO4HDR_CHECK(LoadPluginConfiguration(fixture.path).value.exposureEv == fixture.settings.exposureEv);
}

void TestSettingsPersistenceSave()
{
    auto fixture = CreatePersistenceFixture();
    TestPersistedMainSettings(fixture);
    TestSettingsAutosaveDebounce(fixture);
    TestSettingsAutosaveRetry(fixture);
    const auto& root = fixture.root;
    std::error_code error;
    FO4HDR_CHECK(std::filesystem::remove_all(root, error) != 0 && !error);
}

}  // namespace fo4hdr::tests
