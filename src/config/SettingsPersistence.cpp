// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "SettingsPersistence.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <new>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace fo4hdr {
namespace {

static_assert(sizeof(wchar_t) == 2);

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
    WeatherGradeChannel WeatherGradeResponseSettings::*weatherChannel{};
    double WeatherGradeChannel::*weatherNumber{};
    std::size_t weatherCoefficient{4};
    bool weatherEnabled{};
    // Last, so earlier positional rows are unaffected.
    bool colorGradingCurve{};
    bool HdrSettings::*flag{};
    bool HdrInterfaceSettings::*interfaceFlag{};
    // A released former name, rewritten in place under the current one.
    std::string_view formerKey{};
};

constexpr std::array<PersistedKey, 53> kPersistedKeys{{
    {.section = "General", .key = "HdrEnabled", .interfaceFlag = &HdrInterfaceSettings::hdrEnabled},
    {"WeatherLutResponse", "Enabled", nullptr, nullptr, false, false, false, nullptr, nullptr, nullptr, 4, true},
    {"WeatherLutResponse", "LuminanceBase", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, &WeatherGradeChannel::base, 4},
    {"WeatherLutResponse", "LuminanceGain", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, &WeatherGradeChannel::gain, 4},
    {"WeatherLutResponse", "LuminanceMinimum", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, &WeatherGradeChannel::minimum, 4},
    {"WeatherLutResponse", "LuminanceMaximum", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, &WeatherGradeChannel::maximum, 4},
    {"WeatherLutResponse", "LuminanceIntercept", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, &WeatherGradeChannel::intercept, 4},
    {"WeatherLutResponse", "LuminanceSunlight", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, nullptr, 0},
    {"WeatherLutResponse", "LuminanceGlare", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, nullptr, 1},
    {"WeatherLutResponse", "LuminanceAmbient", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, nullptr, 2},
    {"WeatherLutResponse", "LuminanceColorChange", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::luminance, nullptr, 3},
    {"WeatherLutResponse", "ColorBase", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, &WeatherGradeChannel::base, 4},
    {"WeatherLutResponse", "ColorGain", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, &WeatherGradeChannel::gain, 4},
    {"WeatherLutResponse", "ColorMinimum", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, &WeatherGradeChannel::minimum, 4},
    {"WeatherLutResponse", "ColorMaximum", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, &WeatherGradeChannel::maximum, 4},
    {"WeatherLutResponse", "ColorIntercept", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, &WeatherGradeChannel::intercept, 4},
    {"WeatherLutResponse", "ColorSunlight", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, nullptr, 0},
    {"WeatherLutResponse", "ColorGlare", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, nullptr, 1},
    {"WeatherLutResponse", "ColorAmbient", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, nullptr, 2},
    {"WeatherLutResponse", "ColorColorChange", nullptr, nullptr, false, false, false, nullptr,
        &WeatherGradeResponseSettings::color, nullptr, 3},
    {"Output", "DisplayPeakNits", &HdrSettings::displayPeakNits},
    {"Output", "UiPaperWhiteNits", &HdrSettings::uiPaperWhiteNits},
    {"Output", "VideoWhiteNits", &HdrSettings::videoWhiteNits},
    {"ToneMapper", "Algorithm", nullptr, nullptr, true},
    {.section = "ToneMapper", .key = "AutomaticWeatherResponse", .flag = &HdrSettings::sdrOperatorWeatherResponse},
    {"ACES2", "ContrastLookStrength", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::acesLookStrength},
    {"AgX", "MiddleGrayOffsetEV", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::agxMiddleGrayEv},
    {"AgX", "Contrast", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::agxContrast},
    {.section = "AgX", .key = "ColorResponse",
        .toneMapperNumber = &ToneMapperParameters::agxHdrPurity, .formerKey = "HdrColorResponse"},
    {"AgX", "ShoulderPower", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::agxHdrShoulderPower},
    {"GT7", "ToeStrength", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::gt7ToeStrength},
    {"GT7", "ShoulderStart", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::gt7ShoulderStart},
    {"GT7", "ColorBlend", nullptr, nullptr, false, false, false,
        &ToneMapperParameters::gt7ColorBlend},
    {"Image", "ExposureEV", &HdrSettings::exposureEv},
    {"Image", "Contrast", &HdrSettings::contrast},
    {"Image", "Blacks", &HdrSettings::blacks},
    {"Image", "Highlights", &HdrSettings::highlights},
    {"Image", "Shadows", &HdrSettings::shadows},
    {"Image", "Saturation", &HdrSettings::saturationPercent},
    {"Image", "Temperature", &HdrSettings::temperature},
    {"Image", "Tint", &HdrSettings::tint},
    {"Image", "ColorGradingMethod", nullptr, nullptr, false, false, true},
    {.section = "Image", .key = "ColorGradingCurve", .colorGradingCurve = true},
    {"Effects", "VanillaColorGradingStrength",
        &HdrSettings::vanillaColorGradingStrengthPercent},
    {"Effects", "VanillaGradeLuminanceStrength",
        &HdrSettings::vanillaGradeLuminancePercent},
    {"Effects", "VanillaGradeColorStrength",
        &HdrSettings::vanillaGradeColorPercent},
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
HdrSettingsProfiles g_requestedSettings{};
HdrInterfaceSettings g_requestedInterfaceSettings{};
double g_requestedWindowX{-1.0};
double g_requestedWindowY{-1.0};
std::uint64_t g_requestedPositionGeneration{}; // under g_requestLock
std::atomic<SettingsPersistenceState> g_positionState{
    SettingsPersistenceState::unavailable};
std::atomic_flag g_requestLock = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> g_requestedGeneration{};
std::atomic<std::uint64_t> g_completedGeneration{};
std::atomic<SettingsPersistenceState> g_state{
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

[[nodiscard]] std::string WeatherKeyValue(
    const PersistedKey& key, const WeatherGradeResponseSettings& settings)
{
    std::string result(key.key);
    result.push_back('=');
    if (key.weatherEnabled)
    {
        result.push_back(settings.enabled ? '1' : '0');
        return result;
    }
    const auto& channel = settings.*key.weatherChannel;
    const auto value = key.weatherNumber != nullptr ? channel.*key.weatherNumber :
        channel.coefficients[key.weatherCoefficient];
    std::array<char, 64> buffer{};
    const auto formatted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    result.append(buffer.data(), formatted.ptr);
    return result;
}

// The value of a switch key; other keys have none.
[[nodiscard]] std::optional<std::string> FlagKeyValue(
    const PersistedKey& key,
    const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings)
{
    if (key.flag != nullptr) return settings.*key.flag ? "1" : "0";
    if (key.interfaceFlag != nullptr) return interfaceSettings.*key.interfaceFlag ? "1" : "0";
    return std::nullopt;
}

// The value of a key that names a choice or a switch; numbers have none.
[[nodiscard]] std::optional<std::string> NamedKeyValue(
    const PersistedKey& key,
    const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings)
{
    if (key.algorithm) return std::string(ToString(settings.productionAlgorithm));
    if (key.hotkey) return FormatHdrSettingsHotkey(interfaceSettings.toggleKey).data();
    if (key.colorGradingMethod) return std::string(ToString(settings.colorGradingMethod));
    if (key.colorGradingCurve) return std::string(ToString(settings.colorGradingCurve));
    return FlagKeyValue(key, settings, interfaceSettings);
}

[[nodiscard]] std::string KeyValue(
    const PersistedKey& key,
    const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings)
{
    if (key.section == "WeatherLutResponse")
        return WeatherKeyValue(key, settings.weatherGradeResponse);
    std::string result(key.key);
    result.push_back('=');
    if (const auto named = NamedKeyValue(key, settings, interfaceSettings))
        result.append(*named);
    else if (key.interfaceNumber != nullptr)
        result.append(FormatNumber(interfaceSettings.*key.interfaceNumber));
    else if (key.toneMapperNumber != nullptr)
        result.append(FormatNumber(
            settings.toneMapperParameters.*key.toneMapperNumber));
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

class SettingsIniWriter
{
public:
    SettingsIniWriter(const HdrSettings& settings,
        const HdrInterfaceSettings& interfaceSettings, bool positionOnly,
        bool sdrProfile = false) :
        settings_(settings), interfaceSettings_(interfaceSettings), positionOnly_(positionOnly),
        sdrProfile_(sdrProfile)
    {
        if (sdrProfile_)
        {
            // The SDR pass rewrites only its own sections; HDR rows are already written.
            for (std::size_t index = 0; index < kPersistedKeys.size(); ++index)
                seen_[index] = !IsSdrProfileKey(kPersistedKeys[index].section, kPersistedKeys[index].key);
            return;
        }
        // The HDR pass leaves SDR-only rows to the SDR pass.
        for (std::size_t index = 0; index < kPersistedKeys.size(); ++index)
            seen_[index] = IsSdrOnlyProfileKey(kPersistedKeys[index].section, kPersistedKeys[index].key) ||
                (positionOnly_ && !IsPositionKey(kPersistedKeys[index]));
        if (!positionOnly_) return;
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
        if (positionOnly_ || sdrProfile_) return false;
        return (section_ == "Image" && name == "GradeTransfer") ||
            (section_ == "Effects" && name == "GradeProfilesEnabled") ||
            (section_ == "Debug" && name == "ShowDeveloperOverlay");
    }

    [[nodiscard]] static bool NamesKey(const PersistedKey& key, std::string_view name) noexcept
    {
        return name == key.key || (!key.formerKey.empty() && name == key.formerKey);
    }

    [[nodiscard]] bool Writes(const PersistedKey& key) const noexcept
    {
        if (positionOnly_ && !IsPositionKey(key)) return false;
        return sdrProfile_ ? IsSdrProfileKey(key.section, key.key) :
            !IsSdrOnlyProfileKey(key.section, key.key);
    }

    [[nodiscard]] bool ReplaceKnownKey(std::string_view name)
    {
        for (std::size_t index = 0; index < kPersistedKeys.size(); ++index)
        {
            const auto& key = kPersistedKeys[index];
            if (!Writes(key)) continue;
            if (section_ != key.section || !NamesKey(key, name)) continue;
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
        if (line.IsSection())
            section_ = sdrProfile_ ? SdrProfileBaseSection(line.Section()) : line.Section();
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
        result_.append("[").append(HeaderName("SelectiveColor")).append("]").append(newline_);
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
        result_.append("[").append(HeaderName(group)).append("]").append(newline_);
        for (std::size_t index = 0; index < kPersistedKeys.size(); ++index)
            if (kPersistedKeys[index].section == group && !seen_[index])
                result_.append(KeyValue(kPersistedKeys[index], settings_, interfaceSettings_)).append(newline_);
    }

    void AppendMissingGroups()
    {
        constexpr std::array<std::string_view, 11> groups{"General", "Output", "ToneMapper",
            "ACES2", "AgX", "GT7", "Image", "SelectiveColor", "Effects", "Interface",
            "WeatherLutResponse"};
        for (const auto group : groups)
        {
            if (sdrProfile_ && SdrProfileSectionName(group).empty()) continue;
            if (group == "SelectiveColor") AppendMissingSelective();
            else AppendMissingGroup(group);
        }
    }

    [[nodiscard]] std::string_view HeaderName(std::string_view group) const noexcept
    {
        return sdrProfile_ ? SdrProfileSectionName(group) : group;
    }

    const HdrSettings& settings_;
    const HdrInterfaceSettings& interfaceSettings_;
    bool positionOnly_{};
    bool sdrProfile_{};
    std::array<bool, kPersistedKeys.size()> seen_{};
    std::array<bool, kSelectivePersistedKeys.size()> selectiveSeen_{};
    bool selectiveEnabledSeen_{};
    bool selectiveAmountSeen_{};
    std::string result_;
    std::string_view section_;
    std::string_view newline_;
};

[[nodiscard]] std::string RewriteIni(std::string_view original,
    const HdrSettingsProfiles& settings, const HdrInterfaceSettings& interfaceSettings,
    bool positionOnly = false)
{
    auto rewritten = SettingsIniWriter(settings.hdr, interfaceSettings, positionOnly).Rewrite(original);
    if (positionOnly) return rewritten;
    return SettingsIniWriter(settings.sdr, interfaceSettings, false, true).Rewrite(rewritten);
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

[[nodiscard]] bool SaveSettings(
    const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings,
    bool positionOnly) noexcept
{
    try
    {
        std::string original;
        if (!ReadPersistenceFile(g_iniPath, original)) return false;
        if (positionOnly)
            return ReplacePersistenceFile(g_iniPath,
                RewriteHdrWindowPositionIni(original,
                    interfaceSettings.windowX, interfaceSettings.windowY));
        return ReplacePersistenceFile(g_iniPath,
            RewriteHdrSettingsIni(
                original, settings, interfaceSettings));
    }
    catch (...)
    {
        return false;
    }
}

struct PersistenceGenerations
{
    std::uint64_t settings{};
    std::uint64_t position{};
    bool operator==(const PersistenceGenerations&) const = default;
};

struct PersistenceRequest
{
    HdrSettingsProfiles settings;
    HdrInterfaceSettings interfaceSettings;
    PersistenceGenerations generations;
};

struct PendingPersistence
{
    bool settings{};
    bool position{};
    [[nodiscard]] bool Any() const noexcept { return settings || position; }
};

[[nodiscard]] PersistenceRequest ReadPersistenceRequest() noexcept
{
    LockPersistenceWorker();
    PersistenceRequest request;
    request.settings = g_requestedSettings;
    request.interfaceSettings = g_requestedInterfaceSettings;
    request.generations.settings = g_requestedGeneration.load(std::memory_order_relaxed);
    request.generations.position = g_requestedPositionGeneration;
    if (request.generations.position != 0)
    {
        request.interfaceSettings.windowX = g_requestedWindowX;
        request.interfaceSettings.windowY = g_requestedWindowY;
    }
    g_requestLock.clear(std::memory_order_release);
    return request;
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

[[nodiscard]] PersistenceGenerations PublishPersistenceCompletion(
    const PersistenceRequest& request, PendingPersistence pending, bool saved) noexcept
{
    // Publication shares the request lock so an old completion cannot replace
    // a newer request's in-flight state.
    LockPersistenceWorker();
    PersistenceGenerations latest;
    latest.settings = PublishSettingsCompletion(request, pending.settings, saved);
    latest.position = PublishPositionCompletion(request, pending.position, saved);
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
            request.generations.position != processed.position};
        if (!pending.Any()) continue;
        processed = request.generations;
        const bool saved = SaveSettings(request.settings, request.interfaceSettings, !pending.settings);
        const auto latest = PublishPersistenceCompletion(request, pending, saved);
        if (request.generations != latest) (void)SetEvent(g_saveEvent);
    }
}

[[nodiscard]] bool InitializePersistencePaths(
    const std::filesystem::path& iniPath) noexcept
{
    try
    {
        const auto virtualMainPath = std::filesystem::absolute(iniPath);
        g_iniPath = ResolveBackingPath(virtualMainPath);
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

[[nodiscard]] bool ValidSettingsForPersistence(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept
{
    return ValidateHdrSettings(settings.hdr) == HdrSettingsValidationError::none &&
        ValidateProfileSettings(settings.sdr, true) == HdrSettingsValidationError::none &&
        ValidateHdrInterfaceSettings(interfaceSettings) == HdrInterfaceSettingsValidationError::none;
}

bool SameInterfaceControls(const HdrInterfaceSettings& left,
    const HdrInterfaceSettings& right) noexcept
{
    return left.scale == right.scale && left.windowWidth == right.windowWidth &&
        left.windowHeight == right.windowHeight && left.toggleKey == right.toggleKey &&
        left.hdrEnabled == right.hdrEnabled;
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
    return RewriteIni(original, HdrSettingsProfiles{}, settings, true);
}

std::string RewriteHdrSettingsIni(
    std::string_view original,
    const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings)
{
    if (!ValidSettingsForPersistence(settings, interfaceSettings))
        return std::string(original);
    return RewriteIni(original, settings, interfaceSettings);
}

bool InitializeSettingsPersistence(
    const std::filesystem::path& iniPath) noexcept
{
    if (g_started.load(std::memory_order_acquire))
        return g_iniPath == iniPath;
    if (!InitializePersistencePaths(iniPath)) return false;
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
    g_started.store(true, std::memory_order_release);
    return true;
}

std::uint64_t RequestHdrSettingsSave(
    const HdrSettingsProfiles& settings,
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
    return {
        .state = g_state.load(std::memory_order_acquire),
        .generation =
            g_completedGeneration.load(std::memory_order_acquire),
        .positionState = g_positionState.load(std::memory_order_acquire)};
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

void SettingsAutosave::Initialize(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept
{
    persistedSettings_ = observedSettings_ = settings;
    persistedInterface_ = observedInterface_ = interfaceSettings;
    changedAt_ = retryAt_ = requestGeneration_ = 0;
    requestFailed_ = false;
}

bool SettingsAutosave::Dirty(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings) const noexcept
{
    return settings != persistedSettings_ ||
        !SameInterfaceControls(interfaceSettings, persistedInterface_);
}

void SettingsAutosave::Acknowledge(const SettingsPersistenceSnapshot& persistence) noexcept
{
    if (requestGeneration_ == 0 || persistence.generation != requestGeneration_) return;
    if (persistence.state != SettingsPersistenceState::saved) return;
    persistedSettings_ = requestedSettings_;
    persistedInterface_ = requestedInterface_;
    requestGeneration_ = retryAt_ = 0;
    requestFailed_ = false;
}

bool SettingsAutosave::Ready(const SettingsPersistenceSnapshot& persistence,
    const SettingsAutosaveFrame& frame) const noexcept
{
    if (!frame.rendererReady || frame.now < retryAt_) return false;
    if (frame.windowOpen && frame.now - changedAt_ < 500U) return false;
    return persistence.state != SettingsPersistenceState::saving &&
        persistence.state != SettingsPersistenceState::unavailable;
}

void SettingsAutosave::Update(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings, const SettingsAutosaveFrame& frame) noexcept
{
    const auto persistence = ReadSettingsPersistenceSnapshot();
    Acknowledge(persistence);
    if (settings != observedSettings_ || !SameInterfaceControls(interfaceSettings, observedInterface_))
    {
        observedSettings_ = settings;
        observedInterface_ = interfaceSettings;
        changedAt_ = frame.now;
    }
    if (!Dirty(settings, interfaceSettings) || !Ready(persistence, frame)) return;

    const auto generation = RequestHdrSettingsSave(settings, interfaceSettings);
    retryAt_ = frame.now + 2000U;
    requestFailed_ = generation == 0;
    if (generation == 0) return;
    requestedSettings_ = settings;
    requestedInterface_ = interfaceSettings;
    requestGeneration_ = generation;
}

}  // namespace fo4hdr
