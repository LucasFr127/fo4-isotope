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
    // Defaults from an unreadable INI cannot save the request, so no restart is offered.
    SettingsMenuView fallback{};
    fallback.sdrMode = true;
    fallback.sdrModeReason = SdrModeReason::configurationDefaults;
    FO4HDR_CHECK(HdrRequestStatus(true, fallback) == nullptr);
}

// Every handled cause of SDR mode names itself; HDR off, at start or since, needs no note
// unless the INI cannot save the request.
void TestSdrModeReasonText()
{
    const auto text = [](SdrModeReason reason, bool hdrEnabled = true) {
        SettingsMenuView view{};
        view.sdrMode = true;
        view.sdrModeReason = reason;
        ApplyHdrRequest(view, hdrEnabled);
        const auto* message = SdrModeReasonText(view);
        return std::string_view{message != nullptr ? message : ""};
    };
    FO4HDR_CHECK(text(SdrModeReason::none).empty() && text(SdrModeReason::hdrOff).empty());
    FO4HDR_CHECK(text(SdrModeReason::enbDetected).contains("ENB"));
    FO4HDR_CHECK(text(SdrModeReason::configurationDefaults).contains("FO4Isotope.ini"));
    FO4HDR_CHECK(text(SdrModeReason::displayNotHdr10).contains("turn off Enable HDR"));
    for (const auto reason : {SdrModeReason::displayNotHdr10, SdrModeReason::descriptorUnsupported,
             SdrModeReason::hdrCandidateRejected, SdrModeReason::hdrInstallFailed})
    {
        FO4HDR_CHECK(text(reason).starts_with("SDR is shown because"));
        FO4HDR_CHECK(text(reason, false).empty());
    }
    FO4HDR_CHECK(text(SdrModeReason::configurationDefaults, false).contains("FO4Isotope.ini"));
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
    // A held draw used the response even when it verified no LUT itself.
    view.held = true;
    FO4HDR_CHECK(value(view, true) == "Held" && info(view, true));
    FO4HDR_CHECK(value(view, false) == "Off");
    view.held = false;
    view.validatedSlots = 0b11U;
    FO4HDR_CHECK(value(view, true) == "Not recognized" && info(view, true));
    view.automaticSlots = 0b01U;
    FO4HDR_CHECK(value(view, true) == "Partly active" && info(view, true));
    view.automaticSlots = 0b11U;
    FO4HDR_CHECK(value(view, true) == "Active");
    FO4HDR_CHECK(WeatherResponseStatus(view, true).detail.text == nullptr);
    // A bypass outranks a held response.
    view.held = true;
    view.gradingBypassed = true;
    for (const auto active : {true, false})
    {
        const auto state = WeatherResponseStatus(view, active);
        FO4HDR_CHECK(std::string_view{state.value} == "Bypassed");
        FO4HDR_CHECK(state.detail.severity == StatusSeverity::warning);
    }
}


// The most severe condition names the host status; only problems carry a diagnostic.
void TestSettingsMenuHealth()
{
    SettingsMenuView view{};
    view.presenting = true;
    view.hdrActive = true;
    auto health = DescribeSettingsMenuHealth(view, nullptr);
    FO4HDR_CHECK(health.severity == HealthSeverity::success && health.summary == nullptr);
    view.hdrActive = false;
    view.sdrMode = true;
    view.sdrModeReason = SdrModeReason::hdrOff;
    const auto hdrOff = DescribeSettingsMenuHealth(view, nullptr);
    FO4HDR_CHECK(hdrOff.severity == HealthSeverity::info && hdrOff.summary == nullptr);
    // An SDR display is informational and points at the HDR switch; turning it off settles it.
    view.sdrModeReason = SdrModeReason::displayNotHdr10;
    health = DescribeSettingsMenuHealth(view, nullptr);
    FO4HDR_CHECK(health.severity == HealthSeverity::info && health.summary == nullptr);
    FO4HDR_CHECK(std::string_view{health.status}.contains("turn off Enable HDR"));
    auto settled = view;
    ApplyHdrRequest(settled, false);
    FO4HDR_CHECK(DescribeSettingsMenuHealth(settled, nullptr) == hdrOff);
    view.sdrModeReason = SdrModeReason::hdrCandidateRejected;
    health = DescribeSettingsMenuHealth(view, nullptr);
    FO4HDR_CHECK(health.severity == HealthSeverity::warning);
    FO4HDR_CHECK(health.detail == SdrModeReasonText(view));
    view.sdrControlsUnavailable = true;
    FO4HDR_CHECK(DescribeSettingsMenuHealth(view, nullptr).severity == HealthSeverity::error);
    view.liveSettingsError = true;
    health = DescribeSettingsMenuHealth(view, nullptr);
    FO4HDR_CHECK(std::string_view{health.summary} == "Change not applied");
    constexpr auto saveProblem = "Saving failed.";
    health = DescribeSettingsMenuHealth(view, saveProblem);
    FO4HDR_CHECK(std::string_view{health.summary} == "Settings not saved" && health.detail == saveProblem);
    view.presenting = false;
    health = DescribeSettingsMenuHealth(view, saveProblem);
    FO4HDR_CHECK(health.severity == HealthSeverity::error);
    FO4HDR_CHECK(std::string_view{health.summary} == "Renderer not running");
}

}  // namespace

void TestSettingsStatus()
{
    TestHdrRequestStatus();
    TestSdrModeReasonText();
    TestWeatherResponseStatus();
    TestSettingsMenuHealth();
}

}  // namespace fo4hdr::tests
