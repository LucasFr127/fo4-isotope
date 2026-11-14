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

// A save renders the canonical file: foreign keys and comments are gone.
void CheckRewrittenOutput(const std::string& rewritten, const std::string& bomRewritten)
{
    FO4HDR_CHECK(bomRewritten.starts_with("# Isotope settings."));
    FO4HDR_CHECK(rewritten.starts_with("# Isotope settings."));
    for (const auto* dropped : {"keep this comment", "Unknown", "ThirdPartyEffect", "ThirdPartyNumber"})
        FO4HDR_CHECK(!rewritten.contains(dropped));
    FO4HDR_CHECK(HasIniLine(rewritten, "DisplayPeakNits=430"));
    FO4HDR_CHECK(HasIniLine(rewritten, "UiPaperWhiteNits=180"));
    FO4HDR_CHECK(HasIniLine(rewritten, "VideoWhiteNits=220"));
    FO4HDR_CHECK(HasIniLine(rewritten, "ColorGradingMethod=Affine"));
    FO4HDR_CHECK(HasIniLine(rewritten, "ColorGradingCurve=sRGB"));
    FO4HDR_CHECK(HasIniLine(IniSection(rewritten, "ToneMapper"), "Algorithm=ACES2"));
    FO4HDR_CHECK(HasIniLine(IniSection(rewritten, "ACES2"), "ContrastLookStrength=0.35"));
    const auto agx = IniSection(rewritten, "AgX");
    for (const auto* line : {"MiddleGrayOffsetEV=-0.25", "Contrast=2.8", "ColorResponse=0.55",
             "ShoulderPower=2.05"})
        FO4HDR_CHECK(HasIniLine(agx, line));
    const auto gt7 = IniSection(rewritten, "GT7");
    for (const auto* line : {"ToeStrength=1.4", "ShoulderStart=0.52", "ColorBlend=0.3"})
        FO4HDR_CHECK(HasIniLine(gt7, line));
    const auto image = IniSection(rewritten, "Image");
    for (const auto* line : {"ExposureEV=0.25", "Contrast=-20", "Blacks=-40"})
        FO4HDR_CHECK(HasIniLine(image, line));
}

void CheckRewrittenRoundTrip(const HdrSettings& settings, const HdrInterfaceSettings& interfaceSettings)
{
    const auto roundTripRewrite = RewriteHdrSettingsIni(
        MinimalProductionConfiguration(), {settings}, interfaceSettings);
    FO4HDR_CHECK(RewriteHdrSettingsIni(
        roundTripRewrite, {settings}, interfaceSettings) == roundTripRewrite);
    const auto reloadedRewrite =
        ParsePluginConfiguration(roundTripRewrite);
    FO4HDR_CHECK(reloadedRewrite.valid && !reloadedRewrite.rewriteNeeded);
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
    const auto effects = IniSection(rewritten, "Effects");
    for (const auto* line : {"VanillaColorGradingStrength=100", "FallbackGradeLuminance=65",
             "FallbackGradeColor=35", "BloomStrength=75"})
        FO4HDR_CHECK(HasIniLine(effects, line));
}

// A save drops the released settings-window keys with their section and comments.
void CheckRewrittenRetiredInterface(const HdrSettings& settings, const HdrInterfaceSettings& interfaceSettings)
{
    constexpr std::string_view released =
        "[Effects]\nBloomStrength=50\n\n[Interface]\n# Shortcut comment\nToggleKey=F8\n"
        "Scale=1.25\nWindowWidth=700\nWindowHeight=680\nWindowX=-1\nWindowY=-1\n\n"
        "[Debug]\nTraceRenderPipeline=0\n";
    const auto saved = RewriteHdrSettingsIni(released, {settings}, interfaceSettings);
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "Effects"), "BloomStrength=75"));
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "Debug"), "TraceRenderPipeline=0"));
    FO4HDR_CHECK(!saved.contains("[Interface]"));
    FO4HDR_CHECK(!saved.contains("Shortcut comment"));
    for (const auto* key : {"ToggleKey", "Scale", "WindowWidth", "WindowHeight", "WindowX", "WindowY"})
        FO4HDR_CHECK(!saved.contains(key));
    FO4HDR_CHECK(RewriteHdrSettingsIni(saved, {settings}, interfaceSettings) == saved);

    const auto extended = RewriteHdrSettingsIni(
        "[Interface]\nScale=1.25\nThirdPartyInterface=kept\n", {settings}, interfaceSettings);
    FO4HDR_CHECK(!extended.contains("[Interface]") && !extended.contains("ThirdPartyInterface"));
}

