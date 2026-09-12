// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/SettingsPersistence.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fo4hdr {
namespace {

constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF"};
constexpr std::array<std::string_view, 46> kKeys{
    "DisplayPeakNits", "UiPaperWhiteNits", "VideoWhiteNits", "Algorithm",
    "MiddleGray", "Contrast", "HdrPurity", "ShoulderPower",
    "ToeStrength", "ShoulderStart", "ColorBlend", "VanillaColorGradingStrength",
    "Contrast", "Blacks", "Highlights", "Shadows", "Saturation", "Temperature", "Tint",
    "Exposure", "BloomStrength", "LookStrength", "ColorGradingMethod",
    "Enabled", "Amount", "RedHue",
    "RedSaturation", "RedBrightness", "OrangeHue",
    "OrangeSaturation", "OrangeBrightness", "YellowHue",
    "YellowSaturation", "YellowBrightness", "GreenHue",
    "GreenSaturation", "GreenBrightness", "CyanHue",
    "CyanSaturation", "CyanBrightness", "BlueHue",
    "BlueSaturation", "BlueBrightness", "MagentaHue",
    "MagentaSaturation", "MagentaBrightness"};
constexpr std::array<std::string_view, 46> kSections{
    "Output", "Output", "Output", "ToneMapper", "AgX", "AgX", "AgX", "AgX",
    "GT7", "GT7", "GT7", "Effects",
    "Image", "Image", "Image", "Image", "Image", "Image", "Image",
    "Image", "Effects", "ACES2LMT", "Image",
    "SelectiveColor", "SelectiveColor", "SelectiveColor",
    "SelectiveColor", "SelectiveColor", "SelectiveColor",
    "SelectiveColor", "SelectiveColor", "SelectiveColor",
    "SelectiveColor", "SelectiveColor", "SelectiveColor",
    "SelectiveColor", "SelectiveColor", "SelectiveColor",
    "SelectiveColor", "SelectiveColor", "SelectiveColor",
    "SelectiveColor", "SelectiveColor", "SelectiveColor",
    "SelectiveColor", "SelectiveColor"};

constexpr std::string_view Trim(std::string_view value) noexcept
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

std::string FormatNumber(double value)
{
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) throw std::runtime_error("Invalid settings number");
    return {buffer.data(), result.ptr};
}

std::array<std::string, 46> OwnedValues(const PluginConfiguration& value)
{
    const auto& parameters = value.display.toneMapperParameters;
    std::array<std::string, 46> values{FormatNumber(value.display.displayPeakNits),
        FormatNumber(value.display.uiPaperWhiteNits), FormatNumber(value.display.videoWhiteNits),
        std::string(ToString(value.algorithm)), FormatNumber(parameters.agxMiddleGrayEv),
        FormatNumber(parameters.agxContrast), FormatNumber(parameters.agxHdrPurity),
        FormatNumber(parameters.agxHdrShoulderPower), FormatNumber(parameters.gt7ToeStrength),
        FormatNumber(parameters.gt7ShoulderStart), FormatNumber(parameters.gt7ColorBlend),
        FormatNumber(value.display.vanillaColorGradingStrengthPercent),
        FormatNumber(value.display.contrast), FormatNumber(value.display.blacks),
        FormatNumber(value.display.highlights), FormatNumber(value.display.shadows),
        FormatNumber(value.display.saturationPercent), FormatNumber(value.display.temperature),
        FormatNumber(value.display.tint), FormatNumber(value.display.exposureEv),
        FormatNumber(value.display.bloomStrengthPercent), FormatNumber(parameters.acesLookStrength),
        std::string(ToString(value.display.colorGradingMethod))};
    const auto& selective = value.display.selectiveColor;
    values[23] = selective.enabled ? "1" : "0";
    values[24] = FormatNumber(selective.amountPercent);
    for (std::size_t index = 0; index < selective.regions.size(); ++index)
    {
        const auto& region = selective.regions[index];
        values[25 + index * 3] = FormatNumber(region.hueDegrees);
        values[26 + index * 3] = FormatNumber(region.saturationPercent);
        values[27 + index * 3] = FormatNumber(region.brightness);
    }
    return values;
}

