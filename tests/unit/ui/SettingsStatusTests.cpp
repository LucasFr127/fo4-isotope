// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "FO4Isotope/ui/SettingsMenu.hpp"
#include "SettingsStatus.hpp"
#include "TestHarness.hpp"
#include <cstdint>
#include <string_view>
#include <utility>

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

[[nodiscard]] SettingsMenuView HdrSession(RenderPrecision atStart, std::uint64_t targets = 0)
{
    SettingsMenuView view{};
    view.hdrActive = true;
    view.renderPrecisionAtStart = atStart;
    view.vanillaLightingTargets = targets;
    return view;
}

[[nodiscard]] HdrInterfaceSettings SavedPrecision(RenderPrecision precision, bool hosted = false)
{
    return HdrInterfaceSettings{.hostedPresentation = hosted, .renderPrecision = precision};
}

[[nodiscard]] bool Says(const StatusMessage& status, StatusSeverity severity, std::string_view text)
{
    return status.text != nullptr && status.severity == severity &&
        std::string_view{status.text}.contains(text);
}

void TestRenderPrecisionStatus()
{
    using enum RenderPrecision;
    // The session's own choice applied: nothing to say. Low keeps every target without
    // counting lighting textures, so a zero count is expected there.
    FO4HDR_CHECK(RenderPrecisionStatus(SavedPrecision(high), HdrSession(high)).text == nullptr);
    FO4HDR_CHECK(
        RenderPrecisionStatus(SavedPrecision(medium), HdrSession(medium, 2)).text == nullptr);
    FO4HDR_CHECK(RenderPrecisionStatus(SavedPrecision(low), HdrSession(low)).text == nullptr);
    // A change waits for the next game start.
    for (const auto [choice, atStart] : {std::pair{low, high}, std::pair{high, low},
             std::pair{medium, low}, std::pair{low, medium}})
        FO4HDR_CHECK(Says(RenderPrecisionStatus(SavedPrecision(choice), HdrSession(atStart)),
            StatusSeverity::warning, "Restart the game"));
    // Medium from the start, but no lighting texture kept the game's format.
    FO4HDR_CHECK(Says(RenderPrecisionStatus(SavedPrecision(medium), HdrSession(medium)),
        StatusSeverity::error, "Medium was not applied"));
    auto inactive = HdrSession(medium);
    inactive.hdrActive = false;
    FO4HDR_CHECK(RenderPrecisionStatus(SavedPrecision(medium), inactive).text == nullptr);
    // SDR mode has no promotion, so the choice does nothing there.
    auto sdr = HdrSession(high);
    sdr.sdrMode = true;
    FO4HDR_CHECK(Says(RenderPrecisionStatus(SavedPrecision(low), sdr), StatusSeverity::info,
        "Applies only to HDR"));
    // Defaults from an unreadable INI cannot save it.
    sdr.sdrModeReason = SdrModeReason::configurationDefaults;
    FO4HDR_CHECK(RenderPrecisionStatus(SavedPrecision(low, true), sdr).text == nullptr);
}

// A saved Hosted Presentation promotes nothing at the next start, in HDR or SDR, so a
// changed choice asks for no restart; turning the switch off lets the choice apply again.
void TestHostedRenderPrecisionStatus()
{
    using enum RenderPrecision;
    auto sdr = HdrSession(high);
    sdr.sdrMode = true;
    for (const auto& view : {HdrSession(high), sdr})
        FO4HDR_CHECK(Says(RenderPrecisionStatus(SavedPrecision(low, true), view),
            StatusSeverity::info, "Hosted Presentation"));
    auto hostedSession = HdrSession(high);
    hostedSession.hostedHdr = true;
    FO4HDR_CHECK(Says(RenderPrecisionStatus(SavedPrecision(low), hostedSession),
        StatusSeverity::warning, "Restart the game"));
    // Without a host this start ran unhosted, and so will the next one.
    auto noHost = HdrSession(high);
    noHost.hostedPresentationIgnored = true;
    FO4HDR_CHECK(!HostedAtNextStart(SavedPrecision(low, true), noHost));
    FO4HDR_CHECK(HostedAtNextStart(SavedPrecision(low, true), HdrSession(high)));
    FO4HDR_CHECK(Says(RenderPrecisionStatus(SavedPrecision(low, true), noHost),
        StatusSeverity::warning, "Restart the game"));
}

// The color pages warn only while Community Shaders' own tone mapping is shown.
void TestHostToneMappingReasons()
{
    SettingsMenuView view{};
    for (const auto reason : {SdrModeReason::none, SdrModeReason::enbDetected,
             SdrModeReason::hostedPresentation, SdrModeReason::hostPresentsSdr})
    {
        view.sdrModeReason = reason;
        FO4HDR_CHECK(HostToneMappingStatus(view).text == nullptr);
    }
    for (const auto reason : {SdrModeReason::hostPresentsHdr, SdrModeReason::hostFormatUnknown})
    {
        view.sdrModeReason = reason;
        FO4HDR_CHECK(Says(HostToneMappingStatus(view), StatusSeverity::warning, "no effect"));
    }
}

// A hosted HDR latch whose host setting changed draws the host's shaders too, and failed
// hosted shaders are an error whatever the host shows.
void TestHostedToneMappingStatus()
{
    SettingsMenuView hosted{};
    hosted.hostedHdr = true;
    FO4HDR_CHECK(HostToneMappingStatus(hosted).text == nullptr);
    hosted.hostStateMismatch = true;
    FO4HDR_CHECK(Says(HostToneMappingStatus(hosted), StatusSeverity::warning, "no effect"));
    hosted.sdrControlsUnavailable = true;
    FO4HDR_CHECK(Says(HostToneMappingStatus(hosted), StatusSeverity::error, "could not be built"));
}

