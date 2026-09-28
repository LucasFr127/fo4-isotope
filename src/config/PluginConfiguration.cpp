// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace fo4hdr {
namespace {
namespace detail {

bool ValidGradePercent(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

bool ValidGradeOverride(const LutGradeOverride& value) noexcept
{
    return (value.overrideLuminance || value.overrideColor) &&
        ValidGradePercent(value.luminancePercent) &&
        ValidGradePercent(value.colorPercent);
}

std::size_t Utf8SequenceLength(unsigned char lead) noexcept
{
    if (lead >= 0xC2U && lead <= 0xDFU) return 2;
    if (lead >= 0xE0U && lead <= 0xEFU) return 3;
    if (lead >= 0xF0U && lead <= 0xF4U) return 4;
    return 0;
}

bool ValidUtf8Second(unsigned char lead, unsigned char second) noexcept
{
    unsigned char minimum = 0x80U;
    unsigned char maximum = 0xBFU;
    switch (lead)
    {
    case 0xE0U: minimum = 0xA0U; break;
    case 0xEDU: maximum = 0x9FU; break;
    case 0xF0U: minimum = 0x90U; break;
    case 0xF4U: maximum = 0x8FU; break;
    default: break;
    }
    return second >= minimum && second <= maximum;
}

std::size_t ValidUtf8Prefix(std::string_view text) noexcept
{
    const auto lead = static_cast<unsigned char>(text.front());
    const auto length = Utf8SequenceLength(lead);
    if (length == 0 || length > text.size()) return 0;
    if (!ValidUtf8Second(lead, static_cast<unsigned char>(text[1]))) return 0;
    const auto tail = text.substr(2, length - 2);
    if (!std::ranges::all_of(tail, [](unsigned char byte) {
            return (byte & 0xC0U) == 0x80U;
        })) return 0;
    return length;
}

char NormalizeGradeAscii(unsigned char byte) noexcept
{
    if (byte < 0x20U || byte == 0x7FU) return '\0';
    if (byte == '[' || byte == ']') return '\0';
    if (byte == '\\') return '/';
    if (byte >= 'A' && byte <= 'Z') return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

bool ValidGradePathBounds(std::string_view path) noexcept
{
    if (path.empty() || path.size() > kMaximumLutGradePathLength) return false;
    return path.front() != ' ' && path.back() != ' ';
}

bool ValidGradePlugin(std::string_view plugin) noexcept
{
    if (plugin.empty()) return false;
    if (plugin.back() == ' ' || plugin.back() == '.') return false;
    constexpr std::array extensions{std::string_view{".esm"},
        std::string_view{".esp"}, std::string_view{".esl"}};
    if (!std::ranges::any_of(extensions, [plugin](auto extension) {
            return plugin.ends_with(extension);
        })) return false;
    return plugin.find_first_of("<>:\"/\\|?*") == std::string_view::npos;
}

bool ValidGradeFormId(std::string_view value) noexcept
{
    constexpr std::string_view digits{"0123456789abcdef"};
    return value.find_first_not_of(digits) == std::string_view::npos &&
        value.find_first_not_of('0') != std::string_view::npos;
}

bool NormalizeProfileKey(GradeProfileKind kind, std::string_view path,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept
{
    if (kind == GradeProfileKind::lutAsset) return NormalizeLutGradePath(path, normalized);
    return kind == GradeProfileKind::imageSpace && NormalizeImageSpaceGradeKey(path, normalized);
}

const LutGradeProfile* FindGradeProfile(const LutGradeProfiles& profiles,
    GradeProfileKind kind,
    const std::array<char, kMaximumLutGradePathLength + 1>& key) noexcept
{
    const auto count = std::min(profiles.count, profiles.entries.size());
    const auto end = profiles.entries.begin() + count;
    const auto found = std::find_if(profiles.entries.begin(), end,
        [&](const auto& profile) { return profile.kind == kind && profile.path == key; });
    return found == end ? nullptr : &*found;
}

const LutGradeProfile* FindProfileByKey(const LutGradeProfiles& profiles,
    GradeProfileKind kind, std::string_view key) noexcept
{
    std::array<char, kMaximumLutGradePathLength + 1> normalized{};
    if (!NormalizeProfileKey(kind, key, normalized)) return nullptr;
    return FindGradeProfile(profiles, kind, normalized);
}

bool SetGradeProfile(LutGradeProfiles& profiles, LutGradeProfile profile) noexcept
{
    if (profiles.count > profiles.entries.size()) return false;
    if (!ValidGradeOverride({profile.luminancePercent, profile.overrideLuminance,
            profile.colorPercent, profile.overrideColor})) return false;
    auto* existing = const_cast<LutGradeProfile*>(FindGradeProfile(profiles, profile.kind, profile.path));
    if (existing == nullptr)
    {
        if (profiles.count >= profiles.entries.size()) return false;
        existing = &profiles.entries[profiles.count++];
    }
    *existing = profile;
    return true;
}

bool SetProfileByKey(LutGradeProfiles& profiles, GradeProfileKind kind,
    std::string_view key, LutGradeOverride value) noexcept
{
    LutGradeProfile profile{.luminancePercent = value.luminancePercent,
        .colorPercent = value.colorPercent, .overrideLuminance = value.overrideLuminance,
        .overrideColor = value.overrideColor, .kind = kind};
    if (!NormalizeProfileKey(kind, key, profile.path)) return false;
    return SetGradeProfile(profiles, profile);
}

bool RemoveGradeProfile(LutGradeProfiles& profiles,
    GradeProfileKind kind, std::string_view key) noexcept
{
    if (profiles.count > profiles.entries.size()) return false;
    const auto* profile = FindProfileByKey(profiles, kind, key);
    if (profile == nullptr) return false;
    const auto index = static_cast<std::size_t>(profile - profiles.entries.data());
    for (std::size_t current = index + 1; current < profiles.count; ++current)
        profiles.entries[current - 1] = profiles.entries[current];
    profiles.entries[--profiles.count] = {};
    return true;
}

ResolvedLutGrade ApplyGradeProfile(const LutGradeProfile* profile,
    ResolvedLutGrade result) noexcept
{
    if (profile == nullptr) return result;
    if (profile->overrideLuminance) result.luminancePercent = profile->luminancePercent;
    if (profile->overrideColor) result.colorPercent = profile->colorPercent;
    return result;
}

bool ValidateGradeProfile(const LutGradeProfile& profile) noexcept
{
    if (!ValidGradeOverride({profile.luminancePercent, profile.overrideLuminance,
            profile.colorPercent, profile.overrideColor})) return false;
    const auto terminator = std::find(profile.path.begin(), profile.path.end(), '\0');
    if (terminator == profile.path.end()) return false;
    const std::string_view path(profile.path.data(),
        static_cast<std::size_t>(terminator - profile.path.begin()));
    std::array<char, kMaximumLutGradePathLength + 1> normalized{};
    return NormalizeProfileKey(profile.kind, path, normalized) && normalized == profile.path;
}

}  // namespace detail
}  // namespace

[[nodiscard]] bool NormalizeLutGradePath(
    std::string_view path,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept
{
    normalized.fill('\0');
    if (!detail::ValidGradePathBounds(path)) return false;
    for (std::size_t index = 0; index < path.size();)
    {
        const auto byte = static_cast<unsigned char>(path[index]);
        if (byte <= 0x7FU)
        {
            const auto character = detail::NormalizeGradeAscii(byte);
            if (character == '\0') return false;
            normalized[index++] = character;
            continue;
        }
        const auto length = detail::ValidUtf8Prefix(path.substr(index));
        if (length == 0) return false;
        std::copy_n(path.data() + index, length, normalized.data() + index);
        index += length;
    }
    return true;
}

[[nodiscard]] bool NormalizeImageSpaceGradeKey(
    std::string_view key,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept
{
    if (!NormalizeLutGradePath(key, normalized) || key.size() < 12) return false;
    const std::string_view value(normalized.data(), key.size());
    const auto separator = value.size() - 7;
    if (value[separator] != '|') return false;
    return detail::ValidGradePlugin(value.substr(0, separator)) &&
        detail::ValidGradeFormId(value.substr(separator + 1));
}

[[nodiscard]] const LutGradeProfile* FindLutGradeProfile(
    const LutGradeProfiles& profiles, std::string_view path) noexcept
{
    return detail::FindProfileByKey(profiles, GradeProfileKind::lutAsset, path);
}

[[nodiscard]] LutGradeProfile* FindLutGradeProfile(
    LutGradeProfiles& profiles, std::string_view path) noexcept
{
    return const_cast<LutGradeProfile*>(FindLutGradeProfile(static_cast<const LutGradeProfiles&>(profiles), path));
}

[[nodiscard]] const LutGradeProfile* FindImageSpaceGradeProfile(
    const LutGradeProfiles& profiles, std::string_view key) noexcept
{
    return detail::FindProfileByKey(profiles, GradeProfileKind::imageSpace, key);
}

[[nodiscard]] LutGradeProfile* FindImageSpaceGradeProfile(
    LutGradeProfiles& profiles, std::string_view key) noexcept
{
    return const_cast<LutGradeProfile*>(FindImageSpaceGradeProfile(static_cast<const LutGradeProfiles&>(profiles), key));
}

[[nodiscard]] bool SetLutGradeProfile(LutGradeProfiles& profiles,
    std::string_view path, LutGradeOverride value) noexcept
{
    return detail::SetProfileByKey(profiles, GradeProfileKind::lutAsset, path, value);
}

[[nodiscard]] bool SetImageSpaceGradeProfile(LutGradeProfiles& profiles,
    std::string_view key, LutGradeOverride value) noexcept
{
    return detail::SetProfileByKey(profiles, GradeProfileKind::imageSpace, key, value);
}

[[nodiscard]] bool AddLutGradeProfile(LutGradeProfiles& profiles,
    std::string_view path, LutGradeOverride value) noexcept
{
    if (FindLutGradeProfile(profiles, path) != nullptr) return false;
    return SetLutGradeProfile(profiles, path, value);
}

[[nodiscard]] bool RemoveLutGradeProfile(LutGradeProfiles& profiles,
    std::string_view path) noexcept
{
    return detail::RemoveGradeProfile(profiles, GradeProfileKind::lutAsset, path);
}

[[nodiscard]] bool RemoveImageSpaceGradeProfile(LutGradeProfiles& profiles,
    std::string_view key) noexcept
{
    return detail::RemoveGradeProfile(profiles, GradeProfileKind::imageSpace, key);
}

[[nodiscard]] ResolvedLutGrade ResolveLutGradeProfile(
    const LutGradeProfiles& profiles, std::string_view path,
    double globalLuminancePercent, double globalColorPercent) noexcept
{
    const ResolvedLutGrade global{globalLuminancePercent, globalColorPercent};
    if (!profiles.enabled) return global;
    return detail::ApplyGradeProfile(FindLutGradeProfile(profiles, path), global);
}

[[nodiscard]] ResolvedLutGrade ResolveImageSpaceGradeProfile(
    const LutGradeProfiles& profiles, std::string_view lutPath,
    std::string_view imageSpaceKey, ResolvedLutGrade global) noexcept
{
    const auto result = ResolveLutGradeProfile(profiles, lutPath,
        global.luminancePercent, global.colorPercent);
    if (!profiles.enabled) return result;
    return detail::ApplyGradeProfile(FindImageSpaceGradeProfile(profiles, imageSpaceKey), result);
}

[[nodiscard]] bool ValidateLutGradeProfiles(const LutGradeProfiles& profiles) noexcept
{
    if (profiles.count > profiles.entries.size()) return false;
    for (std::size_t index = 0; index < profiles.count; ++index)
    {
        const auto& profile = profiles.entries[index];
        if (!detail::ValidateGradeProfile(profile)) return false;
        if (detail::FindGradeProfile(profiles, profile.kind, profile.path) != &profile) return false;
    }
    return true;
}

namespace {

enum class RequiredKey : std::size_t
{
    developerMode,
    traceRenderPipeline,
    developerHdr10Reference,
    showDeveloperOverlay,
    forceHdrSwapChainFailure,
    requiredCount,
    enabled = requiredCount,
    displayPeakNits,
    uiPaperWhiteNits,
    algorithm,
    acesLookStrength,
    agxMiddleGrayEv,
    agxContrast,
    agxHdrPurity,
    agxHdrShoulderPower,
    gt7ToeStrength,
    gt7ShoulderStart,
    gt7ColorBlend,
    exposureEv,
    measurePerformance,
    videoWhiteNits,
    contrast,
    blacks,
    highlights,
    shadows,
    saturation,
    temperature,
    tint,
    selectiveColorEnabled,
    selectiveColorAmount,
    selectiveRedHue,
    selectiveRedSaturation,
    selectiveRedBrightness,
    selectiveOrangeHue,
    selectiveOrangeSaturation,
    selectiveOrangeBrightness,
    selectiveYellowHue,
    selectiveYellowSaturation,
    selectiveYellowBrightness,
    selectiveGreenHue,
    selectiveGreenSaturation,
    selectiveGreenBrightness,
    selectiveCyanHue,
    selectiveCyanSaturation,
    selectiveCyanBrightness,
    selectiveBlueHue,
    selectiveBlueSaturation,
    selectiveBlueBrightness,
    selectiveMagentaHue,
    selectiveMagentaSaturation,
    selectiveMagentaBrightness,
    vanillaColorGradingStrength,
    vanillaGradeLuminance,
    vanillaGradeColor,
    lutProfilesEnabled,
    colorGradingMethod,
    bloomStrength,
    toggleKey,
    interfaceScale,
    settingsWindowWidth,
    settingsWindowHeight,
    settingsWindowX,
    settingsWindowY,
    count
};

[[nodiscard]] constexpr std::string_view Trim(std::string_view value) noexcept
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
[[nodiscard]] constexpr bool ParseBoolean(
    std::string_view text,
    bool& value) noexcept
{
    if (text == "1")
    {
        value = true;
        return true;
    }
    if (text == "0")
    {
        value = false;
        return true;
    }
    return false;
}

struct ConfigurationKey { std::string_view section; std::string_view name; RequiredKey key; };
constexpr ConfigurationKey kConfigurationKeys[]{
    {"General", "DeveloperMode", RequiredKey::developerMode},
    {"Debug", "TraceRenderPipeline", RequiredKey::traceRenderPipeline},
    {"Debug", "MeasurePerformance", RequiredKey::measurePerformance},
    {"Debug", "DeveloperHdr10Reference", RequiredKey::developerHdr10Reference},
    {"Debug", "ShowDeveloperOverlay", RequiredKey::showDeveloperOverlay},
    {"Debug", "ForceHdrSwapChainFailure", RequiredKey::forceHdrSwapChainFailure},
    {"General", "Enabled", RequiredKey::enabled},
    {"Output", "DisplayPeakNits", RequiredKey::displayPeakNits},
    {"Output", "UiPaperWhiteNits", RequiredKey::uiPaperWhiteNits},
    {"Output", "VideoWhiteNits", RequiredKey::videoWhiteNits},
    {"ToneMapper", "Algorithm", RequiredKey::algorithm},
    {"ACES2LMT", "LookStrength", RequiredKey::acesLookStrength},
    {"AgX", "MiddleGray", RequiredKey::agxMiddleGrayEv},
    {"AgX", "Contrast", RequiredKey::agxContrast},
    {"AgX", "HdrPurity", RequiredKey::agxHdrPurity},
    {"AgX", "ShoulderPower", RequiredKey::agxHdrShoulderPower},
    {"GT7", "ToeStrength", RequiredKey::gt7ToeStrength},
    {"GT7", "ShoulderStart", RequiredKey::gt7ShoulderStart},
    {"GT7", "ColorBlend", RequiredKey::gt7ColorBlend},
    {"Image", "Contrast", RequiredKey::contrast},
    {"Image", "Blacks", RequiredKey::blacks},
    {"Image", "Highlights", RequiredKey::highlights},
    {"Image", "Shadows", RequiredKey::shadows},
    {"Image", "Saturation", RequiredKey::saturation},
    {"Image", "Temperature", RequiredKey::temperature},
    {"Image", "Tint", RequiredKey::tint},
    {"Image", "ColorGradingMethod", RequiredKey::colorGradingMethod},
    {"SelectiveColor", "Enabled", RequiredKey::selectiveColorEnabled},
    {"SelectiveColor", "Amount", RequiredKey::selectiveColorAmount},
    {"Effects", "VanillaColorGradingStrength", RequiredKey::vanillaColorGradingStrength},
    {"Effects", "VanillaGradeLuminance", RequiredKey::vanillaGradeLuminance},
    {"Effects", "VanillaGradeColor", RequiredKey::vanillaGradeColor},
    {"Effects", "LutProfilesEnabled", RequiredKey::lutProfilesEnabled},
    {"Effects", "BloomStrength", RequiredKey::bloomStrength},
    {"Interface", "ToggleKey", RequiredKey::toggleKey},
    {"Interface", "Scale", RequiredKey::interfaceScale},
    {"Interface", "WindowWidth", RequiredKey::settingsWindowWidth},
    {"Interface", "WindowHeight", RequiredKey::settingsWindowHeight},
    {"Interface", "WindowX", RequiredKey::settingsWindowX},
    {"Interface", "WindowY", RequiredKey::settingsWindowY},
    {"Image", "Exposure", RequiredKey::exposureEv},
    {"ToneMapper", "ExposureEV", RequiredKey::exposureEv},
    {"SelectiveColor", "RedHue", RequiredKey::selectiveRedHue},
    {"SelectiveColor", "RedSaturation", RequiredKey::selectiveRedSaturation},
    {"SelectiveColor", "RedBrightness", RequiredKey::selectiveRedBrightness},
    {"SelectiveColor", "OrangeHue", RequiredKey::selectiveOrangeHue},
    {"SelectiveColor", "OrangeSaturation", RequiredKey::selectiveOrangeSaturation},
    {"SelectiveColor", "OrangeBrightness", RequiredKey::selectiveOrangeBrightness},
    {"SelectiveColor", "YellowHue", RequiredKey::selectiveYellowHue},
    {"SelectiveColor", "YellowSaturation", RequiredKey::selectiveYellowSaturation},
    {"SelectiveColor", "YellowBrightness", RequiredKey::selectiveYellowBrightness},
    {"SelectiveColor", "GreenHue", RequiredKey::selectiveGreenHue},
    {"SelectiveColor", "GreenSaturation", RequiredKey::selectiveGreenSaturation},
    {"SelectiveColor", "GreenBrightness", RequiredKey::selectiveGreenBrightness},
    {"SelectiveColor", "CyanHue", RequiredKey::selectiveCyanHue},
    {"SelectiveColor", "CyanSaturation", RequiredKey::selectiveCyanSaturation},
    {"SelectiveColor", "CyanBrightness", RequiredKey::selectiveCyanBrightness},
    {"SelectiveColor", "BlueHue", RequiredKey::selectiveBlueHue},
    {"SelectiveColor", "BlueSaturation", RequiredKey::selectiveBlueSaturation},
    {"SelectiveColor", "BlueBrightness", RequiredKey::selectiveBlueBrightness},
    {"SelectiveColor", "MagentaHue", RequiredKey::selectiveMagentaHue},
    {"SelectiveColor", "MagentaSaturation", RequiredKey::selectiveMagentaSaturation},
    {"SelectiveColor", "MagentaBrightness", RequiredKey::selectiveMagentaBrightness},
};

[[nodiscard]] constexpr bool MatchKey(std::string_view section,
    std::string_view key, RequiredKey& result) noexcept
{
    const auto found = std::ranges::find_if(kConfigurationKeys,
        [&](const auto& entry) { return entry.section == section && entry.name == key; });
    if (found == std::end(kConfigurationKeys)) return false;
    result = found->key;
    return true;
}
[[nodiscard]] double* NumericTarget(PluginConfiguration& configuration,
    RequiredKey key) noexcept
{
    auto& regions = configuration.selectiveColor.regions;
    struct Target { RequiredKey key; double* value; };
    const Target targets[]{
        {RequiredKey::displayPeakNits, &configuration.displayPeakNits},
        {RequiredKey::uiPaperWhiteNits, &configuration.uiPaperWhiteNits},
        {RequiredKey::videoWhiteNits, &configuration.videoWhiteNits},
        {RequiredKey::acesLookStrength, &configuration.toneMapperParameters.acesLookStrength},
        {RequiredKey::agxMiddleGrayEv, &configuration.toneMapperParameters.agxMiddleGrayEv},
        {RequiredKey::agxContrast, &configuration.toneMapperParameters.agxContrast},
        {RequiredKey::agxHdrPurity, &configuration.toneMapperParameters.agxHdrPurity},
        {RequiredKey::agxHdrShoulderPower, &configuration.toneMapperParameters.agxHdrShoulderPower},
        {RequiredKey::gt7ToeStrength, &configuration.toneMapperParameters.gt7ToeStrength},
        {RequiredKey::gt7ShoulderStart, &configuration.toneMapperParameters.gt7ShoulderStart},
        {RequiredKey::gt7ColorBlend, &configuration.toneMapperParameters.gt7ColorBlend},
        {RequiredKey::exposureEv, &configuration.exposureEv},
        {RequiredKey::contrast, &configuration.contrast},
        {RequiredKey::blacks, &configuration.blacks},
        {RequiredKey::highlights, &configuration.highlights},
        {RequiredKey::shadows, &configuration.shadows},
        {RequiredKey::saturation, &configuration.saturationPercent},
        {RequiredKey::temperature, &configuration.temperature},
        {RequiredKey::tint, &configuration.tint},
        {RequiredKey::selectiveColorAmount, &configuration.selectiveColor.amountPercent},
        {RequiredKey::selectiveRedHue, &regions[0].hueDegrees},
        {RequiredKey::selectiveRedSaturation, &regions[0].saturationPercent},
        {RequiredKey::selectiveRedBrightness, &regions[0].brightness},
        {RequiredKey::selectiveOrangeHue, &regions[1].hueDegrees},
        {RequiredKey::selectiveOrangeSaturation, &regions[1].saturationPercent},
        {RequiredKey::selectiveOrangeBrightness, &regions[1].brightness},
        {RequiredKey::selectiveYellowHue, &regions[2].hueDegrees},
        {RequiredKey::selectiveYellowSaturation, &regions[2].saturationPercent},
        {RequiredKey::selectiveYellowBrightness, &regions[2].brightness},
        {RequiredKey::selectiveGreenHue, &regions[3].hueDegrees},
        {RequiredKey::selectiveGreenSaturation, &regions[3].saturationPercent},
        {RequiredKey::selectiveGreenBrightness, &regions[3].brightness},
        {RequiredKey::selectiveCyanHue, &regions[4].hueDegrees},
        {RequiredKey::selectiveCyanSaturation, &regions[4].saturationPercent},
        {RequiredKey::selectiveCyanBrightness, &regions[4].brightness},
        {RequiredKey::selectiveBlueHue, &regions[5].hueDegrees},
        {RequiredKey::selectiveBlueSaturation, &regions[5].saturationPercent},
        {RequiredKey::selectiveBlueBrightness, &regions[5].brightness},
        {RequiredKey::selectiveMagentaHue, &regions[6].hueDegrees},
        {RequiredKey::selectiveMagentaSaturation, &regions[6].saturationPercent},
        {RequiredKey::selectiveMagentaBrightness, &regions[6].brightness},
        {RequiredKey::vanillaColorGradingStrength, &configuration.vanillaColorGradingStrengthPercent},
        {RequiredKey::vanillaGradeLuminance, &configuration.vanillaGradeLuminancePercent},
        {RequiredKey::vanillaGradeColor, &configuration.vanillaGradeColorPercent},
        {RequiredKey::bloomStrength, &configuration.bloomStrengthPercent},
        {RequiredKey::interfaceScale, &configuration.interfaceSettings.scale},
        {RequiredKey::settingsWindowWidth, &configuration.interfaceSettings.windowWidth},
        {RequiredKey::settingsWindowHeight, &configuration.interfaceSettings.windowHeight},
        {RequiredKey::settingsWindowX, &configuration.interfaceSettings.windowX},
        {RequiredKey::settingsWindowY, &configuration.interfaceSettings.windowY},
    };
    const auto found = std::ranges::find(targets, key, &Target::key);
    return found == std::end(targets) ? nullptr : found->value;
}
struct ConfigurationRange { RequiredKey key; std::array<double, 2> range; };
constexpr ConfigurationRange kConfigurationRanges[]{
    {RequiredKey::displayPeakNits, {kMinimumDisplayPeakNits, kMaximumDisplayPeakNits}},
    {RequiredKey::uiPaperWhiteNits, {kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits}},
    {RequiredKey::videoWhiteNits, {kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits}},
    {RequiredKey::exposureEv, {kMinimumSceneExposureEv, kMaximumSceneExposureEv}},
    {RequiredKey::acesLookStrength, {kMinimumAcesLookStrength, kMaximumAcesLookStrength}},
    {RequiredKey::agxMiddleGrayEv, {kMinimumAgxMiddleGrayEv, kMaximumAgxMiddleGrayEv}},
    {RequiredKey::agxContrast, {kMinimumAgxContrast, kMaximumAgxContrast}},
    {RequiredKey::agxHdrPurity, {kMinimumAgxHdrPurity, kMaximumAgxHdrPurity}},
    {RequiredKey::agxHdrShoulderPower, {kMinimumAgxHdrShoulderPower, kMaximumAgxHdrShoulderPower}},
    {RequiredKey::gt7ToeStrength, {kMinimumGt7ToeStrength, kMaximumGt7ToeStrength}},
    {RequiredKey::gt7ShoulderStart, {kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart}},
    {RequiredKey::gt7ColorBlend, {kMinimumGt7ColorBlend, kMaximumGt7ColorBlend}},
    {RequiredKey::selectiveColorAmount, {kMinimumSelectiveColorAmountPercent, kMaximumSelectiveColorAmountPercent}},
    {RequiredKey::selectiveRedHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {RequiredKey::selectiveOrangeHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {RequiredKey::selectiveYellowHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {RequiredKey::selectiveGreenHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {RequiredKey::selectiveCyanHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {RequiredKey::selectiveBlueHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {RequiredKey::selectiveMagentaHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {RequiredKey::selectiveRedSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {RequiredKey::selectiveOrangeSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {RequiredKey::selectiveYellowSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {RequiredKey::selectiveGreenSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {RequiredKey::selectiveCyanSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {RequiredKey::selectiveBlueSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {RequiredKey::selectiveMagentaSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {RequiredKey::selectiveRedBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {RequiredKey::selectiveOrangeBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {RequiredKey::selectiveYellowBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {RequiredKey::selectiveGreenBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {RequiredKey::selectiveCyanBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {RequiredKey::selectiveBlueBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {RequiredKey::selectiveMagentaBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {RequiredKey::saturation, {kMinimumSaturationPercent, kMaximumSaturationPercent}},
    {RequiredKey::vanillaColorGradingStrength, {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent}},
    {RequiredKey::vanillaGradeLuminance, {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent}},
    {RequiredKey::vanillaGradeColor, {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent}},
    {RequiredKey::bloomStrength, {kMinimumBloomStrengthPercent, kMaximumBloomStrengthPercent}},
    {RequiredKey::interfaceScale, {kMinimumInterfaceScale, kMaximumInterfaceScale}},
    {RequiredKey::settingsWindowWidth, {kMinimumSettingsWindowWidth, kMaximumSettingsWindowWidth}},
    {RequiredKey::settingsWindowHeight, {kMinimumSettingsWindowHeight, kMaximumSettingsWindowHeight}},
    {RequiredKey::settingsWindowX, {-1.0, 65535.0}},
    {RequiredKey::settingsWindowY, {-1.0, 65535.0}},
};

[[nodiscard]] constexpr std::array<double, 2> NumericRange(RequiredKey key) noexcept
{
    const auto found = std::ranges::find(kConfigurationRanges, key, &ConfigurationRange::key);
    return found == std::end(kConfigurationRanges) ?
        std::array{kMinimumImageAdjustment, kMaximumImageAdjustment} : found->range;
}
PluginConfigurationError ParseNumber(std::string_view value,
    std::array<double, 2> range, double& target) noexcept
{
    double number{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        return PluginConfigurationError::invalidNumber;
    if (!std::isfinite(number)) return PluginConfigurationError::invalidNumber;
    if (number < range[0] || number > range[1]) return PluginConfigurationError::valueOutOfRange;
    target = number;
    return PluginConfigurationError::none;
}

PluginConfigurationError ParseAlgorithm(std::string_view value,
    ProductionToneMapperAlgorithm& target) noexcept
{
    constexpr std::array names{std::string_view{"ACES2LMT"}, std::string_view{"AgX"}, std::string_view{"GT7"}};
    const auto found = std::ranges::find(names, value);
    if (found == names.end()) return PluginConfigurationError::unsupportedAlgorithm;
    target = static_cast<ProductionToneMapperAlgorithm>(found - names.begin());
    return PluginConfigurationError::none;
}

PluginConfigurationError ParseGradingMethod(std::string_view value,
    ColorGradingMethod& target) noexcept
{
    if (value == "Radial") target = ColorGradingMethod::radial;
    else if (value == "NativeReference") target = ColorGradingMethod::nativeReference;
    else return PluginConfigurationError::unsupportedColorGradingMethod;
    return PluginConfigurationError::none;
}

bool& BooleanTarget(PluginConfiguration& parsed, RequiredKey key) noexcept
{
    struct Target { RequiredKey key; bool* value; };
    const Target targets[]{
        {RequiredKey::developerMode, &parsed.developerMode},
        {RequiredKey::selectiveColorEnabled, &parsed.selectiveColor.enabled},
        {RequiredKey::lutProfilesEnabled, &parsed.lutGradeProfiles.enabled},
        {RequiredKey::traceRenderPipeline, &parsed.traceRenderPipeline},
        {RequiredKey::measurePerformance, &parsed.measurePerformance},
        {RequiredKey::developerHdr10Reference, &parsed.developerHdr10Reference},
        {RequiredKey::showDeveloperOverlay, &parsed.showDeveloperOverlay},
        {RequiredKey::forceHdrSwapChainFailure, &parsed.forceHdrSwapChainFailure}};
    const auto found = std::ranges::find(targets, key, &Target::key);
    return found == std::end(targets) ? parsed.enabled : *found->value;
}

PluginConfigurationError ParseNamedSetting(PluginConfiguration& parsed,
    RequiredKey key, std::string_view value) noexcept
{
    if (key == RequiredKey::algorithm) return ParseAlgorithm(value, parsed.productionAlgorithm);
    if (key == RequiredKey::colorGradingMethod) return ParseGradingMethod(value, parsed.colorGradingMethod);
    if (key == RequiredKey::toggleKey)
        return ParseHdrSettingsHotkey(value, parsed.interfaceSettings.toggleKey) ?
            PluginConfigurationError::none : PluginConfigurationError::unsupportedToggleKey;
    return ParseBoolean(value, BooleanTarget(parsed, key)) ?
        PluginConfigurationError::none : PluginConfigurationError::invalidBoolean;
}

PluginConfigurationError ParseSetting(PluginConfiguration& parsed,
    RequiredKey key, std::string_view value) noexcept
{
    auto* target = NumericTarget(parsed, key);
    if (target == nullptr) return ParseNamedSetting(parsed, key, value);
    const auto error = ParseNumber(value, NumericRange(key), *target);
    if (error != PluginConfigurationError::none) return error;
    if (key == RequiredKey::exposureEv) parsed.sceneExposureMultiplier = std::exp2(*target);
    return PluginConfigurationError::none;
}

bool HasDiagnosticRequest(const PluginConfiguration& parsed) noexcept
{
    const std::array flags{parsed.traceRenderPipeline, parsed.measurePerformance,
        parsed.developerHdr10Reference, parsed.showDeveloperOverlay, parsed.forceHdrSwapChainFailure};
    return std::ranges::find(flags, true) != flags.end();
}

PluginConfigurationError ValidateDiagnosticConfiguration(const PluginConfiguration& parsed) noexcept
{
    if (HasDiagnosticRequest(parsed) && !parsed.developerMode)
        return PluginConfigurationError::invalidCombination;
    const auto referenceFeature = parsed.showDeveloperOverlay || parsed.forceHdrSwapChainFailure;
    const auto referenceAvailable = parsed.developerHdr10Reference || parsed.productionRequested;
    if (referenceFeature && !referenceAvailable) return PluginConfigurationError::invalidCombination;
    return PluginConfigurationError::none;
}

PluginConfigurationError ValidateConfiguration(const PluginConfiguration& parsed) noexcept
{
    const auto error = ValidateDiagnosticConfiguration(parsed);
    if (error != PluginConfigurationError::none) return error;
    if (ValidateHdrInterfaceSettings(parsed.interfaceSettings) != HdrInterfaceSettingsValidationError::none)
        return PluginConfigurationError::valueOutOfRange;
    if (ValidateHdrSettings(parsed) != HdrSettingsValidationError::none)
        return PluginConfigurationError::valueOutOfRange;
    return PluginConfigurationError::none;
}

class ConfigurationParser
{
public:
    explicit ConfigurationParser(bool profilesOnly) noexcept : profilesOnly_(profilesOnly)
    {
        // Files containing only diagnostic keys remain valid, but cannot request a production operator.
        output_.value.enabled = true;
    }

    PluginConfigurationResult Parse(std::string_view text) noexcept
    {
        if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        while (!text.empty())
        {
            ++lineNumber_;
            const auto line = NextLine(text);
            if (const auto error = ParseLine(line); error != PluginConfigurationError::none)
                return Failure(error);
        }
        if (const auto error = FinishProfile(); error != PluginConfigurationError::none)
            return Failure(error);
        if (const auto error = FinishConfiguration(); error != PluginConfigurationError::none)
            return Failure(error);
        output_.error = PluginConfigurationError::none;
        output_.valid = true;
        return std::move(output_);
    }

private:
    static std::string_view NextLine(std::string_view& text) noexcept
    {
        const auto newline = text.find('\n');
        const auto line = Trim(text.substr(0, newline));
        if (newline == std::string_view::npos) text = {};
        else text.remove_prefix(newline + 1);
        return line;
    }

    PluginConfigurationResult Failure(PluginConfigurationError error) noexcept
    {
        output_.error = error;
        output_.line = lineNumber_;
        return std::move(output_);
    }

    PluginConfigurationError FinishProfile() noexcept
    {
        if (!hasProfile_) return PluginConfigurationError::none;
        const auto hasOverride = currentProfile_.overrideColor || currentProfile_.overrideLuminance;
        if (!inherit_ && !hasOverride) return PluginConfigurationError::invalidSyntax;
        auto& profiles = inherit_ ? output_.profileInheritances : output_.value.lutGradeProfiles;
        if (profiles.count == profiles.entries.size()) return PluginConfigurationError::valueOutOfRange;
        profiles.entries[profiles.count++] = currentProfile_;
        return PluginConfigurationError::none;
    }

    PluginConfigurationError StartProfile(bool imageSpace) noexcept
    {
        std::array<char, kMaximumLutGradePathLength + 1> path{};
        const auto key = section_.substr(imageSpace ? 5 : 4);
        const auto kind = imageSpace ? GradeProfileKind::imageSpace : GradeProfileKind::lutAsset;
        if (!detail::NormalizeProfileKey(kind, key, path)) return PluginConfigurationError::invalidSyntax;
        if (detail::FindGradeProfile(output_.value.lutGradeProfiles, kind, path) ||
            detail::FindGradeProfile(output_.profileInheritances, kind, path))
            return PluginConfigurationError::duplicateKey;
        currentProfile_ = {};
        currentProfile_.path = path;
        currentProfile_.kind = kind;
        hasProfile_ = true;
        return PluginConfigurationError::none;
    }

    PluginConfigurationError ParseSection(std::string_view line) noexcept
    {
        if (const auto error = FinishProfile(); error != PluginConfigurationError::none) return error;
        if (line.size() < 3 || line.back() != ']') return PluginConfigurationError::invalidSyntax;
        section_ = Trim(line.substr(1, line.size() - 2));
        if (section_.empty()) return PluginConfigurationError::invalidSyntax;
        hasProfile_ = false;
        inherit_ = false;
        if (section_.starts_with("IMGS:")) return StartProfile(true);
        if (section_.starts_with("LUT:")) return StartProfile(false);
        return PluginConfigurationError::none;
    }

    PluginConfigurationError ParseInheritance(std::string_view value) noexcept
    {
        const auto hasOverride = currentProfile_.overrideColor || currentProfile_.overrideLuminance;
        if (inherit_ || hasOverride)
            return PluginConfigurationError::duplicateKey;
        if (value != "1") return PluginConfigurationError::invalidBoolean;
        inherit_ = true;
        return PluginConfigurationError::none;
    }

    PluginConfigurationError ParseProfileValue(std::string_view key, std::string_view value) noexcept
    {
        if (key == "Inherit") return ParseInheritance(value);
        if (key != "Color" && key != "Luminance") return PluginConfigurationError::invalidSyntax;
        if (inherit_) return PluginConfigurationError::duplicateKey;
        return ParseProfileChannel(key == "Color", value);
    }

    PluginConfigurationError ParseProfileChannel(bool color, std::string_view value) noexcept
    {
        auto& present = color ? currentProfile_.overrideColor : currentProfile_.overrideLuminance;
        auto& target = color ? currentProfile_.colorPercent : currentProfile_.luminancePercent;
        if (present) return PluginConfigurationError::duplicateKey;
        const auto error = ParseNumber(value,
            {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent}, target);
        if (error == PluginConfigurationError::none) present = true;
        return error;
    }

    PluginConfigurationError ParseValue(std::string_view key, std::string_view value) noexcept
    {
        if (hasProfile_) return ParseProfileValue(key, value);
        if (profilesOnly_) return PluginConfigurationError::none;
        RequiredKey required{};
        if (!MatchKey(section_, key, required)) return PluginConfigurationError::none;
        auto& seen = seen_[static_cast<std::size_t>(required)];
        if (seen) return PluginConfigurationError::duplicateKey;
        seen = true;
        return ParseSetting(output_.value, required, value);
    }

    PluginConfigurationError ParseAssignment(std::string_view line) noexcept
    {
        const auto equals = line.find('=');
        if (equals == std::string_view::npos || section_.empty()) return PluginConfigurationError::invalidSyntax;
        const auto key = Trim(line.substr(0, equals));
        const auto value = Trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) return PluginConfigurationError::invalidSyntax;
        return ParseValue(key, value);
    }

    PluginConfigurationError ParseLine(std::string_view line) noexcept
    {
        if (line.empty()) return PluginConfigurationError::none;
        if (line.front() == ';' || line.front() == '#') return PluginConfigurationError::none;
        if (line.front() == '[') return ParseSection(line);
        return ParseAssignment(line);
    }

    bool Seen(RequiredKey key) const noexcept { return seen_[static_cast<std::size_t>(key)]; }

    PluginConfigurationError FinishProductionRequest() noexcept
    {
        const std::array productionKeys{Seen(RequiredKey::displayPeakNits), Seen(RequiredKey::uiPaperWhiteNits),
            Seen(RequiredKey::algorithm), Seen(RequiredKey::exposureEv)};
        const auto present = std::ranges::find(productionKeys, true) != productionKeys.end();
        const auto complete = std::ranges::find(productionKeys, false) == productionKeys.end();
        const auto missing = !Seen(RequiredKey::enabled) || !complete;
        if (present && missing) return PluginConfigurationError::missingKey;
        auto& parsed = output_.value;
        // An explicit diagnostic reference request suppresses the production path.
        parsed.productionRequested = parsed.enabled && present && !parsed.developerHdr10Reference;
        return ValidateConfiguration(parsed);
    }

    PluginConfigurationError FinishConfiguration() noexcept
    {
        if (profilesOnly_)
            return ValidateLutGradeProfiles(output_.value.lutGradeProfiles) ?
                PluginConfigurationError::none : PluginConfigurationError::valueOutOfRange;
        const auto end = seen_.begin() + static_cast<std::size_t>(RequiredKey::requiredCount);
        if (std::find(seen_.begin(), end, false) != end) return PluginConfigurationError::missingKey;
        return FinishProductionRequest();
    }

    PluginConfigurationResult output_{};
    std::array<bool, static_cast<std::size_t>(RequiredKey::count)> seen_{};
    bool profilesOnly_{};
    bool inherit_{};
    bool hasProfile_{};
    std::string_view section_;
    LutGradeProfile currentProfile_{};
    std::size_t lineNumber_{};
};

PluginConfigurationResult ParseConfiguration(std::string_view text, bool profilesOnly) noexcept
{
    return ConfigurationParser(profilesOnly).Parse(text);
}

bool ContainsProfile(const LutGradeProfiles* profiles, const LutGradeProfile& profile) noexcept
{
    return profiles && detail::FindGradeProfile(*profiles, profile.kind, profile.path);
}

[[nodiscard]] bool AppendUnclaimedProfiles(
    LutGradeProfiles& destination, const LutGradeProfiles& source,
    const LutGradeProfiles* suppressed = nullptr,
    const LutGradeProfiles* alsoSuppressed = nullptr) noexcept
{
    for (std::size_t index = 0; index < source.count; ++index)
    {
        const auto& profile = source.entries[index];
        const std::array<const LutGradeProfiles*, 3> claimed{&destination, suppressed, alsoSuppressed};
        if (std::ranges::any_of(claimed, [&](const auto* profiles) {
                return ContainsProfile(profiles, profile);
            })) continue;
        if (destination.count == destination.entries.size()) return false;
        destination.entries[destination.count++] = profile;
    }
    return true;
}

void RemoveInheritanceMasks(LutGradeProfiles& profiles) noexcept
{
    std::size_t retained{};
    const auto count = profiles.count;
    for (std::size_t index = 0; index < count; ++index)
        if (profiles.entries[index].overrideColor ||
            profiles.entries[index].overrideLuminance)
            profiles.entries[retained++] = profiles.entries[index];
    profiles.count = retained;
    for (auto index = retained; index < count; ++index)
        profiles.entries[index] = {};
}

[[nodiscard]] PluginConfigurationResult ReadConfiguration(
    const std::filesystem::path& path, bool profilesOnly)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    const std::string text(std::istreambuf_iterator<char>(stream), {});
    if (stream.bad()) return {};
    return ParseConfiguration(text, profilesOnly);
}

void CollectProfileFile(const std::filesystem::directory_entry& entry,
    std::vector<std::filesystem::path>& files, PluginConfigurationResult& result)
{

    if (CompareStringOrdinal(entry.path().extension().c_str(), -1,
            L".ini", -1, TRUE) == CSTR_EQUAL &&
        CompareStringOrdinal(entry.path().filename().c_str(), -1,
            L"Personal.ini", -1, TRUE) != CSTR_EQUAL)
    {
        std::error_code entryError;
        if (entry.is_regular_file(entryError)) files.push_back(entry.path());
        else if (entryError)
            result.profileIssues.push_back({entry.path(),
                PluginConfigurationError::fileUnavailable, 0});
    }
}

void LoadDistributedFile(const std::filesystem::path& file, PluginConfigurationResult& result)
{
    auto parsed = ReadConfiguration(file, true);
    if (!parsed.valid)
    {
        result.profileIssues.push_back({file, parsed.error, parsed.line});
        return;
    }
    auto candidate = result.distributedProfiles;
    if (!AppendUnclaimedProfiles(candidate, parsed.value.lutGradeProfiles) ||
        !AppendUnclaimedProfiles(candidate, parsed.profileInheritances))
    {
        parsed.valid = false;
        parsed.error = PluginConfigurationError::valueOutOfRange;
    }
    if (!parsed.valid)
        result.profileIssues.push_back({file, parsed.error, parsed.line});
    else
        result.distributedProfiles = candidate;
}

void LoadDistributedProfiles(const std::filesystem::path& directory,
    PluginConfigurationResult& result)
{
    std::error_code error;
    if (!std::filesystem::exists(directory, error) && !error) return;
    std::vector<std::filesystem::path> files;
    std::filesystem::directory_iterator iterator(directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end)
    {
        CollectProfileFile(*iterator, files, result);
        iterator.increment(error);
    }
    if (error)
    {
        result.profileIssues.push_back({directory,
            PluginConfigurationError::fileUnavailable, 0});
        return;
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        const auto leftName = left.filename().native();
        const auto rightName = right.filename().native();
        const auto order = CompareStringOrdinal(leftName.c_str(), -1,
            rightName.c_str(), -1, TRUE);
        return order == CSTR_EQUAL ? leftName > rightName :
            order == CSTR_GREATER_THAN;
    });
    for (const auto& file : files) LoadDistributedFile(file, result);
    RemoveInheritanceMasks(result.distributedProfiles);
}

}  // namespace

PluginConfigurationResult ParsePluginConfiguration(std::string_view text) noexcept
{
    return ParseConfiguration(text, false);
}

PluginConfigurationResult ParseGradeProfileConfiguration(std::string_view text) noexcept
{
    return ParseConfiguration(text, true);
}

PluginConfigurationResult LoadPluginConfiguration(
    const std::filesystem::path& path) noexcept
{
    try
    {
        auto result = ReadConfiguration(path, false);
        if (!result.valid) return result;
        const auto directory = path.parent_path() / path.stem();
        LoadDistributedProfiles(directory, result);
        const auto personalPath = directory / L"Personal.ini";
        PluginConfigurationResult personal;
        std::error_code error;
        if (std::filesystem::exists(personalPath, error))
        {
            personal = ReadConfiguration(personalPath, true);
            if (!personal.valid)
            {
                result.profileIssues.push_back({personalPath,
                    personal.error, personal.line});
                personal = {};
            }
        }
        else if (error)
            result.profileIssues.push_back({personalPath,
                PluginConfigurationError::fileUnavailable, 0});
        auto merged = personal.value.lutGradeProfiles;
        merged.enabled = result.value.lutGradeProfiles.enabled;
        if (!AppendUnclaimedProfiles(merged, result.value.lutGradeProfiles,
                &personal.profileInheritances) ||
            !AppendUnclaimedProfiles(merged, result.distributedProfiles,
                &personal.profileInheritances, &result.profileInheritances))
        {
            result.valid = false;
            result.error = PluginConfigurationError::valueOutOfRange;
            return result;
        }
        RemoveInheritanceMasks(merged);
        result.value.lutGradeProfiles = merged;
        return result;
    }
    catch (...)
    {
        return {};
    }
}

std::string_view ToString(
    ProductionToneMapperAlgorithm algorithm) noexcept
{
    switch (algorithm)
    {
    case ProductionToneMapperAlgorithm::aces2Lmt: return "ACES2LMT";
    case ProductionToneMapperAlgorithm::agX: return "AgX";
    case ProductionToneMapperAlgorithm::gt7: return "GT7";
    }
    return "unknown";
}

std::string_view ToString(
    ColorGradingMethod method) noexcept
{
    switch (method)
    {
    case ColorGradingMethod::radial: return "Radial";
    case ColorGradingMethod::nativeReference: return "NativeReference";
    }
    return "unknown";
}

std::string_view ToString(PluginConfigurationError error) noexcept
{
    struct ErrorName { PluginConfigurationError error; std::string_view name; };
    constexpr ErrorName names[]{
        {PluginConfigurationError::none, "none"},
        {PluginConfigurationError::fileUnavailable, "file-unavailable"},
        {PluginConfigurationError::invalidSyntax, "invalid-syntax"},
        {PluginConfigurationError::missingKey, "missing-key"},
        {PluginConfigurationError::duplicateKey, "duplicate-key"},
        {PluginConfigurationError::invalidBoolean, "invalid-boolean"},
        {PluginConfigurationError::invalidNumber, "invalid-number"},
        {PluginConfigurationError::valueOutOfRange, "value-out-of-range"},
        {PluginConfigurationError::unsupportedAlgorithm, "unsupported-algorithm"},
        {PluginConfigurationError::unsupportedColorGradingMethod, "unsupported-color-grading-method"},
        {PluginConfigurationError::unsupportedToggleKey, "unsupported-toggle-key"},
        {PluginConfigurationError::invalidCombination, "invalid-combination"},
    };
    const auto found = std::ranges::find(names, error, &ErrorName::error);
    return found == std::end(names) ? "unknown" : found->name;
}
}  // namespace fo4hdr