std::string_view ValueSuffix(std::string_view raw, std::size_t equals) noexcept
{
    auto suffix = raw.find_first_of(";#", equals + 1);
    if (suffix == std::string_view::npos) suffix = raw.size();
    while (suffix > equals + 1)
    {
        const auto previous = raw[suffix - 1];
        if (previous != ' ' && previous != '\t') break;
        --suffix;
    }
    return raw.substr(suffix);
}

class IniWriter
{
public:
    explicit IniWriter(const PluginConfiguration& value) : values_{OwnedValues(value)} {}

    std::string Rewrite(std::string_view original)
    {
        if (original.starts_with(kUtf8Bom))
        {
            result_.append(kUtf8Bom);
            original.remove_prefix(kUtf8Bom.size());
        }
        newline_ = original.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
        while (!original.empty()) AppendLine(original);
        AppendMissing("Output");
        AppendMissing("ToneMapper");
        AppendMissing("AgX");
        AppendMissing("GT7");
        AppendMissing("ACES2LMT");
        AppendMissing("Effects");
        AppendMissing("Image");
        AppendMissing("SelectiveColor");
        return std::move(result_);
    }

private:
    std::size_t OwnedIndex(std::string_view key) const noexcept
    {
        for (std::size_t index = 0; index < kKeys.size(); ++index)
            if (section_ == kSections[index] && key == kKeys[index]) return index;
        return kKeys.size();
    }

    void AppendRaw(std::string_view raw)
    {
        const auto trimmed = Trim(raw);
        if (trimmed.empty()) { result_.append(raw); return; }
        if (trimmed.front() == ';' || trimmed.front() == '#') { result_.append(raw); return; }
        if (trimmed.front() == '[')
        {
            if (trimmed.back() == ']') section_ = Trim(trimmed.substr(1, trimmed.size() - 2));
            result_.append(raw);
            return;
        }
        const auto equals = raw.find('=');
        const auto index = OwnedIndex(Trim(raw.substr(0, equals)));
        if (equals == std::string_view::npos) { result_.append(raw); return; }
        if (index == kKeys.size()) { result_.append(raw); return; }
        seen_[index] = true;
        result_.append(raw.substr(0, equals + 1)).append(values_[index]);
        result_.append(ValueSuffix(raw, equals));
    }

    void AppendLine(std::string_view& original)
    {
        const auto newline = original.find('\n');
        auto raw = original.substr(0, newline);
        const auto crlf = newline != std::string_view::npos && raw.ends_with('\r');
        if (crlf) raw.remove_suffix(1);
        AppendRaw(raw);
        if (newline == std::string_view::npos) { original = {}; return; }
        result_.append(crlf ? "\r\n" : "\n");
        original.remove_prefix(newline + 1);
    }

    bool IsMissing(std::string_view section, std::size_t index) const noexcept
    {
        return kSections[index] == section && !seen_[index];
    }

    void AppendMissing(std::string_view section)
    {
        bool missing{};
        for (std::size_t index = 0; index < kKeys.size(); ++index)
            missing |= IsMissing(section, index);
        if (!missing) return;
        if (!result_.empty() && result_.back() != '\n') result_.append(newline_);
        result_.append("[").append(section).append("]").append(newline_);
        for (std::size_t index = 0; index < kKeys.size(); ++index)
            if (IsMissing(section, index))
                result_.append(kKeys[index]).append("=").append(values_[index]).append(newline_);
    }

    std::array<std::string, 46> values_;
    std::array<bool, 46> seen_{};
    std::string result_;
    std::string_view section_;
    std::string_view newline_;
};

struct IniLine
{
    std::string_view full;
    std::string_view trimmed;

