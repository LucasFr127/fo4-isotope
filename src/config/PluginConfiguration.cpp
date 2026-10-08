// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"

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
    colorGradingMethod,
    bloomStrength,
    toggleKey,
    interfaceScale,
    settingsWindowWidth,
    settingsWindowHeight,
    settingsWindowX,
    settingsWindowY,
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
    {"WeatherLutResponse", "Enabled", RequiredKey::weatherEnabled},
    {"WeatherLutResponse", "LuminanceBase", RequiredKey::weatherLuminanceBase},
    {"WeatherLutResponse", "LuminanceGain", RequiredKey::weatherLuminanceGain},
    {"WeatherLutResponse", "LuminanceMinimum", RequiredKey::weatherLuminanceMinimum},
    {"WeatherLutResponse", "LuminanceMaximum", RequiredKey::weatherLuminanceMaximum},
    {"WeatherLutResponse", "LuminanceIntercept", RequiredKey::weatherLuminanceIntercept},
    {"WeatherLutResponse", "LuminanceSunlight", RequiredKey::weatherLuminanceSunlight},
    {"WeatherLutResponse", "LuminanceGlare", RequiredKey::weatherLuminanceGlare},
    {"WeatherLutResponse", "LuminanceAmbient", RequiredKey::weatherLuminanceAmbient},
    {"WeatherLutResponse", "LuminanceColorChange", RequiredKey::weatherLuminanceColorChange},
    {"WeatherLutResponse", "ColorBase", RequiredKey::weatherColorBase},
    {"WeatherLutResponse", "ColorGain", RequiredKey::weatherColorGain},
    {"WeatherLutResponse", "ColorMinimum", RequiredKey::weatherColorMinimum},
    {"WeatherLutResponse", "ColorMaximum", RequiredKey::weatherColorMaximum},
    {"WeatherLutResponse", "ColorIntercept", RequiredKey::weatherColorIntercept},
    {"WeatherLutResponse", "ColorSunlight", RequiredKey::weatherColorSunlight},
    {"WeatherLutResponse", "ColorGlare", RequiredKey::weatherColorGlare},
    {"WeatherLutResponse", "ColorAmbient", RequiredKey::weatherColorAmbient},
    {"WeatherLutResponse", "ColorColorChange", RequiredKey::weatherColorColorChange},
    {"General", "DeveloperMode", RequiredKey::developerMode},
    {"Debug", "TraceRenderPipeline", RequiredKey::traceRenderPipeline},
    {"Debug", "MeasurePerformance", RequiredKey::measurePerformance},
    {"Debug", "DeveloperHdr10Reference", RequiredKey::developerHdr10Reference},
    {"Debug", "ShowDeveloperOverlay", RequiredKey::showDeveloperOverlay},
    {"Debug", "ForceHdrSwapChainFailure", RequiredKey::forceHdrSwapChainFailure},
    {"General", "HdrEnabled", RequiredKey::enabled},
    {"Output", "DisplayPeakNits", RequiredKey::displayPeakNits},
    {"Output", "UiPaperWhiteNits", RequiredKey::uiPaperWhiteNits},
    {"Output", "VideoWhiteNits", RequiredKey::videoWhiteNits},
    {"ToneMapper", "Algorithm", RequiredKey::algorithm},
    {"ACES2", "ContrastLookStrength", RequiredKey::acesLookStrength},
    {"AgX", "MiddleGrayOffsetEV", RequiredKey::agxMiddleGrayEv},
    {"AgX", "Contrast", RequiredKey::agxContrast},
    {"AgX", "HdrColorResponse", RequiredKey::agxHdrPurity},
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
    {"Effects", "VanillaGradeLuminanceStrength", RequiredKey::vanillaGradeLuminance},
    {"Effects", "VanillaGradeColorStrength", RequiredKey::vanillaGradeColor},
    {"Effects", "BloomStrength", RequiredKey::bloomStrength},
    {"Interface", "ToggleKey", RequiredKey::toggleKey},
    {"Interface", "Scale", RequiredKey::interfaceScale},
    {"Interface", "WindowWidth", RequiredKey::settingsWindowWidth},
    {"Interface", "WindowHeight", RequiredKey::settingsWindowHeight},
    {"Interface", "WindowX", RequiredKey::settingsWindowX},
    {"Interface", "WindowY", RequiredKey::settingsWindowY},
    {"Image", "ExposureEV", RequiredKey::exposureEv},
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
[[nodiscard]] double* WeatherNumericTarget(PluginConfiguration& configuration,
    RequiredKey key) noexcept
{
    struct Target { RequiredKey key; double* value; };
    const Target targets[]{
        {RequiredKey::weatherLuminanceBase, &configuration.weatherGradeResponse.luminance.base},
        {RequiredKey::weatherLuminanceGain, &configuration.weatherGradeResponse.luminance.gain},
        {RequiredKey::weatherLuminanceMinimum, &configuration.weatherGradeResponse.luminance.minimum},
        {RequiredKey::weatherLuminanceMaximum, &configuration.weatherGradeResponse.luminance.maximum},
        {RequiredKey::weatherLuminanceIntercept, &configuration.weatherGradeResponse.luminance.intercept},
        {RequiredKey::weatherLuminanceSunlight, &configuration.weatherGradeResponse.luminance.coefficients[0]},
        {RequiredKey::weatherLuminanceGlare, &configuration.weatherGradeResponse.luminance.coefficients[1]},
        {RequiredKey::weatherLuminanceAmbient, &configuration.weatherGradeResponse.luminance.coefficients[2]},
        {RequiredKey::weatherLuminanceColorChange, &configuration.weatherGradeResponse.luminance.coefficients[3]},
        {RequiredKey::weatherColorBase, &configuration.weatherGradeResponse.color.base},
        {RequiredKey::weatherColorGain, &configuration.weatherGradeResponse.color.gain},
        {RequiredKey::weatherColorMinimum, &configuration.weatherGradeResponse.color.minimum},
        {RequiredKey::weatherColorMaximum, &configuration.weatherGradeResponse.color.maximum},
        {RequiredKey::weatherColorIntercept, &configuration.weatherGradeResponse.color.intercept},
        {RequiredKey::weatherColorSunlight, &configuration.weatherGradeResponse.color.coefficients[0]},
        {RequiredKey::weatherColorGlare, &configuration.weatherGradeResponse.color.coefficients[1]},
        {RequiredKey::weatherColorAmbient, &configuration.weatherGradeResponse.color.coefficients[2]},
        {RequiredKey::weatherColorColorChange, &configuration.weatherGradeResponse.color.coefficients[3]},
    };
    const auto found = std::ranges::find(targets, key, &Target::key);
    return found == std::end(targets) ? nullptr : found->value;
}

