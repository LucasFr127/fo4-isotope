// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#include "FO4Isotope/config/HdrSettings.hpp"
namespace fo4hdr {
struct SettingsWindowView;
struct ToneMapperRecommendedRange;
const char* SettingsOperatorDisplayName(ProductionToneMapperAlgorithm algorithm) noexcept;
[[nodiscard]] const char* ToneMapperRecommendationStatus(
    double value, const ToneMapperRecommendedRange& range) noexcept;
// The Output tab's note on the saved HDR request, or null when it needs none.
[[nodiscard]] const char* HdrRequestStatus(bool hdrEnabled, const SettingsWindowView& view) noexcept;
// Why this session runs SDR mode, or null when HDR was simply off.
[[nodiscard]] const char* SdrModeReasonText(const SettingsWindowView& view) noexcept;
// The drafts the color tabs edit. hdrEnabled is the saved HDR request, which
// applies at the next game start.
struct ColorSettingsDrafts
{
    HdrSettings& settings;
    bool& commitRequested;
    bool& hdrEnabled;
};
void DrawColorSettingsTabs(const ColorSettingsDrafts& drafts,
    const SettingsWindowView& view, float footerHeight) noexcept;
// The SDR profile hides controls without an SDR effect.
[[nodiscard]] bool DrawToneMapperPanel(HdrSettings& settings, bool sdrProfile = false) noexcept;
[[nodiscard]] bool DrawVanillaGradePanel(HdrSettings& settings, const HdrSettings& defaults) noexcept;
}
