// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/PluginConfiguration.hpp"

#include <filesystem>

namespace fo4hdr {

struct GraphicsStartupConfiguration
{
    PluginConfiguration settings;
    std::filesystem::path persistencePath;
    bool measurePerformance{};
};

enum class GraphicsDeviceBindingStatus
{
    installed,
    alreadyInitialized,
    imageUnavailable,
    importUnavailable,
    foreignPredecessor,
    installationFailed,
    settingsUnavailable
};

// Call only after validating the main executable/runtime/F4SE tuple. Callback code and
// ownership records remain resident for process lifetime; this owner never unhooks or unloads.
[[nodiscard]] GraphicsDeviceBindingStatus InitializeGameDeviceGeneration(
    const GraphicsStartupConfiguration& configuration) noexcept;

}  // namespace fo4hdr