[[nodiscard]] double* NumericTarget(PluginConfiguration& configuration,
    RequiredKey key) noexcept
{
    if (auto* weather = WeatherNumericTarget(configuration, key)) return weather;
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
    {RequiredKey::weatherLuminanceBase, {0.0, 100.0}},
    {RequiredKey::weatherLuminanceGain, {0.0, kMaximumWeatherGradeGain}},
    {RequiredKey::weatherLuminanceMinimum, {0.0, 100.0}},
    {RequiredKey::weatherLuminanceMaximum, {0.0, 100.0}},
    {RequiredKey::weatherLuminanceIntercept, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherLuminanceSunlight, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherLuminanceGlare, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherLuminanceAmbient, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherLuminanceColorChange, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherColorBase, {0.0, 100.0}},
    {RequiredKey::weatherColorGain, {0.0, kMaximumWeatherGradeGain}},
    {RequiredKey::weatherColorMinimum, {0.0, 100.0}},
    {RequiredKey::weatherColorMaximum, {0.0, 100.0}},
    {RequiredKey::weatherColorIntercept, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherColorSunlight, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherColorGlare, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherColorAmbient, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},
    {RequiredKey::weatherColorColorChange, {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient}},

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
    constexpr std::array names{std::string_view{"ACES2"}, std::string_view{"AgX"}, std::string_view{"GT7"}};
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
        {RequiredKey::weatherEnabled, &parsed.weatherGradeResponse.enabled},
        {RequiredKey::developerMode, &parsed.developerMode},
        {RequiredKey::selectiveColorEnabled, &parsed.selectiveColor.enabled},
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
        parsed.developerHdr10Reference, parsed.showDeveloperOverlay,
        parsed.forceHdrSwapChainFailure};
    return std::ranges::find(flags, true) != flags.end();
}

bool DiagnosticReferenceAvailable(const PluginConfiguration& parsed) noexcept
{
    if (!parsed.showDeveloperOverlay && !parsed.forceHdrSwapChainFailure) return true;
    return parsed.developerHdr10Reference || parsed.productionRequested;
}

PluginConfigurationError ValidateDiagnosticConfiguration(const PluginConfiguration& parsed) noexcept
{
    if (HasDiagnosticRequest(parsed) && !parsed.developerMode)
        return PluginConfigurationError::invalidCombination;
    if (!DiagnosticReferenceAvailable(parsed)) return PluginConfigurationError::invalidCombination;
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
    ConfigurationParser() noexcept
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

    PluginConfigurationError ParseSection(std::string_view line) noexcept
    {
        if (line.size() < 3 || line.back() != ']') return PluginConfigurationError::invalidSyntax;
        section_ = Trim(line.substr(1, line.size() - 2));
        if (section_.empty()) return PluginConfigurationError::invalidSyntax;
        return PluginConfigurationError::none;
    }

    PluginConfigurationError ParseValue(std::string_view key, std::string_view value) noexcept
    {
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
        if (!Seen(RequiredKey::weatherEnabled))
            parsed.weatherGradeResponse.enabled = parsed.productionRequested;
        return ValidateConfiguration(parsed);
    }

    PluginConfigurationError FinishConfiguration() noexcept
    {
        const auto end = seen_.begin() + static_cast<std::size_t>(RequiredKey::requiredCount);
        if (std::find(seen_.begin(), end, false) != end) return PluginConfigurationError::missingKey;
        return FinishProductionRequest();
    }

    PluginConfigurationResult output_{};
    std::array<bool, static_cast<std::size_t>(RequiredKey::count)> seen_{};
    std::string_view section_;
    std::size_t lineNumber_{};
};

}  // namespace

PluginConfigurationResult ParsePluginConfiguration(std::string_view text) noexcept
{
    return ConfigurationParser{}.Parse(text);
}

PluginConfigurationResult LoadPluginConfiguration(
    const std::filesystem::path& path) noexcept
{
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return {};
        const std::string text(std::istreambuf_iterator<char>(stream), {});
        if (stream.bad()) return {};
        return ParsePluginConfiguration(text);
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
