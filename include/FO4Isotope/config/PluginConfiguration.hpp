// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"

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
    PluginConfigurationError error) noexcept;

}  // namespace fo4hdr
