// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace fo4hdr {
namespace {

bool InRange(double value, double minimum, double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

using Matrix3 = std::array<double, 9>;

constexpr Matrix3 kBt709ToXyz{
    0.4123907992659595, 0.35758433938387796, 0.1804807884018343,
    0.21263900587151036, 0.7151686787677559, 0.07219231536073371,
    0.01933081871559185, 0.11919477979462599, 0.9505321522496607};
constexpr Matrix3 kXyzToBt709{
    3.2409699419045226, -1.537383177570094, -0.4986107602930034,
    -0.9692436362808796, 1.8759675015077202, 0.04155505740717559,
    0.05563007969699366, -0.20397695888897652, 1.0569715142428786};
constexpr Matrix3 kBradford{
    0.8951, 0.2664, -0.1614,
    -0.7502, 1.7135, 0.0367,
    0.0389, -0.0685, 1.0296};
constexpr Matrix3 kBradfordInverse{
    0.9869929054667123, -0.14705425642099013, 0.15996265166373122,
    0.4323052697233945, 0.5183602715367776, 0.0492912282128556,
    -0.008528664575177328, 0.04004282165408487, 0.9684866957875502};
constexpr std::array<double, 3> kD65Xyz{
    0.9504559270516716, 1.0, 1.0890577507598784};

[[nodiscard]] constexpr Matrix3 Multiply(
    const Matrix3& left,
    const Matrix3& right) noexcept
{
    Matrix3 result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            for (std::size_t inner = 0; inner < 3; ++inner)
                result[row * 3 + column] +=
                    left[row * 3 + inner] * right[inner * 3 + column];
    return result;
}

[[nodiscard]] constexpr std::array<double, 3> Multiply(
    const Matrix3& matrix,
    const std::array<double, 3>& vector) noexcept
{
    return {
        matrix[0] * vector[0] + matrix[1] * vector[1] +
            matrix[2] * vector[2],
        matrix[3] * vector[0] + matrix[4] * vector[1] +
            matrix[5] * vector[2],
        matrix[6] * vector[0] + matrix[7] * vector[1] +
            matrix[8] * vector[2]};
}

[[nodiscard]] std::array<double, 2> PlanckianXy(
    double kelvin) noexcept
{
    const auto inverse = 1000.0 / kelvin;
    const auto inverse2 = inverse * inverse;
    const auto inverse3 = inverse2 * inverse;
    double x{};
    if (kelvin <= 4000.0)
        x = -0.2661239 * inverse3 - 0.2343589 * inverse2 +
            0.8776956 * inverse + 0.179910;
    else
        x = -3.0258469 * inverse3 + 2.1070379 * inverse2 +
            0.2226347 * inverse + 0.240390;

    double y{};
    if (kelvin <= 2222.0)
        y = -1.1063814 * x * x * x - 1.34811020 * x * x +
            2.18555832 * x - 0.20219683;
    else if (kelvin <= 4000.0)
        y = -0.9549476 * x * x * x - 1.37418593 * x * x +
            2.09137015 * x - 0.16748867;
    else
        y = 3.0817580 * x * x * x - 5.87338670 * x * x +
            3.75112997 * x - 0.37001483;
    return {x, y};
}

[[nodiscard]] Matrix3 BuildColorBalanceMatrix(
    double temperature,
    double tint) noexcept
{
    if (temperature == 0.0 && tint == 0.0)
        return {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    constexpr double d65X = 0.3127;
    constexpr double d65Y = 0.3290;
    const auto normalizedTemperature = temperature / 100.0;
    const auto endpoint = PlanckianXy(
        normalizedTemperature >= 0.0 ? 4000.0 : 10000.0);
    const auto amount = std::abs(normalizedTemperature);
    const auto targetX = std::lerp(d65X, endpoint[0], amount);
    // Positive Tint is magenta and negative Tint is green. The bounded 0.02
    // xy displacement is deliberately modest and is applied to the target
    // white before Bradford adaptation, never as independent RGB gains.
    const auto targetY = std::clamp(
        std::lerp(d65Y, endpoint[1], amount) - tint * 0.0002,
        0.25,
        0.40);
    const std::array<double, 3> targetXyz{
        targetX / targetY,
        1.0,
        (1.0 - targetX - targetY) / targetY};
    const auto sourceCone = Multiply(kBradford, kD65Xyz);
    const auto targetCone = Multiply(kBradford, targetXyz);
    Matrix3 coneScale{
        targetCone[0] / sourceCone[0], 0.0, 0.0,
        0.0, targetCone[1] / sourceCone[1], 0.0,
        0.0, 0.0, targetCone[2] / sourceCone[2]};
    const auto adaptation = Multiply(
        kBradfordInverse,
        Multiply(coneScale, kBradford));
    return Multiply(kXyzToBt709, Multiply(adaptation, kBt709ToXyz));
}

struct ImageRule
{
    double HdrSettings::* member;
    double minimum;
    double maximum;
    HdrSettingsValidationError error;
};

bool ValidVanillaGradeControls(const HdrSettings& settings) noexcept
{
    const std::array values{settings.vanillaColorGradingStrengthPercent,
        settings.vanillaGradeLuminancePercent, settings.vanillaGradeColorPercent};
    return std::all_of(values.begin(), values.end(), [](double value) {
        return InRange(value, kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent);
    });
}

HdrSettingsValidationError ValidateImageSettings(const HdrSettings& settings) noexcept
{
    constexpr ImageRule rules[]{
        {&HdrSettings::contrast, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::contrastOutOfRange},
        {&HdrSettings::blacks, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::blacksOutOfRange},
        {&HdrSettings::highlights, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::highlightsOutOfRange},
        {&HdrSettings::shadows, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::shadowsOutOfRange},
        {&HdrSettings::saturationPercent, kMinimumSaturationPercent, kMaximumSaturationPercent,
            HdrSettingsValidationError::saturationOutOfRange},
        {&HdrSettings::temperature, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::temperatureOutOfRange},
        {&HdrSettings::tint, kMinimumImageAdjustment, kMaximumImageAdjustment,
            HdrSettingsValidationError::tintOutOfRange},
        {&HdrSettings::exposureEv, kMinimumSceneExposureEv, kMaximumSceneExposureEv,
            HdrSettingsValidationError::exposureOutOfRange},
        {&HdrSettings::bloomStrengthPercent, kMinimumBloomStrengthPercent, kMaximumBloomStrengthPercent,
            HdrSettingsValidationError::bloomStrengthOutOfRange},
    };
    for (const auto& rule : rules)
        if (!InRange(settings.*rule.member, rule.minimum, rule.maximum)) return rule.error;
    return HdrSettingsValidationError::none;
}

bool AgxParametersValid(const ToneMapperParameters& parameters) noexcept
{
    return InRange(parameters.agxMiddleGrayEv, kMinimumAgxMiddleGrayEv, kMaximumAgxMiddleGrayEv) &&
        InRange(parameters.agxContrast, kMinimumAgxContrast, kMaximumAgxContrast) &&
        InRange(parameters.agxHdrPurity, kMinimumAgxHdrPurity, kMaximumAgxHdrPurity) &&
        InRange(parameters.agxHdrShoulderPower, kMinimumAgxHdrShoulderPower,
            kMaximumAgxHdrShoulderPower);
}

bool Gt7ParametersValid(const ToneMapperParameters& parameters) noexcept
{
    return InRange(parameters.gt7ToeStrength, kMinimumGt7ToeStrength, kMaximumGt7ToeStrength) &&
        InRange(parameters.gt7ShoulderStart, kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart) &&
        InRange(parameters.gt7ColorBlend, kMinimumGt7ColorBlend, kMaximumGt7ColorBlend);
}

}  // namespace

bool ValidateToneMapperParameters(
    OperatorLutAlgorithm algorithm, const ToneMapperParameters& parameters) noexcept
{
    switch (algorithm)
    {
    case OperatorLutAlgorithm::aces2: return true;
    case OperatorLutAlgorithm::aces2Lmt:
        return InRange(parameters.acesLookStrength,
            kMinimumAcesLookStrength, kMaximumAcesLookStrength);
    case OperatorLutAlgorithm::agX:
        return AgxParametersValid(parameters);
    case OperatorLutAlgorithm::gt7:
        return Gt7ParametersValid(parameters);
    }
    return false;
}


namespace {
namespace detail {

bool ValidProfileValues(const LutGradeProfile& profile) noexcept
{
    return (profile.overrideLuminance || profile.overrideColor) &&
        std::isfinite(profile.luminancePercent) && profile.luminancePercent >= 0.0 &&
        profile.luminancePercent <= 100.0 && std::isfinite(profile.colorPercent) &&
        profile.colorPercent >= 0.0 && profile.colorPercent <= 100.0;
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
    if (!ValidProfileValues(profile)) return false;
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
    if (!ValidProfileValues(profile)) return false;
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

[[nodiscard]] const LutGradeProfile* FindImageSpaceGradeProfile(
    const LutGradeProfiles& profiles, std::string_view key) noexcept
{
    return detail::FindProfileByKey(profiles, GradeProfileKind::imageSpace, key);
}

[[nodiscard]] LutGradeProfile* FindLutGradeProfile(
    LutGradeProfiles& profiles, std::string_view path) noexcept
{
    return const_cast<LutGradeProfile*>(FindLutGradeProfile(static_cast<const LutGradeProfiles&>(profiles), path));
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

HdrSettingsValidationError ValidateColorGradingSettings(const HdrSettings& settings) noexcept
{
    if (settings.colorGradingMethod != ColorGradingMethod::radial &&
        settings.colorGradingMethod != ColorGradingMethod::nativeReference)
        return HdrSettingsValidationError::unsupportedColorGradingMethod;
    if (!ValidateSelectiveColorSettings(settings.selectiveColor))
        return HdrSettingsValidationError::selectiveColorOutOfRange;
    if (!ValidateLutGradeProfiles(settings.lutGradeProfiles))
        return HdrSettingsValidationError::invalidLutGradeProfiles;
    return HdrSettingsValidationError::none;
}

}  // namespace

HdrSettingsValidationError ValidateHdrSettings(const HdrSettings& settings) noexcept
{
    if (!InRange(settings.displayPeakNits, kMinimumDisplayPeakNits, kMaximumDisplayPeakNits))
    {
        return HdrSettingsValidationError::displayPeakOutOfRange;
    }
    if (!InRange(settings.uiPaperWhiteNits, kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits))
    {
        return HdrSettingsValidationError::uiPaperWhiteOutOfRange;
    }
    if (!InRange(settings.videoWhiteNits, kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits))
    {
        return HdrSettingsValidationError::videoWhiteOutOfRange;
    }
    if (!ValidVanillaGradeControls(settings))
    {
        return HdrSettingsValidationError::vanillaColorGradingOutOfRange;
    }
    const auto gradeError = ValidateColorGradingSettings(settings);
    if (gradeError != HdrSettingsValidationError::none) return gradeError;
    return ValidateImageSettings(settings);
}

HdrSettingsMath DeriveHdrSettingsMath(const HdrSettings& settings) noexcept
{
    HdrSettingsMath result;
    if (ValidateHdrSettings(settings) != HdrSettingsValidationError::none) return result;
    result.contrastSlope = std::exp2(settings.contrast / 100.0);
    result.blacksStrength = settings.blacks * 0.0075;
    result.highlightsEv = settings.highlights / 100.0 * 2.0;
    result.shadowsEv = settings.shadows / 100.0 * 2.0;
    result.saturationScale = settings.saturationPercent / 100.0;
    result.colorBalanceBt709 = BuildColorBalanceMatrix(settings.temperature, settings.tint);
    result.sceneExposureMultiplier = std::exp2(settings.exposureEv);
    result.bloomMultiplier = settings.bloomStrengthPercent / 100.0;
    result.vanillaGradeLuminanceRemoval = 1.0 - settings.vanillaGradeLuminancePercent / 100.0;
    result.vanillaGradeColorRemoval = 1.0 - settings.vanillaGradeColorPercent / 100.0;
    return result;
}

}  // namespace fo4hdr
