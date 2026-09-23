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

SettingsPersistenceSnapshot WaitSaved(std::uint64_t token)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto snapshot = ReadSettingsPersistenceSnapshot();
        if (snapshot.generation == token && snapshot.state != SettingsPersistenceState::saving)
            return snapshot;
        std::this_thread::yield();
    }
    throw std::runtime_error("Resident settings save did not complete");
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
    FO4HDR_CHECK(!LoadPluginConfiguration(path).valid);
    WriteFile(path, kOriginal);
    const auto loaded = LoadPluginConfiguration(path);
    if (!loaded.valid || !InitializeSettingsPersistence(path))
        throw std::runtime_error("Resident settings initialization rejected");
    HdrSettings requested = loaded.value;
    requested.productionAlgorithm = ProductionToneMapperAlgorithm::gt7;
    requested.displayPeakNits = 1500;
    requested.uiPaperWhiteNits = 100;
    requested.videoWhiteNits = 300;
    requested.vanillaColorGradingStrengthPercent = 50;
    requested.colorGradingMethod = ColorGradingMethod::nativeReference;
    FO4HDR_CHECK(SetLutGradeProfile(requested.lutGradeProfiles,
        "textures/lut/authored.dds", {65, true, 35, true}));
    // The resident worker rereads edits made after startup. Completion closes
    // the files; this fixture makes no joined-thread or multi-file atomic claim.
    WriteFile(path, std::string(kOriginal) + "Newest=kept\r\n");
    const auto token = RequestHdrSettingsSave(requested);
    if (!token) throw std::runtime_error("Resident settings save rejected");
    FO4HDR_CHECK(WaitSaved(token).state == SettingsPersistenceState::saved);
    const auto reloaded = LoadPluginConfiguration(path);
    FO4HDR_CHECK(reloaded.valid);
    FO4HDR_CHECK(static_cast<const HdrSettings&>(reloaded.value) == requested);
    CheckPreserved(ReadFile(path));
    FO4HDR_CHECK(!std::filesystem::exists(path.string() + ".tmp"));
}

}  // namespace

void RunHdrPersistenceTests()
{
    CheckRoundTrip();
}

}  // namespace fo4hdr::tests
