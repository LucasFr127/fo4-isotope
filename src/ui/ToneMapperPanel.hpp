// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#include "FO4Isotope/config/HdrSettings.hpp"
namespace fo4hdr {
struct SettingsWindowView;
struct ToneMapperRecommendedRange;
const char* SettingsOperatorDisplayName(ProductionToneMapperAlgorithm algorithm) noexcept;
[[nodiscard]] const char* ToneMapperRecommendationStatus(
    double value, const ToneMapperRecommendedRange& range) noexcept;
void DrawColorSettingsTabs(HdrSettings& settings, bool& commitRequested,
    const SettingsWindowView& view, float footerHeight) noexcept;
// The SDR profile hides controls without an SDR effect.
[[nodiscard]] bool DrawToneMapperPanel(HdrSettings& settings, bool sdrProfile = false) noexcept;
[[nodiscard]] bool DrawVanillaGradePanel(HdrSettings& settings, const HdrSettings& defaults) noexcept;
}
