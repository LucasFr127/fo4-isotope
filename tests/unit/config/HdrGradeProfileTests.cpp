// SPDX-License-Identifier: GPL-3.0-or-later

#include "../UnitTestSuite.hpp"
#include "HdrSettingsTestSupport.hpp"

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "SettingsPersistence.hpp"
#include "TestHarness.hpp"
#include <array>
#include <limits>
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

void TestLutOverrides(LutGradeProfiles& profiles)
{

    FO4HDR_CHECK(SetLutGradeProfile(profiles,
        "Textures\\Effects\\Night.DDS", { 45.0, true, 100.0, false}));
    FO4HDR_CHECK(profiles.count == 1);
    FO4HDR_CHECK(FindLutGradeProfile(
        profiles, "textures/effects/NIGHT.dds") != nullptr);
    constexpr ResolvedLutGrade nightExpected{45.0, 70.0};
    FO4HDR_CHECK(ResolveLutGradeProfile(profiles,
        "TEXTURES\\EFFECTS\\NIGHT.DDS", 80.0, 70.0) ==
        nightExpected);
    constexpr ResolvedLutGrade globalExpected{80.0, 70.0};
    FO4HDR_CHECK(ResolveLutGradeProfile(
        profiles, "textures/effects/missing.dds", 80.0, 70.0) ==
        globalExpected);
    profiles.enabled = false;
    FO4HDR_CHECK(ResolveLutGradeProfile(profiles,
        "textures/effects/night.dds", 80.0, 70.0) ==
        globalExpected);
    profiles.enabled = true;
    FO4HDR_CHECK(SetLutGradeProfile(profiles,
        "textures/effects/night.dds", { 55.0, true, 35.0, true}));
    FO4HDR_CHECK(profiles.count == 1);
    FO4HDR_CHECK(!SetLutGradeProfile(profiles,
        "textures/effects/empty.dds", { 100.0, false, 100.0, false}));
    FO4HDR_CHECK(!SetLutGradeProfile(profiles,
        "textures/effects/invalid.dds", { 101.0, true, 100.0, false}));
    FO4HDR_CHECK(!SetLutGradeProfile(profiles,
        "textures/effects/invalid-unused.dds", {
        std::numeric_limits<double>::quiet_NaN(), false, 50.0, true}));
    FO4HDR_CHECK(ValidateLutGradeProfiles(profiles));
    profiles.entries[0].luminancePercent =
        std::numeric_limits<double>::quiet_NaN();
    FO4HDR_CHECK(!ValidateLutGradeProfiles(profiles));
    profiles.entries[0].luminancePercent = 55.0;
    FO4HDR_CHECK(RemoveLutGradeProfile(
        profiles, "TEXTURES\\EFFECTS\\NIGHT.DDS"));
    FO4HDR_CHECK(profiles.count == 0);
}

void TestImageSpaceOverrides(LutGradeProfiles& profiles)
{
    constexpr ResolvedLutGrade globalExpected{80.0, 70.0};
    FO4HDR_CHECK(SetLutGradeProfile(profiles,
        "textures/effects/night.dds", { 45.0, true, 60.0, true}));
    FO4HDR_CHECK(SetImageSpaceGradeProfile(profiles,
        "Fallout4.ESM|00ABCD", { 100.0, false, 25.0, true}));
    constexpr ResolvedLutGrade layeredExpected{45.0, 25.0};
    FO4HDR_CHECK(ResolveImageSpaceGradeProfile(profiles,
        "textures/effects/night.dds", "fallout4.esm|00abcd", {
        80.0, 70.0}) == layeredExpected);
    constexpr ResolvedLutGrade lutExpected{45.0, 60.0};
    FO4HDR_CHECK(ResolveImageSpaceGradeProfile(profiles,
        "textures/effects/night.dds", {}, { 80.0, 70.0}) == lutExpected);
    FO4HDR_CHECK(ResolveImageSpaceGradeProfile(profiles,
        "textures/effects/night.dds", "unverified", { 80.0, 70.0}) ==
        lutExpected);
    profiles.enabled = false;
    FO4HDR_CHECK(ResolveImageSpaceGradeProfile(profiles,
        "textures/effects/night.dds", "fallout4.esm|00abcd", {
        80.0, 70.0}) == globalExpected);
    profiles.enabled = true;
    FO4HDR_CHECK(RemoveImageSpaceGradeProfile(
        profiles, "FALLOUT4.ESM|00ABCD"));
    FO4HDR_CHECK(FindLutGradeProfile(
        profiles, "textures/effects/night.dds") != nullptr);
    FO4HDR_CHECK(RemoveLutGradeProfile(
        profiles, "textures/effects/night.dds"));
}

void TestGradeProfileCapacity(LutGradeProfiles& profiles)
{
    for (std::size_t index = 0;
         index < kMaximumLutGradeProfiles / 2; ++index)
    {
        const auto key = "plugin" + std::to_string(index) +
            ".esm|000001";
        FO4HDR_CHECK(SetLutGradeProfile(profiles, key, {
            100.0, false, static_cast<double>(index), true}));
        FO4HDR_CHECK(SetImageSpaceGradeProfile(profiles, key, {
            static_cast<double>(index), true, 100.0, false}));
    }
    FO4HDR_CHECK(!SetLutGradeProfile(profiles,
        "textures/overflow.dds", { 100.0, false, 50.0, true}));
    FO4HDR_CHECK(!SetImageSpaceGradeProfile(profiles,
        "overflow.esm|000001", { 50.0, true, 100.0, false}));
    FO4HDR_CHECK(profiles.count == kMaximumLutGradeProfiles);
    FO4HDR_CHECK(ValidateLutGradeProfiles(profiles));
}