// The color pages report what the Output page does: the ENB note, then the host's note,
// then SDR mode's failed shader build.
void TestColorSettingsStatus()
{
    SettingsMenuView view{};
    view.sdrControlsUnavailable = true;
    FO4HDR_CHECK(ColorSettingsStatus(view).text == nullptr);
    view.sdrMode = true;
    FO4HDR_CHECK(Says(ColorSettingsStatus(view), StatusSeverity::error, "The SDR color settings"));
    FO4HDR_CHECK(Says(SdrShadersStatus(view), StatusSeverity::error, "could not be built"));
    view.sdrModeReason = SdrModeReason::hostPresentsHdr;
    FO4HDR_CHECK(Says(ColorSettingsStatus(view), StatusSeverity::warning, "no effect"));
    // Nothing substitutes under ENB, so its note outranks the host's and the build's.
    view.enbDetected = true;
    FO4HDR_CHECK(Says(ColorSettingsStatus(view), StatusSeverity::warning, "ENB is installed"));
    view = SettingsMenuView{};
    view.sdrMode = true;
    FO4HDR_CHECK(ColorSettingsStatus(view).text == nullptr);
    FO4HDR_CHECK(SdrShadersStatus(view).text == nullptr);
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
    // Hosted presentation keeps its note with HDR on or off.
    FO4HDR_CHECK(text(SdrModeReason::hostedPresentation).contains("Community Shaders"));
    FO4HDR_CHECK(text(SdrModeReason::hostedPresentation, false).contains("Community Shaders"));
    FO4HDR_CHECK(text(SdrModeReason::hostPresentsSdr).contains("reports SDR"));
    FO4HDR_CHECK(text(SdrModeReason::hostNotFound).contains("HDR Display"));
    // With HDR turned off, those hosted chains would start with the hosted note.
    for (const auto reason : {SdrModeReason::hostPresentsSdr, SdrModeReason::hostNotFound})
        FO4HDR_CHECK(text(reason, false).contains("Enable HDR is off"));
    // While the host shows HDR, SDR mode yields whatever Enable HDR says, so these notes stay.
    for (const auto hdrEnabled : {true, false})
    {
        FO4HDR_CHECK(text(SdrModeReason::hostFormatUnknown, hdrEnabled).contains("Update Isotope"));
        FO4HDR_CHECK(text(SdrModeReason::hostPresentsHdr, hdrEnabled).contains("Enable HDR"));
    }
}

// A hosted-HDR session reports HDR through the host, and a changed host setting asks for a restart.
void TestHostedHdrStatus()
{
    SettingsMenuView view{};
    view.presenting = true;
    FO4HDR_CHECK(HostedHdrStatus(view).text == nullptr);
    view.hostedHdr = true;
    FO4HDR_CHECK(Says(HostedHdrStatus(view), StatusSeverity::info, "presents in HDR"));
    auto health = DescribeSettingsMenuHealth(view, nullptr);
    FO4HDR_CHECK(health.severity == HealthSeverity::success);
    FO4HDR_CHECK(std::string_view{health.status}.contains("through Community Shaders"));
    view.hostStateMismatch = true;
    FO4HDR_CHECK(Says(HostedHdrStatus(view), StatusSeverity::warning, "Restart the game"));
    health = DescribeSettingsMenuHealth(view, nullptr);
    FO4HDR_CHECK(health.severity == HealthSeverity::warning && health.summary != nullptr);
}

// Failed hosted shaders are an error on the Output page and in the host status.
void TestHostedHdrFailure()
{
    SettingsMenuView view{};
    view.presenting = true;
    view.hostedHdr = true;
    view.sdrControlsUnavailable = true;
    FO4HDR_CHECK(Says(HostedHdrStatus(view), StatusSeverity::error, "could not be built"));
    const auto health = DescribeSettingsMenuHealth(view, nullptr);
    FO4HDR_CHECK(health.severity == HealthSeverity::error &&
        std::string_view{health.status}.contains("could not start"));
}

// The hosted-presentation switch asks for a restart only when it differs from this session.
void TestHostedPresentationStatus()
{
    const auto status = [](bool hosted, bool atStart) {
        SettingsMenuView view{};
        view.hostedPresentationAtStart = atStart;
        const auto* text = HostedPresentationStatus(hosted, view);
        return std::string_view{text != nullptr ? text : ""};
    };
    FO4HDR_CHECK(status(false, false).empty() && status(true, true).empty());
    FO4HDR_CHECK(status(true, false).starts_with("Restart the game"));
    FO4HDR_CHECK(status(false, true).starts_with("Restart the game"));
    SettingsMenuView fallback{};
    fallback.sdrModeReason = SdrModeReason::configurationDefaults;
    FO4HDR_CHECK(HostedPresentationStatus(true, fallback) == nullptr);
    // Without a host the saved switch is named, and turning it off needs no restart.
    SettingsMenuView noHost{};
    noHost.hostedPresentationIgnored = true;
    const auto* note = HostedPresentationStatus(true, noHost);
    FO4HDR_CHECK(note != nullptr && std::string_view{note}.starts_with("Community Shaders is not"));
    FO4HDR_CHECK(HostedPresentationStatus(false, noHost) == nullptr);
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
    TestRenderPrecisionStatus();
    TestHostedRenderPrecisionStatus();
    TestSdrModeReasonText();
    TestHostedPresentationStatus();
    TestHostedHdrStatus();
    TestHostedHdrFailure();
    TestHostToneMappingReasons();
    TestHostedToneMappingStatus();
    TestColorSettingsStatus();
    TestWeatherResponseStatus();
    TestSettingsMenuHealth();
}

}  // namespace fo4hdr::tests
