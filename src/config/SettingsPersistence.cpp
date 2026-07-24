// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/SettingsPersistence.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fo4hdr {
namespace {

constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF"};
constexpr std::array<std::string_view, 4> kKeys{
    "DisplayPeakNits", "UiPaperWhiteNits", "VideoWhiteNits", "Algorithm"};

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

std::array<std::string, 4> OwnedValues(const PluginConfiguration& value)
{
    return {FormatNumber(value.display.displayPeakNits),
        FormatNumber(value.display.uiPaperWhiteNits), FormatNumber(value.display.videoWhiteNits),
        std::string(ToString(value.algorithm))};
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
        AppendMissing("Output", 0, 3);
        AppendMissing("ToneMapper", 3, 4);
        return std::move(result_);
    }

private:
    std::size_t OwnedIndex(std::string_view key) const noexcept
    {
        const auto found = std::ranges::find(kKeys, key);
        if (found == kKeys.end()) return kKeys.size();
        const auto index = static_cast<std::size_t>(found - kKeys.begin());
        const auto expected = index == 3 ? "ToneMapper" : "Output";
        return section_ == expected ? index : kKeys.size();
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

    void AppendMissing(std::string_view section, std::size_t begin, std::size_t end)
    {
        const auto missing = std::ranges::find(seen_.begin() + begin, seen_.begin() + end, false);
        if (missing == seen_.begin() + end) return;
        if (!result_.empty() && result_.back() != '\n') result_.append(newline_);
        result_.append("[").append(section).append("]").append(newline_);
        for (auto index = begin; index < end; ++index)
            if (!seen_[index])
                result_.append(kKeys[index]).append("=").append(values_[index]).append(newline_);
    }

    std::array<std::string, 4> values_;
    std::array<bool, 4> seen_{};
    std::string result_;
    std::string_view section_;
    std::string_view newline_;
};

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

void SettingsPersistence::Run() noexcept
{
    for (;;)
    {
        std::unique_lock lock{mutex_};
        wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
        if (!pending_) return;
        const auto request = *pending_;
        pending_.reset();
        lock.unlock();
        Publish(request.generation, Save(path_, request.value));
    }
}

}  // namespace fo4hdr
