// SPDX-License-Identifier: GPL-3.0-or-later

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "SettingsPersistence.hpp"
#include "TestHarness.hpp"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fo4hdr::tests {

void WriteGradePack(const std::filesystem::path& path,
                           std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    FO4HDR_CHECK(output.good());
}
std::string ReadGradePack(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
std::size_t CountGradePackText(std::string_view text, std::string_view value)
{
    std::size_t count{};
    for (auto position = text.find(value);
         position != std::string_view::npos;
         position = text.find(value, position + value.size()))
        ++count;
    return count;
}

void TestGradePackParsing()
{
    const auto parsed = ParseGradeProfileConfiguration(
        "\xEF\xBB\xBF"
        "[General]\nEnabled=invalid-but-ignored\n"
        "[LUT:Textures\\Effects\\Night.DDS]\nColor=35\n"
        "[IMGS:Fallout4.ESM|00ABCD]\nInherit=1\n");
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.lutGradeProfiles.count == 1);
    FO4HDR_CHECK(FindLutGradeProfile(parsed.value.lutGradeProfiles,
        "textures/effects/night.dds") != nullptr);
    FO4HDR_CHECK(parsed.profileInheritances.count == 1);
    FO4HDR_CHECK(FindImageSpaceGradeProfile(parsed.profileInheritances,
        "fallout4.esm|00abcd") != nullptr);

    auto invalid = ParseGradeProfileConfiguration(
        "[LUT:textures/a.dds]\nInherit=0\n");
    FO4HDR_CHECK(!invalid.valid);
    FO4HDR_CHECK(invalid.error ==
        PluginConfigurationError::invalidBoolean);
    FO4HDR_CHECK(invalid.line == 2);
    invalid = ParseGradeProfileConfiguration(
        "[LUT:textures/a.dds]\nInherit=1\nColor=50\n");
    FO4HDR_CHECK(!invalid.valid);
    FO4HDR_CHECK(invalid.error ==
        PluginConfigurationError::duplicateKey);
    FO4HDR_CHECK(invalid.line == 3);
}

void TestMissingGradePacks(const std::filesystem::path& root)
{
    const auto noProfilesPath = root / "NoProfiles.ini";
    WriteGradePack(noProfilesPath, LegacyProductionConfiguration());
    const auto noProfiles = LoadPluginConfiguration(noProfilesPath);
    FO4HDR_CHECK(noProfiles.valid);
    FO4HDR_CHECK(noProfiles.distributedProfiles.count == 0);
    FO4HDR_CHECK(noProfiles.profileIssues.empty());
}

void TestMalformedPersonalGradePack(const std::filesystem::path& root)
{
    const auto malformedPath = root / "Malformed.ini";
    const auto malformedDirectory = root / "Malformed";
    FO4HDR_CHECK(std::filesystem::create_directory(malformedDirectory));
    WriteGradePack(malformedPath, LegacyProductionConfiguration());
    WriteGradePack(malformedDirectory / "Z.ini",
        "[LUT:textures/still-loaded.dds]\nColor=12\n");
    WriteGradePack(malformedDirectory / "Personal.ini",
        "[LUT:textures/rejected-personal.dds]\nColor=nan\n");
    const auto malformed = LoadPluginConfiguration(malformedPath);
    FO4HDR_CHECK(malformed.valid);
    FO4HDR_CHECK(malformed.profileIssues.size() == 1);
    FO4HDR_CHECK(malformed.profileIssues[0].path ==
        malformedDirectory / "Personal.ini");
    FO4HDR_CHECK(FindLutGradeProfile(malformed.value.lutGradeProfiles,
        "textures/still-loaded.dds") != nullptr);
    FO4HDR_CHECK(FindLutGradeProfile(malformed.value.lutGradeProfiles,
        "textures/rejected-personal.dds") == nullptr);
}

void TestGradePackOverflow(const std::filesystem::path& root)
{
    const auto overflowPath = root / "Overflow.ini";
    const auto overflowDirectory = root / "Overflow";
    FO4HDR_CHECK(std::filesystem::create_directory(overflowDirectory));
    WriteGradePack(overflowPath, LegacyProductionConfiguration());
    std::string highProfiles;
    for (std::size_t index = 0; index < 63; ++index)
        highProfiles += "[LUT:textures/high-" + std::to_string(index) +
            ".dds]\nColor=1\n";
    WriteGradePack(overflowDirectory / "Z.ini", highProfiles);
    WriteGradePack(overflowDirectory / "A.ini",
        "[LUT:textures/low-0.dds]\nColor=2\n"
        "[LUT:textures/low-1.dds]\nInherit=1\n");
    const auto overflow = LoadPluginConfiguration(overflowPath);
    FO4HDR_CHECK(overflow.valid);
    FO4HDR_CHECK(overflow.distributedProfiles.count == 63);
    FO4HDR_CHECK(overflow.profileIssues.size() == 1);
    FO4HDR_CHECK(overflow.profileIssues[0].path ==
        overflowDirectory / "A.ini");
    FO4HDR_CHECK(overflow.profileIssues[0].error ==
        PluginConfigurationError::valueOutOfRange);
    FO4HDR_CHECK(overflow.profileIssues[0].line == 0);
    FO4HDR_CHECK(FindLutGradeProfile(overflow.distributedProfiles,
        "textures/low-0.dds") == nullptr);
}

