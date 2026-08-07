// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fo4hdr {

enum class GraphicsDeviceBindingStatus
{
    installed,
    alreadyInitialized,
    imageUnavailable,
    importUnavailable,
    foreignPredecessor,
    installationFailed
};

// Call only after validating the main executable/runtime/F4SE tuple. Callback code and
// ownership records remain resident for process lifetime; this owner never unhooks or unloads.
[[nodiscard]] GraphicsDeviceBindingStatus InitializeGameDeviceGeneration() noexcept;

}  // namespace fo4hdr
