// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"

#include <string>
#include <string_view>

namespace fo4hdr {

[[nodiscard]] std::string RewriteHdrWindowPositionIni(
    std::string_view original, double x, double y);
[[nodiscard]] std::string RewriteHdrSettingsIni(
    std::string_view original, const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings);
}  // namespace fo4hdr