void CheckRewrittenMigration(const HdrSettings& settings, const HdrInterfaceSettings& interfaceSettings)
{
    constexpr std::string_view retiredGradeTransfer =
        "[ToneMapper]\nAlgorithm=ACES2\n"
        "[Image]\nGradeTransfer=PreserveHDRColor\nContrast=0\n";
    const auto migrated = RewriteHdrSettingsIni(
        retiredGradeTransfer, {settings}, interfaceSettings);
    FO4HDR_CHECK(!migrated.contains("GradeTransfer"));
    FO4HDR_CHECK(HasIniLine(migrated, "ExposureEV=0.25"));

    // A save renames HdrColorResponse in both profiles and drops the retired
    // overlay switch.
    constexpr std::string_view formerNames =
        "[AgX]\nHdrColorResponse=0.4\n[SdrAgX]\nHdrColorResponse=0.4\n"
        "[Debug]\nShowDeveloperOverlay=0\n";
    const auto renamed = RewriteHdrSettingsIni(formerNames, {settings}, interfaceSettings);
    FO4HDR_CHECK(!renamed.contains("HdrColorResponse"));
    FO4HDR_CHECK(!renamed.contains("ShowDeveloperOverlay"));
    FO4HDR_CHECK(HasIniLine(IniSection(renamed, "AgX"), "ColorResponse=0.55"));
    FO4HDR_CHECK(HasIniLine(IniSection(renamed, "SdrAgX"), "ColorResponse=0.5"));

    // A save renames the former Fallback Grade keys in both profiles.
    constexpr std::string_view formerGrade =
        "[Effects]\nVanillaGradeLuminanceStrength=50\nVanillaGradeColorStrength=50\n"
        "[SdrEffects]\nVanillaGradeLuminanceStrength=50\nVanillaGradeColorStrength=50\n";
    const auto regraded = RewriteHdrSettingsIni(formerGrade, {settings}, interfaceSettings);
    FO4HDR_CHECK(!regraded.contains("VanillaGradeLuminanceStrength"));
    FO4HDR_CHECK(!regraded.contains("VanillaGradeColorStrength"));
    const auto effects = IniSection(regraded, "Effects");
    FO4HDR_CHECK(HasIniLine(effects, "FallbackGradeLuminance=65"));
    FO4HDR_CHECK(HasIniLine(effects, "FallbackGradeColor=35"));
    const auto sdrEffects = IniSection(regraded, "SdrEffects");
    FO4HDR_CHECK(HasIniLine(sdrEffects, "FallbackGradeLuminance=100"));
    FO4HDR_CHECK(HasIniLine(sdrEffects, "FallbackGradeColor=100"));

    // Former method names still load, and a save writes the current names.
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
        FO4HDR_CHECK(HasIniLine(saved, std::string("ColorGradingMethod=") + current));
        FO4HDR_CHECK(!saved.contains(former));
    }

    auto precise = settings;
    precise.exposureEv = 0.123456;
    const auto preciseSaved = RewriteHdrSettingsIni(
        MinimalProductionConfiguration(), {precise}, interfaceSettings);
    const auto preciseLoaded = ParsePluginConfiguration(preciseSaved);
    FO4HDR_CHECK(preciseLoaded.valid);
    FO4HDR_CHECK(preciseLoaded.value.exposureEv == 0.123);
}

