// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "SettingsPersistence.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace fo4hdr {
namespace {

static_assert(sizeof(wchar_t) == 2);

enum class DisplayCalibrationOperation : std::uint8_t
{
    none,
    save,
    load
};

enum class DisplayCalibrationReadResult : std::uint8_t
{
    loaded,
    missing,
    failed
};

struct PersistedKey
{
    std::string_view section;
    std::string_view key;
    double HdrSettings::*number{};
    double HdrInterfaceSettings::*interfaceNumber{};
    bool algorithm{};
    bool hotkey{};
    bool colorGradingMethod{};
    double ToneMapperParameters::*toneMapperNumber{};
    bool lutProfilesEnabled{};
};

constexpr std::array<PersistedKey, 32> kPersistedKeys{{
    {"Output", "DisplayPeakNits", &HdrSettings::displayPeakNits},
    {"Output", "UiPaperWhiteNits", &HdrSettings::uiPaperWhiteNits},
    {"Output", "VideoWhiteNits", &HdrSettings::videoWhiteNits},
    {"ToneMapper", "Algorithm", nullptr, nullptr, true},
    {"ACES2LMT", "LookStrength", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::acesLookStrength},
    {"AgX", "MiddleGray", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::agxMiddleGrayEv},
    {"AgX", "Contrast", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::agxContrast},
    {"AgX", "HdrPurity", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::agxHdrPurity},
    {"AgX", "ShoulderPower", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::agxHdrShoulderPower},
    {"GT7", "ToeStrength", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::gt7ToeStrength},
    {"GT7", "ShoulderStart", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::gt7ShoulderStart},
    {"GT7", "ColorBlend", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::gt7ColorBlend},
    {"Image", "Exposure", &HdrSettings::exposureEv},
    {"Image", "Contrast", &HdrSettings::contrast},
    {"Image", "Blacks", &HdrSettings::blacks},
    {"Image", "Highlights", &HdrSettings::highlights},
    {"Image", "Shadows", &HdrSettings::shadows},
    {"Image", "Saturation", &HdrSettings::saturationPercent},
    {"Image", "Temperature", &HdrSettings::temperature},
    {"Image", "Tint", &HdrSettings::tint},
    {"Image", "ColorGradingMethod", nullptr, nullptr, false, false, true},
    {"Effects", "VanillaColorGradingStrength",
        &HdrSettings::vanillaColorGradingStrengthPercent},
    {"Effects", "VanillaGradeLuminance",
        &HdrSettings::vanillaGradeLuminancePercent},
    {"Effects", "VanillaGradeColor",
        &HdrSettings::vanillaGradeColorPercent},
    {"Effects", "LutProfilesEnabled", nullptr, nullptr, false, false,
        false, nullptr, true},
    {"Effects", "BloomStrength", &HdrSettings::bloomStrengthPercent},
    {"Interface", "Scale", nullptr, &HdrInterfaceSettings::scale},
    {"Interface", "WindowWidth", nullptr,
        &HdrInterfaceSettings::windowWidth},
    {"Interface", "WindowHeight", nullptr,
        &HdrInterfaceSettings::windowHeight},
    {"Interface", "WindowX", nullptr, &HdrInterfaceSettings::windowX},
    {"Interface", "WindowY", nullptr, &HdrInterfaceSettings::windowY},
    {"Interface", "ToggleKey", nullptr, nullptr, false, true}
}};

struct SelectivePersistedKey
{
    std::string_view key;
    std::size_t region{};
    double SelectiveColorRegionSettings::*number{};
};

constexpr std::array<SelectivePersistedKey, 21> kSelectivePersistedKeys{{
    {"RedHue", 0, &SelectiveColorRegionSettings::hueDegrees},
    {"RedSaturation", 0, &SelectiveColorRegionSettings::saturationPercent},
    {"RedBrightness", 0, &SelectiveColorRegionSettings::brightness},
    {"OrangeHue", 1, &SelectiveColorRegionSettings::hueDegrees},
    {"OrangeSaturation", 1, &SelectiveColorRegionSettings::saturationPercent},
    {"OrangeBrightness", 1, &SelectiveColorRegionSettings::brightness},
    {"YellowHue", 2, &SelectiveColorRegionSettings::hueDegrees},
    {"YellowSaturation", 2, &SelectiveColorRegionSettings::saturationPercent},
    {"YellowBrightness", 2, &SelectiveColorRegionSettings::brightness},
    {"GreenHue", 3, &SelectiveColorRegionSettings::hueDegrees},
    {"GreenSaturation", 3, &SelectiveColorRegionSettings::saturationPercent},
    {"GreenBrightness", 3, &SelectiveColorRegionSettings::brightness},
    {"CyanHue", 4, &SelectiveColorRegionSettings::hueDegrees},
    {"CyanSaturation", 4, &SelectiveColorRegionSettings::saturationPercent},
    {"CyanBrightness", 4, &SelectiveColorRegionSettings::brightness},
    {"BlueHue", 5, &SelectiveColorRegionSettings::hueDegrees},
    {"BlueSaturation", 5, &SelectiveColorRegionSettings::saturationPercent},
    {"BlueBrightness", 5, &SelectiveColorRegionSettings::brightness},
    {"MagentaHue", 6, &SelectiveColorRegionSettings::hueDegrees},
    {"MagentaSaturation", 6, &SelectiveColorRegionSettings::saturationPercent},
    {"MagentaBrightness", 6, &SelectiveColorRegionSettings::brightness}
}};

std::filesystem::path g_iniPath;
std::filesystem::path g_personalIniPath;
// Full saves derive personal profile overrides from this startup snapshot;
// distributed INIs remain read-only for the process lifetime.
LutGradeProfiles g_distributedProfiles{};
HdrSettings g_requestedSettings{};
HdrInterfaceSettings g_requestedInterfaceSettings{};
double g_requestedWindowX{-1.0};
double g_requestedWindowY{-1.0};
DisplayCalibrationProfile g_requestedDisplayCalibration{};
DisplayCalibrationOperation g_requestedDisplayCalibrationOperation{};
std::uint64_t g_requestedPositionGeneration{}; // under g_requestLock
std::atomic<SettingsPersistenceState> g_positionState{
    SettingsPersistenceState::unavailable};
std::atomic_flag g_requestLock = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> g_requestedGeneration{};
std::atomic<std::uint64_t> g_completedGeneration{};
std::atomic<std::uint64_t> g_requestedDisplayCalibrationGeneration{};
DisplayCalibrationProfile g_completedDisplayCalibration{};
std::uint64_t g_completedDisplayCalibrationGeneration{}; // under g_requestLock
std::atomic<SettingsPersistenceState> g_state{
    SettingsPersistenceState::unavailable};
std::atomic<SettingsPersistenceState> g_displayCalibrationState{
    SettingsPersistenceState::unavailable};
std::atomic<bool> g_started{};
HANDLE g_saveEvent{};

void LockPersistenceWorker() noexcept
{
    while (g_requestLock.test_and_set(std::memory_order_acquire))
        YieldProcessor();
}

[[nodiscard]] constexpr std::string_view Trim(std::string_view value) noexcept
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return value.substr(value.size());
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

struct IniLine
{
    std::string_view raw;
    std::string_view trimmed;
    bool terminated{};

