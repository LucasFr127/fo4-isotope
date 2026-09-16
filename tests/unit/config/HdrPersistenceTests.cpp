// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"
#include "HdrSettingsTestSupport.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace fo4hdr::tests {
namespace {

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("Fallout4HDR-settings-" + std::to_string(token));
        if (!std::filesystem::create_directory(path_))
            throw std::runtime_error("Settings test directory already exists");
    }
    ~TemporaryDirectory() noexcept
    {
        std::error_code error;
        (void)std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] std::filesystem::path IniPath() const { return path_ / "settings.ini"; }

private:
    std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    FO4HDR_CHECK(static_cast<bool>(output));
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    FO4HDR_CHECK(static_cast<bool>(input));
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool QueueSave(SettingsPersistence& persistence, const PluginConfiguration& value)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (persistence.RequestSave(value, 1)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

void CheckPreserved(std::string_view text)
{
    FO4HDR_CHECK(text.starts_with("\xEF\xBB\xBF; retained heading\r\n"));
    FO4HDR_CHECK(text.find(" ; peak comment\r\n") != std::string_view::npos);
    FO4HDR_CHECK(text.find(" # operator comment\r\n") != std::string_view::npos);
    FO4HDR_CHECK(text.find("UnknownOutput=kept\r\n") != std::string_view::npos);
    FO4HDR_CHECK(text.find("[Other]\r\nUntouched = yes\r\nNewest=kept\r\n") !=
        std::string_view::npos);
}

void CheckRoundTrip()
{
    const TemporaryDirectory directory;
    const auto path = directory.IniPath();
    FO4HDR_CHECK(LoadPluginConfiguration(path).status == PluginConfigurationStatus::missing);
    WriteFile(path, kOriginal);
    const auto loaded = LoadPluginConfiguration(path);
    FO4HDR_CHECK(loaded.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(loaded.value.display.colorGradingMethod == ColorGradingMethod::radial);
    FO4HDR_CHECK(ParsePluginConfiguration(std::string(kOriginal) +
        "[Image]\nColorGradingMethod=invalid\n").status == PluginConfigurationStatus::invalid);
    auto requested = loaded.value;
    requested.algorithm = OperatorLutAlgorithm::gt7;
    requested.display = {1500.0, 100.0, 300.0};
    requested.display.vanillaColorGradingStrengthPercent = 50.0;
    requested.display.colorGradingMethod = ColorGradingMethod::nativeReference;
    requested.display.exposureEv = 1;
    requested.display.bloomStrengthPercent = 50;
    requested.display.contrast = 25;
    requested.display.blacks = -20;
    requested.display.highlights = 30;
    requested.display.shadows = -40;
    requested.display.saturationPercent = 120;
    requested.display.temperature = 15;
    requested.display.tint = -10;
    {
        SettingsPersistence persistence;
        FO4HDR_CHECK(persistence.Initialize(path));
        // The worker must reread this edit rather than retain startup bytes.
        WriteFile(path, std::string(kOriginal) + "Newest=kept\r\n");
        FO4HDR_CHECK(QueueSave(persistence, requested));
    }
    const auto reloaded = LoadPluginConfiguration(path);
    FO4HDR_CHECK(reloaded.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(reloaded.value == requested);
    CheckPreserved(ReadFile(path));
    FO4HDR_CHECK(!std::filesystem::exists(path.string() + ".tmp"));
    const auto original = ReadFile(path);
    const std::filesystem::path temporary{path.string() + ".tmp"};
    WriteFile(temporary, "unrelated temporary");
    {
        SettingsPersistence persistence;
        FO4HDR_CHECK(persistence.Initialize(path));
        requested.display.displayPeakNits = 1000.0;
        FO4HDR_CHECK(QueueSave(persistence, requested));
    }
    FO4HDR_CHECK(ReadFile(path) == original);
    FO4HDR_CHECK(ReadFile(temporary) == "unrelated temporary");
}

std::uint64_t QueueProfile(SettingsPersistence& persistence,
    const DisplayCalibrationProfile& profile, bool load)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto token = load ? persistence.RequestDisplayCalibrationLoad(profile) :
            persistence.RequestDisplayCalibrationSave(profile);
        if (token) return token;
        std::this_thread::yield();
    }
    return 0;
}

DisplayCalibrationSnapshot WaitProfile(SettingsPersistence& persistence, std::uint64_t token)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto status = persistence.DisplayCalibrationStatus();
        if (status.completedGeneration == token) return status;
        std::this_thread::yield();
    }
    throw std::runtime_error("Display profile did not complete");
}

void CheckProfileRead(SettingsPersistence& persistence, const DisplayCalibrationProfile& profile)
{
    const auto token = QueueProfile(persistence, profile, true);
    FO4HDR_CHECK(token != 0);
    if (!token) throw std::runtime_error("Display profile load rejected");
    const auto result = WaitProfile(persistence, token);
    FO4HDR_CHECK(result.state == SettingsPersistenceState::loaded);
    FO4HDR_CHECK(result.profile.monitorDevicePath == profile.monitorDevicePath);
    FO4HDR_CHECK(result.profile.displayPeakNits == 1200);
    FO4HDR_CHECK(result.profile.uiPaperWhiteNits == 150);
    FO4HDR_CHECK(result.profile.videoWhiteNits == 175);
}