void TestGradeProfileParsing()
{
    const auto parsed = ParsePluginConfiguration(
        ExtendedProductionConfiguration() +
        "\n[Effects]\nLutProfilesEnabled=0\n"
        "[LUT:Textures\\Effects\\Night.DDS]\nColor=35\n"
        "[LUT:textures/effects/day.dds]\nLuminance=65\nColor=75\n"
        "[IMGS:Fallout4.ESM|00ABCD]\nLuminance=55\n");
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(!parsed.value.lutGradeProfiles.enabled);
    FO4HDR_CHECK(parsed.value.lutGradeProfiles.count == 3);
    const auto* night = FindLutGradeProfile(
        parsed.value.lutGradeProfiles, "textures/effects/night.dds");
    FO4HDR_CHECK(night != nullptr);
    FO4HDR_CHECK(night->overrideColor && !night->overrideLuminance);
    FO4HDR_CHECK(night->colorPercent == 35.0);
    const auto* imageSpace = FindImageSpaceGradeProfile(
        parsed.value.lutGradeProfiles, "fallout4.esm|00abcd");
    FO4HDR_CHECK(imageSpace != nullptr);
    FO4HDR_CHECK(imageSpace->overrideLuminance &&
        !imageSpace->overrideColor);
    FO4HDR_CHECK(imageSpace->luminancePercent == 55.0);

    for (const auto invalid : {
             "[LUT:bad]path.dds]\nColor=50\n",
             "[LUT:textures/a.dds]\nColor=nan\n",
             "[LUT:textures/a.dds]\nLuminance=101\n",
             "[LUT:textures/a.dds]\nUnknown=1\n",
             "[LUT:textures/a.dds]\nColor=1\nColor=2\n",
             "[LUT:Textures\\A.DDS]\nColor=1\n"
             "[LUT:textures/a.dds]\nLuminance=2\n",
             "[IMGS:fallout4.esm|000000]\nColor=50\n",
             "[IMGS:fallout4.txt|000001]\nColor=50\n",
             "[IMGS:Fallout4.ESM|00ABCD]\nColor=1\n"
             "[IMGS:fallout4.esm|00abcd]\nLuminance=2\n"})
        FO4HDR_CHECK(!ParsePluginConfiguration(
            ExtendedProductionConfiguration() + "\n" + invalid).valid);
}

void TestGradeProfileRoundTrip()
{
    HdrSettings settings{};
    settings.lutGradeProfiles.enabled = false;
    FO4HDR_CHECK(SetLutGradeProfile(settings.lutGradeProfiles,
        "Textures\\Effects\\Night.DDS", { 65.0, true, 35.0, true}));
    FO4HDR_CHECK(SetImageSpaceGradeProfile(settings.lutGradeProfiles,
        "Fallout4.ESM|00ABCD", { 55.0, true, 45.0, true}));
    const auto original = LegacyProductionConfiguration() +
        "[LUT:stale.dds]\nColor=1\nStaleOwnedValue=removed\n"
        "[IMGS:stale.esm|000001]\nColor=2\n"
        "[Unrelated]\nKeep=1\n";
    const auto saved = RewriteHdrSettingsIni(original, settings);
    FO4HDR_CHECK(saved.find("stale.dds") == std::string::npos);
    FO4HDR_CHECK(saved.find("stale.esm|000001") == std::string::npos);
    FO4HDR_CHECK(saved.find("StaleOwnedValue") == std::string::npos);
    FO4HDR_CHECK(saved.find("[Unrelated]\nKeep=1\n") !=
        std::string::npos);
    FO4HDR_CHECK(saved.find("LutProfilesEnabled=0") != std::string::npos);
    FO4HDR_CHECK(saved.find(
        "[LUT:textures/effects/night.dds]\nColor=35\nLuminance=65\n") !=
        std::string::npos);
    FO4HDR_CHECK(saved.find(
        "[IMGS:fallout4.esm|00abcd]\nColor=45\nLuminance=55\n") !=
        std::string::npos);
    const auto roundTrip = ParsePluginConfiguration(saved);
    FO4HDR_CHECK(roundTrip.valid);
    FO4HDR_CHECK(roundTrip.value.lutGradeProfiles ==
        settings.lutGradeProfiles);
    const auto resaved = RewriteHdrSettingsIni(saved, settings);
    FO4HDR_CHECK(resaved == saved);}

void TestLutGradeProfiles()
{
    TestGradePathNormalization();
    LutGradeProfiles profiles{};
    TestLutOverrides(profiles);
    TestImageSpaceOverrides(profiles);
    TestGradeProfileCapacity(profiles);
    TestGradeProfileParsing();
    TestGradeProfileRoundTrip();
}

}  // namespace fo4hdr::tests