void TestGradePackCapacity(const std::filesystem::path& root)
{
        LutGradeProfiles baseline{};
        HdrSettings boundarySettings{};
        std::string baselineText;
    for (std::size_t index = 0; index < kMaximumLutGradeProfiles; ++index)
    {
        const auto baselineKey =
            "textures/baseline-" + std::to_string(index) + ".dds";
        const auto personalKey =
            "textures/personal-" + std::to_string(index) + ".dds";
        FO4HDR_CHECK(SetLutGradeProfile(
            baseline, baselineKey, { 100.0, false, 10.0, true}));
        FO4HDR_CHECK(SetLutGradeProfile(boundarySettings.lutGradeProfiles,
            personalKey, { 100.0, false, 20.0, true}));
        baselineText += "[LUT:" + baselineKey + "]\nColor=10\n";
    }
    const auto boundaryText = RewriteGradeProfilesIni(
        {}, boundarySettings.lutGradeProfiles, baseline);
    FO4HDR_CHECK(CountGradePackText(boundaryText, "Inherit=1") ==
        kMaximumLutGradeProfiles);
    const auto boundaryParsed = ParseGradeProfileConfiguration(boundaryText);
    FO4HDR_CHECK(boundaryParsed.valid);
    FO4HDR_CHECK(boundaryParsed.value.lutGradeProfiles.count ==
        kMaximumLutGradeProfiles);
    FO4HDR_CHECK(boundaryParsed.profileInheritances.count ==
        kMaximumLutGradeProfiles);
    const auto boundaryPath = root / "Boundary.ini";
    const auto boundaryDirectory = root / "Boundary";
    FO4HDR_CHECK(std::filesystem::create_directory(boundaryDirectory));
    WriteGradePack(boundaryPath, LegacyProductionConfiguration());
    WriteGradePack(boundaryDirectory / "Personal.ini", boundaryText);
    WriteGradePack(boundaryDirectory / "Z.ini", baselineText);
    const auto boundary = LoadPluginConfiguration(boundaryPath);
    FO4HDR_CHECK(boundary.valid);
    FO4HDR_CHECK(boundary.value.lutGradeProfiles.count ==
        kMaximumLutGradeProfiles);
    FO4HDR_CHECK(FindLutGradeProfile(boundary.value.lutGradeProfiles,
        "textures/personal-63.dds") != nullptr);
    FO4HDR_CHECK(FindLutGradeProfile(boundary.value.lutGradeProfiles,
        "textures/baseline-0.dds") == nullptr);
}

struct GradePackFixture
{
    std::filesystem::path mainPath;
    std::filesystem::path profileDirectory;
    std::filesystem::path personalPath;
    std::filesystem::path highPath;
    std::filesystem::path lowPath;
    std::filesystem::path invalidPath;
    std::string mainText;
    std::string personalText;
};
void WriteDistributedGradePacks(const GradePackFixture& fixture)
{
    const auto& [mainPath, profileDirectory, personalPath, highPath, lowPath, invalidPath, mainText, personalText] = fixture;
        WriteGradePack(highPath, std::string("\xEF\xBB\xBF") + R"ini([Image]
Exposure=4
[LUT:Textures\Shared.DDS]
Color=82
[LUT:textures/z-only.dds]
Luminance=72
[LUT:textures/masked-by-z.dds]
Inherit=1
[IMGS:Fallout4.esm|000111]
Color=61
)ini");
        WriteGradePack(lowPath, R"ini([LUT:textures/shared.dds]
Luminance=21
[IMGS:FALLOUT4.ESM|000111]
Luminance=19
[LUT:textures/a-only.dds]
Luminance=31
[LUT:textures/masked-by-z.dds]
Color=11
[IMGS:fallout4.esm|000222]
Luminance=41
)ini");
        WriteGradePack(invalidPath, R"ini([LUT:textures/invalid-file-good.dds]
Color=1
[LUT:textures/invalid-file-bad.dds]
Color=nan
)ini");
    WriteGradePack(profileDirectory / "ignored.txt",
        "[LUT:textures/ignored-extension.dds]\nColor=1\n");
    FO4HDR_CHECK(std::filesystem::create_directory(
        profileDirectory / "directory.ini"));
    FO4HDR_CHECK(std::filesystem::create_directory(
        profileDirectory / "nested"));
    WriteGradePack(profileDirectory / "nested" / "ignored.ini",
        "[LUT:textures/ignored-nested.dds]\nColor=1\n");
}

