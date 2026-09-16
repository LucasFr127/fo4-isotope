// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/PluginConfiguration.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include <Windows.h>

namespace fo4hdr {
namespace {

constexpr std::array<std::string_view, 4> kAlgorithms{"ACES2", "AgX", "GT7", "ACES2LMT"};

std::size_t OwnedKeyIndex(std::string_view section, std::string_view name) noexcept
{
    constexpr std::array<std::array<std::string_view, 2>, 49> keys{{
        {"Output", "DisplayPeakNits"}, {"Output", "UiPaperWhiteNits"},
        {"Output", "VideoWhiteNits"}, {"ToneMapper", "Algorithm"},
        {"AgX", "MiddleGray"}, {"AgX", "Contrast"}, {"AgX", "HdrPurity"},
        {"AgX", "ShoulderPower"}, {"GT7", "ToeStrength"},
        {"GT7", "ShoulderStart"}, {"GT7", "ColorBlend"},
        {"Effects", "VanillaColorGradingStrength"},
        {"Image", "Contrast"}, {"Image", "Blacks"}, {"Image", "Highlights"},
        {"Image", "Shadows"}, {"Image", "Saturation"},
        {"Image", "Temperature"}, {"Image", "Tint"},
        {"Image", "Exposure"}, {"Effects", "BloomStrength"}, {"ACES2LMT", "LookStrength"},
        {"Image", "ColorGradingMethod"},
        {"SelectiveColor", "Enabled"},
        {"SelectiveColor", "Amount"},
        {"SelectiveColor", "RedHue"},
        {"SelectiveColor", "RedSaturation"},
        {"SelectiveColor", "RedBrightness"},
        {"SelectiveColor", "OrangeHue"},
        {"SelectiveColor", "OrangeSaturation"},
        {"SelectiveColor", "OrangeBrightness"},
        {"SelectiveColor", "YellowHue"},
        {"SelectiveColor", "YellowSaturation"},
        {"SelectiveColor", "YellowBrightness"},
        {"SelectiveColor", "GreenHue"},
        {"SelectiveColor", "GreenSaturation"},
        {"SelectiveColor", "GreenBrightness"},
        {"SelectiveColor", "CyanHue"},
        {"SelectiveColor", "CyanSaturation"},
        {"SelectiveColor", "CyanBrightness"},
        {"SelectiveColor", "BlueHue"},
        {"SelectiveColor", "BlueSaturation"},
        {"SelectiveColor", "BlueBrightness"},
        {"SelectiveColor", "MagentaHue"},
        {"SelectiveColor", "MagentaSaturation"},
        {"SelectiveColor", "MagentaBrightness"},
        {"Effects", "VanillaGradeLuminance"}, {"Effects", "VanillaGradeColor"},
        {"Effects", "LutProfilesEnabled"}}};
    const auto found = std::ranges::find_if(keys, [section, name](const auto& key) {
        return key[0] == section && key[1] == name;
    });
    return static_cast<std::size_t>(found - keys.begin());
}

constexpr std::string_view Trim(std::string_view value) noexcept
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

bool ParseNumber(std::string_view text, double& target) noexcept
{
    if (text.empty()) return false;
    double value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) return false;
    if (result.ptr != text.data() + text.size()) return false;
    if (!std::isfinite(value)) return false;
    target = value;
    return true;
}

bool HasProfileOverride(const LutGradeProfile& profile) noexcept
{
    return profile.overrideColor || profile.overrideLuminance;
}

const LutGradeProfile* FindProfile(const LutGradeProfiles& profiles,
    GradeProfileKind kind, std::string_view key) noexcept
{
    return kind == GradeProfileKind::imageSpace ? FindImageSpaceGradeProfile(profiles, key) :
        FindLutGradeProfile(profiles, key);
}

bool ParseProfileNumber(std::string_view text, double& target) noexcept
{
    return ParseNumber(text, target) && target >= 0.0 && target <= 100.0;
}

class IniParser
{
public:
    explicit IniParser(bool profilesOnly = false) noexcept : profilesOnly_(profilesOnly) {}

    PluginConfigurationResult Parse(std::string_view text) noexcept
    {
        if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        while (!text.empty())
        {
            ++result_.line;
            const auto newline = text.find('\n');
            const auto line = Trim(text.substr(0, newline));
            text = newline == std::string_view::npos ?
                std::string_view{} : text.substr(newline + 1);
            if (!ParseLine(line)) return std::move(result_);
        }
        if (!FinishProfile() || !FinishConfiguration()) return std::move(result_);
        result_.status = PluginConfigurationStatus::loaded;
        result_.line = 0;
        return std::move(result_);
    }

private:
    bool SetAlgorithm(std::string_view value) noexcept
    {
        const auto found = std::ranges::find(kAlgorithms, value);
        if (found == kAlgorithms.end()) return false;
        result_.value.algorithm =
            static_cast<OperatorLutAlgorithm>(found - kAlgorithms.begin());
        return true;
    }

