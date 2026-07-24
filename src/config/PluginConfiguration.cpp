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

namespace fo4hdr {
namespace {

constexpr std::array<std::string_view, 3> kAlgorithms{"ACES2", "AgX", "GT7"};

std::size_t OwnedKeyIndex(std::string_view section, std::string_view name) noexcept
{
    constexpr std::array<std::array<std::string_view, 2>, 11> keys{{
        {"Output", "DisplayPeakNits"}, {"Output", "UiPaperWhiteNits"},
        {"Output", "VideoWhiteNits"}, {"ToneMapper", "Algorithm"},
        {"AgX", "MiddleGray"}, {"AgX", "Contrast"}, {"AgX", "HdrPurity"},
        {"AgX", "ShoulderPower"}, {"GT7", "ToeStrength"},
        {"GT7", "ShoulderStart"}, {"GT7", "ColorBlend"}}};
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

class IniParser
{
public:
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
            if (!ParseLine(line)) return result_;
        }
        // The original four keys are required; absent new controls use defaults.
        if (std::find(seen_.begin(), seen_.begin() + 4, false) != seen_.begin() + 4)
            return result_;
        if (!ValidatePluginConfiguration(result_.value)) return result_;
        result_.status = PluginConfigurationStatus::loaded;
        result_.line = 0;
        return result_;
    }

private:
    bool SetValue(std::size_t index, std::string_view value) noexcept
    {
        if (seen_[index]) return false;
        seen_[index] = true;
        auto& settings = result_.value.display;
        auto& parameters = settings.toneMapperParameters;
        if (index == 3)
        {
            const auto found = std::ranges::find(kAlgorithms, value);
            if (found == kAlgorithms.end()) return false;
            result_.value.algorithm =
                static_cast<OperatorLutAlgorithm>(found - kAlgorithms.begin());
            return true;
        }
        const std::array targets{&settings.displayPeakNits, &settings.uiPaperWhiteNits,
            &settings.videoWhiteNits, static_cast<double*>(nullptr),
            &parameters.agxMiddleGrayEv, &parameters.agxContrast, &parameters.agxHdrPurity,
            &parameters.agxHdrShoulderPower, &parameters.gt7ToeStrength,
            &parameters.gt7ShoulderStart, &parameters.gt7ColorBlend};
        return ParseNumber(value, *targets[index]);
    }

    bool ParseAssignment(std::string_view line) noexcept
    {
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) return false;
        const auto key = Trim(line.substr(0, equals));
        const auto remainder = line.substr(equals + 1);
        const auto value = Trim(remainder.substr(0, remainder.find_first_of(";#")));
        if (key.empty()) return false;
        const auto index = OwnedKeyIndex(section_, key);
        if (index == seen_.size()) return true;
        return SetValue(index, value);
    }

    bool ParseSection(std::string_view line) noexcept
    {
        if (line.size() < 3) return false;
        if (line.back() != ']') return false;
        section_ = Trim(line.substr(1, line.size() - 2));
        return !section_.empty();
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
    std::array<bool, 11> seen_{};
};

}  // namespace

bool PluginConfiguration::operator==(const PluginConfiguration& other) const noexcept
{
    if (algorithm != other.algorithm) return false;
    const std::array current{
        display.displayPeakNits, display.uiPaperWhiteNits, display.videoWhiteNits};
    const std::array candidate{other.display.displayPeakNits,
        other.display.uiPaperWhiteNits, other.display.videoWhiteNits};
    return current == candidate &&
        display.toneMapperParameters == other.display.toneMapperParameters;
}

bool ValidatePluginConfiguration(const PluginConfiguration& value) noexcept
{
    if (ValidateHdrSettings(value.display) != HdrSettingsValidationError::none) return false;
    if (value.display.displayPeakNits < 400.0) return false;
    if (value.display.displayPeakNits > 2000.0) return false;
    if (!ValidateToneMapperParameters(OperatorLutAlgorithm::agX,
            value.display.toneMapperParameters)) return false;
    if (!ValidateToneMapperParameters(OperatorLutAlgorithm::gt7,
            value.display.toneMapperParameters)) return false;
    return !ToString(value.algorithm).empty();
}

PluginConfigurationResult ParsePluginConfiguration(std::string_view text) noexcept
{
    return IniParser{}.Parse(text);
}

PluginConfigurationResult LoadPluginConfiguration(const std::filesystem::path& path) noexcept
{
    try
    {
        std::error_code error;
        const auto exists = std::filesystem::exists(path, error);
        if (error) return {{}, PluginConfigurationStatus::ioError};
        if (!exists) return {{}, PluginConfigurationStatus::missing};
        std::ifstream input(path, std::ios::binary);
        if (!input) return {{}, PluginConfigurationStatus::ioError};
        const std::string text{
            std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if (input.bad()) return {{}, PluginConfigurationStatus::ioError};
        return ParsePluginConfiguration(text);
    }
    catch (...) { return {{}, PluginConfigurationStatus::ioError}; }
}

std::string_view ToString(OperatorLutAlgorithm algorithm) noexcept
{
    const auto index = static_cast<std::size_t>(algorithm);
    return index < kAlgorithms.size() ? kAlgorithms[index] : std::string_view{};
}

}  // namespace fo4hdr