GradePackFixture CreateGradePackFixture(const std::filesystem::path& root)
{
    const auto directory = root / "Fallout4HDR";
    GradePackFixture fixture{root / "Fallout4HDR.ini", directory, directory / "Personal.ini",
        directory / "z-HIGH.INI", directory / "A-low.iNi", directory / "m-invalid.ini"};
    FO4HDR_CHECK(std::filesystem::create_directory(directory));
    WriteDistributedGradePacks(fixture);
    fixture.mainText = LegacyProductionConfiguration() + R"ini(
[LUT:textures/shared.dds]
Color=55
[LUT:textures/legacy-only.dds]
Color=44
[LUT:textures/main-masked.dds]
Color=42
)ini";
    fixture.personalText = std::string("\xEF\xBB\xBF") + R"ini([Unrelated]
Keep=1
[LUT:TEXTURES/SHARED.dds]
Luminance=91
[LUT:textures/a-only.dds]
Inherit=1
[LUT:textures/main-masked.dds]
Inherit=1
[IMGS:fallout4.esm|000333]
Color=51
)ini";
    WriteGradePack(fixture.mainPath, fixture.mainText);
    WriteGradePack(fixture.personalPath, fixture.personalText);

    return fixture;
}

void CheckGradePackPrecedence(const PluginConfigurationResult& loaded, const GradePackFixture& fixture)
{
    const auto& [mainPath, profileDirectory, personalPath, highPath, lowPath, invalidPath, mainText, personalText] = fixture;
    FO4HDR_CHECK(loaded.valid);
    FO4HDR_CHECK(loaded.value.productionRequested);
    FO4HDR_CHECK(loaded.value.exposureEv == 0.0);
    FO4HDR_CHECK(loaded.profileIssues.size() == 1);
    FO4HDR_CHECK(loaded.profileIssues[0].path == invalidPath);
    FO4HDR_CHECK(loaded.profileIssues[0].error ==
        PluginConfigurationError::invalidNumber);
    FO4HDR_CHECK(loaded.profileIssues[0].line == 4);
    FO4HDR_CHECK(loaded.distributedProfiles.count == 5);
    const auto* distributedShared = FindLutGradeProfile(
        loaded.distributedProfiles, "textures/shared.dds");
    FO4HDR_CHECK(distributedShared != nullptr);
    FO4HDR_CHECK(distributedShared->overrideColor &&
        !distributedShared->overrideLuminance);
    FO4HDR_CHECK(distributedShared->colorPercent == 82.0);
    FO4HDR_CHECK(FindLutGradeProfile(loaded.distributedProfiles,
        "textures/a-only.dds") != nullptr);
    FO4HDR_CHECK(FindLutGradeProfile(loaded.distributedProfiles,
        "textures/masked-by-z.dds") == nullptr);
    const auto* distributedImageSpace = FindImageSpaceGradeProfile(
        loaded.distributedProfiles, "fallout4.esm|000111");
    FO4HDR_CHECK(distributedImageSpace != nullptr);
    FO4HDR_CHECK(distributedImageSpace->overrideColor &&
        !distributedImageSpace->overrideLuminance);
    FO4HDR_CHECK(distributedImageSpace->colorPercent == 61.0);
    const auto* effectiveShared = FindLutGradeProfile(
        loaded.value.lutGradeProfiles, "textures/shared.dds");
    FO4HDR_CHECK(effectiveShared != nullptr);
    FO4HDR_CHECK(!effectiveShared->overrideColor &&
        effectiveShared->overrideLuminance);
    FO4HDR_CHECK(effectiveShared->luminancePercent == 91.0);
    FO4HDR_CHECK(FindLutGradeProfile(loaded.value.lutGradeProfiles,
        "textures/a-only.dds") == nullptr);
    FO4HDR_CHECK(FindLutGradeProfile(loaded.value.lutGradeProfiles,
        "textures/main-masked.dds") == nullptr);
    FO4HDR_CHECK(FindLutGradeProfile(loaded.value.lutGradeProfiles,
        "textures/legacy-only.dds") != nullptr);
    FO4HDR_CHECK(FindLutGradeProfile(loaded.value.lutGradeProfiles,
        "textures/z-only.dds") != nullptr);
    FO4HDR_CHECK(FindImageSpaceGradeProfile(loaded.value.lutGradeProfiles,
        "fallout4.esm|000111") != nullptr);
    FO4HDR_CHECK(FindImageSpaceGradeProfile(loaded.value.lutGradeProfiles,
        "fallout4.esm|000222") != nullptr);
    for (const auto ignored : {"textures/invalid-file-good.dds",
             "textures/ignored-extension.dds",
             "textures/ignored-nested.dds"})
        FO4HDR_CHECK(FindLutGradeProfile(
            loaded.value.lutGradeProfiles, ignored) == nullptr);
}