    bool SetGradingMethod(std::string_view value) noexcept
    {
        if (value == "Radial") result_.value.display.colorGradingMethod = ColorGradingMethod::radial;
        else if (value == "NativeReference")
            result_.value.display.colorGradingMethod = ColorGradingMethod::nativeReference;
        else return false;
        return true;
    }

    bool SetSelectiveValue(std::size_t index, std::string_view value) noexcept
    {
        auto& settings = result_.value.display.selectiveColor;
        if (index == 23)
        {
            if (value != "0" && value != "1") return false;
            settings.enabled = value == "1";
            return true;
        }
        if (index == 24) return ParseNumber(value, settings.amountPercent);
        const auto offset = index - 25;
        auto& region = settings.regions[offset / 3];
        const std::array targets{&region.hueDegrees, &region.saturationPercent, &region.brightness};
        return ParseNumber(value, *targets[offset % 3]);
    }

    bool SetProfilesEnabled(std::string_view value) noexcept
    {
        if (value != "0" && value != "1") return false;
        result_.value.display.lutGradeProfiles.enabled = value == "1";
        return true;
    }

    bool SetNumericValue(std::size_t index, std::string_view value) noexcept
    {
        if (index >= 46)
        {
            const std::array targets{&result_.value.display.vanillaGradeLuminancePercent,
                &result_.value.display.vanillaGradeColorPercent};
            return ParseNumber(value, *targets[index - 46]);
        }
        if (index >= 23) return SetSelectiveValue(index, value);
        auto& settings = result_.value.display;
        auto& parameters = settings.toneMapperParameters;
        const std::array targets{&settings.displayPeakNits, &settings.uiPaperWhiteNits,
            &settings.videoWhiteNits, static_cast<double*>(nullptr),
            &parameters.agxMiddleGrayEv, &parameters.agxContrast, &parameters.agxHdrPurity,
            &parameters.agxHdrShoulderPower, &parameters.gt7ToeStrength,
            &parameters.gt7ShoulderStart, &parameters.gt7ColorBlend,
            &settings.vanillaColorGradingStrengthPercent, &settings.contrast, &settings.blacks,
            &settings.highlights, &settings.shadows, &settings.saturationPercent,
            &settings.temperature, &settings.tint,
            &settings.exposureEv, &settings.bloomStrengthPercent, &parameters.acesLookStrength};
        return ParseNumber(value, *targets[index]);
    }

    bool SetValue(std::size_t index, std::string_view value) noexcept
    {
        if (seen_[index]) return false;
        seen_[index] = true;
        if (index == 3) return SetAlgorithm(value);
        if (index == 22) return SetGradingMethod(value);
        if (index == 48) return SetProfilesEnabled(value);
        return SetNumericValue(index, value);
    }

    bool FinishConfiguration() const noexcept
    {
        if (profilesOnly_) return ValidateLutGradeProfiles(result_.value.display.lutGradeProfiles);
        // The original four keys remain required; absent optional controls use defaults.
        return std::find(seen_.begin(), seen_.begin() + 4, false) == seen_.begin() + 4 &&
            ValidatePluginConfiguration(result_.value);
    }

    bool FinishProfile() noexcept
    {
        if (!hasProfile_) return true;
        if (!inherit_ && !HasProfileOverride(currentProfile_))
            return false;
        auto& profiles = inherit_ ? result_.profileInheritances : result_.value.display.lutGradeProfiles;
        if (profiles.count == profiles.entries.size()) return false;
        profiles.entries[profiles.count++] = currentProfile_;
        return true;
    }

    bool StartProfile(bool imageSpace) noexcept
    {
        const auto key = section_.substr(imageSpace ? 5 : 4);
        currentProfile_ = {};
        currentProfile_.kind = imageSpace ? GradeProfileKind::imageSpace : GradeProfileKind::lutAsset;
        const auto normalized = imageSpace ? NormalizeImageSpaceGradeKey(key, currentProfile_.path) :
            NormalizeLutGradePath(key, currentProfile_.path);
        if (!normalized) return false;
        const auto& profiles = result_.value.display.lutGradeProfiles;
        const auto& masks = result_.profileInheritances;
        const auto found = FindProfile(profiles, currentProfile_.kind, key);
        const auto masked = FindProfile(masks, currentProfile_.kind, key);
        if (found || masked) return false;
        hasProfile_ = true;
        return true;
    }