// The settings menu saves the HDR request for the next game start and keeps the
// switches it does not edit.
void CheckRewrittenHdrRequest(const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings, std::string_view original)
{
    auto hdrOff = interfaceSettings;
    hdrOff.hdrEnabled = false;
    FO4HDR_CHECK(HasIniLine(IniSection(RewriteHdrSettingsIni(original, {settings}, hdrOff),
        "General"), "HdrEnabled=0"));
    const auto reloaded = ParsePluginConfiguration(
        RewriteHdrSettingsIni(MinimalProductionConfiguration(), {settings}, hdrOff));
    FO4HDR_CHECK(reloaded.valid && !reloaded.value.enabled);
    FO4HDR_CHECK(!reloaded.value.interfaceSettings.hdrEnabled);

    const auto switches = RewriteHdrSettingsIni(
        "[General]\nSdrEnabled=0\nDeveloperMode=1\n[Debug]\nTraceRenderPipeline=1\n",
        {settings}, interfaceSettings);
    for (const auto* line : {"SdrEnabled=0", "DeveloperMode=1", "TraceRenderPipeline=1"})
        FO4HDR_CHECK(HasIniLine(switches, line));
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
    // Every SDR section is written under its own name, without HDR-only keys.
    const auto saved = RewriteHdrSettingsIni(MinimalProductionConfiguration(), profiles, {});
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "SdrToneMapper"), "Algorithm=Vanilla"));
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "SdrAgX"), "Contrast=2.5"));
    FO4HDR_CHECK(!IniSection(saved, "SdrAgX").contains("\r\nShoulderPower="));
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "SdrGT7"), "ToeStrength=1.28"));
    // The SDR-only key is written only to its SDR section.
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "SdrToneMapper"), "AutomaticWeatherResponse=1"));
    FO4HDR_CHECK(saved.find("AutomaticWeatherResponse=") == saved.rfind("AutomaticWeatherResponse="));
    // The SDR shoulder minimum survives the saved 0.001 precision.
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "SdrGT7"), "ShoulderStart=0.216"));
    const auto image = IniSection(saved, "SdrImage");
    for (const auto* line : {"ExposureEV=-0.5", "Contrast=15", "HighlightSaturation=70"})
        FO4HDR_CHECK(HasIniLine(image, line));
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "SdrEffects"), "BloomStrength=60"));
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "SdrSelectiveColor"), "Enabled=1"));
    FO4HDR_CHECK(HasIniLine(IniSection(saved, "SdrWeatherLutResponse"), "Enabled=1"));
    FO4HDR_CHECK(saved.find("ColorGradingCurve=") == saved.rfind("ColorGradingCurve="));
    FO4HDR_CHECK(RewriteHdrSettingsIni(saved, profiles, {}) == saved);
    const auto reloaded = ParsePluginConfiguration(saved);
    FO4HDR_CHECK(reloaded.valid);
    FO4HDR_CHECK(reloaded.value.sdrSettings == sdr);
    FO4HDR_CHECK(reloaded.value.exposureEv == 0.0);
    FO4HDR_CHECK(reloaded.value.bloomStrengthPercent == HdrSettings{}.bloomStrengthPercent);

    // Foreign and HDR-only names in an SDR section are dropped.
    const auto existing = IniSection(RewriteHdrSettingsIni(
        "[SdrImage]\nExposureEV=1\nColorGradingMethod=Affine\nThirdParty=kept\n", profiles, {}),
        "SdrImage");
    FO4HDR_CHECK(HasIniLine(existing, "ExposureEV=-0.5"));
    FO4HDR_CHECK(!existing.contains("\r\nColorGradingMethod=") && !existing.contains("ThirdParty"));
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

[[nodiscard]] SettingsPersistenceSnapshot WaitForGeneration(std::uint64_t generation)
{
    SettingsPersistenceSnapshot snapshot{};
    for (unsigned attempt = 0; attempt < 2000; ++attempt)
    {
        snapshot = ReadSettingsPersistenceSnapshot();
        if (snapshot.generation == generation &&
            snapshot.state != SettingsPersistenceState::saving)
            break;
        Sleep(1);
    }
    return snapshot;
}

