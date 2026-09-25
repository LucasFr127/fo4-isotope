// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"
#include "SettingsPersistence.hpp"
#include "TestHarness.hpp"
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

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
    FO4HDR_CHECK(rewritten.find("ColorGradingMethod=NativeReference\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[ToneMapper]\r\nAlgorithm=ACES2LMT\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[ACES2LMT]\r\nLookStrength=0.35\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[AgX]\r\nMiddleGray=-0.25\r\nContrast=2.8\r\n"
        "HdrPurity=0.55\r\nShoulderPower=2.05\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[GT7]\r\nToeStrength=1.4\r\nShoulderStart=0.52\r\n"
        "ColorBlend=0.3\r\n") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("[Image]\r\nExposure=0.25\r\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "Exposure=0.25\r\nContrast=-20\r\nBlacks=-40\r\n") !=
        std::string::npos);
}

void CheckRewrittenRoundTrip(const HdrSettings& settings)
{
    const auto roundTripRewrite = RewriteHdrSettingsIni(
        LegacyProductionConfiguration(), settings);
    FO4HDR_CHECK(RewriteHdrSettingsIni(
        roundTripRewrite, settings) == roundTripRewrite);
    const auto reloadedRewrite =
        ParsePluginConfiguration(roundTripRewrite);
    FO4HDR_CHECK(reloadedRewrite.valid);
    FO4HDR_CHECK(reloadedRewrite.value.exposureEv == settings.exposureEv);
    FO4HDR_CHECK(reloadedRewrite.value.toneMapperParameters ==
        settings.toneMapperParameters);
    FO4HDR_CHECK(reloadedRewrite.value.colorGradingMethod == settings.colorGradingMethod);
    FO4HDR_CHECK(test::Approx(reloadedRewrite.value.sceneExposureMultiplier,
        std::exp2(settings.exposureEv), 1.0e-15));
    FO4HDR_CHECK(reloadedRewrite.value.blacks == settings.blacks);
    FO4HDR_CHECK(reloadedRewrite.value.vanillaGradeLuminancePercent ==
        settings.vanillaGradeLuminancePercent);
    FO4HDR_CHECK(reloadedRewrite.value.vanillaGradeColorPercent ==
        settings.vanillaGradeColorPercent);
    auto resetParameters = settings;
    resetParameters.toneMapperParameters = {};
    resetParameters.vanillaGradeLuminancePercent = 100.0;
    resetParameters.vanillaGradeColorPercent = 100.0;
    const auto resetParametersReloaded = ParsePluginConfiguration(
        RewriteHdrSettingsIni(
            roundTripRewrite, resetParameters));
    FO4HDR_CHECK(resetParametersReloaded.valid);
    FO4HDR_CHECK(resetParametersReloaded.value.toneMapperParameters ==
        ToneMapperParameters{});
    FO4HDR_CHECK(
        resetParametersReloaded.value.vanillaGradeLuminancePercent == 100.0);
    FO4HDR_CHECK(
        resetParametersReloaded.value.vanillaGradeColorPercent == 100.0);
}

void CheckRewrittenEffects(const std::string& rewritten)
{
    FO4HDR_CHECK(rewritten.find("[Effects]\r\n") != std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "VanillaColorGradingStrength=100\r\n"
        "VanillaGradeLuminance=65\r\nVanillaGradeColor=35\r\n"
        "LutProfilesEnabled=1\r\n"
        "BloomStrength=175\r\n") != std::string::npos);
    FO4HDR_CHECK(rewritten.find("BloomStrength=175\r\n") !=
        std::string::npos);
}