void CheckProfileFailures(SettingsPersistence& persistence, DisplayCalibrationProfile profile,
    const std::filesystem::path& path)
{
    auto invalid = profile;
    invalid.displayPeakNits = 99;
    FO4HDR_CHECK(persistence.RequestDisplayCalibrationSave(invalid) == 0);
    FO4HDR_CHECK(persistence.RequestDisplayCalibrationLoad({}) == 0);
    profile.monitorDevicePath[0] = L'B';
    const auto missing = QueueProfile(persistence, profile, true);
    FO4HDR_CHECK(missing != 0);
    if (!missing) throw std::runtime_error("Missing display load rejected");
    FO4HDR_CHECK(WaitProfile(persistence, missing).state == SettingsPersistenceState::missing);
    profile.monitorDevicePath[0] = L'A';
    WriteFile(path, ReadFile(path) + "[DisplayProfile:0041]\nDisplayPeakNits=1200\n"
        "UiPaperWhiteNits=150\nVideoWhiteNits=175\n");
    const auto duplicate = QueueProfile(persistence, profile, true);
    FO4HDR_CHECK(duplicate != 0);
    if (!duplicate) throw std::runtime_error("Duplicate display load rejected");
    FO4HDR_CHECK(WaitProfile(persistence, duplicate).state == SettingsPersistenceState::failed);
}

void CheckDisplayProfiles()
{
    const TemporaryDirectory directory;
    const auto path = directory.IniPath();
    WriteFile(path, std::string(kOriginal) + "Newest=kept\r\n");
    SettingsPersistence persistence;
    FO4HDR_CHECK(persistence.Initialize(path));
    DisplayCalibrationProfile profile;
    profile.monitorDevicePath[0] = L'A';
    profile.displayPeakNits = 1200;
    profile.uiPaperWhiteNits = 150;
    profile.videoWhiteNits = 175;
    const auto saved = QueueProfile(persistence, profile, false);
    FO4HDR_CHECK(saved != 0);
    if (!saved) throw std::runtime_error("Display profile save rejected");
    FO4HDR_CHECK(WaitProfile(persistence, saved).state == SettingsPersistenceState::saved);
    CheckPreserved(ReadFile(path));
    CheckProfileRead(persistence, profile);
    CheckProfileFailures(persistence, profile, path);
}

void SeedGradeProfiles(const std::filesystem::path& path)
{
    WriteFile(path, std::string(kOriginal) +
        "[LUT:textures/lut/shared.dds]\nColor=30\n"
        "[LUT:textures/lut/main.dds]\nLuminance=60\n");
    const auto packs = path.parent_path() / path.stem();
    if (!std::filesystem::create_directory(packs))
        throw std::runtime_error("Grade profile directory already exists");
    WriteFile(packs / "Distributed.ini", "[LUT:textures/lut/shared.dds]\nColor=10\n"
        "[LUT:textures/lut/pack.dds]\nColor=20\n");
    WriteFile(packs / "Personal.ini", "[Other]\nRetained=yes\n"
        "[LUT:textures/lut/shared.dds]\nColor=80\n"
        "[LUT:textures/lut/main.dds]\nInherit=1\n");
}

void AddImageSpaceProfile(LutGradeProfiles& profiles)
{
    if (!SetImageSpaceGradeProfile(profiles, "Example.esp|000123", {40, true, 100, false}))
        throw std::runtime_error("ImageSpace grade creation rejected");
    auto* imageSpace = FindImageSpaceGradeProfile(profiles, "example.esp|000123");
    if (!imageSpace) throw std::runtime_error("ImageSpace grade missing");
    FO4HDR_CHECK(imageSpace->luminancePercent == 40 && !imageSpace->overrideColor);
}

void EditGradeProfiles(PluginConfiguration& value)
{
    auto& profiles = value.display.lutGradeProfiles;
    auto* shared = FindLutGradeProfile(profiles, "Textures\\LUT\\Shared.dds");
    if (!shared) throw std::runtime_error("Personal grade override missing");
    FO4HDR_CHECK(shared->colorPercent == 80);
    FO4HDR_CHECK(FindLutGradeProfile(profiles, "textures/lut/main.dds") == nullptr);
    shared->colorPercent = 75;
    AddImageSpaceProfile(profiles);
    if (!RemoveLutGradeProfile(profiles, "textures/lut/pack.dds"))
        throw std::runtime_error("Distributed grade removal rejected");
}

void CheckGradeProfileRoundTrip()
{
    const TemporaryDirectory directory;
    const auto path = directory.IniPath();
    SeedGradeProfiles(path);
    const auto loaded = LoadPluginConfiguration(path);
    if (loaded.status != PluginConfigurationStatus::loaded || !loaded.profileIssues.empty())
        throw std::runtime_error("Grade profile configuration rejected");
    FO4HDR_CHECK(loaded.distributedProfiles.count == 2);
    auto requested = loaded.value;
    EditGradeProfiles(requested);
    {
        SettingsPersistence persistence;
        if (!persistence.Initialize(path, loaded.distributedProfiles) || !QueueSave(persistence, requested))
            throw std::runtime_error("Grade profile save rejected");
    }
    const auto reloaded = LoadPluginConfiguration(path);
    FO4HDR_CHECK(reloaded.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(reloaded.value == requested);
    const auto personal = ReadFile(path.parent_path() / path.stem() / "Personal.ini");
    FO4HDR_CHECK(personal.find("[Other]\nRetained=yes\n") != std::string::npos);
    const auto saved = ParseGradeProfileConfiguration(personal);
    FO4HDR_CHECK(saved.status == PluginConfigurationStatus::loaded);
    FO4HDR_CHECK(FindLutGradeProfile(saved.profileInheritances, "textures/lut/pack.dds") != nullptr);
    FO4HDR_CHECK(ReadFile(path).find("UnknownOutput=kept") != std::string::npos);
}

}  // namespace

void RunHdrPersistenceTests()
{
    CheckRoundTrip();
    CheckDisplayProfiles();
    CheckGradeProfileRoundTrip();
}

}  // namespace fo4hdr::tests