void TestPersistedMainSettings(PersistenceFixture& fixture)
{
    auto& [root, path, profileDirectory, personalPath, settings, interfaceSettings] = fixture;
    const auto generation =
        RequestHdrSettingsSave({settings}, interfaceSettings);
    FO4HDR_CHECK(generation != 0);
    const auto snapshot = WaitForGeneration(generation);
    FO4HDR_CHECK(snapshot.generation == generation);
    FO4HDR_CHECK(snapshot.state == SettingsPersistenceState::saved);
    std::ifstream input(path, std::ios::binary);
    const std::string persisted(std::istreambuf_iterator<char>(input), {});
    FO4HDR_CHECK(HasIniLine(persisted, "DisplayPeakNits=430"));
    const auto image = IniSection(persisted, "Image");
    FO4HDR_CHECK(HasIniLine(image, "ExposureEV=0.75"));
    FO4HDR_CHECK(HasIniLine(image, "Blacks=35"));
    FO4HDR_CHECK(HasIniLine(persisted, "FallbackGradeLuminance=60"));
    FO4HDR_CHECK(HasIniLine(persisted, "FallbackGradeColor=30"));
    FO4HDR_CHECK(!persisted.contains("[LUT:stale.dds]"));
    std::ifstream personalInput(personalPath, std::ios::binary);
    const std::string personalBytes(std::istreambuf_iterator<char>(personalInput), {});
    FO4HDR_CHECK(personalBytes == "user-owned sidecar\r\n[LUT:test.dds]\r\nColor=nan\r\n");
    std::ifstream distributedInput(profileDirectory / "Z.ini", std::ios::binary);
    const std::string distributedBytes(std::istreambuf_iterator<char>(distributedInput), {});
    FO4HDR_CHECK(distributedBytes == "[LUT:textures/distributed.dds]\nColor=66\n");
    FO4HDR_CHECK(HasIniLine(IniSection(persisted, "ACES2"), "ContrastLookStrength=0.4"));
    const auto agx = IniSection(persisted, "AgX");
    FO4HDR_CHECK(HasIniLine(agx, "MiddleGrayOffsetEV=0.25") && HasIniLine(agx, "Contrast=3"));
    const auto gt7 = IniSection(persisted, "GT7");
    FO4HDR_CHECK(HasIniLine(gt7, "ToeStrength=1.5") && HasIniLine(gt7, "ShoulderStart=0.5"));
    FO4HDR_CHECK(!persisted.contains("GradeTransfer"));
    FO4HDR_CHECK(!persisted.contains("[Interface]"));
    const auto reloaded = LoadPluginConfiguration(path);
    FO4HDR_CHECK(reloaded.valid && !reloaded.rewriteNeeded);
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
    FO4HDR_CHECK(HasIniLine(persisted, "ColorGradingCurve=sRGB"));
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

// A save after the INI was deleted writes it again from the saved settings.
void TestSettingsSaveRecreatesMissingFile(PersistenceFixture& fixture)
{
    std::error_code error;
    FO4HDR_CHECK(std::filesystem::remove(fixture.path, error) && !error);
    auto settings = fixture.settings;
    settings.exposureEv = -0.5;
    const auto generation = RequestHdrSettingsSave({settings}, fixture.interfaceSettings);
    FO4HDR_CHECK(generation != 0);
    FO4HDR_CHECK(WaitForGeneration(generation).state == SettingsPersistenceState::saved);
    const auto reloaded = LoadPluginConfiguration(fixture.path);
    FO4HDR_CHECK(reloaded.valid && !reloaded.fileMissing && !reloaded.rewriteNeeded);
    FO4HDR_CHECK(reloaded.value.exposureEv == -0.5);
}

void TestSettingsPersistenceSave()
{
    auto fixture = CreatePersistenceFixture();
    TestPersistedMainSettings(fixture);
    TestSettingsAutosaveDebounce(fixture);
    TestSettingsAutosaveRetry(fixture);
    TestSettingsSaveRecreatesMissingFile(fixture);
    const auto& root = fixture.root;
    std::error_code error;
    FO4HDR_CHECK(std::filesystem::remove_all(root, error) != 0 && !error);
}

}  // namespace fo4hdr::tests