void CheckRewrittenMigration(const HdrSettings& settings)
{
    constexpr std::string_view legacyExposure =
        "[ToneMapper]\nAlgorithm=ACES2LMT\nExposureEV=-0.5\n"
        "[Image]\nGradeTransfer=PreserveHDRColor\nContrast=0\n";
    const auto migrated = RewriteHdrSettingsIni(
        legacyExposure, settings);
    FO4HDR_CHECK(migrated.find("ExposureEV") == std::string::npos);
    FO4HDR_CHECK(migrated.find("GradeTransfer") == std::string::npos);
    FO4HDR_CHECK(migrated.find("Exposure=0.25") != std::string::npos);

    auto precise = settings;
    precise.exposureEv = 0.123456;
    const auto preciseSaved = RewriteHdrSettingsIni(
        LegacyProductionConfiguration(), precise);
    const auto preciseLoaded = ParsePluginConfiguration(preciseSaved);
    FO4HDR_CHECK(preciseLoaded.valid);
    FO4HDR_CHECK(preciseLoaded.value.exposureEv == 0.123);
}

void TestSettingsPersistenceRewrite()
{
    HdrSettings settings{};

    settings.productionAlgorithm = ProductionToneMapperAlgorithm::aces2Lmt;
    settings.toneMapperParameters = {
        .acesLookStrength = 0.35,
        .agxMiddleGrayEv = -0.25,
        .agxContrast = 2.8,
        .agxHdrPurity = 0.55,
        .agxHdrShoulderPower = 2.05,
        .gt7ToeStrength = 1.4,
        .gt7ShoulderStart = 0.52,
        .gt7ColorBlend = 0.3};
    settings.colorGradingMethod = ColorGradingMethod::nativeReference;
    settings.displayPeakNits = 430.0;
    settings.uiPaperWhiteNits = 180.0;
    settings.videoWhiteNits = 220.0;
    settings.exposureEv = 0.25;
    settings.contrast = -20.0;
    settings.blacks = -40.0;
    settings.vanillaGradeLuminancePercent = 65.0;
    settings.vanillaGradeColorPercent = 35.0;
    settings.bloomStrengthPercent = 175.0;
    constexpr std::string_view original =
        "# keep this comment\r\n"
        "[General]\r\nEnabled=1\r\nUnknown=kept\r\n"
        "[Output]\r\nDisplayPeakNits=1000\r\n"
        "UiPaperWhiteNits=203\r\n"
        "[Effects]\r\nThirdPartyNumber=2\r\n"
        "ThirdPartyEffect=kept\r\n";
    const auto rewritten = RewriteHdrSettingsIni(
        original, settings);
    const auto bomRewritten = RewriteHdrSettingsIni(
        std::string("\xEF\xBB\xBF") + LegacyProductionConfiguration(),
        settings);
    CheckRewrittenOutput(rewritten, bomRewritten);
    CheckRewrittenRoundTrip(settings);
    CheckRewrittenEffects(rewritten);
    CheckRewrittenMigration(settings);
    settings.colorGradingMethod = static_cast<ColorGradingMethod>(0xFF);
    FO4HDR_CHECK(RewriteHdrSettingsIni(
        original, settings) == original);
    settings.colorGradingMethod = ColorGradingMethod::nativeReference;
    settings.displayPeakNits = 399.0;
    FO4HDR_CHECK(RewriteHdrSettingsIni(
        original, settings) == original);
}

constexpr std::wstring_view kTestDisplayIdentity =
        L"\\\\?\\DISPLAY#Fallout4HDR_\x03A9#{A1B2C3D4}";


template <class Request>
std::uint64_t RetryProfileRequest(Request request)
{
    for (unsigned attempt = 0; attempt < 2000; ++attempt)
    {
        const auto requested = request();
        if (requested != 0) return requested;
        Sleep(1);
    }
    return 0;
}

std::uint64_t RequestProfileSave(const HdrSettings& settings)
{
    return RetryProfileRequest([&] { return RequestDisplayCalibrationSave(kTestDisplayIdentity, settings); });
}

std::uint64_t RequestProfileLoad(std::wstring_view identity)
{
    return RetryProfileRequest([&] { return RequestDisplayCalibrationLoad(identity); });
}
SettingsPersistenceSnapshot WaitProfile(std::uint64_t requested)
{

    SettingsPersistenceSnapshot result{};
    for (unsigned attempt = 0; attempt < 2000; ++attempt)
    {
        result = ReadSettingsPersistenceSnapshot();
        const auto busy = result.displayCalibrationState == SettingsPersistenceState::saving ||
            result.displayCalibrationState == SettingsPersistenceState::loading;
        if (result.displayCalibrationGeneration == requested && !busy)
            return result;
        Sleep(1);
    }
    return result;
}