    [[nodiscard]] bool IsSection() const noexcept
    {
        return trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
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
    const auto full = remaining.substr(0, end == std::string_view::npos ? end : end + 1);
    auto raw = remaining.substr(0, end);
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    const bool terminated = end != std::string_view::npos;
    remaining = terminated ? remaining.substr(end + 1) : std::string_view{};
    auto trimmed = Trim(raw);
    if (trimmed.starts_with(kUtf8Bom)) trimmed.remove_prefix(kUtf8Bom.size());
    return {full, trimmed};
}

[[nodiscard]] std::string_view IniNewline(std::string_view original) noexcept
{
    return original.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
}

void FinishIniLine(std::string& result, std::string_view newline)
{
    if (!result.empty() && !result.ends_with(newline)) result.append(newline);
}

constexpr std::array<std::string_view, 3> kDisplayCalibrationKeys{
    "DisplayPeakNits", "UiPaperWhiteNits", "VideoWhiteNits"};

bool ValidDisplayIdentity(const DisplayCalibrationProfile& profile) noexcept
{
    const auto& identity = profile.monitorDevicePath;
    if (identity[0] == L'\0') return false;
    if (identity.back() != L'\0') return false;
    for (const auto character : identity)
    {
        if (character == L'\0') return true;
        if (character < 32) return false;
    }
    return false;
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

[[nodiscard]] SettingsPersistenceState CalibrationReadResult(
    std::size_t sectionCount, const std::array<bool, kDisplayCalibrationKeys.size()>& seen,
    const DisplayCalibrationProfile& loaded) noexcept
{
    if (sectionCount == 0) return SettingsPersistenceState::missing;
    if (sectionCount != 1) return SettingsPersistenceState::failed;
    if (std::ranges::find(seen, false) != seen.end()) return SettingsPersistenceState::failed;
    return ValidDisplayCalibration(loaded) ? SettingsPersistenceState::loaded :
        SettingsPersistenceState::failed;
}

[[nodiscard]] SettingsPersistenceState ParseDisplayCalibrationIni(
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
                return SettingsPersistenceState::failed;
        }
        return CalibrationReadResult(sectionCount, seen, loaded);
    }
    catch (...) { return SettingsPersistenceState::failed; }
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
        FinishIniLine(result_, newline_);
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
        if (!ReplaceKey(line)) result_.append(line.full);
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

bool ReadLatest(const std::filesystem::path& path, std::string& contents)
{
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) return false;
    if (!exists) return true;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    return !input.bad();
}

bool WriteAndClose(HANDLE file, std::string_view contents) noexcept
{
    DWORD written{};
    const auto wrote = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
        &written, nullptr);
    const auto closed = CloseHandle(file);
    if (!wrote) return false;
    if (!closed) return false;
    return written == contents.size();
}

bool ReplaceFile(const std::filesystem::path& path, std::string_view contents)
{
    if (contents.size() > MAXDWORD) return false;
    auto temporary = path;
    temporary += L".tmp";
    // Exclusive creation cannot truncate another writer's temporary file.
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    if (!WriteAndClose(file, contents))
    {
        (void)DeleteFileW(temporary.c_str());
        return false;
    }
    const auto replaced = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!replaced) (void)DeleteFileW(temporary.c_str());
    return replaced;
}

bool Save(const std::filesystem::path& path, const PluginConfiguration& value) noexcept
{
    try
    {
        std::string original;
        if (!ReadLatest(path, original)) return false;
        const auto updated = RewritePluginConfigurationIni(original, value);
        if (ParsePluginConfiguration(updated).status != PluginConfigurationStatus::loaded)
            return false;
        return ReplaceFile(path, updated);
    }
    catch (...) { return false; }
}

DisplayCalibrationSnapshot LoadCalibration(const std::filesystem::path& path,
    const DisplayCalibrationProfile& identity, std::uint64_t generation) noexcept
{
    DisplayCalibrationSnapshot result;
    result.requestedGeneration = result.completedGeneration = generation;
    result.profile = identity;
    result.state = SettingsPersistenceState::failed;
    try
    {
        std::string original;
        if (ReadLatest(path, original))
            result.state = ParseDisplayCalibrationIni(original, identity, result.profile);
    }
    catch (...) {}
    return result;
}

bool SaveCalibration(const std::filesystem::path& path,
    const DisplayCalibrationProfile& profile) noexcept
{
    try
    {
        std::string original;
        if (!ReadLatest(path, original)) return false;
        const auto updated = RewriteDisplayCalibrationIni(original, profile);
        if (!updated) return false;
        return ReplaceFile(path, *updated);
    }
    catch (...) { return false; }
}

}  // namespace

std::string RewritePluginConfigurationIni(
    std::string_view original, const PluginConfiguration& value)
{
    if (!ValidatePluginConfiguration(value)) return std::string(original);
    return IniWriter{value}.Rewrite(original);
}

