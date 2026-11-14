// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fo4hdr {

struct PluginConfiguration : HdrSettings
{
    PluginConfiguration() noexcept
    {
        // Default construction must remain a neutral, non-production request.
        exposureEv = 0.0;
        weatherGradeResponse.enabled = false;
    }

    bool enabled{};
    // The parser requests SDR mode by default; default construction stays inert.
    bool sdrEnabled{};
    HdrSettings sdrSettings{DefaultSdrSettings()};
    bool productionRequested{};
    double sceneExposureMultiplier{1.0};
    HdrInterfaceSettings interfaceSettings{};
    bool developerMode{};
    bool traceRenderPipeline{};
    bool measurePerformance{};
    // Numerical/display reference validation is independent of production HDR.
    bool developerHdr10Reference{};
    bool forceHdrSwapChainFailure{};
    // Startup took FallbackPluginConfiguration because the INI was unreadable.
    bool configurationDefaults{};
    bool operator==(const PluginConfiguration&) const = default;
};

// Neutral SDR-only settings for an INI that exists but cannot be read, so the
// settings menu can still open and name the problem. HDR, diagnostics and
// saving stay off.
[[nodiscard]] PluginConfiguration FallbackPluginConfiguration() noexcept;

enum class PluginConfigurationError
{
    none,
    fileUnavailable
};

// How the load pass changed one INI entry. An added key was absent
// and takes its default; a defaulted key had text that does not parse.
enum class ConfigurationRepairKind : std::uint8_t
{
    added,
    renamed,
    defaulted,
    clamped,
    dropped
};

struct ConfigurationRepair
{
    ConfigurationRepairKind kind{};
    std::string section;
    std::string key;
    // The entry's text before the repair; empty for an added key.
    std::string text;
};

inline constexpr std::size_t kMaximumListedConfigurationRepairs{32};

struct ConfigurationRepairs
{
    // Indexed by ConfigurationRepairKind; complete even when entries is full.
    std::array<std::uint32_t, 5> counts{};
    std::vector<ConfigurationRepair> entries;
};

struct PluginConfigurationResult
{
    // The configuration in effect: diagnostics without DeveloperMode are off.
    PluginConfiguration value{};
    // Every key as resolved from the file, which the render writes back.
    PluginConfiguration stored{};
    PluginConfigurationError error{
        PluginConfigurationError::fileUnavailable};
    bool valid{};
    // The canonical INI text of stored.
    std::string text;
    ConfigurationRepairs repairs;
    bool fileMissing{};
    // The file is missing or its bytes differ from text.
    bool rewriteNeeded{};
};

// Resolves every key from INI text: unknown entries are dropped, former names
// renamed, invalid values defaulted and out-of-range numbers clamped.
[[nodiscard]] PluginConfigurationResult
ParsePluginConfiguration(std::string_view text) noexcept;
// A missing file reads as empty; an unreadable one returns an invalid result.
[[nodiscard]] PluginConfigurationResult
LoadPluginConfiguration(
    const std::filesystem::path& path) noexcept;
// The canonical INI text, comments included, for stored values; throws on
// allocation failure.
[[nodiscard]] std::string RenderPluginConfiguration(
    const PluginConfiguration& stored);
[[nodiscard]] std::string_view ToString(
    ProductionToneMapperAlgorithm algorithm) noexcept;
[[nodiscard]] std::string_view ToString(
    ColorGradingMethod method) noexcept;
[[nodiscard]] std::string_view ToString(
    ColorGradingCurve curve) noexcept;
[[nodiscard]] std::string_view ToString(
    PluginConfigurationError error) noexcept;
[[nodiscard]] std::string_view ToString(
    ConfigurationRepairKind kind) noexcept;

}  // namespace fo4hdr