void TestGradePackRewrite(const GradePackFixture& fixture, PluginConfigurationResult loaded)
{
    const auto& [mainPath, profileDirectory, personalPath, highPath, lowPath, invalidPath, mainText, personalText] = fixture;
    auto edited = loaded.value;
    FO4HDR_CHECK(RemoveLutGradeProfile(
        edited.lutGradeProfiles, "textures/z-only.dds"));
    FO4HDR_CHECK(SetLutGradeProfile(edited.lutGradeProfiles,
        "textures/personal.dds", { 100.0, false, 27.0, true}));
    const auto highBefore = ReadGradePack(highPath);
    const auto lowBefore = ReadGradePack(lowPath);
    const auto rewritten = RewriteGradeProfilesIni(
        personalText, edited.lutGradeProfiles,
        loaded.distributedProfiles);
    FO4HDR_CHECK(rewritten.find(
        "[LUT:textures/z-only.dds]\nInherit=1\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[LUT:textures/a-only.dds]\nInherit=1\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[LUT:textures/shared.dds]\nLuminance=91\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find(
        "[LUT:textures/personal.dds]\nColor=27\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.starts_with("\xEF\xBB\xBF"));
    FO4HDR_CHECK(rewritten.find("[Unrelated]\nKeep=1\n") !=
        std::string::npos);
    const auto bomFirstProfile = RewriteGradeProfilesIni(
        std::string("\xEF\xBB\xBF") +
            "[LUT:textures/stale-first.dds]\nColor=1\n"
            "[Unrelated]\nKeep=1\n",
        edited.lutGradeProfiles, loaded.distributedProfiles);
    FO4HDR_CHECK(bomFirstProfile.starts_with("\xEF\xBB\xBF"));
    FO4HDR_CHECK(CountGradePackText(bomFirstProfile, "\xEF\xBB\xBF") == 1);
    FO4HDR_CHECK(bomFirstProfile.find("stale-first.dds") ==
        std::string::npos);
    FO4HDR_CHECK(bomFirstProfile.find("[Unrelated]\nKeep=1\n") !=
        std::string::npos);
    FO4HDR_CHECK(rewritten.find("[IMGS:fallout4.esm|000222]") ==
        std::string::npos);
    FO4HDR_CHECK(ReadGradePack(highPath) == highBefore);
    FO4HDR_CHECK(ReadGradePack(lowPath) == lowBefore);
    FO4HDR_CHECK(ReadGradePack(mainPath) == mainText);
    auto globalOnly = edited;
    globalOnly.lutGradeProfiles = {};
    const auto rewrittenMain = RewriteHdrSettingsIni(
        mainText, globalOnly);
    FO4HDR_CHECK(rewrittenMain.find("[LUT:") == std::string::npos);
    WriteGradePack(personalPath, rewritten);
    WriteGradePack(mainPath, rewrittenMain);
    loaded = LoadPluginConfiguration(mainPath);
    FO4HDR_CHECK(loaded.valid);
    FO4HDR_CHECK(FindLutGradeProfile(loaded.value.lutGradeProfiles,
        "textures/z-only.dds") == nullptr);
    FO4HDR_CHECK(FindLutGradeProfile(loaded.value.lutGradeProfiles,
        "textures/personal.dds") != nullptr);
    FO4HDR_CHECK(FindImageSpaceGradeProfile(loaded.value.lutGradeProfiles,
        "fallout4.esm|000222") != nullptr);
}

void TestGradePackPrecedence(const std::filesystem::path& root)
{
    const auto fixture = CreateGradePackFixture(root);
    const auto loaded = LoadPluginConfiguration(fixture.mainPath);
    CheckGradePackPrecedence(loaded, fixture);
    TestMissingGradePacks(root);
    TestMalformedPersonalGradePack(root);
    TestGradePackRewrite(fixture, loaded);
}
void TestGradeProfilePacks()
{
    TestGradePackParsing();

    const auto root = std::filesystem::temp_directory_path() /
        ("Fallout4HDR-profile-packs-" +
         std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    std::error_code error;
    const auto created = std::filesystem::create_directory(root, error);
    FO4HDR_CHECK(created && !error);
    if (!created || error) return;
    TestGradePackPrecedence(root);

    TestGradePackOverflow(root);

    TestGradePackCapacity(root);

    error.clear();
    FO4HDR_CHECK(std::filesystem::remove_all(root, error) != 0 && !error);}

}  // namespace fo4hdr::tests
