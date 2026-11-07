// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/ui/SettingsMenu.hpp"

namespace fo4hdr {

// A short state for the value column and the feedback that explains it.
struct WeatherResponseState
{
    const char* value;
    StatusMessage detail;
};

[[nodiscard]] const char* SettingsOperatorDisplayName(ProductionToneMapperAlgorithm algorithm) noexcept;
// The note on the saved HDR request, or null when it needs none.
[[nodiscard]] const char* HdrRequestStatus(bool hdrEnabled, const SettingsMenuView& view) noexcept;
// Why this session runs SDR mode, or null when HDR was simply off.
[[nodiscard]] const char* SdrModeReasonText(const SettingsMenuView& view) noexcept;
// Whether the weather response reaches the current frame's LUT draw.
[[nodiscard]] WeatherResponseState WeatherResponseStatus(
    const WeatherLutResponseView& view, bool active) noexcept;

}  // namespace fo4hdr
