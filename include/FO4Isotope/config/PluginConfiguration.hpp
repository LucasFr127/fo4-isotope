// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

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
    bool showDeveloperOverlay{};
    bool forceHdrSwapChainFailure{};
    bool operator==(const PluginConfiguration&) const = default;
};

// SDR profile sections reuse their HDR section's key names.
struct SdrProfileSection { std::string_view sdr; std::string_view base; };
inline constexpr std::array kSdrProfileSections{
    SdrProfileSection{"SdrImage", "Image"},
    SdrProfileSection{"SdrSelectiveColor", "SelectiveColor"},
    SdrProfileSection{"SdrEffects", "Effects"},
    SdrProfileSection{"SdrWeatherLutResponse", "WeatherLutResponse"}};

// The HDR base section of an SDR profile section, or empty for any other section.
[[nodiscard]] constexpr std::string_view SdrProfileBaseSection(std::string_view section) noexcept
{
    for (const auto& entry : kSdrProfileSections)
        if (entry.sdr == section) return entry.base;
    return {};
}

// The SDR profile section for an HDR base section, or empty when it has none.
[[nodiscard]] constexpr std::string_view SdrProfileSectionName(std::string_view base) noexcept
{
    for (const auto& entry : kSdrProfileSections)
        if (entry.base == base) return entry.sdr;
    return {};
}

// The HDR-only grading method and curve are not part of the SDR profile.
[[nodiscard]] constexpr bool IsSdrProfileKey(std::string_view base, std::string_view key) noexcept
{
    return !SdrProfileSectionName(base).empty() &&
        !(base == "Image" && (key == "ColorGradingMethod" || key == "ColorGradingCurve"));
}

enum class PluginConfigurationError
{
    none,
    fileUnavailable,
    invalidSyntax,
    missingKey,
    duplicateKey,
    invalidBoolean,
    invalidNumber,
    valueOutOfRange,
    unsupportedAlgorithm,
    unsupportedColorGradingMethod,
    unsupportedColorGradingCurve,
    unsupportedToggleKey,
    invalidCombination
};

struct PluginConfigurationResult
{
    PluginConfiguration value{};
    PluginConfigurationError error{
        PluginConfigurationError::fileUnavailable};
    std::size_t line{};
    bool valid{};
};

[[nodiscard]] PluginConfigurationResult
ParsePluginConfiguration(std::string_view text) noexcept;
[[nodiscard]] PluginConfigurationResult
LoadPluginConfiguration(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] std::string_view ToString(
    ProductionToneMapperAlgorithm algorithm) noexcept;
[[nodiscard]] std::string_view ToString(
    ColorGradingMethod method) noexcept;
[[nodiscard]] std::string_view ToString(
    ColorGradingCurve curve) noexcept;
[[nodiscard]] std::string_view ToString(
    PluginConfigurationError error) noexcept;

}  // namespace fo4hdr
