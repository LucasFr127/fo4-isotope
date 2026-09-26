// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#include "Fallout4HDR/config/HdrSettings.hpp"
namespace fo4hdr {
struct SettingsWindowView;
const char* SettingsOperatorDisplayName(ProductionToneMapperAlgorithm algorithm) noexcept;
void DrawColorSettingsTabs(HdrSettings& settings, bool& commitRequested,
    const SettingsWindowView& view, float footerHeight) noexcept;
[[nodiscard]] bool DrawToneMapperPanel(HdrSettings& settings) noexcept;
[[nodiscard]] bool DrawVanillaGradePanel(HdrSettings& settings) noexcept;
}
