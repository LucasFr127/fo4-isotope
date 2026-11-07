// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "FO4Isotope/ui/SettingsMenu.hpp"
#include "SettingsStatus.hpp"
#include "TestHarness.hpp"
#include <string_view>

namespace fo4hdr::tests {

namespace {

// A restart is requested only when it would change the HDR outcome.
void TestHdrRequestStatus()
{
    const auto status = [](bool enabled, bool atStart, bool sdrMode) {
        SettingsMenuView view{};
        view.hdrEnabledAtStart = atStart;
        view.sdrMode = sdrMode;
        const auto* text = HdrRequestStatus(enabled, view);
        return std::string_view{text != nullptr ? text : ""};
    };
    // HDR session: turning HDR off needs a restart.
    FO4HDR_CHECK(status(true, true, false).empty());
    FO4HDR_CHECK(status(false, true, false).starts_with("Restart the game to turn HDR off"));
    // HDR requested, but the session entered SDR mode: the SDR reason explains it.
    FO4HDR_CHECK(status(true, true, true).empty());
    FO4HDR_CHECK(status(false, true, true).empty());
    // HDR off at start: turning it on needs a restart.
    FO4HDR_CHECK(status(false, false, true).empty());
    FO4HDR_CHECK(status(true, false, true).starts_with("Restart the game to turn HDR on"));
    // Defaults from an unusable INI cannot save the request, so no restart is offered.
    SettingsMenuView fallback{};
    fallback.sdrMode = true;
    fallback.sdrModeReason = SdrModeReason::configurationDefaults;
    FO4HDR_CHECK(HdrRequestStatus(true, fallback) == nullptr);
}

// Every handled cause of SDR mode names itself; HDR simply off needs no note.
void TestSdrModeReasonText()
{
    const auto text = [](SdrModeReason reason) {
        SettingsMenuView view{};
        view.sdrMode = true;
        view.sdrModeReason = reason;
        const auto* message = SdrModeReasonText(view);
        return std::string_view{message != nullptr ? message : ""};
    };
    FO4HDR_CHECK(text(SdrModeReason::none).empty() && text(SdrModeReason::hdrOff).empty());
    FO4HDR_CHECK(text(SdrModeReason::enbDetected).contains("ENB"));
    FO4HDR_CHECK(text(SdrModeReason::configurationDefaults).contains("FO4Isotope.ini"));
    for (const auto reason : {SdrModeReason::displayNotHdr10, SdrModeReason::descriptorUnsupported,
             SdrModeReason::hdrCandidateRejected, SdrModeReason::hdrInstallFailed})
        FO4HDR_CHECK(text(reason).starts_with("SDR is shown because"));
}


// The status names the first reason the response cannot reach the current draw;
// only a bypass is a warning, and an active response needs no feedback.
void TestWeatherResponseStatus()
{
    const auto value = [](const WeatherLutResponseView& view, bool active) {
        return std::string_view{WeatherResponseStatus(view, active).value};
    };
    const auto info = [](const WeatherLutResponseView& view, bool active) {
        const auto detail = WeatherResponseStatus(view, active).detail;
        return detail.text != nullptr && detail.severity == StatusSeverity::info;
    };
    WeatherLutResponseView view{};
    FO4HDR_CHECK(value(view, false) == "Off" && info(view, false));
    FO4HDR_CHECK(value(view, true) == "Waiting" && info(view, true));
    view.validatedSlots = 0b11U;
    FO4HDR_CHECK(value(view, true) == "Not recognized" && info(view, true));
    view.automaticSlots = 0b01U;
    FO4HDR_CHECK(value(view, true) == "Partly active" && info(view, true));
    view.automaticSlots = 0b11U;
    FO4HDR_CHECK(value(view, true) == "Active");
    FO4HDR_CHECK(WeatherResponseStatus(view, true).detail.text == nullptr);
    view.gradingBypassed = true;
    for (const auto active : {true, false})
    {
        const auto state = WeatherResponseStatus(view, active);
        FO4HDR_CHECK(std::string_view{state.value} == "Bypassed");
        FO4HDR_CHECK(state.detail.severity == StatusSeverity::warning);
    }
}

}  // namespace

void TestSettingsStatus()
{
    TestHdrRequestStatus();
    TestSdrModeReasonText();
    TestWeatherResponseStatus();
}

}  // namespace fo4hdr::tests
