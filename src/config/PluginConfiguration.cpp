// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"

#pragma warning(push, 0)
#include <SimpleIni.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fo4hdr {
namespace {
namespace detail {

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
    if (byte == '\\') return '/';
    if (byte >= 'A' && byte <= 'Z') return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

bool ValidGradePathBounds(std::string_view path) noexcept
{
    return !path.empty() && path.size() <= kMaximumLutGradePathLength;
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

namespace {

enum class SettingField : std::size_t
{
    enabled,
    developerMode,
    traceRenderPipeline,
    developerHdr10Reference,
    forceHdrSwapChainFailure,
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
    highlightSaturation,
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
    fallbackGradeLuminance,
    fallbackGradeColor,
    colorGradingMethod,
    colorGradingCurve,
    bloomStrength,
    weatherEnabled,
    weatherLuminanceBase,
    weatherLuminanceGain,
    weatherLuminanceMinimum,
    weatherLuminanceMaximum,
    weatherLuminanceIntercept,
    weatherLuminanceSunlight,
    weatherLuminanceGlare,
    weatherLuminanceAmbient,
    weatherLuminanceColorChange,
    weatherColorBase,
    weatherColorGain,
    weatherColorMinimum,
    weatherColorMaximum,
    weatherColorIntercept,
    weatherColorSunlight,
    weatherColorGlare,
    weatherColorAmbient,
    weatherColorColorChange,
    sdrEnabled,
    sdrOperatorWeatherResponse
};

// Which sections hold a key: a switch or HDR-only section, both an HDR
// section and its Sdr twin, or the Sdr twin alone.
enum class SettingProfile : std::uint8_t
{
    global,
    hdr,
    shared,
    sdr
};

struct SettingKey
{
    std::string_view section;
    std::string_view name;
    SettingField field;
    SettingProfile profile;
    const char* comment{};
    const char* sdrComment{};
    // A name that shipped in a release; a launch rewrites it as name.
    std::string_view formerName{};
};

// The file's keys in order. The Sdr twins follow the HDR sections,
// and [Debug] ends the file.
constexpr SettingKey kSettingKeys[]{
    {"General", "HdrEnabled", SettingField::enabled, SettingProfile::global,
        "# Enable HDR: needs an HDR10 display with Windows HDR on; applies at game start."},
    {"General", "SdrEnabled", SettingField::sdrEnabled, SettingProfile::global,
        "# SDR color controls (the Sdr sections) when HDR is off or unavailable; they\n"
        "# have no effect under ENB."},
    {"General", "DeveloperMode", SettingField::developerMode, SettingProfile::global,
        "# Required by the [Debug] keys; adds the Diagnostics menu and detailed logging."},
    {"Output", "DisplayPeakNits", SettingField::displayPeakNits, SettingProfile::hdr,
        "# Peak Brightness: 400 to 2000 nits."},
    {"Output", "UiPaperWhiteNits", SettingField::uiPaperWhiteNits, SettingProfile::hdr,
        "# Menu & HUD White: 100 to 300 nits."},
    {"Output", "VideoWhiteNits", SettingField::videoWhiteNits, SettingProfile::hdr,
        "# Video White: 100 to 300 nits."},
    {"ToneMapper", "Algorithm", SettingField::algorithm, SettingProfile::shared,
        "# Tone Mapper: ACES2, AgX or GT7.",
        "# The Sdr sections apply when HDR is off or unavailable. They take the keys and\n"
        "# ranges of the matching sections above except where noted; these defaults\n"
        "# reproduce the game's own SDR image.\n"
        "# Tone Mapper: Vanilla keeps the game's curve; ACES2, AgX or GT7 replace it."},
    {"ToneMapper", "AutomaticWeatherResponse", SettingField::sdrOperatorWeatherResponse,
        SettingProfile::sdr, nullptr,
        "# Weather Response (Recommended): with ACES2, AgX or GT7, turns it on whatever\n"
        "# [SdrWeatherLutResponse] Enabled says; Vanilla follows that setting."},
    {"ACES2", "ContrastLookStrength", SettingField::acesLookStrength, SettingProfile::shared,
        "# Contrast Look: 0 to 1."},
    {"AgX", "MiddleGrayOffsetEV", SettingField::agxMiddleGrayEv, SettingProfile::shared,
        "# Middle Gray: -0.5 to 0.5 EV.",
        "# Middle Gray: -0.5 to 0 EV; AgX can only lower middle gray in SDR.\n"
        "# No ShoulderPower: it has no effect at the SDR white."},
    {"AgX", "Contrast", SettingField::agxContrast, SettingProfile::shared,
        "# Curve Contrast: 2.2 to 3.0."},
    {"AgX", "ColorResponse", SettingField::agxHdrPurity, SettingProfile::shared,
        "# Color Response: 0.25 to 0.60.", nullptr, "HdrColorResponse"},
    {"AgX", "ShoulderPower", SettingField::agxHdrShoulderPower, SettingProfile::hdr,
        "# Highlight Shoulder, HDR only: 1.75 to 2.25."},
    {"GT7", "ToeStrength", SettingField::gt7ToeStrength, SettingProfile::shared,
        "# Shadow Toe: 1.0 to 1.5."},
    {"GT7", "ShoulderStart", SettingField::gt7ShoulderStart, SettingProfile::shared,
        "# Shoulder Start: 0.35 to 0.60.",
        "# Shoulder Start: 0.216 to 0.60; 0.216 starts the shoulder where the toe ends."},
    {"GT7", "ColorBlend", SettingField::gt7ColorBlend, SettingProfile::shared,
        "# Color Blend: 0 to 1."},
    {"Image", "ColorGradingMethod", SettingField::colorGradingMethod, SettingProfile::hdr,
        "# Grading Method, HDR only: RgbRatio or Affine."},
    {"Image", "ColorGradingCurve", SettingField::colorGradingCurve, SettingProfile::hdr,
        "# Grading Curve, HDR only: Gamma22 or sRGB. Gamma22 keeps the look's\n"
        "# deeper shadows; sRGB lifts graded shadows."},
    {"Image", "ExposureEV", SettingField::exposureEv, SettingProfile::shared,
        "# Exposure: -4 to 4 EV.",
        "# No ColorGradingMethod or ColorGradingCurve: they apply only to HDR."},
    {"Image", "Contrast", SettingField::contrast, SettingProfile::shared,
        "# -100 to 100, except Saturation and HighlightSaturation: 0 to 200 percent."},
    {"Image", "Blacks", SettingField::blacks, SettingProfile::shared},
    {"Image", "Highlights", SettingField::highlights, SettingProfile::shared},
    {"Image", "Shadows", SettingField::shadows, SettingProfile::shared},
    {"Image", "Saturation", SettingField::saturation, SettingProfile::shared},
    {"Image", "HighlightSaturation", SettingField::highlightSaturation, SettingProfile::shared},
    {"Image", "Temperature", SettingField::temperature, SettingProfile::shared},
    {"Image", "Tint", SettingField::tint, SettingProfile::shared},
    {"SelectiveColor", "Enabled", SettingField::selectiveColorEnabled, SettingProfile::shared,
        "# Amount: 0 to 100 percent. Per color, Hue: -10 to 10 degrees; Saturation:\n"
        "# 50 to 150 percent; Brightness: -100 to 100."},
    {"SelectiveColor", "Amount", SettingField::selectiveColorAmount, SettingProfile::shared},
    {"SelectiveColor", "RedHue", SettingField::selectiveRedHue, SettingProfile::shared},
    {"SelectiveColor", "RedSaturation", SettingField::selectiveRedSaturation, SettingProfile::shared},
    {"SelectiveColor", "RedBrightness", SettingField::selectiveRedBrightness, SettingProfile::shared},
    {"SelectiveColor", "OrangeHue", SettingField::selectiveOrangeHue, SettingProfile::shared},
    {"SelectiveColor", "OrangeSaturation", SettingField::selectiveOrangeSaturation,
        SettingProfile::shared},
    {"SelectiveColor", "OrangeBrightness", SettingField::selectiveOrangeBrightness,
        SettingProfile::shared},
    {"SelectiveColor", "YellowHue", SettingField::selectiveYellowHue, SettingProfile::shared},
    {"SelectiveColor", "YellowSaturation", SettingField::selectiveYellowSaturation,
        SettingProfile::shared},
    {"SelectiveColor", "YellowBrightness", SettingField::selectiveYellowBrightness,
        SettingProfile::shared},
    {"SelectiveColor", "GreenHue", SettingField::selectiveGreenHue, SettingProfile::shared},
    {"SelectiveColor", "GreenSaturation", SettingField::selectiveGreenSaturation,
        SettingProfile::shared},
    {"SelectiveColor", "GreenBrightness", SettingField::selectiveGreenBrightness,
        SettingProfile::shared},
    {"SelectiveColor", "CyanHue", SettingField::selectiveCyanHue, SettingProfile::shared},
    {"SelectiveColor", "CyanSaturation", SettingField::selectiveCyanSaturation,
        SettingProfile::shared},
    {"SelectiveColor", "CyanBrightness", SettingField::selectiveCyanBrightness,
        SettingProfile::shared},
    {"SelectiveColor", "BlueHue", SettingField::selectiveBlueHue, SettingProfile::shared},
    {"SelectiveColor", "BlueSaturation", SettingField::selectiveBlueSaturation,
        SettingProfile::shared},
    {"SelectiveColor", "BlueBrightness", SettingField::selectiveBlueBrightness,
        SettingProfile::shared},
    {"SelectiveColor", "MagentaHue", SettingField::selectiveMagentaHue, SettingProfile::shared},
    {"SelectiveColor", "MagentaSaturation", SettingField::selectiveMagentaSaturation,
        SettingProfile::shared},
    {"SelectiveColor", "MagentaBrightness", SettingField::selectiveMagentaBrightness,
        SettingProfile::shared},
    {"Effects", "VanillaColorGradingStrength", SettingField::vanillaColorGradingStrength,
        SettingProfile::shared,
        "# 0 to 100 percent. The menu shows VanillaColorGradingStrength as Grading\n"
        "# Strength. The Fallback Grade keys apply where Weather Response does not."},
    {"Effects", "FallbackGradeLuminance", SettingField::fallbackGradeLuminance,
        SettingProfile::shared, nullptr, nullptr, "VanillaGradeLuminanceStrength"},
    {"Effects", "FallbackGradeColor", SettingField::fallbackGradeColor,
        SettingProfile::shared, nullptr, nullptr, "VanillaGradeColorStrength"},
    {"Effects", "BloomStrength", SettingField::bloomStrength, SettingProfile::shared},
    {"WeatherLutResponse", "Enabled", SettingField::weatherEnabled, SettingProfile::shared,
        "# Weather Response: one automatic, fixed response per weather ImageSpace.\n"
        "# Base, Minimum and Maximum: 0 to 100 percent, Minimum not above Maximum.\n"
        "# Gain: 0 to 2. Intercept and the four weights: -1000 to 1000.\n"
        "# The menu shows Base as Baseline, Gain as Response Amount, Minimum and Maximum\n"
        "# as Minimum and Maximum Retention, and ColorChange as Sunlight Color Change."},
    {"WeatherLutResponse", "LuminanceBase", SettingField::weatherLuminanceBase,
        SettingProfile::shared},
    {"WeatherLutResponse", "LuminanceGain", SettingField::weatherLuminanceGain,
        SettingProfile::shared},
    {"WeatherLutResponse", "LuminanceMinimum", SettingField::weatherLuminanceMinimum,
        SettingProfile::shared},
    {"WeatherLutResponse", "LuminanceMaximum", SettingField::weatherLuminanceMaximum,
        SettingProfile::shared},
    {"WeatherLutResponse", "LuminanceIntercept", SettingField::weatherLuminanceIntercept,
        SettingProfile::shared},
    {"WeatherLutResponse", "LuminanceSunlight", SettingField::weatherLuminanceSunlight,
        SettingProfile::shared},
    {"WeatherLutResponse", "LuminanceGlare", SettingField::weatherLuminanceGlare,
        SettingProfile::shared},
    {"WeatherLutResponse", "LuminanceAmbient", SettingField::weatherLuminanceAmbient,
        SettingProfile::shared},
    {"WeatherLutResponse", "LuminanceColorChange", SettingField::weatherLuminanceColorChange,
        SettingProfile::shared},
    {"WeatherLutResponse", "ColorBase", SettingField::weatherColorBase, SettingProfile::shared},
    {"WeatherLutResponse", "ColorGain", SettingField::weatherColorGain, SettingProfile::shared},
    {"WeatherLutResponse", "ColorMinimum", SettingField::weatherColorMinimum,
        SettingProfile::shared},
    {"WeatherLutResponse", "ColorMaximum", SettingField::weatherColorMaximum,
        SettingProfile::shared},
    {"WeatherLutResponse", "ColorIntercept", SettingField::weatherColorIntercept,
        SettingProfile::shared},
    {"WeatherLutResponse", "ColorSunlight", SettingField::weatherColorSunlight,
        SettingProfile::shared},
    {"WeatherLutResponse", "ColorGlare", SettingField::weatherColorGlare, SettingProfile::shared},
    {"WeatherLutResponse", "ColorAmbient", SettingField::weatherColorAmbient,
        SettingProfile::shared},
    {"WeatherLutResponse", "ColorColorChange", SettingField::weatherColorColorChange,
        SettingProfile::shared},
    {"Debug", "TraceRenderPipeline", SettingField::traceRenderPipeline, SettingProfile::global,
        "# Developer diagnostics; each has an effect only with DeveloperMode=1.\n"
        "# Logs a draw and Present trace: Ctrl+Shift+F11 captures eight frames; with HDR\n"
        "# on, Ctrl+Alt+Shift+F11 captures up to the first black frame."},
    {"Debug", "MeasurePerformance", SettingField::measurePerformance, SettingProfile::global,
        "# Logs CPU and GPU frame times after a save; runs only at exactly 2560x1440."},
    {"Debug", "DeveloperHdr10Reference", SettingField::developerHdr10Reference,
        SettingProfile::global,
        "# Replaces the tone mapper with a fixed reference: scene 1.0 maps to 203 nits."},
    {"Debug", "ForceHdrSwapChainFailure", SettingField::forceHdrSwapChainFailure,
        SettingProfile::global,
        "# Fails HDR swap-chain creation once to test the fallback to the original one."},
};

// The file comment, written before [General].
constexpr const char* kFileComment =
    "# Isotope settings. Isotope rewrites this file each time the game starts:\n"
    "# it adds new settings, corrects invalid values, and removes unknown\n"
    "# settings and added comments. Corrections are listed in FO4Isotope.log.\n"
    "# Edit this file only while the game is closed.";

// SDR profile sections reuse their HDR section's key names.
struct SdrProfileSection { std::string_view sdr; std::string_view base; };
constexpr std::array kSdrProfileSections{
    SdrProfileSection{"SdrToneMapper", "ToneMapper"},
    SdrProfileSection{"SdrACES2", "ACES2"},
    SdrProfileSection{"SdrAgX", "AgX"},
    SdrProfileSection{"SdrGT7", "GT7"},
    SdrProfileSection{"SdrImage", "Image"},
    SdrProfileSection{"SdrSelectiveColor", "SelectiveColor"},
    SdrProfileSection{"SdrEffects", "Effects"},
    SdrProfileSection{"SdrWeatherLutResponse", "WeatherLutResponse"}};

[[nodiscard]] constexpr std::string_view SdrProfileSectionName(std::string_view base) noexcept
{
    for (const auto& entry : kSdrProfileSections)
        if (entry.base == base) return entry.sdr;
    return {};
}

[[nodiscard]] constexpr bool HasSdrTwin(SettingProfile profile) noexcept
{
    return profile == SettingProfile::shared || profile == SettingProfile::sdr;
}

static_assert(std::ranges::all_of(kSettingKeys, [](const SettingKey& key) {
    return !HasSdrTwin(key.profile) || !SdrProfileSectionName(key.section).empty();
}));

[[nodiscard]] constexpr bool IsDebugKey(const SettingKey& key) noexcept
{
    return key.section == "Debug";
}

[[nodiscard]] constexpr std::string_view SectionOf(const SettingKey& key, bool sdr) noexcept
{
    return sdr ? SdrProfileSectionName(key.section) : key.section;
}

// The render writes the HDR and switch sections, then the Sdr twins, then [Debug].
enum class RenderPass : std::uint8_t
{
    hdr,
    sdr,
    debug
};

[[nodiscard]] constexpr bool InRenderPass(const SettingKey& key, RenderPass pass) noexcept
{
    switch (pass)
    {
    case RenderPass::hdr: return key.profile != SettingProfile::sdr && !IsDebugKey(key);
    case RenderPass::sdr: return HasSdrTwin(key.profile);
    case RenderPass::debug: return IsDebugKey(key);
    }
    return false;
}

[[nodiscard]] constexpr char AsciiLower(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] constexpr bool EqualsIgnoreCase(std::string_view left, std::string_view right) noexcept
{
    return std::ranges::equal(left, right,
        [](char first, char second) { return AsciiLower(first) == AsciiLower(second); });
}

[[nodiscard]] double* WeatherNumericTarget(HdrSettings& configuration,
    SettingField key) noexcept
{
    struct Target { SettingField key; double* value; };
    const Target targets[]{
        {SettingField::weatherLuminanceBase, &configuration.weatherGradeResponse.luminance.base},
        {SettingField::weatherLuminanceGain, &configuration.weatherGradeResponse.luminance.gain},
        {SettingField::weatherLuminanceMinimum, &configuration.weatherGradeResponse.luminance.minimum},
        {SettingField::weatherLuminanceMaximum, &configuration.weatherGradeResponse.luminance.maximum},
        {SettingField::weatherLuminanceIntercept, &configuration.weatherGradeResponse.luminance.intercept},
        {SettingField::weatherLuminanceSunlight, &configuration.weatherGradeResponse.luminance.coefficients[0]},
        {SettingField::weatherLuminanceGlare, &configuration.weatherGradeResponse.luminance.coefficients[1]},
        {SettingField::weatherLuminanceAmbient, &configuration.weatherGradeResponse.luminance.coefficients[2]},
        {SettingField::weatherLuminanceColorChange, &configuration.weatherGradeResponse.luminance.coefficients[3]},
        {SettingField::weatherColorBase, &configuration.weatherGradeResponse.color.base},
        {SettingField::weatherColorGain, &configuration.weatherGradeResponse.color.gain},
        {SettingField::weatherColorMinimum, &configuration.weatherGradeResponse.color.minimum},
        {SettingField::weatherColorMaximum, &configuration.weatherGradeResponse.color.maximum},
        {SettingField::weatherColorIntercept, &configuration.weatherGradeResponse.color.intercept},
        {SettingField::weatherColorSunlight, &configuration.weatherGradeResponse.color.coefficients[0]},
        {SettingField::weatherColorGlare, &configuration.weatherGradeResponse.color.coefficients[1]},
        {SettingField::weatherColorAmbient, &configuration.weatherGradeResponse.color.coefficients[2]},
        {SettingField::weatherColorColorChange, &configuration.weatherGradeResponse.color.coefficients[3]},
    };
    const auto found = std::ranges::find(targets, key, &Target::key);
    return found == std::end(targets) ? nullptr : found->value;
}

// Creative controls shared by the HDR settings and the SDR profile.
[[nodiscard]] double* ProfileNumericTarget(HdrSettings& configuration,
    SettingField key) noexcept
{
    if (auto* weather = WeatherNumericTarget(configuration, key)) return weather;
    auto& regions = configuration.selectiveColor.regions;
    struct Target { SettingField key; double* value; };
    const Target targets[]{
        {SettingField::exposureEv, &configuration.exposureEv},
        {SettingField::contrast, &configuration.contrast},
        {SettingField::blacks, &configuration.blacks},
        {SettingField::highlights, &configuration.highlights},
        {SettingField::shadows, &configuration.shadows},
        {SettingField::saturation, &configuration.saturationPercent},
        {SettingField::highlightSaturation, &configuration.highlightSaturationPercent},
        {SettingField::temperature, &configuration.temperature},
        {SettingField::tint, &configuration.tint},
        {SettingField::selectiveColorAmount, &configuration.selectiveColor.amountPercent},
        {SettingField::selectiveRedHue, &regions[0].hueDegrees},
        {SettingField::selectiveRedSaturation, &regions[0].saturationPercent},
        {SettingField::selectiveRedBrightness, &regions[0].brightness},
        {SettingField::selectiveOrangeHue, &regions[1].hueDegrees},
        {SettingField::selectiveOrangeSaturation, &regions[1].saturationPercent},
        {SettingField::selectiveOrangeBrightness, &regions[1].brightness},
        {SettingField::selectiveYellowHue, &regions[2].hueDegrees},
        {SettingField::selectiveYellowSaturation, &regions[2].saturationPercent},
        {SettingField::selectiveYellowBrightness, &regions[2].brightness},
        {SettingField::selectiveGreenHue, &regions[3].hueDegrees},
        {SettingField::selectiveGreenSaturation, &regions[3].saturationPercent},
        {SettingField::selectiveGreenBrightness, &regions[3].brightness},
        {SettingField::selectiveCyanHue, &regions[4].hueDegrees},
        {SettingField::selectiveCyanSaturation, &regions[4].saturationPercent},
        {SettingField::selectiveCyanBrightness, &regions[4].brightness},
        {SettingField::selectiveBlueHue, &regions[5].hueDegrees},
        {SettingField::selectiveBlueSaturation, &regions[5].saturationPercent},
        {SettingField::selectiveBlueBrightness, &regions[5].brightness},
        {SettingField::selectiveMagentaHue, &regions[6].hueDegrees},
        {SettingField::selectiveMagentaSaturation, &regions[6].saturationPercent},
        {SettingField::selectiveMagentaBrightness, &regions[6].brightness},
        {SettingField::vanillaColorGradingStrength, &configuration.vanillaColorGradingStrengthPercent},
        {SettingField::fallbackGradeLuminance, &configuration.fallbackGradeLuminancePercent},
        {SettingField::fallbackGradeColor, &configuration.fallbackGradeColorPercent},
        {SettingField::bloomStrength, &configuration.bloomStrengthPercent},
        {SettingField::acesLookStrength, &configuration.toneMapperParameters.acesLookStrength},
        {SettingField::agxMiddleGrayEv, &configuration.toneMapperParameters.agxMiddleGrayEv},
        {SettingField::agxContrast, &configuration.toneMapperParameters.agxContrast},
        {SettingField::agxHdrPurity, &configuration.toneMapperParameters.agxHdrPurity},
        {SettingField::agxHdrShoulderPower, &configuration.toneMapperParameters.agxHdrShoulderPower},
        {SettingField::gt7ToeStrength, &configuration.toneMapperParameters.gt7ToeStrength},
        {SettingField::gt7ShoulderStart, &configuration.toneMapperParameters.gt7ShoulderStart},
        {SettingField::gt7ColorBlend, &configuration.toneMapperParameters.gt7ColorBlend},
    };
    const auto found = std::ranges::find(targets, key, &Target::key);
    return found == std::end(targets) ? nullptr : found->value;
}

[[nodiscard]] double* NumericTarget(PluginConfiguration& configuration,
    SettingField key) noexcept
{
    if (auto* profile = ProfileNumericTarget(configuration, key)) return profile;
    struct Target { SettingField key; double* value; };
    const Target targets[]{
        {SettingField::displayPeakNits, &configuration.displayPeakNits},
        {SettingField::uiPaperWhiteNits, &configuration.uiPaperWhiteNits},
        {SettingField::videoWhiteNits, &configuration.videoWhiteNits},
    };
    const auto found = std::ranges::find(targets, key, &Target::key);
    return found == std::end(targets) ? nullptr : found->value;
}

struct ConfigurationRange { SettingField key; std::array<double, 2> range; };
constexpr ConfigurationRange kConfigurationRanges[]{
    {SettingField::weatherLuminanceBase, {0.0, 100.0}},
    {SettingField::weatherLuminanceGain, {0.0, kMaximumWeatherGradeGain}},
    {SettingField::weatherLuminanceMinimum, {0.0, 100.0}},
    {SettingField::weatherLuminanceMaximum, {0.0, 100.0}},
    {SettingField::weatherLuminanceIntercept, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherLuminanceSunlight, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherLuminanceGlare, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherLuminanceAmbient, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherLuminanceColorChange, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherColorBase, {0.0, 100.0}},
    {SettingField::weatherColorGain, {0.0, kMaximumWeatherGradeGain}},
    {SettingField::weatherColorMinimum, {0.0, 100.0}},
    {SettingField::weatherColorMaximum, {0.0, 100.0}},
    {SettingField::weatherColorIntercept, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherColorSunlight, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherColorGlare, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherColorAmbient, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {SettingField::weatherColorColorChange, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},

    {SettingField::displayPeakNits, {kMinimumDisplayPeakNits, kMaximumDisplayPeakNits}},
    {SettingField::uiPaperWhiteNits, {kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits}},
    {SettingField::videoWhiteNits, {kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits}},
    {SettingField::exposureEv, {kMinimumSceneExposureEv, kMaximumSceneExposureEv}},
    {SettingField::acesLookStrength, {kMinimumAcesLookStrength, kMaximumAcesLookStrength}},
    {SettingField::agxMiddleGrayEv, {kMinimumAgxMiddleGrayEv, kMaximumAgxMiddleGrayEv}},
    {SettingField::agxContrast, {kMinimumAgxContrast, kMaximumAgxContrast}},
    {SettingField::agxHdrPurity, {kMinimumAgxHdrPurity, kMaximumAgxHdrPurity}},
    {SettingField::agxHdrShoulderPower, {kMinimumAgxHdrShoulderPower, kMaximumAgxHdrShoulderPower}},
    {SettingField::gt7ToeStrength, {kMinimumGt7ToeStrength, kMaximumGt7ToeStrength}},
    {SettingField::gt7ShoulderStart, {kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart}},
    {SettingField::gt7ColorBlend, {kMinimumGt7ColorBlend, kMaximumGt7ColorBlend}},
    {SettingField::selectiveColorAmount, {kMinimumSelectiveColorAmountPercent, kMaximumSelectiveColorAmountPercent}},
    {SettingField::selectiveRedHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {SettingField::selectiveOrangeHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {SettingField::selectiveYellowHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {SettingField::selectiveGreenHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {SettingField::selectiveCyanHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {SettingField::selectiveBlueHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {SettingField::selectiveMagentaHue, {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees}},
    {SettingField::selectiveRedSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {SettingField::selectiveOrangeSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {SettingField::selectiveYellowSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {SettingField::selectiveGreenSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {SettingField::selectiveCyanSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {SettingField::selectiveBlueSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {SettingField::selectiveMagentaSaturation, {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent}},
    {SettingField::selectiveRedBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {SettingField::selectiveOrangeBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {SettingField::selectiveYellowBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {SettingField::selectiveGreenBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {SettingField::selectiveCyanBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {SettingField::selectiveBlueBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {SettingField::selectiveMagentaBrightness, {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness}},
    {SettingField::saturation, {kMinimumSaturationPercent, kMaximumSaturationPercent}},
    {SettingField::highlightSaturation, {kMinimumSaturationPercent, kMaximumSaturationPercent}},
    {SettingField::vanillaColorGradingStrength, {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent}},
    {SettingField::fallbackGradeLuminance, {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent}},
    {SettingField::fallbackGradeColor, {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent}},
    {SettingField::bloomStrength, {kMinimumBloomStrengthPercent, kMaximumBloomStrengthPercent}},
};

[[nodiscard]] constexpr std::array<double, 2> NumericRange(SettingField key) noexcept
{
    const auto found = std::ranges::find(kConfigurationRanges, key, &ConfigurationRange::key);
    return found == std::end(kConfigurationRanges) ?
        std::array{kMinimumImageAdjustment, kMaximumImageAdjustment} : found->range;
}

// The HDR ranges with the SDR AgX Middle Gray and GT7 Shoulder Start limits.
[[nodiscard]] constexpr std::array<double, 2> SdrNumericRange(SettingField key) noexcept
{
    if (key == SettingField::agxMiddleGrayEv)
        return {kMinimumAgxMiddleGrayEv, kMaximumSdrAgxMiddleGrayEv};
    if (key == SettingField::gt7ShoulderStart)
        return {kMinimumSdrGt7ShoulderStart, kMaximumGt7ShoulderStart};
    return NumericRange(key);
}

bool& BooleanTarget(PluginConfiguration& parsed, SettingField key) noexcept
{
    struct Target { SettingField key; bool* value; };
    const Target targets[]{
        {SettingField::weatherEnabled, &parsed.weatherGradeResponse.enabled},
        {SettingField::developerMode, &parsed.developerMode},
        {SettingField::selectiveColorEnabled, &parsed.selectiveColor.enabled},
        {SettingField::traceRenderPipeline, &parsed.traceRenderPipeline},
        {SettingField::measurePerformance, &parsed.measurePerformance},
        {SettingField::developerHdr10Reference, &parsed.developerHdr10Reference},
        {SettingField::forceHdrSwapChainFailure, &parsed.forceHdrSwapChainFailure},
        {SettingField::sdrEnabled, &parsed.sdrEnabled}};
    const auto found = std::ranges::find(targets, key, &Target::key);
    return found == std::end(targets) ? parsed.enabled : *found->value;
}

bool* ProfileBooleanTarget(HdrSettings& profile, SettingField key) noexcept
{
    if (key == SettingField::weatherEnabled) return &profile.weatherGradeResponse.enabled;
    if (key == SettingField::selectiveColorEnabled) return &profile.selectiveColor.enabled;
    if (key == SettingField::sdrOperatorWeatherResponse) return &profile.sdrOperatorWeatherResponse;
    return nullptr;
}

[[nodiscard]] HdrSettings& Profile(PluginConfiguration& stored, bool sdr) noexcept
{
    return sdr ? stored.sdrSettings : static_cast<HdrSettings&>(stored);
}

[[nodiscard]] double* KeyNumber(PluginConfiguration& stored, SettingField field, bool sdr) noexcept
{
    return sdr ? ProfileNumericTarget(stored.sdrSettings, field) : NumericTarget(stored, field);
}

[[nodiscard]] bool* KeyFlag(PluginConfiguration& stored, SettingField field, bool sdr) noexcept
{
    return sdr ? ProfileBooleanTarget(stored.sdrSettings, field) : &BooleanTarget(stored, field);
}

// Weather weights keep every digit; other numbers keep the menu's 0.001 steps.
[[nodiscard]] constexpr bool IsPreciseNumber(SettingField field) noexcept
{
    return field >= SettingField::weatherLuminanceBase &&
        field <= SettingField::weatherColorColorChange;
}

[[nodiscard]] std::string FormatNumber(double value, bool precise)
{
    std::array<char, 64> buffer{};
    auto* const end = buffer.data() + buffer.size();
    const auto result = precise ?
        std::to_chars(buffer.data(), end, value) :
        std::to_chars(buffer.data(), end, value, std::chars_format::fixed, 3);
    // Fixed notation overflows only far outside every range. Returning nothing
    // lets CanonicalNumber keep the value, which the clamp then bounds.
    if (result.ec != std::errc{}) return {};
    std::string text(buffer.data(), result.ptr);
    if (!precise)
    {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    return text == "-0" ? std::string{"0"} : text;
}

[[nodiscard]] std::optional<double> ParseNumber(std::string_view text) noexcept
{
    // from_chars takes no plus sign; strip one, but not before a minus sign.
    if (text.starts_with('+') && !text.starts_with("+-")) text.remove_prefix(1);
    double number{};
    const auto* const end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(number))
        return std::nullopt;
    return number;
}

// The value its saved text reads back as, so the file and the settings agree.
[[nodiscard]] double CanonicalNumber(double value, bool precise)
{
    return precise ? value : ParseNumber(FormatNumber(value, false)).value_or(value);
}

[[nodiscard]] bool ParseBoolean(std::string_view text, bool& value) noexcept
{
    constexpr std::array<std::string_view, 4> on{"1", "true", "yes", "on"};
    constexpr std::array<std::string_view, 4> off{"0", "false", "no", "off"};
    const auto matches = [text](std::string_view word) { return EqualsIgnoreCase(text, word); };
    if (std::ranges::any_of(on, matches)) value = true;
    else if (std::ranges::any_of(off, matches)) value = false;
    else return false;
    return true;
}

// The SDR profile adds the vanilla curve.
[[nodiscard]] bool ParseAlgorithm(std::string_view text,
    ProductionToneMapperAlgorithm& target, bool sdr) noexcept
{
    using enum ProductionToneMapperAlgorithm;
    for (const auto algorithm : {aces2, agX, gt7, native})
    {
        const auto offered = sdr ? IsSelectableSdrToneMapper(algorithm) : algorithm != native;
        if (!offered || !EqualsIgnoreCase(text, ToString(algorithm))) continue;
        target = algorithm;
        return true;
    }
    return false;
}

// Radial and NativeReference shipped as method names; a launch rewrites them.
[[nodiscard]] bool ParseGradingMethod(std::string_view text,
    ColorGradingMethod& target, bool& formerName) noexcept
{
    struct Name { std::string_view text; ColorGradingMethod method; bool former; };
    constexpr std::array names{
        Name{"RgbRatio", ColorGradingMethod::rgbRatio, false},
        Name{"Affine", ColorGradingMethod::affine, false},
        Name{"Radial", ColorGradingMethod::rgbRatio, true},
        Name{"NativeReference", ColorGradingMethod::affine, true}};
    const auto found = std::ranges::find_if(names,
        [text](const Name& name) { return EqualsIgnoreCase(text, name.text); });
    if (found == names.end()) return false;
    target = found->method;
    formerName = found->former;
    return true;
}

[[nodiscard]] bool ParseGradingCurve(std::string_view text, ColorGradingCurve& target) noexcept
{
    for (const auto curve : {ColorGradingCurve::gamma22, ColorGradingCurve::srgb})
    {
        if (!EqualsIgnoreCase(text, ToString(curve))) continue;
        target = curve;
        return true;
    }
    return false;
}

struct AppliedValue
{
    bool accepted{};
    bool clamped{};
    bool formerName{};
};

[[nodiscard]] AppliedValue ApplyNumber(double& target, std::string_view text,
    std::array<double, 2> range, bool precise)
{
    const auto number = ParseNumber(text);
    if (!number) return {};
    const auto canonical = CanonicalNumber(*number, precise);
    target = std::clamp(canonical, range[0], range[1]);
    return {.accepted = true, .clamped = target != canonical};
}

[[nodiscard]] AppliedValue ApplySetting(PluginConfiguration& stored,
    const SettingKey& key, bool sdr, std::string_view text)
{
    if (auto* number = KeyNumber(stored, key.field, sdr))
        return ApplyNumber(*number, text,
            sdr ? SdrNumericRange(key.field) : NumericRange(key.field), IsPreciseNumber(key.field));
    auto& profile = Profile(stored, sdr);
    AppliedValue applied{};
    switch (key.field)
    {
    case SettingField::algorithm:
        applied.accepted = ParseAlgorithm(text, profile.productionAlgorithm, sdr);
        break;
    case SettingField::colorGradingMethod:
        applied.accepted = ParseGradingMethod(text, profile.colorGradingMethod, applied.formerName);
        break;
    case SettingField::colorGradingCurve:
        applied.accepted = ParseGradingCurve(text, profile.colorGradingCurve);
        break;
    default:
    {
        auto* flag = KeyFlag(stored, key.field, sdr);
        applied.accepted = flag != nullptr && ParseBoolean(text, *flag);
        break;
    }
    }
    return applied;
}

[[nodiscard]] std::string FormatSetting(PluginConfiguration& stored,
    const SettingKey& key, bool sdr)
{
    if (const auto* number = KeyNumber(stored, key.field, sdr))
        return FormatNumber(*number, IsPreciseNumber(key.field));
    const auto& profile = Profile(stored, sdr);
    switch (key.field)
    {
    case SettingField::algorithm: return std::string(ToString(profile.productionAlgorithm));
    case SettingField::colorGradingMethod: return std::string(ToString(profile.colorGradingMethod));
    case SettingField::colorGradingCurve: return std::string(ToString(profile.colorGradingCurve));
    default:
    {
        const auto* flag = KeyFlag(stored, key.field, sdr);
        return flag != nullptr && *flag ? "1" : "0";
    }
    }
}

// Resolves every schema key from a loaded INI and records each repair.
class ConfigurationResolver
{
public:
    ConfigurationResolver(const CSimpleIniA& ini, ConfigurationRepairs& repairs) noexcept :
        ini_(ini), repairs_(repairs)
    {
    }

    void Resolve(PluginConfiguration& stored)
    {
        for (const auto& key : kSettingKeys)
        {
            if (key.profile != SettingProfile::sdr) ResolveKey(stored, key, false);
            if (HasSdrTwin(key.profile)) ResolveKey(stored, key, true);
        }
        KeepMaximumsAboveMinimums(stored, false);
        KeepMaximumsAboveMinimums(stored, true);
        RecordDroppedEntries();
    }

private:
    struct FoundText
    {
        const char* text{};
        std::string_view name;
    };

    // The current name first, then the released former name.
    [[nodiscard]] FoundText Find(std::string_view section, const SettingKey& key) const
    {
        if (const auto* text = ini_.GetValue(section.data(), key.name.data(), nullptr))
            return {text, key.name};
        if (key.formerName.empty()) return {nullptr, key.name};
        return {ini_.GetValue(section.data(), key.formerName.data(), nullptr), key.formerName};
    }

    void ResolveKey(PluginConfiguration& stored, const SettingKey& key, bool sdr)
    {
        const auto section = SectionOf(key, sdr);
        const auto found = Find(section, key);
        if (found.text == nullptr) return Record(ConfigurationRepairKind::added, section, key.name, {});
        consumed_.emplace_back(section, found.name);
        const auto applied = ApplySetting(stored, key, sdr, found.text);
        if (!applied.accepted)
            return Record(ConfigurationRepairKind::defaulted, section, key.name, found.text);
        if (found.name != key.name || applied.formerName)
            Record(ConfigurationRepairKind::renamed, section, key.name, found.text);
        if (applied.clamped) Record(ConfigurationRepairKind::clamped, section, key.name, found.text);
    }

    // A channel's Minimum must not exceed its Maximum.
    void KeepMaximumsAboveMinimums(PluginConfiguration& stored, bool sdr)
    {
        const std::string_view base{"WeatherLutResponse"};
        const auto section = sdr ? SdrProfileSectionName(base) : base;
        auto& response = Profile(stored, sdr).weatherGradeResponse;
        struct Channel { WeatherGradeChannel& channel; std::string_view maximum; };
        for (const auto& [channel, maximum] : {Channel{response.luminance, "LuminanceMaximum"},
                 Channel{response.color, "ColorMaximum"}})
        {
            if (channel.maximum >= channel.minimum) continue;
            Record(ConfigurationRepairKind::clamped, section, maximum,
                FormatNumber(channel.maximum, true));
            channel.maximum = channel.minimum;
        }
    }

    void RecordDroppedEntries()
    {
        CSimpleIniA::TNamesDepend sections;
        ini_.GetAllSections(sections);
        sections.sort(CSimpleIniA::Entry::LoadOrder());
        for (const auto& section : sections)
        {
            CSimpleIniA::TNamesDepend keys;
            ini_.GetAllKeys(section.pItem, keys);
            keys.sort(CSimpleIniA::Entry::LoadOrder());
            for (const auto& key : keys)
                if (!Consumed(section.pItem, key.pItem))
                    Record(ConfigurationRepairKind::dropped, section.pItem, key.pItem,
                        ini_.GetValue(section.pItem, key.pItem, ""));
        }
    }

    [[nodiscard]] bool Consumed(std::string_view section, std::string_view key) const noexcept
    {
        return std::ranges::any_of(consumed_, [section, key](const auto& entry) {
            return EqualsIgnoreCase(entry.first, section) && EqualsIgnoreCase(entry.second, key);
        });
    }

    void Record(ConfigurationRepairKind kind, std::string_view section,
        std::string_view key, std::string_view text)
    {
        constexpr std::size_t kMaximumRecordedText{64};
        ++repairs_.counts[static_cast<std::size_t>(kind)];
        if (repairs_.entries.size() < kMaximumListedConfigurationRepairs)
            repairs_.entries.push_back({kind, std::string(section), std::string(key),
                std::string(text.substr(0, kMaximumRecordedText))});
    }

    const CSimpleIniA& ini_;
    ConfigurationRepairs& repairs_;
    std::vector<std::pair<std::string_view, std::string_view>> consumed_;
};

// Missing or invalid keys keep these: production HDR and SDR mode requested, diagnostics off.
[[nodiscard]] PluginConfiguration StoredDefaults() noexcept
{
    PluginConfiguration stored{};
    static_cast<HdrSettings&>(stored) = HdrSettings{};
    stored.enabled = true;
    stored.sdrEnabled = true;
    return stored;
}

// Diagnostics need DeveloperMode, and the forced failure needs an HDR request.
[[nodiscard]] PluginConfiguration EffectiveConfiguration(PluginConfiguration configuration) noexcept
{
    if (!configuration.developerMode)
    {
        configuration.traceRenderPipeline = false;
        configuration.measurePerformance = false;
        configuration.developerHdr10Reference = false;
        configuration.forceHdrSwapChainFailure = false;
    }
    // An explicit diagnostic reference request suppresses the production path.
    configuration.productionRequested =
        configuration.enabled && !configuration.developerHdr10Reference;
    configuration.forceHdrSwapChainFailure = configuration.forceHdrSwapChainFailure &&
        (configuration.developerHdr10Reference || configuration.productionRequested);
    configuration.sceneExposureMultiplier = std::exp2(configuration.exposureEv);
    configuration.interfaceSettings.hdrEnabled = configuration.enabled;
    return configuration;
}

void Require(SI_Error result)
{
    if (result < 0) throw std::runtime_error("SimpleIni could not build the INI.");
}

// Adds the keys of one pass in table order, each with its comment.
void AddKeys(CSimpleIniA& ini, PluginConfiguration& values, RenderPass pass)
{
    const auto sdr = pass == RenderPass::sdr;
    for (const auto& key : kSettingKeys)
    {
        if (!InRenderPass(key, pass)) continue;
        const auto text = FormatSetting(values, key, sdr);
        Require(ini.SetValue(SectionOf(key, sdr).data(), key.name.data(), text.c_str(),
            sdr ? key.sdrComment : key.comment));
    }
}

}  // namespace

PluginConfiguration FallbackPluginConfiguration() noexcept
{
    PluginConfiguration fallback{};
    fallback.sdrEnabled = true;
    fallback.configurationDefaults = true;
    fallback.interfaceSettings.hdrEnabled = false;
    return fallback;
}

std::string RenderPluginConfiguration(const PluginConfiguration& stored)
{
    auto values = stored;  // the field accessors return mutable references
    CSimpleIniA ini;
    ini.SetUnicode(true);
    ini.SetSpaces(false);
    // SimpleIni takes its file comment only from loaded text.
    const std::string_view fileComment{kFileComment};
    Require(ini.LoadData(fileComment.data(), fileComment.size()));
    for (const auto pass : {RenderPass::hdr, RenderPass::sdr, RenderPass::debug})
        AddKeys(ini, values, pass);
    std::string text;
    Require(ini.Save(text, false));
    return text;
}

PluginConfigurationResult ParsePluginConfiguration(std::string_view text) noexcept
{
    try
    {
        CSimpleIniA ini;
        ini.SetUnicode(true);
        if (ini.LoadData(text.data(), text.size()) < 0) return {};
        PluginConfigurationResult result;
        result.stored = StoredDefaults();
        ConfigurationResolver(ini, result.repairs).Resolve(result.stored);
        result.value = EffectiveConfiguration(result.stored);
        result.text = RenderPluginConfiguration(result.stored);
        result.rewriteNeeded = text != result.text;
        result.error = PluginConfigurationError::none;
        result.valid = true;
        return result;
    }
    catch (...)
    {
        return {};
    }
}

PluginConfigurationResult LoadPluginConfiguration(
    const std::filesystem::path& path) noexcept
{
    try
    {
        std::error_code error;
        const auto exists = std::filesystem::exists(path, error);
        if (error) return {};
        std::string text;
        if (exists)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) return {};
            text.assign(std::istreambuf_iterator<char>(stream), {});
            if (stream.bad()) return {};
        }
        auto result = ParsePluginConfiguration(text);
        result.fileMissing = !exists;
        result.rewriteNeeded = result.valid && (result.fileMissing || result.rewriteNeeded);
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
    case ProductionToneMapperAlgorithm::aces2: return "ACES2";
    case ProductionToneMapperAlgorithm::agX: return "AgX";
    case ProductionToneMapperAlgorithm::gt7: return "GT7";
    case ProductionToneMapperAlgorithm::native: return "Vanilla";
    }
    return "unknown";
}

std::string_view ToString(
    ColorGradingMethod method) noexcept
{
    switch (method)
    {
    case ColorGradingMethod::rgbRatio: return "RgbRatio";
    case ColorGradingMethod::affine: return "Affine";
    }
    return "unknown";
}

std::string_view ToString(
    ColorGradingCurve curve) noexcept
{
    switch (curve)
    {
    case ColorGradingCurve::gamma22: return "Gamma22";
    case ColorGradingCurve::srgb: return "sRGB";
    }
    return "unknown";
}

std::string_view ToString(PluginConfigurationError error) noexcept
{
    switch (error)
    {
    case PluginConfigurationError::none: return "none";
    case PluginConfigurationError::fileUnavailable: return "file-unavailable";
    }
    return "unknown";
}

std::string_view ToString(ConfigurationRepairKind kind) noexcept
{
    switch (kind)
    {
    case ConfigurationRepairKind::added: return "added";
    case ConfigurationRepairKind::renamed: return "renamed";
    case ConfigurationRepairKind::defaulted: return "defaulted";
    case ConfigurationRepairKind::clamped: return "clamped";
    case ConfigurationRepairKind::dropped: return "dropped";
    }
    return "unknown";
}

}  // namespace fo4hdr
