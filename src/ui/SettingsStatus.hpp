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

enum class HealthSeverity : std::uint8_t
{
    info,
    success,
    warning,
    error
};

// What FO4Isotope reports to the host: one status line and, for a problem the
// user can act on, a diagnostic summary with its detail.
struct SettingsMenuHealth
{
    HealthSeverity severity{};
    const char* status{};
    const char* summary{};
    const char* detail{};
    bool operator==(const SettingsMenuHealth&) const = default;
};

// The most severe condition wins: renderer stopped, saving failed, a rejected
// change, then the output mode.
[[nodiscard]] SettingsMenuHealth DescribeSettingsMenuHealth(
    const SettingsMenuView& view, const char* saveProblem) noexcept;

[[nodiscard]] const char* SettingsOperatorDisplayName(ProductionToneMapperAlgorithm algorithm) noexcept;
// The note on the saved HDR request, or null when it needs none.
[[nodiscard]] const char* HdrRequestStatus(bool hdrEnabled, const SettingsMenuView& view) noexcept;
// The note on the saved Render Precision, which the saved Hosted Presentation switch
// decides too; its text is null when it needs none.
[[nodiscard]] StatusMessage RenderPrecisionStatus(
    const HdrInterfaceSettings& saved, const SettingsMenuView& view) noexcept;
// The Output note of a hosted-HDR session; its text is null outside one.
[[nodiscard]] StatusMessage HostedHdrStatus(const SettingsMenuView& view) noexcept;
// The note on the saved hosted-presentation request, or null when it needs none.
[[nodiscard]] const char* HostedPresentationStatus(bool hosted, const SettingsMenuView& view) noexcept;
// Whether the next start would be hosted: saved on, unless this start found no host.
[[nodiscard]] bool HostedAtNextStart(
    const HdrInterfaceSettings& saved, const SettingsMenuView& view) noexcept;
// The color pages' warning while Community Shaders' own tone mapping is shown, or
// their error after the hosted shaders failed; its text is null otherwise.
[[nodiscard]] StatusMessage HostToneMappingStatus(const SettingsMenuView& view) noexcept;
// SDR mode's error after its shaders failed to build; its text is null otherwise.
[[nodiscard]] StatusMessage SdrShadersStatus(const SettingsMenuView& view) noexcept;
// What the Output and color pages report when the color settings cannot act: the ENB
// note, then the host's note, then the SDR shader error; its text is null while they act.
[[nodiscard]] StatusMessage ColorSettingsStatus(const SettingsMenuView& view) noexcept;
// The renderer rejected the last change, or null when it applied.
[[nodiscard]] const char* LiveSettingsErrorText(const SettingsMenuView& view) noexcept;
// Reports SDR mode as HDR off once the saved request turns HDR off, so the reason's
// advice goes away without a restart.
void ApplyHdrRequest(SettingsMenuView& view, bool hdrEnabled) noexcept;
// Why this session runs SDR mode, or null when HDR was simply off.
[[nodiscard]] const char* SdrModeReasonText(const SettingsMenuView& view) noexcept;
// Whether the weather response reaches the current frame's LUT draw.
[[nodiscard]] WeatherResponseState WeatherResponseStatus(
    const WeatherLutResponseView& view, bool active) noexcept;

}  // namespace fo4hdr
