// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"

namespace fo4hdr {

[[nodiscard]] bool DrawWeatherLutResponsePanel(
    HdrSettings& settings, const WeatherLutResponseView& view) noexcept;

}  // namespace fo4hdr
