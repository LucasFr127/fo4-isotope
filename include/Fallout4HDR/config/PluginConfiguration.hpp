// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fo4hdr {

struct PluginConfiguration
{
    HdrSettings display;
    OperatorLutAlgorithm algorithm{OperatorLutAlgorithm::aces2};
    bool measurePerformance{};
    bool enabled{};
    bool developerMode{};
    bool traceRenderPipeline{};
    bool developerHdr10Reference{};
    bool showDeveloperOverlay{};
    bool forceHdrSwapChainFailure{};
    [[nodiscard]] bool operator==(const PluginConfiguration& other) const noexcept;
};

enum class PluginConfigurationStatus : std::uint8_t
{
    loaded,
    missing,
    invalid,
    ioError
};

struct GradeProfileFileIssue
{
    std::filesystem::path path;
    PluginConfigurationStatus status{PluginConfigurationStatus::invalid};
    std::size_t line{};
};

struct PluginConfigurationResult
{
    PluginConfiguration value;
    PluginConfigurationStatus status{PluginConfigurationStatus::invalid};
    std::size_t line{};
    LutGradeProfiles profileInheritances;
    LutGradeProfiles distributedProfiles;
    std::vector<GradeProfileFileIssue> profileIssues;
};

[[nodiscard]] bool ValidatePluginConfiguration(const PluginConfiguration& value) noexcept;
[[nodiscard]] PluginConfigurationResult ParsePluginConfiguration(std::string_view text) noexcept;
[[nodiscard]] PluginConfigurationResult ParseGradeProfileConfiguration(std::string_view text) noexcept;
[[nodiscard]] PluginConfigurationResult LoadPluginConfiguration(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] std::string_view ToString(OperatorLutAlgorithm algorithm) noexcept;
[[nodiscard]] std::string_view ToString(ColorGradingMethod method) noexcept;

}  // namespace fo4hdr