    bool ParseInheritance(std::string_view value) noexcept
    {
        if (inherit_ || HasProfileOverride(currentProfile_)) return false;
        if (value != "1") return false;
        inherit_ = true;
        return true;
    }

    bool ParseProfileValue(std::string_view key, std::string_view value) noexcept
    {
        if (key == "Inherit") return ParseInheritance(value);
        if (inherit_) return false;
        if (key == "Color") return ParseProfileChannel(true, value);
        if (key == "Luminance") return ParseProfileChannel(false, value);
        return false;
    }

    bool ParseProfileChannel(bool color, std::string_view value) noexcept
    {
        auto& present = color ? currentProfile_.overrideColor : currentProfile_.overrideLuminance;
        auto& target = color ? currentProfile_.colorPercent : currentProfile_.luminancePercent;
        if (present || !ParseProfileNumber(value, target)) return false;
        present = true;
        return true;
    }

    bool ValidAssignmentContext(std::string_view value) const noexcept
    {
        if (!profilesOnly_) return true;
        return !section_.empty() && !value.empty();
    }

    bool ParseAssignment(std::string_view line) noexcept
    {
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) return false;
        const auto key = Trim(line.substr(0, equals));
        const auto remainder = line.substr(equals + 1);
        const auto value = Trim(remainder.substr(0, remainder.find_first_of(";#")));
        if (key.empty()) return false;
        if (!ValidAssignmentContext(value)) return false;
        if (hasProfile_) return ParseProfileValue(key, value);
        if (profilesOnly_) return true;
        const auto index = OwnedKeyIndex(section_, key);
        if (index == seen_.size()) return true;
        return SetValue(index, value);
    }

    bool ParseSection(std::string_view line) noexcept
    {
        if (!FinishProfile()) return false;
        if (line.size() < 3) return false;
        if (line.back() != ']') return false;
        section_ = Trim(line.substr(1, line.size() - 2));
        if (section_.empty()) return false;
        hasProfile_ = false;
        inherit_ = false;
        if (section_.starts_with("IMGS:")) return StartProfile(true);
        if (section_.starts_with("LUT:")) return StartProfile(false);
        return true;
    }

    bool ParseLine(std::string_view line) noexcept
    {
        if (line.empty()) return true;
        if (line.front() == ';' || line.front() == '#') return true;
        if (line.front() == '[') return ParseSection(line);
        return ParseAssignment(line);
    }

    PluginConfigurationResult result_;
    std::string_view section_;
    std::array<bool, 49> seen_{};
    bool profilesOnly_{};
    bool inherit_{};
    bool hasProfile_{};
    LutGradeProfile currentProfile_{};
};

bool ContainsProfile(const LutGradeProfiles* profiles, const LutGradeProfile& profile) noexcept
{
    return profiles && FindProfile(*profiles, profile.kind, profile.path.data());
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
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) return {{}, PluginConfigurationStatus::ioError};
    if (!exists) return {{}, PluginConfigurationStatus::missing};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {{}, PluginConfigurationStatus::ioError};
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) return {{}, PluginConfigurationStatus::ioError};
    return IniParser(profilesOnly).Parse(text);
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
                PluginConfigurationStatus::ioError, 0});
    }
}

void LoadDistributedFile(const std::filesystem::path& file, PluginConfigurationResult& result)
{
    auto parsed = ReadConfiguration(file, true);
    if (parsed.status != PluginConfigurationStatus::loaded)
    {
        result.profileIssues.push_back({file, parsed.status, parsed.line});
        return;
    }
    auto candidate = result.distributedProfiles;
    if (!AppendUnclaimedProfiles(candidate, parsed.value.display.lutGradeProfiles) ||
        !AppendUnclaimedProfiles(candidate, parsed.profileInheritances))
    {
        parsed.status = PluginConfigurationStatus::invalid;
    }
    if (parsed.status != PluginConfigurationStatus::loaded)
        result.profileIssues.push_back({file, parsed.status, parsed.line});
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
            PluginConfigurationStatus::ioError, 0});
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

PluginConfigurationResult ReadPersonalProfiles(const std::filesystem::path& directory,
    PluginConfigurationResult& result)
{
    const auto path = directory / L"Personal.ini";
    auto personal = ReadConfiguration(path, true);
    if (personal.status == PluginConfigurationStatus::loaded ||
        personal.status == PluginConfigurationStatus::missing) return personal;
    result.profileIssues.push_back({path, personal.status, personal.line});
    return {};
}