struct PersistenceFixture
{
    std::filesystem::path root;
    std::filesystem::path path;
    std::filesystem::path profileDirectory;
    std::filesystem::path personalPath;
    HdrSettings settings;

};
PersistenceFixture CreatePersistenceFixture()
{

    const auto root = std::filesystem::temp_directory_path() /
        ("Fallout4HDR-settings-save-" +
         std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    FO4HDR_CHECK(std::filesystem::create_directory(root));
    const auto path = root / "Fallout4HDR.ini";
    const auto profileDirectory = root / "Fallout4HDR";
    FO4HDR_CHECK(std::filesystem::create_directory(profileDirectory));
    const auto personalPath = profileDirectory / "Personal.ini";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << LegacyProductionConfiguration()
               << "\n[Image]\nGradeTransfer=PreserveHDRColor\n"
               << "[LUT:stale.dds]\nColor=1\n";
    }
    {
        std::ofstream output(
            profileDirectory / "Z.ini", std::ios::binary | std::ios::trunc);
        output << "[LUT:textures/distributed.dds]\nColor=66\n";
    }
    auto startup = LoadPluginConfiguration(path);
    FO4HDR_CHECK(startup.valid);
    FO4HDR_CHECK(startup.distributedProfiles.count == 1);
    FO4HDR_CHECK(InitializeSettingsPersistence(
        path, startup.distributedProfiles));
    startup.distributedProfiles = {};
    HdrSettings settings{};
    settings.displayPeakNits = 430.0;
    settings.exposureEv = 0.75;
    settings.blacks = 35.0;
    settings.vanillaGradeLuminancePercent = 60.0;
    settings.vanillaGradeColorPercent = 30.0;
    FO4HDR_CHECK(SetLutGradeProfile(settings.lutGradeProfiles,
        "Textures\\Effects\\Night.DDS", { 60.0, true, 25.0, true}));
    settings.toneMapperParameters = {
        .acesLookStrength = 0.4,
        .agxMiddleGrayEv = 0.25,
        .agxContrast = 3.0,
        .agxHdrPurity = 0.55,
        .agxHdrShoulderPower = 2.1,
        .gt7ToeStrength = 1.5,
        .gt7ShoulderStart = 0.5,
        .gt7ColorBlend = 0.2};
    settings.colorGradingMethod = ColorGradingMethod::nativeReference;

    return {root, path, profileDirectory, personalPath, settings};
}

void TestPersistedMainSettings(PersistenceFixture& fixture)
{
    auto& [root, path, profileDirectory, personalPath, settings] = fixture;
    const auto generation =
        RequestHdrSettingsSave(settings);
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
    FO4HDR_CHECK(persisted.find("[Image]\nExposure=0.75") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("Blacks=35") != std::string::npos);
    FO4HDR_CHECK(persisted.find("VanillaGradeLuminance=60") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("VanillaGradeColor=30") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("stale.dds") == std::string::npos);
    FO4HDR_CHECK(persisted.find("[LUT:") == std::string::npos);
    std::ifstream gradeProfileInput(personalPath, std::ios::binary);
    const std::string persistedProfiles(
        std::istreambuf_iterator<char>(gradeProfileInput), {});
    gradeProfileInput.close();
    FO4HDR_CHECK(persistedProfiles.find(
        "[LUT:textures/effects/night.dds]\nColor=25\nLuminance=60\n") !=
        std::string::npos);
    FO4HDR_CHECK(persistedProfiles.find(
        "[LUT:textures/distributed.dds]\nInherit=1\n") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("[ACES2LMT]\nLookStrength=0.4") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("[AgX]\nMiddleGray=0.25\nContrast=3") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find(
        "[GT7]\nToeStrength=1.5\nShoulderStart=0.5") !=
        std::string::npos);
    FO4HDR_CHECK(persisted.find("GradeTransfer") == std::string::npos);
    FO4HDR_CHECK(persisted.find("ExposureEV") == std::string::npos);
    const auto reloaded = LoadPluginConfiguration(path);
    FO4HDR_CHECK(reloaded.valid);
    FO4HDR_CHECK(!reloaded.value.developerMode);
    FO4HDR_CHECK(reloaded.value.blacks == settings.blacks);
    FO4HDR_CHECK(reloaded.value.vanillaGradeLuminancePercent ==
        settings.vanillaGradeLuminancePercent);
    FO4HDR_CHECK(reloaded.value.vanillaGradeColorPercent ==
        settings.vanillaGradeColorPercent);
    FO4HDR_CHECK(reloaded.value.lutGradeProfiles ==
        settings.lutGradeProfiles);
    FO4HDR_CHECK(reloaded.value.toneMapperParameters ==
        settings.toneMapperParameters);
    FO4HDR_CHECK(reloaded.value.colorGradingMethod == settings.colorGradingMethod);
    input.close();
}