SettingsPersistence::~SettingsPersistence() noexcept
{
    {
        const std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

bool SettingsPersistence::Initialize(const std::filesystem::path& path) noexcept
{
    if (path.empty()) return false;
    try
    {
        const auto absolute = std::filesystem::absolute(path);
        if (thread_.joinable()) return path_ == absolute;
        path_ = absolute;
        thread_ = std::thread{&SettingsPersistence::Run, this};
        snapshot_.state = SettingsPersistenceState::idle;
        profileSnapshot_.state = SettingsPersistenceState::idle;
        return true;
    }
    catch (...) { return false; }
}

bool SettingsPersistence::RequestSave(
    const PluginConfiguration& value, std::uint64_t generation) noexcept
{
    if (!ValidatePluginConfiguration(value)) return false;
    if (generation == 0) return false;
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) return false;
    if (!thread_.joinable() || stopping_) return false;
    if (generation <= snapshot_.requestedGeneration) return false;
    pending_ = SaveRequest{value, generation};
    snapshot_.requestedGeneration = generation;
    snapshot_.state = SettingsPersistenceState::saving;
    wake_.notify_one();
    return true;
}

SettingsPersistenceSnapshot SettingsPersistence::Snapshot() const noexcept
{
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) return {SettingsPersistenceState::busy};
    return snapshot_;
}

void SettingsPersistence::Publish(std::uint64_t generation, bool saved) noexcept
{
    const std::lock_guard lock{mutex_};
    if (generation != snapshot_.requestedGeneration) return;
    snapshot_.completedGeneration = generation;
    snapshot_.state = saved ? SettingsPersistenceState::saved : SettingsPersistenceState::failed;
}

std::uint64_t SettingsPersistence::QueueProfile(
    const DisplayCalibrationProfile& profile, bool load) noexcept
{
    if (!ValidDisplayIdentity(profile)) return 0;
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) return 0;
    if (!thread_.joinable() || stopping_) return 0;
    if (profileSnapshot_.requestedGeneration == std::numeric_limits<std::uint64_t>::max()) return 0;
    const auto generation = ++profileSnapshot_.requestedGeneration;
    pendingProfile_ = ProfileRequest{profile, generation, load};
    profileSnapshot_.state = load ? SettingsPersistenceState::loading : SettingsPersistenceState::saving;
    wake_.notify_one();
    return generation;
}

std::uint64_t SettingsPersistence::RequestDisplayCalibrationSave(
    const DisplayCalibrationProfile& profile) noexcept
{
    if (!ValidDisplayCalibration(profile)) return 0;
    return QueueProfile(profile, false);
}

std::uint64_t SettingsPersistence::RequestDisplayCalibrationLoad(
    const DisplayCalibrationProfile& identity) noexcept
{
    return QueueProfile(identity, true);
}

DisplayCalibrationSnapshot SettingsPersistence::DisplayCalibrationStatus() const noexcept
{
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) return {SettingsPersistenceState::busy};
    return profileSnapshot_;
}

void SettingsPersistence::PublishProfile(const DisplayCalibrationSnapshot& result) noexcept
{
    const std::lock_guard lock{mutex_};
    if (result.completedGeneration != profileSnapshot_.requestedGeneration) return;
    profileSnapshot_ = result;
}

void SettingsPersistence::RunProfile(const ProfileRequest& request) noexcept
{
    if (request.load)
    {
        PublishProfile(LoadCalibration(path_, request.profile, request.generation));
        return;
    }
    const bool saved = SaveCalibration(path_, request.profile);
    PublishProfile({saved ? SettingsPersistenceState::saved : SettingsPersistenceState::failed,
        request.generation, request.generation, request.profile});
}

void SettingsPersistence::Run() noexcept
{
    for (;;)
    {
        std::unique_lock lock{mutex_};
        wake_.wait(lock, [this] { return stopping_ || pending_.has_value() || pendingProfile_.has_value(); });
        if (pendingProfile_)
        {
            const auto request = *pendingProfile_;
            pendingProfile_.reset();
            lock.unlock();
            RunProfile(request);
            continue;
        }
        if (!pending_) return;
        const auto request = *pending_;
        pending_.reset();
        lock.unlock();
        Publish(request.generation, Save(path_, request.value));
    }
}

}  // namespace fo4hdr
