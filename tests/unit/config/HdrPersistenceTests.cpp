// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"
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

constexpr std::string_view kOriginal{
    "\xEF\xBB\xBF"
    "; retained heading\r\n[Output]\r\nDisplayPeakNits=1000 ; peak comment\r\n"
    "UiPaperWhiteNits=203\r\nVideoWhiteNits=203\r\nUnknownOutput=kept\r\n"
    "[ToneMapper]\r\nAlgorithm=AgX # operator comment\r\n"
    "[Other]\r\nUntouched = yes\r\n"};

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

void CheckRoundTrip()
{
    const TemporaryDirectory directory;
    const auto path = directory.IniPath();
    FO4HDR_CHECK(LoadPluginConfiguration(path).status == PluginConfigurationStatus::missing);
    WriteFile(path, kOriginal);
    const auto loaded = LoadPluginConfiguration(path);
    FO4HDR_CHECK(loaded.status == PluginConfigurationStatus::loaded);
    auto requested = loaded.value;
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

}  // namespace

void RunHdrPersistenceTests()
{
    CheckParser();
    CheckRoundTrip();
}

}  // namespace fo4hdr::tests