void TestDisplayProfileLoad()
{
    auto profileGeneration = RequestProfileLoad(kTestDisplayIdentity);
    FO4HDR_CHECK(profileGeneration != 0);
    auto profileSnapshot = WaitProfile(profileGeneration);
    FO4HDR_CHECK(profileSnapshot.displayCalibrationState ==
        SettingsPersistenceState::loaded);
    FO4HDR_CHECK(std::wstring_view(
        profileSnapshot.displayCalibration.monitorDevicePath.data()) ==
        kTestDisplayIdentity);
    FO4HDR_CHECK(profileSnapshot.displayCalibration.displayPeakNits == 430.0);
    FO4HDR_CHECK(profileSnapshot.displayCalibration.uiPaperWhiteNits == 180.0);
    FO4HDR_CHECK(profileSnapshot.displayCalibration.videoWhiteNits == 220.0);

    constexpr std::wstring_view otherDisplayIdentity =
        L"\\\\?\\DISPLAY#Fallout4HDR_Other#{A1B2C3D4}";
    profileGeneration = RequestProfileLoad(otherDisplayIdentity);
    FO4HDR_CHECK(profileGeneration != 0);
    profileSnapshot = WaitProfile(profileGeneration);
    FO4HDR_CHECK(profileSnapshot.displayCalibrationState ==
        SettingsPersistenceState::missing);
    FO4HDR_CHECK(std::wstring_view(
        profileSnapshot.displayCalibration.monitorDevicePath.data()) ==
        otherDisplayIdentity);
}

void TestInvalidDisplayProfiles(const std::filesystem::path& path, const std::string& validProfileText)
{
    std::uint64_t profileGeneration{};
    const auto profileSection = validProfileText.find("[DisplayProfile:");
    FO4HDR_CHECK(profileSection != std::string::npos);
    const auto writeProfileText = [&](std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    };
    writeProfileText(validProfileText + validProfileText.substr(profileSection));
    profileGeneration = RequestProfileLoad(kTestDisplayIdentity);
    FO4HDR_CHECK(profileGeneration != 0);
    FO4HDR_CHECK(WaitProfile(profileGeneration).displayCalibrationState ==
        SettingsPersistenceState::failed);

    auto partialProfileText = validProfileText;
    const auto partialStart = partialProfileText.find("VideoWhiteNits=220");
    FO4HDR_CHECK(partialStart != std::string::npos);
    partialProfileText.erase(partialStart,
        partialProfileText.find('\n', partialStart) - partialStart + 1);
    writeProfileText(partialProfileText);
    profileGeneration = RequestProfileLoad(kTestDisplayIdentity);
    FO4HDR_CHECK(profileGeneration != 0);
    FO4HDR_CHECK(WaitProfile(profileGeneration).displayCalibrationState ==
        SettingsPersistenceState::failed);

    auto invalidProfileText = validProfileText;
    const auto invalidStart = invalidProfileText.find(
        "DisplayPeakNits=430", profileSection);
    FO4HDR_CHECK(invalidStart != std::string::npos);
    invalidProfileText.replace(invalidStart,
        std::string_view("DisplayPeakNits=430").size(),
        "DisplayPeakNits=399");
    writeProfileText(invalidProfileText);
    profileGeneration = RequestProfileLoad(kTestDisplayIdentity);
    FO4HDR_CHECK(profileGeneration != 0);
    FO4HDR_CHECK(WaitProfile(profileGeneration).displayCalibrationState ==
        SettingsPersistenceState::failed);
    writeProfileText(validProfileText);

    FO4HDR_CHECK(RequestDisplayCalibrationLoad({}) == 0);
    FO4HDR_CHECK(RequestDisplayCalibrationLoad(L"bad]\n[Output") == 0);
    FO4HDR_CHECK(RequestDisplayCalibrationLoad(
        std::wstring(kMaximumDisplayIdentityLength + 1, L'x')) == 0);
}

