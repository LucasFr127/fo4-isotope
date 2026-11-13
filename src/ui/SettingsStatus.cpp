// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SettingsStatus.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/ui/SettingsMenu.hpp"

#include <array>
#include <cstddef>

namespace fo4hdr {
namespace {

// Indexed by SdrModeReason; HDR simply off needs no note.
constexpr std::array<const char*, 8> kSdrModeReasonTexts{
    nullptr,
    nullptr,
    "FO4Isotope.ini is missing or has an invalid setting, so defaults are active and "
        "changes are not saved. The F4SE log names the setting. Replace the INI with the "
        "one from the download, then restart the game.",
    "SDR is shown because ENB is installed. Remove ENB to use HDR.",
    "SDR is shown because Windows HDR was off or no HDR display was found when the game "
        "started. On an SDR display, turn off Enable HDR. On an HDR display, turn on Use HDR "
        "in Windows display settings, then restart the game.",
    "SDR is shown because the game's swap chain cannot carry HDR, for example in exclusive "
        "fullscreen or with High FPS Physics Fix's sequential swap effect. Use borderless or "
        "windowed mode with a flip or discard swap effect, then restart the game.",
    "SDR is shown because the graphics driver or another graphics mod refused the HDR "
        "swap chain. The F4SE log names the failed check.",
    "SDR is shown because HDR resources could not be created on this swap chain, for "
        "example under another graphics mod. The F4SE log names the failure."};
static_assert(kSdrModeReasonTexts.size() ==
    static_cast<std::size_t>(SdrModeReason::hdrInstallFailed) + 1U);

// The host status for each SdrModeReason. HDR off and an SDR display are normal setups,
// so they carry no diagnostic: the host keeps diagnostics after the cause is gone.
constexpr std::array<SettingsMenuHealth, 8> kSdrModeHealth{{
    {HealthSeverity::info, "SDR output."},
    {HealthSeverity::info, "SDR output: HDR is off."},
    {HealthSeverity::warning, "SDR output: FO4Isotope.ini is invalid.", "HDR unavailable"},
    {HealthSeverity::warning, "SDR output: ENB is installed.", "HDR unavailable"},
    {HealthSeverity::info, "SDR output: no HDR display. On an SDR display, turn off Enable HDR."},
    {HealthSeverity::warning, "SDR output: the swap chain cannot carry HDR.", "HDR unavailable"},
    {HealthSeverity::warning, "SDR output: the HDR swap chain was refused.", "HDR unavailable"},
    {HealthSeverity::warning, "SDR output: HDR resources could not be created.",
        "HDR unavailable"}}};
static_assert(kSdrModeHealth.size() == kSdrModeReasonTexts.size());

[[nodiscard]] SettingsMenuHealth DescribeSdrHealth(const SettingsMenuView& view) noexcept
{
    if (view.sdrControlsUnavailable)
        return {HealthSeverity::error, "SDR color settings unavailable.",
            "SDR color settings unavailable", "Their shaders could not be built. Restart the game "
            "to retry."};
    const auto index = static_cast<std::size_t>(view.sdrModeReason);
    const auto* detail = SdrModeReasonText(view);
    if (index >= kSdrModeHealth.size() || detail == nullptr) return kSdrModeHealth[1];
    auto health = kSdrModeHealth[index];
    health.detail = detail;
    return health;
}

}  // namespace

const char* LiveSettingsErrorText(const SettingsMenuView& view) noexcept
{
    return view.liveSettingsError ? "The last change could not be applied, so the previous "
        "settings stay active. Change a setting to retry." : nullptr;
}

SettingsMenuHealth DescribeSettingsMenuHealth(
    const SettingsMenuView& view, const char* saveProblem) noexcept
{
    if (!view.presenting)
        return {HealthSeverity::error, "Renderer not running. See FO4Isotope.log.",
            "Renderer not running", "FO4Isotope is not presenting this session, so its settings "
            "cannot apply."};
    if (saveProblem != nullptr)
        return {HealthSeverity::error, "Settings are not being saved.", "Settings not saved",
            saveProblem};
    if (const auto* rejected = LiveSettingsErrorText(view))
        return {HealthSeverity::warning, "The last change was not applied.", "Change not applied",
            rejected};
    if (view.sdrMode) return DescribeSdrHealth(view);
    if (view.hdrActive) return {HealthSeverity::success, "HDR10 output active."};
    return {HealthSeverity::info, "HDR10 output inactive."};
}

const char* SettingsOperatorDisplayName(ProductionToneMapperAlgorithm algorithm) noexcept
{
    return algorithm == ProductionToneMapperAlgorithm::aces2 ?
        "ACES 2" : ToString(algorithm).data();
}

const char* HdrRequestStatus(bool hdrEnabled, const SettingsMenuView& view) noexcept
{
    // Defaults from an unusable INI cannot save the request; the reason names the fix.
    if (view.sdrModeReason == SdrModeReason::configurationDefaults) return nullptr;
    if (hdrEnabled && !view.hdrEnabledAtStart) return "Restart the game to turn HDR on.";
    // A session already in SDR mode starts the same way with HDR off, so it needs no restart.
    if (!hdrEnabled && !view.sdrMode) return "Restart the game to turn HDR off.";
    return nullptr;
}

void ApplyHdrRequest(SettingsMenuView& view, bool hdrEnabled) noexcept
{
    // Once HDR is turned off, SDR is the user's choice; only an unusable INI still needs a fix.
    if (!hdrEnabled && view.sdrModeReason != SdrModeReason::configurationDefaults)
        view.sdrModeReason = SdrModeReason::hdrOff;
}

const char* SdrModeReasonText(const SettingsMenuView& view) noexcept
{
    const auto index = static_cast<std::size_t>(view.sdrModeReason);
    return index < kSdrModeReasonTexts.size() ? kSdrModeReasonTexts[index] : nullptr;
}

WeatherResponseState WeatherResponseStatus(const WeatherLutResponseView& view, bool active) noexcept
{
    if (view.gradingBypassed)
        return {"Bypassed", {"Grading replacement bypassed: the response is not applied.",
            StatusSeverity::warning}};
    if (!active) return {"Off", {"The Fallback Grade settings apply."}};
    if (view.held)
        return {"Held", {"The game is changing its LUTs; the last verified response is held."}};
    if (view.validatedSlots == 0)
        return {"Waiting",
            {"Waiting for a recognized vanilla LUT draw. The Fallback Grade settings apply."}};
    if (view.automaticSlots == 0)
        return {"Not recognized",
            {"No weather response is recognized for this scene. The Fallback Grade settings apply."}};
    if ((view.automaticSlots & view.validatedSlots) != view.validatedSlots)
        return {"Partly active", {"Some contributors use the Fallback Grade settings."}};
    return {"Active", {}};
}

}  // namespace fo4hdr
