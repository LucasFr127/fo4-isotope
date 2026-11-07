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


// The status names the first reason the response cannot reach the current draw.
void TestWeatherResponseStatusText()
{
    const auto text = [](const WeatherLutResponseView& view, bool active) {
        return std::string_view{WeatherResponseStatusText(view, active)};
    };
    WeatherLutResponseView view{};
    FO4HDR_CHECK(text(view, false).starts_with("Off"));
    FO4HDR_CHECK(text(view, true).starts_with("Waiting"));
    view.validatedSlots = 0b11U;
    FO4HDR_CHECK(text(view, true).starts_with("No weather response"));
    view.automaticSlots = 0b01U;
    FO4HDR_CHECK(text(view, true).starts_with("Partly active"));
    view.automaticSlots = 0b11U;
    FO4HDR_CHECK(text(view, true).starts_with("Active"));
    view.gradingBypassed = true;
    FO4HDR_CHECK(text(view, true).starts_with("Grading replacement bypassed"));
    FO4HDR_CHECK(text(view, false).starts_with("Grading replacement bypassed"));
}

}  // namespace

void TestSettingsStatus()
{
    TestHdrRequestStatus();
    TestSdrModeReasonText();
    TestWeatherResponseStatusText();
}

}  // namespace fo4hdr::tests