void TestPersistedDisplayProfiles(PersistenceFixture& fixture)
{
    auto& [root, path, profileDirectory, personalPath, settings] = fixture;
    {
        std::ofstream append(path, std::ios::binary | std::ios::app);
        append << "\n[UnrelatedProfileTest]\nKeep=1\n";
    }
    settings.displayPeakNits = 430.0;
    settings.uiPaperWhiteNits = 180.0;
    settings.videoWhiteNits = 220.0;

    auto profileGeneration = RequestProfileSave(settings);
    FO4HDR_CHECK(profileGeneration != 0);
    auto profileSnapshot = WaitProfile(profileGeneration);
    FO4HDR_CHECK(profileSnapshot.displayCalibrationState ==
        SettingsPersistenceState::saved);
    FO4HDR_CHECK(std::wstring_view(
        profileSnapshot.displayCalibration.monitorDevicePath.data()) ==
        kTestDisplayIdentity);
    FO4HDR_CHECK(profileSnapshot.displayCalibration.displayPeakNits == 430.0);
    FO4HDR_CHECK(profileSnapshot.displayCalibration.uiPaperWhiteNits == 180.0);
    FO4HDR_CHECK(profileSnapshot.displayCalibration.videoWhiteNits == 220.0);
    std::ifstream profileInput(path, std::ios::binary);
    std::string validProfileText(
        std::istreambuf_iterator<char>(profileInput), {});
    profileInput.close();
    FO4HDR_CHECK(validProfileText.find("[UnrelatedProfileTest]\nKeep=1\n") !=
        std::string::npos);
    const auto firstProfileSection = validProfileText.find("[DisplayProfile:");
    FO4HDR_CHECK(firstProfileSection != std::string::npos);
    const auto firstProfileText = validProfileText.substr(firstProfileSection);
    FO4HDR_CHECK(firstProfileText.find("Exposure=") == std::string::npos);
    FO4HDR_CHECK(firstProfileText.find("Algorithm=") == std::string::npos);
    const auto firstProfileHeaderEnd =
        validProfileText.find('\n', firstProfileSection);
    FO4HDR_CHECK(firstProfileHeaderEnd != std::string::npos);
    validProfileText.insert(firstProfileHeaderEnd + 1, "VendorKey=kept\n");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << validProfileText;
    }
    profileGeneration = RequestProfileSave(settings);
    FO4HDR_CHECK(profileGeneration != 0);
    FO4HDR_CHECK(WaitProfile(profileGeneration).displayCalibrationState ==
        SettingsPersistenceState::saved);
    profileInput.open(path, std::ios::binary);
    validProfileText.assign(std::istreambuf_iterator<char>(profileInput), {});
    profileInput.close();
    FO4HDR_CHECK(validProfileText.find("VendorKey=kept\n") !=
        std::string::npos);

    TestDisplayProfileLoad();

    TestInvalidDisplayProfiles(path, validProfileText);
}

void TestSettingsPersistenceSave()
{
    auto fixture = CreatePersistenceFixture();
    TestPersistedMainSettings(fixture);
    TestPersistedDisplayProfiles(fixture);
    const auto& root = fixture.root;
    std::error_code error;
    FO4HDR_CHECK(std::filesystem::remove_all(root, error) != 0 && !error);
}

}  // namespace fo4hdr::tests