    [[nodiscard]] bool IsSection() const noexcept
    {
        return trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
    }

    [[nodiscard]] std::string_view Section() const noexcept
    {
        return Trim(trimmed.substr(1, trimmed.size() - 2));
    }

    [[nodiscard]] std::optional<std::string_view> Key() const noexcept
    {
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#')
            return std::nullopt;
        const auto equals = trimmed.find('=');
        if (equals == std::string_view::npos) return std::nullopt;
        return Trim(trimmed.substr(0, equals));
    }
};

[[nodiscard]] IniLine TakeIniLine(std::string_view& remaining) noexcept
{
    const auto end = remaining.find('\n');
    auto raw = remaining.substr(0, end);
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    const bool terminated = end != std::string_view::npos;
    remaining = terminated ? remaining.substr(end + 1) : std::string_view{};
    return {raw, Trim(raw), terminated};
}

[[nodiscard]] std::string_view IniNewline(std::string_view original) noexcept
{
    return original.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
}

void FinishIniLine(std::string& result, std::string_view newline)
{
    if (!result.empty() && !result.ends_with(newline)) result.append(newline);
}

constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF"};

[[nodiscard]] std::string FormatNumber(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    auto result = stream.str();
    while (result.size() > 1 && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    return result;
}

[[nodiscard]] std::string KeyValue(
    const PersistedKey& key,
    const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings)
{
    std::string result(key.key);
    result.push_back('=');
    if (key.algorithm)
        result.append(ToString(settings.productionAlgorithm));
    else if (key.hotkey)
        result.append(FormatHdrSettingsHotkey(interfaceSettings.toggleKey).data());
    else if (key.colorGradingMethod)
        result.append(ToString(settings.colorGradingMethod));
    else if (key.interfaceNumber != nullptr)
        result.append(FormatNumber(interfaceSettings.*key.interfaceNumber));
    else if (key.toneMapperNumber != nullptr)
        result.append(FormatNumber(
            settings.toneMapperParameters.*key.toneMapperNumber));
    else if (key.lutProfilesEnabled)
        result.push_back(settings.lutGradeProfiles.enabled ? '1' : '0');
    else
        result.append(FormatNumber(settings.*key.number));
    return result;
}

[[nodiscard]] std::string SelectiveKeyValue(
    const SelectivePersistedKey& key,
    const HdrSettings& settings)
{
    const auto& region = settings.selectiveColor.regions[key.region];
    return std::string(key.key) + "=" + FormatNumber(region.*key.number);
}

constexpr std::array<std::string_view, 3> kDisplayCalibrationKeys{
    "DisplayPeakNits", "UiPaperWhiteNits", "VideoWhiteNits"};

[[nodiscard]] bool ValidDisplayIdentity(
    std::wstring_view monitorDevicePath) noexcept
{
    if (monitorDevicePath.empty() ||
        monitorDevicePath.size() > kMaximumDisplayIdentityLength)
        return false;
    for (const auto character : monitorDevicePath)
        if (character < L' ' || std::wstring_view{L"[]="}.find(character) != std::wstring_view::npos)
            return false;
    return true;
}

void CopyDisplayIdentity(
    std::wstring_view source,
    std::array<wchar_t, kMaximumDisplayIdentityLength + 1>& destination) noexcept
{
    destination.fill(L'\0');
    std::copy(source.begin(), source.end(), destination.begin());
}

[[nodiscard]] bool ValidDisplayCalibration(
    const DisplayCalibrationProfile& profile) noexcept
{
    HdrSettings settings{};
    settings.displayPeakNits = profile.displayPeakNits;
    settings.uiPaperWhiteNits = profile.uiPaperWhiteNits;
    settings.videoWhiteNits = profile.videoWhiteNits;
    return ValidateHdrSettings(settings) == HdrSettingsValidationError::none;
}

[[nodiscard]] double DisplayCalibrationValue(
    const DisplayCalibrationProfile& profile, std::size_t index) noexcept
{
    if (index == 0) return profile.displayPeakNits;
    if (index == 1) return profile.uiPaperWhiteNits;
    return profile.videoWhiteNits;
}

void SetDisplayCalibrationValue(
    DisplayCalibrationProfile& profile, std::size_t index, double value) noexcept
{
    if (index == 0) profile.displayPeakNits = value;
    else if (index == 1) profile.uiPaperWhiteNits = value;
    else profile.videoWhiteNits = value;
}

[[nodiscard]] std::optional<std::size_t> DisplayCalibrationKeyIndex(
    std::string_view key) noexcept
{
    for (std::size_t index = 0; index < kDisplayCalibrationKeys.size(); ++index)
        if (key == kDisplayCalibrationKeys[index]) return index;
    return std::nullopt;
}

[[nodiscard]] std::string DisplayCalibrationSection(
    const DisplayCalibrationProfile& profile)
{
    constexpr char digits[]{"0123456789ABCDEF"};
    std::string section{"[DisplayProfile:"};
    const std::wstring_view identity(profile.monitorDevicePath.data());
    section.reserve(section.size() + identity.size() * 4 + 1);
    for (const auto character : identity)
    {
        const auto code = static_cast<std::uint16_t>(character);
        section.push_back(digits[(code >> 12) & 0xF]);
        section.push_back(digits[(code >> 8) & 0xF]);
        section.push_back(digits[(code >> 4) & 0xF]);
        section.push_back(digits[code & 0xF]);
    }
    section.push_back(']');
    return section;
}

[[nodiscard]] bool ParseCalibrationKey(const IniLine& line,
    DisplayCalibrationProfile& loaded, std::array<bool, kDisplayCalibrationKeys.size()>& seen) noexcept
{
    const auto key = line.Key();
    if (!key) return true;
    const auto index = DisplayCalibrationKeyIndex(*key);
    if (!index) return true;
    if (seen[*index]) return false;
    const auto value = Trim(line.trimmed.substr(line.trimmed.find('=') + 1));
    double number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
    if (!std::isfinite(number)) return false;
    SetDisplayCalibrationValue(loaded, *index, number);
    seen[*index] = true;
    return true;
}

[[nodiscard]] DisplayCalibrationReadResult CalibrationReadResult(
    std::size_t sectionCount, const std::array<bool, kDisplayCalibrationKeys.size()>& seen,
    const DisplayCalibrationProfile& loaded) noexcept
{
    if (sectionCount == 0) return DisplayCalibrationReadResult::missing;
    if (sectionCount != 1) return DisplayCalibrationReadResult::failed;
    if (std::ranges::find(seen, false) != seen.end()) return DisplayCalibrationReadResult::failed;
    return ValidDisplayCalibration(loaded) ? DisplayCalibrationReadResult::loaded :
        DisplayCalibrationReadResult::failed;
}

[[nodiscard]] DisplayCalibrationReadResult ParseDisplayCalibrationIni(
    std::string_view original, const DisplayCalibrationProfile& requested,
    DisplayCalibrationProfile& loaded) noexcept
{
    try
    {
        const auto targetSection = DisplayCalibrationSection(requested);
        std::array<bool, kDisplayCalibrationKeys.size()> seen{};
        std::size_t sectionCount{};
        bool target{};
        loaded = requested;
        while (!original.empty())
        {
            const auto line = TakeIniLine(original);
            if (line.IsSection())
            {
                target = line.trimmed == targetSection;
                sectionCount += target;
            }
            else if (target && !ParseCalibrationKey(line, loaded, seen))
                return DisplayCalibrationReadResult::failed;
        }
        return CalibrationReadResult(sectionCount, seen, loaded);
    }
    catch (...) { return DisplayCalibrationReadResult::failed; }
}

[[nodiscard]] std::size_t CountIniSection(std::string_view remaining, std::string_view target)
{
    std::size_t count{};
    while (!remaining.empty()) count += TakeIniLine(remaining).trimmed == target;
    return count;
}

class DisplayCalibrationIniWriter
{
public:
    DisplayCalibrationIniWriter(const DisplayCalibrationProfile& profile, std::string_view original) :
        profile_(profile), targetSection_(DisplayCalibrationSection(profile)), newline_(IniNewline(original))
    {}

    [[nodiscard]] std::optional<std::string> Rewrite(std::string_view original)
    {
        const auto count = CountIniSection(original, targetSection_);
        if (count > 1) return std::nullopt;
        result_.reserve(original.size() + targetSection_.size() + 96);
        if (count == 0)
        {
            result_.assign(original);
            FinishIniLine(result_, newline_);
            result_.append(targetSection_).append(newline_);
            AppendMissing();
            return std::move(result_);
        }
        while (!original.empty()) AppendLine(TakeIniLine(original));
        if (target_) AppendMissing();
        return std::move(result_);
    }

private:
    void AppendValue(std::size_t index)
    {
        result_.append(kDisplayCalibrationKeys[index]).append("=")
            .append(FormatNumber(DisplayCalibrationValue(profile_, index))).append(newline_);
    }

    void AppendMissing()
    {
        for (std::size_t index = 0; index < seen_.size(); ++index)
            if (!seen_[index]) AppendValue(index);
    }

    [[nodiscard]] bool ReplaceKey(const IniLine& line)
    {
        if (!target_) return false;
        const auto key = line.Key();
        if (!key) return false;
        const auto index = DisplayCalibrationKeyIndex(*key);
        if (!index) return false;
        if (!seen_[*index]) AppendValue(*index);
        seen_[*index] = true;
        return true;
    }

    void AppendLine(const IniLine& line)
    {
        if (line.IsSection())
        {
            if (target_) AppendMissing();
            target_ = line.trimmed == targetSection_;
        }
        if (!ReplaceKey(line)) result_.append(line.raw).append(newline_);
    }

    const DisplayCalibrationProfile& profile_;
    std::string targetSection_;
    std::string_view newline_;
    std::string result_;
    std::array<bool, kDisplayCalibrationKeys.size()> seen_{};
    bool target_{};
};

[[nodiscard]] std::optional<std::string> RewriteDisplayCalibrationIni(
    std::string_view original, const DisplayCalibrationProfile& profile)
{
    return DisplayCalibrationIniWriter(profile, original).Rewrite(original);
}

[[nodiscard]] const LutGradeProfile* FindMatchingProfile(
    const LutGradeProfiles& profiles,
    const LutGradeProfile& profile) noexcept
{
    return profile.kind == GradeProfileKind::lutAsset ?
        FindLutGradeProfile(profiles, profile.path.data()) :
        FindImageSpaceGradeProfile(profiles, profile.path.data());
}

[[nodiscard]] bool SameProfileValues(
    const LutGradeProfile& left,
    const LutGradeProfile& right) noexcept
{
    return left.kind == right.kind && left.path == right.path &&
        left.overrideColor == right.overrideColor &&
        left.overrideLuminance == right.overrideLuminance &&
        (!left.overrideColor || left.colorPercent == right.colorPercent) &&
        (!left.overrideLuminance ||
            left.luminancePercent == right.luminancePercent);
}

class GradeProfileIniWriter
{
public:
    [[nodiscard]] std::string Rewrite(std::string_view original,
        const LutGradeProfiles& profiles, const LutGradeProfiles& distributedProfiles)
    {
        const auto hasBom = original.starts_with(kUtf8Bom);
        if (hasBom) original.remove_prefix(kUtf8Bom.size());
        newline_ = IniNewline(original);
        result_.reserve(original.size() + kUtf8Bom.size() +
            (profiles.count + distributedProfiles.count) * 320);
        if (hasBom) result_.append(kUtf8Bom);
        CopyOtherSections(original);
        FinishIniLine(result_, newline_);
        AppendOverrides(profiles, distributedProfiles);
        AppendInherited(profiles, distributedProfiles);
        return std::move(result_);
    }

private:
    void CopyOtherSections(std::string_view original)
    {
        bool gradeProfileSection{};
        while (!original.empty())
        {
            const auto line = TakeIniLine(original);
            if (line.IsSection())
            {
                const auto section = line.Section();
                gradeProfileSection = section.starts_with("LUT:") || section.starts_with("IMGS:");
            }
            if (gradeProfileSection) continue;
            result_.append(line.raw);
            if (line.terminated) result_.append(newline_);
        }
    }

    void AppendProfile(const LutGradeProfile& profile, bool inherit)
    {
        result_.append(profile.kind == GradeProfileKind::imageSpace ? "[IMGS:" : "[LUT:")
            .append(profile.path.data()).append("]").append(newline_);
        if (inherit)
        {
            result_.append("Inherit=1").append(newline_);
            return;
        }
        if (profile.overrideColor)
            result_.append("Color=").append(FormatNumber(profile.colorPercent)).append(newline_);
        if (profile.overrideLuminance)
            result_.append("Luminance=").append(FormatNumber(profile.luminancePercent)).append(newline_);
    }

    void AppendOverrides(const LutGradeProfiles& profiles, const LutGradeProfiles& distributedProfiles)
    {
        for (std::size_t index = 0; index < profiles.count; ++index)
        {
            const auto& profile = profiles.entries[index];
            const auto* distributed = FindMatchingProfile(distributedProfiles, profile);
            if (distributed == nullptr || !SameProfileValues(profile, *distributed))
                AppendProfile(profile, false);
        }
    }

    void AppendInherited(const LutGradeProfiles& profiles, const LutGradeProfiles& distributedProfiles)
    {
        for (std::size_t index = 0; index < distributedProfiles.count; ++index)
        {
            const auto& profile = distributedProfiles.entries[index];
            if (FindMatchingProfile(profiles, profile) == nullptr) AppendProfile(profile, true);
        }
    }

    std::string result_;
    std::string_view newline_;
};

[[nodiscard]] std::string RewriteGradeProfileSections(std::string_view original,
    const LutGradeProfiles& profiles, const LutGradeProfiles& distributedProfiles)
{
    return GradeProfileIniWriter{}.Rewrite(original, profiles, distributedProfiles);
}

class SettingsIniWriter
{
public:
    SettingsIniWriter(const HdrSettings& settings,
        const HdrInterfaceSettings& interfaceSettings, bool positionOnly) :
        settings_(settings), interfaceSettings_(interfaceSettings), positionOnly_(positionOnly)
    {
        if (!positionOnly_) return;
        for (std::size_t index = 0; index < kPersistedKeys.size(); ++index)
            seen_[index] = !IsPositionKey(kPersistedKeys[index]);
        selectiveSeen_.fill(true);
        selectiveEnabledSeen_ = true;
        selectiveAmountSeen_ = true;
    }

    [[nodiscard]] std::string Rewrite(std::string_view original)
    {
        const auto hasBom = original.starts_with(kUtf8Bom);
        if (hasBom) original.remove_prefix(kUtf8Bom.size());
        newline_ = IniNewline(original);
        result_.reserve(original.size() + kUtf8Bom.size() + 512);
        if (hasBom) result_.append(kUtf8Bom);
        while (!original.empty()) AppendLine(TakeIniLine(original));
        FinishIniLine(result_, newline_);
        AppendMissingGroups();
        return std::move(result_);
    }

private:
    [[nodiscard]] static bool IsPositionKey(const PersistedKey& key) noexcept
    {
        return key.key == "WindowX" || key.key == "WindowY";
    }

    [[nodiscard]] bool IsRetiredKey(std::string_view name) const noexcept
    {
        // A full save performs one-way migration of retired names.
        if (positionOnly_) return false;
        if (section_ == "ToneMapper") return name == "ExposureEV";
        return section_ == "Image" && name == "GradeTransfer";
    }

    [[nodiscard]] bool ReplaceKnownKey(std::string_view name)
    {
        for (std::size_t index = 0; index < kPersistedKeys.size(); ++index)
        {
            const auto& key = kPersistedKeys[index];
            if (positionOnly_ && !IsPositionKey(key)) continue;
            if (section_ != key.section || name != key.key) continue;
            result_.append(KeyValue(key, settings_, interfaceSettings_));
            seen_[index] = true;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool ReplaceSelectiveRegion(std::string_view name)
    {
        for (std::size_t index = 0; index < kSelectivePersistedKeys.size(); ++index)
        {
            if (name != kSelectivePersistedKeys[index].key) continue;
            result_.append(SelectiveKeyValue(kSelectivePersistedKeys[index], settings_));
            selectiveSeen_[index] = true;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool ReplaceSelectiveKey(std::string_view name)
    {
        if (name == "Enabled")
        {
            result_.append("Enabled=").push_back(settings_.selectiveColor.enabled ? '1' : '0');
            selectiveEnabledSeen_ = true;
            return true;
        }
        if (name == "Amount")
        {
            result_.append("Amount=").append(FormatNumber(settings_.selectiveColor.amountPercent));
            selectiveAmountSeen_ = true;
            return true;
        }
        return ReplaceSelectiveRegion(name);
    }

    [[nodiscard]] bool ReplaceKey(std::string_view name)
    {
        const auto retired = IsRetiredKey(name);
        const auto known = ReplaceKnownKey(name);
        if (!positionOnly_ && section_ == "SelectiveColor")
            return ReplaceSelectiveKey(name) || known || retired;
        return known || retired;
    }

    void AppendLine(const IniLine& line)
    {
        if (line.IsSection()) section_ = line.Section();
        const auto key = line.Key();
        const auto replaced = key && ReplaceKey(*key);
        if (!replaced) result_.append(line.raw);
        if (line.terminated) result_.append(newline_);
    }

    [[nodiscard]] bool MissingSelectiveKeys() const noexcept
    {
        return !selectiveEnabledSeen_ || !selectiveAmountSeen_ ||
            std::ranges::find(selectiveSeen_, false) != selectiveSeen_.end();
    }
    void AppendMissingSelective()
    {
        const auto missing = MissingSelectiveKeys();
        if (!missing) return;
        result_.append("[SelectiveColor]").append(newline_);
        if (!selectiveEnabledSeen_)
            result_.append("Enabled=").append(settings_.selectiveColor.enabled ? "1" : "0").append(newline_);
        if (!selectiveAmountSeen_)
            result_.append("Amount=").append(FormatNumber(settings_.selectiveColor.amountPercent)).append(newline_);
        for (std::size_t index = 0; index < kSelectivePersistedKeys.size(); ++index)
            if (!selectiveSeen_[index])
                result_.append(SelectiveKeyValue(kSelectivePersistedKeys[index], settings_)).append(newline_);
    }

    void AppendMissingGroup(std::string_view group)
    {
        bool missing{};
        for (std::size_t index = 0; index < kPersistedKeys.size(); ++index)
            missing = missing || (kPersistedKeys[index].section == group && !seen_[index]);
        if (!missing) return;
        result_.append("[").append(group).append("]").append(newline_);
        for (std::size_t index = 0; index < kPersistedKeys.size(); ++index)
            if (kPersistedKeys[index].section == group && !seen_[index])
                result_.append(KeyValue(kPersistedKeys[index], settings_, interfaceSettings_)).append(newline_);
    }

    void AppendMissingGroups()
    {
        constexpr std::array<std::string_view, 9> groups{"Output", "ToneMapper",
            "ACES2LMT", "AgX", "GT7", "Image", "SelectiveColor", "Effects", "Interface"};
        for (const auto group : groups)
        {
            if (group == "SelectiveColor") AppendMissingSelective();
            else AppendMissingGroup(group);
        }
    }

    const HdrSettings& settings_;
    const HdrInterfaceSettings& interfaceSettings_;
    bool positionOnly_{};
    std::array<bool, kPersistedKeys.size()> seen_{};
    std::array<bool, kSelectivePersistedKeys.size()> selectiveSeen_{};
    bool selectiveEnabledSeen_{};
    bool selectiveAmountSeen_{};
    std::string result_;
    std::string_view section_;
    std::string_view newline_;
};

[[nodiscard]] std::string RewriteIni(std::string_view original,
    const HdrSettings& settings, const HdrInterfaceSettings& interfaceSettings,
    bool positionOnly = false)
{
    return SettingsIniWriter(settings, interfaceSettings, positionOnly).Rewrite(original);
}

[[nodiscard]] std::filesystem::path ResolveBackingPath(
    const std::filesystem::path& path)
{
    const auto file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return path;
    std::wstring buffer(32768U, L'\0');
    const auto length = GetFinalPathNameByHandleW(file, buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (length == 0 || length >= buffer.size()) return path;
    buffer.resize(length);
    constexpr std::wstring_view extendedPrefix{L"\\\\?\\"};
    constexpr std::wstring_view uncPrefix{L"\\\\?\\UNC\\"};
    if (buffer.starts_with(uncPrefix))
        buffer = L"\\\\" + buffer.substr(uncPrefix.size());
    else if (buffer.starts_with(extendedPrefix))
        buffer.erase(0, extendedPrefix.size());
    return std::filesystem::path(std::move(buffer));
}

// Keep reading and replacement in separate calls: Windows cannot replace the
// INI while std::ifstream owns its non-delete-sharing handle.
[[nodiscard]] bool ReadPersistenceFile(
    const std::filesystem::path& path,
    std::string& contents,
    bool allowMissing = false) noexcept
{
    try
    {
        if (allowMissing)
        {
            std::error_code error;
            if (!std::filesystem::exists(path, error))
            {
                if (error) return false;
                contents.clear();
                return true;
            }
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        contents.assign(std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>{});
        return !input.bad();
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool ReplacePersistenceFile(
    const std::filesystem::path& path,
    std::string_view contents) noexcept
{
    try
    {
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream output(
                temporary, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(contents.data(),
                static_cast<std::streamsize>(contents.size()));
            output.flush();
            if (!output) return false;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            (void)DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool SavePersonalProfiles(const HdrSettings& settings)
{
    std::error_code error;
    std::filesystem::create_directories(
        g_personalIniPath.parent_path(), error);
    if (error) return false;
    std::string personalOriginal;
    if (!ReadPersistenceFile(
            g_personalIniPath, personalOriginal, true)) return false;
    const auto personalUpdated = RewriteGradeProfilesIni(
        personalOriginal, settings.lutGradeProfiles,
        g_distributedProfiles);
    if (!ParseGradeProfileConfiguration(personalUpdated).valid)
        return false;
    if (!ReplacePersistenceFile(
            g_personalIniPath, personalUpdated)) return false;
    return true;
}

[[nodiscard]] bool SaveSettings(
    const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings,
    bool positionOnly) noexcept
{
    try
    {
        if (!positionOnly && !SavePersonalProfiles(settings)) return false;

        std::string original;
        if (!ReadPersistenceFile(g_iniPath, original)) return false;
        if (positionOnly)
            return ReplacePersistenceFile(g_iniPath,
                RewriteHdrWindowPositionIni(original,
                    interfaceSettings.windowX, interfaceSettings.windowY));
        auto mainSettings = settings;
        mainSettings.lutGradeProfiles.count = 0;
        return ReplacePersistenceFile(g_iniPath,
            RewriteHdrSettingsIni(
                original, mainSettings, interfaceSettings));
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool SaveDisplayCalibration(
    const DisplayCalibrationProfile& profile) noexcept
{
    try
    {
        std::string original;
        if (!ReadPersistenceFile(g_iniPath, original)) return false;
        const auto updated = RewriteDisplayCalibrationIni(original, profile);
        return updated && ReplacePersistenceFile(g_iniPath, *updated);
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] DisplayCalibrationReadResult LoadDisplayCalibration(
    const DisplayCalibrationProfile& requested,
    DisplayCalibrationProfile& loaded) noexcept
{
    std::string original;
    if (!ReadPersistenceFile(g_iniPath, original))
        return DisplayCalibrationReadResult::failed;
    return ParseDisplayCalibrationIni(original, requested, loaded);
}

struct PersistenceGenerations
{
    std::uint64_t settings{};
    std::uint64_t position{};
    std::uint64_t calibration{};
    bool operator==(const PersistenceGenerations&) const = default;
};

struct PersistenceRequest
{
    HdrSettings settings;
    HdrInterfaceSettings interfaceSettings;
    DisplayCalibrationProfile calibration;
    DisplayCalibrationOperation calibrationOperation{};
    PersistenceGenerations generations;
};

struct PendingPersistence
{
    bool settings{};
    bool position{};
    bool calibration{};
    [[nodiscard]] bool Any() const noexcept { return settings || position || calibration; }
};

struct PersistenceCompletion
{
    bool saved{};
    DisplayCalibrationProfile calibration;
    DisplayCalibrationReadResult calibrationRead{DisplayCalibrationReadResult::failed};
    bool calibrationSaved{};
};

[[nodiscard]] PersistenceRequest ReadPersistenceRequest() noexcept
{
    LockPersistenceWorker();
    PersistenceRequest request;
    request.settings = g_requestedSettings;
    request.interfaceSettings = g_requestedInterfaceSettings;
    request.generations.settings = g_requestedGeneration.load(std::memory_order_relaxed);
    request.generations.position = g_requestedPositionGeneration;
    request.calibration = g_requestedDisplayCalibration;
    request.calibrationOperation = g_requestedDisplayCalibrationOperation;
    request.generations.calibration = g_requestedDisplayCalibrationGeneration.load(std::memory_order_relaxed);
    if (request.generations.position != 0)
    {
        request.interfaceSettings.windowX = g_requestedWindowX;
        request.interfaceSettings.windowY = g_requestedWindowY;
    }
    g_requestLock.clear(std::memory_order_release);
    return request;
}

[[nodiscard]] PersistenceCompletion ExecutePersistenceRequest(
    const PersistenceRequest& request, PendingPersistence pending) noexcept
{
    PersistenceCompletion completion;
    completion.saved = (pending.settings || pending.position) &&
        SaveSettings(request.settings, request.interfaceSettings, !pending.settings);
    completion.calibration = request.calibration;
    if (!pending.calibration) return completion;
    if (request.calibrationOperation == DisplayCalibrationOperation::save)
        completion.calibrationSaved = SaveDisplayCalibration(request.calibration);
    else if (request.calibrationOperation == DisplayCalibrationOperation::load)
        completion.calibrationRead = LoadDisplayCalibration(request.calibration, completion.calibration);
    return completion;
}

[[nodiscard]] SettingsPersistenceState CalibrationCompletionState(
    DisplayCalibrationOperation operation, const PersistenceCompletion& completion) noexcept
{
    if (operation == DisplayCalibrationOperation::save && completion.calibrationSaved)
        return SettingsPersistenceState::saved;
    if (operation != DisplayCalibrationOperation::load) return SettingsPersistenceState::failed;
    if (completion.calibrationRead == DisplayCalibrationReadResult::loaded)
        return SettingsPersistenceState::loaded;
    if (completion.calibrationRead == DisplayCalibrationReadResult::missing)
        return SettingsPersistenceState::missing;
    return SettingsPersistenceState::failed;
}

[[nodiscard]] std::uint64_t PublishSettingsCompletion(
    const PersistenceRequest& request, bool pending, bool saved) noexcept
{
    const auto generation = request.generations.settings;
    if (pending) g_completedGeneration.store(generation, std::memory_order_release);
    const auto latest = g_requestedGeneration.load(std::memory_order_acquire);
    if (!pending) return latest;
    auto state = SettingsPersistenceState::saving;
    if (generation == latest)
        state = saved ? SettingsPersistenceState::saved : SettingsPersistenceState::failed;
    g_state.store(state, std::memory_order_release);
    return latest;
}

[[nodiscard]] std::uint64_t PublishPositionCompletion(
    const PersistenceRequest& request, bool pending, bool saved) noexcept
{
    const auto latest = g_requestedPositionGeneration;
    if (pending && request.generations.position == latest)
        g_positionState.store(saved ? SettingsPersistenceState::saved :
            SettingsPersistenceState::failed, std::memory_order_release);
    return latest;
}

[[nodiscard]] std::uint64_t PublishCalibrationCompletion(
    const PersistenceRequest& request, bool pending, const PersistenceCompletion& completion) noexcept
{
    const auto latest = g_requestedDisplayCalibrationGeneration.load(std::memory_order_acquire);
    if (pending && request.generations.calibration == latest)
    {
        g_completedDisplayCalibration = completion.calibration;
        g_completedDisplayCalibrationGeneration = request.generations.calibration;
        g_displayCalibrationState.store(CalibrationCompletionState(request.calibrationOperation, completion),
            std::memory_order_release);
    }
    return latest;
}

[[nodiscard]] PersistenceGenerations PublishPersistenceCompletion(
    const PersistenceRequest& request, PendingPersistence pending,
    const PersistenceCompletion& completion) noexcept
{
    // Publication shares the request lock so an old completion cannot replace
    // a newer request's in-flight state.
    LockPersistenceWorker();
    PersistenceGenerations latest;
    latest.settings = PublishSettingsCompletion(request, pending.settings, completion.saved);
    latest.position = PublishPositionCompletion(request, pending.position, completion.saved);
    latest.calibration = PublishCalibrationCompletion(request, pending.calibration, completion);
    g_requestLock.clear(std::memory_order_release);
    return latest;
}

DWORD WINAPI PersistenceWorker(void*) noexcept
{
    PersistenceGenerations processed;
    for (;;)
    {
        (void)WaitForSingleObject(g_saveEvent, INFINITE);
        const auto request = ReadPersistenceRequest();
        const PendingPersistence pending{
            request.generations.settings != processed.settings,
            request.generations.position != processed.position,
            request.generations.calibration != processed.calibration};
        if (!pending.Any()) continue;
        processed = request.generations;
        const auto completion = ExecutePersistenceRequest(request, pending);
        const auto latest = PublishPersistenceCompletion(request, pending, completion);
        if (request.generations != latest) (void)SetEvent(g_saveEvent);
    }
}

[[nodiscard]] bool InitializePersistencePaths(
    const std::filesystem::path& iniPath, const LutGradeProfiles& distributedProfiles) noexcept
{
    try
    {
        const auto virtualMainPath = std::filesystem::absolute(iniPath);
        g_iniPath = ResolveBackingPath(virtualMainPath);
        const auto virtualPersonalPath = virtualMainPath.parent_path() /
            virtualMainPath.stem() / L"Personal.ini";
        std::error_code error;
        const auto personalExists =
            std::filesystem::exists(virtualPersonalPath, error);
        if (error) return false;
        g_personalIniPath = personalExists ?
            ResolveBackingPath(virtualPersonalPath) :
            g_iniPath.parent_path() / g_iniPath.stem() / L"Personal.ini";
        g_distributedProfiles = distributedProfiles;
    }
    catch (...) { return false; }
    return true;
}

[[nodiscard]] bool TryLockPersistenceRequest() noexcept
{
    bool locked{};
    for (unsigned attempt = 0; attempt < 64 && !locked; ++attempt)
    {
        locked = !g_requestLock.test_and_set(std::memory_order_acquire);
        if (!locked) YieldProcessor();
    }
    return locked;
}

[[nodiscard]] bool IsCalibrationTerminal(SettingsPersistenceState state) noexcept
{
    constexpr std::array terminal{SettingsPersistenceState::saved, SettingsPersistenceState::loaded,
        SettingsPersistenceState::missing, SettingsPersistenceState::failed};
    return std::ranges::find(terminal, state) != terminal.end();
}

[[nodiscard]] bool ValidSettingsForPersistence(const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept
{
    return ValidateHdrSettings(settings) == HdrSettingsValidationError::none &&
        ValidateHdrInterfaceSettings(interfaceSettings) == HdrInterfaceSettingsValidationError::none;
}
}  // namespace

std::string RewriteHdrWindowPositionIni(
    std::string_view original, double x, double y)
{
    HdrInterfaceSettings settings{};
    settings.windowX = x;
    settings.windowY = y;
    if (ValidateHdrInterfaceSettings(settings) !=
        HdrInterfaceSettingsValidationError::none) return std::string(original);
    return RewriteIni(original, HdrSettings{}, settings, true);
}

std::string RewriteHdrSettingsIni(
    std::string_view original,
    const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings)
{
    if (!ValidSettingsForPersistence(settings, interfaceSettings))
        return std::string(original);
    return RewriteGradeProfileSections(
        RewriteIni(original, settings, interfaceSettings),
        settings.lutGradeProfiles, {});
}

std::string RewriteGradeProfilesIni(
    std::string_view original,
    const LutGradeProfiles& profiles,
    const LutGradeProfiles& distributedProfiles)
{
    if (!ValidateLutGradeProfiles(profiles) ||
        !ValidateLutGradeProfiles(distributedProfiles))
        return std::string(original);
    return RewriteGradeProfileSections(
        original, profiles, distributedProfiles);
}

bool InitializeSettingsPersistence(
    const std::filesystem::path& iniPath,
    const LutGradeProfiles& distributedProfiles) noexcept
{
    if (g_started.load(std::memory_order_acquire))
        return g_iniPath == iniPath &&
            g_distributedProfiles == distributedProfiles;
    if (!ValidateLutGradeProfiles(distributedProfiles)) return false;
    if (!InitializePersistencePaths(iniPath, distributedProfiles)) return false;
    g_saveEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_saveEvent == nullptr) return false;
    const auto thread = CreateThread(
        nullptr, 0, PersistenceWorker, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        CloseHandle(g_saveEvent);
        g_saveEvent = nullptr;
        return false;
    }
    CloseHandle(thread);
    g_state.store(SettingsPersistenceState::idle, std::memory_order_release);
    g_positionState.store(SettingsPersistenceState::idle, std::memory_order_release);
    g_displayCalibrationState.store(
        SettingsPersistenceState::idle, std::memory_order_release);
    g_started.store(true, std::memory_order_release);
    return true;
}

std::uint64_t RequestHdrSettingsSave(
    const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept
{
    if (!g_started.load(std::memory_order_acquire) ||
        !ValidSettingsForPersistence(settings, interfaceSettings))
        return 0;
    if (!TryLockPersistenceRequest()) return 0;
    g_requestedSettings = settings;
    g_requestedInterfaceSettings = interfaceSettings;
    g_requestedWindowX = interfaceSettings.windowX;
    g_requestedWindowY = interfaceSettings.windowY;
    ++g_requestedPositionGeneration;
    const auto generation =
        g_requestedGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_state.store(SettingsPersistenceState::saving, std::memory_order_release);
    g_requestLock.clear(std::memory_order_release);
    if (!SetEvent(g_saveEvent))
    {
        g_state.store(SettingsPersistenceState::failed,
            std::memory_order_release);
        return 0;
    }
    return generation;
}

SettingsPersistenceSnapshot ReadSettingsPersistenceSnapshot() noexcept
{
    SettingsPersistenceSnapshot snapshot{
        .state = g_state.load(std::memory_order_acquire),
        .generation =
            g_completedGeneration.load(std::memory_order_acquire),
        .positionState = g_positionState.load(std::memory_order_acquire),
        .displayCalibrationState =
            g_displayCalibrationState.load(std::memory_order_acquire)
    };
    if (!IsCalibrationTerminal(snapshot.displayCalibrationState))
        return snapshot;
    if (g_requestLock.test_and_set(std::memory_order_acquire))
    {
        snapshot.displayCalibrationState = SettingsPersistenceState::loading;
        return snapshot;
    }
    snapshot.displayCalibrationState =
        g_displayCalibrationState.load(std::memory_order_acquire);
    if (IsCalibrationTerminal(snapshot.displayCalibrationState))
    {
        snapshot.displayCalibrationGeneration =
            g_completedDisplayCalibrationGeneration;
        snapshot.displayCalibration = g_completedDisplayCalibration;
    }
    g_requestLock.clear(std::memory_order_release);
    return snapshot;
}

bool RequestHdrWindowPositionSave(double x, double y) noexcept
{
    HdrInterfaceSettings settings{};
    settings.windowX = x;
    settings.windowY = y;
    if (!g_started.load(std::memory_order_acquire) ||
        ValidateHdrInterfaceSettings(settings) !=
            HdrInterfaceSettingsValidationError::none) return false;
    // Render-thread publication is one try; a busy worker is retried next frame.
    if (g_requestLock.test_and_set(std::memory_order_acquire)) return false;
    g_requestedWindowX = x;
    g_requestedWindowY = y;
    ++g_requestedPositionGeneration;
    g_positionState.store(SettingsPersistenceState::saving, std::memory_order_release);
    g_requestLock.clear(std::memory_order_release);
    if (SetEvent(g_saveEvent)) return true;
    g_positionState.store(SettingsPersistenceState::failed, std::memory_order_release);
    return false;
}

std::uint64_t RequestDisplayCalibrationSave(
    std::wstring_view monitorDevicePath,
    const HdrSettings& settings) noexcept
{
    DisplayCalibrationProfile profile{};
    if (!ValidDisplayIdentity(monitorDevicePath)) return 0;
    CopyDisplayIdentity(monitorDevicePath, profile.monitorDevicePath);
    profile.displayPeakNits = settings.displayPeakNits;
    profile.uiPaperWhiteNits = settings.uiPaperWhiteNits;
    profile.videoWhiteNits = settings.videoWhiteNits;
    if (!g_started.load(std::memory_order_acquire) ||
        !ValidDisplayCalibration(profile)) return 0;
    if (g_requestLock.test_and_set(std::memory_order_acquire)) return 0;
    g_requestedDisplayCalibration = profile;
    g_requestedDisplayCalibrationOperation = DisplayCalibrationOperation::save;
    const auto generation =
        g_requestedDisplayCalibrationGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    g_displayCalibrationState.store(
        SettingsPersistenceState::saving, std::memory_order_release);
    g_requestLock.clear(std::memory_order_release);
    if (SetEvent(g_saveEvent)) return generation;
    if (!g_requestLock.test_and_set(std::memory_order_acquire))
    {
        if (g_requestedDisplayCalibrationGeneration.load(
                std::memory_order_acquire) == generation)
            g_displayCalibrationState.store(
                SettingsPersistenceState::failed,
                std::memory_order_release);
        g_requestLock.clear(std::memory_order_release);
    }
    return 0;
}

std::uint64_t RequestDisplayCalibrationLoad(
    std::wstring_view monitorDevicePath) noexcept
{
    DisplayCalibrationProfile profile{};
    if (!g_started.load(std::memory_order_acquire) ||
        !ValidDisplayIdentity(monitorDevicePath)) return 0;
    if (g_requestLock.test_and_set(std::memory_order_acquire)) return 0;
    CopyDisplayIdentity(monitorDevicePath, profile.monitorDevicePath);
    g_requestedDisplayCalibration = profile;
    g_requestedDisplayCalibrationOperation = DisplayCalibrationOperation::load;
    const auto generation =
        g_requestedDisplayCalibrationGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    g_displayCalibrationState.store(
        SettingsPersistenceState::loading, std::memory_order_release);
    g_requestLock.clear(std::memory_order_release);
    if (SetEvent(g_saveEvent)) return generation;
    if (!g_requestLock.test_and_set(std::memory_order_acquire))
    {
        if (g_requestedDisplayCalibrationGeneration.load(
                std::memory_order_acquire) == generation)
            g_displayCalibrationState.store(
                SettingsPersistenceState::failed,
                std::memory_order_release);
        g_requestLock.clear(std::memory_order_release);
    }
    return 0;
}

}  // namespace fo4hdr
