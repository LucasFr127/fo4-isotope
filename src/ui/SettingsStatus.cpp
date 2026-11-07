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
        "started. Turn on Use HDR in Windows display settings, then restart the game.",
    "SDR is shown because the game's swap chain cannot carry HDR, for example in exclusive "
        "fullscreen or with High FPS Physics Fix's sequential swap effect. Use borderless or "
        "windowed mode with a flip or discard swap effect, then restart the game.",
    "SDR is shown because the graphics driver or another graphics mod refused the HDR "
        "swap chain. The F4SE log names the failed check.",
    "SDR is shown because HDR resources could not be created on this swap chain, for "
        "example under another graphics mod. The F4SE log names the failure."};
static_assert(kSdrModeReasonTexts.size() ==
    static_cast<std::size_t>(SdrModeReason::hdrInstallFailed) + 1U);

}  // namespace

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

const char* SdrModeReasonText(const SettingsMenuView& view) noexcept
{
    const auto index = static_cast<std::size_t>(view.sdrModeReason);
    return index < kSdrModeReasonTexts.size() ? kSdrModeReasonTexts[index] : nullptr;
}

const char* WeatherResponseStatusText(const WeatherLutResponseView& view, bool active) noexcept
{
    if (view.gradingBypassed) return "Grading replacement bypassed: the response is not applied.";
    if (!active) return "Off. The Global Grade settings apply.";
    if (view.validatedSlots == 0)
        return "Waiting for a recognized vanilla LUT draw. The Global Grade settings apply.";
    if (view.automaticSlots == 0)
        return "No weather response is recognized for this scene. The Global Grade settings apply.";
    if ((view.automaticSlots & view.validatedSlots) != view.validatedSlots)
        return "Partly active: some contributors use the Global Grade settings.";
    return "Active for the current scene.";
}

}  // namespace fo4hdr
