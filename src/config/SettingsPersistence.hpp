// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"

#include <string>
#include <string_view>

namespace fo4hdr {

[[nodiscard]] std::string RewriteHdrWindowPositionIni(
    std::string_view original, double x, double y);
[[nodiscard]] std::string RewriteHdrSettingsIni(
    std::string_view original, const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings);
[[nodiscard]] std::string RewriteGradeProfilesIni(
    std::string_view original, const LutGradeProfiles& profiles,
    const LutGradeProfiles& distributedProfiles);

}  // namespace fo4hdr