bool MergePersonalProfiles(PluginConfigurationResult& result,
    const PluginConfigurationResult& personal) noexcept
{
    // A claimed key replaces its lower-priority file, including unset components.
    auto merged = personal.value.display.lutGradeProfiles;
    merged.enabled = result.value.display.lutGradeProfiles.enabled;
    if (!AppendUnclaimedProfiles(merged, result.value.display.lutGradeProfiles,
            &personal.profileInheritances)) return false;
    if (!AppendUnclaimedProfiles(merged, result.distributedProfiles,
            &personal.profileInheritances, &result.profileInheritances)) return false;
    RemoveInheritanceMasks(merged);
    result.value.display.lutGradeProfiles = merged;
    return true;
}

}  // namespace

bool PluginConfiguration::operator==(const PluginConfiguration& other) const noexcept
{
    if (algorithm != other.algorithm) return false;
    if (display.colorGradingMethod != other.display.colorGradingMethod) return false;
    if (display.selectiveColor != other.display.selectiveColor) return false;
    if (display.lutGradeProfiles != other.display.lutGradeProfiles) return false;
    const std::array current{
        display.displayPeakNits, display.uiPaperWhiteNits, display.videoWhiteNits,
        display.vanillaColorGradingStrengthPercent, display.vanillaGradeLuminancePercent,
        display.vanillaGradeColorPercent, display.contrast, display.blacks,
        display.highlights, display.shadows, display.saturationPercent,
        display.temperature, display.tint, display.exposureEv, display.bloomStrengthPercent};
    const std::array candidate{other.display.displayPeakNits,
        other.display.uiPaperWhiteNits, other.display.videoWhiteNits,
        other.display.vanillaColorGradingStrengthPercent,
        other.display.vanillaGradeLuminancePercent, other.display.vanillaGradeColorPercent,
        other.display.contrast, other.display.blacks, other.display.highlights,
        other.display.shadows, other.display.saturationPercent,
        other.display.temperature, other.display.tint,
        other.display.exposureEv, other.display.bloomStrengthPercent};
    return current == candidate &&
        display.toneMapperParameters == other.display.toneMapperParameters;
}

bool ValidatePluginConfiguration(const PluginConfiguration& value) noexcept
{
    if (ValidateHdrSettings(value.display) != HdrSettingsValidationError::none) return false;
    if (value.display.displayPeakNits < 400.0 || value.display.displayPeakNits > 2000.0) return false;
    // Saved inactive operator controls must remain valid when their operator is selected later.
    constexpr std::array algorithms{OperatorLutAlgorithm::aces2Lmt,
        OperatorLutAlgorithm::agX, OperatorLutAlgorithm::gt7};
    const auto parametersValid = std::ranges::all_of(algorithms, [&](auto algorithm) {
        return ValidateToneMapperParameters(algorithm, value.display.toneMapperParameters);
    });
    return parametersValid && !ToString(value.algorithm).empty();
}

PluginConfigurationResult ParsePluginConfiguration(std::string_view text) noexcept
{
    return IniParser{}.Parse(text);
}

PluginConfigurationResult ParseGradeProfileConfiguration(std::string_view text) noexcept
{
    return IniParser(true).Parse(text);
}

PluginConfigurationResult LoadPluginConfiguration(const std::filesystem::path& path) noexcept
{
    try
    {
        auto result = ReadConfiguration(path, false);
        if (result.status != PluginConfigurationStatus::loaded) return result;
        const auto directory = path.parent_path() / path.stem();
        LoadDistributedProfiles(directory, result);
        const auto personal = ReadPersonalProfiles(directory, result);
        if (!MergePersonalProfiles(result, personal)) result.status = PluginConfigurationStatus::invalid;
        return result;
    }
    catch (...) { return {{}, PluginConfigurationStatus::ioError}; }
}

std::string_view ToString(OperatorLutAlgorithm algorithm) noexcept
{
    const auto index = static_cast<std::size_t>(algorithm);
    return index < kAlgorithms.size() ? kAlgorithms[index] : std::string_view{};
}

std::string_view ToString(ColorGradingMethod method) noexcept
{
    constexpr std::array<std::string_view, 2> names{"Radial", "NativeReference"};
    const auto index = static_cast<std::size_t>(method);
    return index < names.size() ? names[index] : std::string_view{};
}

}  // namespace fo4hdr
