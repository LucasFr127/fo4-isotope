// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "DisplayCalibrationPanel.hpp"
#include "SettingsPanelSlider.hpp"

#include "FO4Isotope/config/HdrSettings.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fo4hdr {

bool CalibrationMeasurementAvailable(double nits) noexcept
{
    return std::isfinite(nits) && nits > 0.0;
}

void DrawCalibrationMeasurement(const char* format, double nits) noexcept
{
    if (CalibrationMeasurementAvailable(nits)) ImGui::Text(format, nits);
}

void DrawCalibrationDisplay(const DisplayCalibrationView& view) noexcept
{
    ImGui::TextUnformatted("1. Confirm the display and profile");
    ImGui::Separator();
    ImGui::TextUnformatted("Display:");
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(view.displayNameUtf8 != nullptr &&
            view.displayNameUtf8[0] != '\0' ?
        view.displayNameUtf8 : "Unavailable");
    ImGui::PopTextWrapPos();

    if (view.identityAvailable)
    {
        DrawCalibrationMeasurement("Reported peak: %.0f nits", view.reportedPeakNits);
        DrawCalibrationMeasurement("Reported full-frame: %.0f nits", view.reportedFullFrameNits);
        DrawCalibrationMeasurement("OS SDR white: %.0f nits", view.osSdrWhiteNits);
    }
    else
    {
        ImGui::TextWrapped(
            "Display identity is unavailable. Per-display profiles are disabled.");
    }
}

void DrawCalibrationProfileActions(const DisplayCalibrationView& view, DisplayCalibrationActions& actions) noexcept
{
    ImGui::BeginDisabled(!view.identityAvailable || view.profileBusy);
    if (ImGui::Button("Load Display Profile")) actions.load = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Load saved display peak, menu and HUD white, and video white for this display.");
    if (ImGui::GetContentRegionAvail().x >=
        ImGui::CalcTextSize("Load Display Profile").x +
        ImGui::CalcTextSize("Save Display Profile").x +
        ImGui::GetStyle().FramePadding.x * 4.0F + ImGui::GetStyle().ItemSpacing.x)
        ImGui::SameLine();
    ImGui::BeginDisabled(!view.identityAvailable || view.profileBusy ||
        !view.saveReady);
    if (ImGui::Button("Save Display Profile")) actions.save = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Save display peak, menu and HUD white, and video white for this display.");
}

void DrawCalibrationProfileStatus(const DisplayCalibrationView& view) noexcept
{
    if (view.status != nullptr && view.status[0] != '\0')
    {
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(view.status);
        ImGui::PopTextWrapPos();
    }
    else if (view.profileBusy)
        ImGui::TextUnformatted("Display profile request in progress.");
    else if (view.identityAvailable && !view.saveReady)
        ImGui::TextUnformatted("Display profile is not ready to save.");
}

void DrawCalibrationPeak(HdrSettings& settings, std::uint32_t& mode, const DisplayCalibrationView& view, DisplayCalibrationActions& actions) noexcept
{
    ImGui::Spacing();
    ImGui::TextUnformatted("2. Set peak brightness");
    ImGui::Separator();
    int selectedMode = static_cast<int>(mode);
    ImGui::RadioButton("Peak-White Clipping Ramp", &selectedMode, 10);
    ImGui::SetItemTooltip("Shows steps from 0 to 112.5 percent of the selected peak.\n"
        "Match the display's clipping point; steps above its peak may merge.");
    mode = static_cast<std::uint32_t>(selectedMode);

    float peak = static_cast<float>(settings.displayPeakNits);
    if (detail::DrawSettingsSlider("Display Peak Brightness", &peak, detail::kDisplayPeakSlider))
        settings.displayPeakNits = std::round(peak);
    if (ImGui::IsItemDeactivatedAfterEdit()) actions.changed = true;
    if (detail::DrawRestoreDefaultButton("calibration-peak",
            settings.displayPeakNits != HdrSettings{}.displayPeakNits))
    {
        settings.displayPeakNits = HdrSettings{}.displayPeakNits;
        actions.changed = true;
    }

    if (view.identityAvailable && CalibrationMeasurementAvailable(view.reportedPeakNits))
    {
        if (ImGui::Button("Use Reported Peak"))
        {
            settings.displayPeakNits = std::clamp(
                std::round(view.reportedPeakNits), 400.0, 2000.0);
            actions.changed = true;
        }
        ImGui::SetItemTooltip("Set Display Peak Brightness to the reported peak, within the supported range.");
    }
}

void DrawCalibrationVideoWhite(HdrSettings& settings, DisplayCalibrationActions& actions) noexcept
{
    float videoWhite = static_cast<float>(settings.videoWhiteNits);
    if (detail::DrawSettingsSlider("Video White", &videoWhite, detail::kVideoWhiteSlider))
    {
        settings.videoWhiteNits = videoWhite;
        actions.changed = true;
    }
    if (detail::DrawRestoreDefaultButton("calibration-video-white",
            settings.videoWhiteNits != HdrSettings{}.videoWhiteNits))
    {
        settings.videoWhiteNits = HdrSettings{}.videoWhiteNits;
        actions.changed = true;
    }
}

void DrawCalibrationWhite(HdrSettings& settings, std::uint32_t& mode, const DisplayCalibrationView& view, DisplayCalibrationActions& actions) noexcept
{
    ImGui::Spacing();
    ImGui::TextUnformatted("3. Set menu, HUD and video white");
    ImGui::Separator();
    int selectedMode = static_cast<int>(mode);
    ImGui::RadioButton("Menu & HUD White Patch", &selectedMode, 11);
    ImGui::SetItemTooltip("Shows a white patch at Menu & HUD White brightness. Adjust it for comfortable viewing.");
    mode = static_cast<std::uint32_t>(selectedMode);

    float uiWhite = static_cast<float>(settings.uiPaperWhiteNits);
    if (detail::DrawSettingsSlider("Menu & HUD White", &uiWhite, detail::kUiWhiteSlider))
    {
        settings.uiPaperWhiteNits = uiWhite;
        actions.changed = true;
    }
    if (detail::DrawRestoreDefaultButton("calibration-ui-white",
            settings.uiPaperWhiteNits != HdrSettings{}.uiPaperWhiteNits))
    {
        settings.uiPaperWhiteNits = HdrSettings{}.uiPaperWhiteNits;
        actions.changed = true;
    }
    const bool osWhiteAvailable = view.identityAvailable &&
        CalibrationMeasurementAvailable(view.osSdrWhiteNits);
    if (osWhiteAvailable)
    {
        if (ImGui::Button("Use OS SDR White"))
        {
            settings.uiPaperWhiteNits = std::clamp(
                view.osSdrWhiteNits, 100.0, 300.0);
            actions.changed = true;
        }
        ImGui::SetItemTooltip("Set Menu & HUD White to the OS SDR white level, within the supported range.");
    }

    DrawCalibrationVideoWhite(settings, actions);
}

void DrawCalibrationPatternControls(std::uint32_t& mode, DisplayCalibrationActions& actions) noexcept
{
    int selectedMode = static_cast<int>(mode);
    ImGui::RadioButton("Patterns Off", &selectedMode, 0);
    ImGui::SetItemTooltip("Hide the calibration pattern and return to the scene.");
    mode = static_cast<std::uint32_t>(selectedMode);
    ImGui::BeginDisabled(mode == 0);
    ImGui::Button("Hold to Hide Controls");
    actions.hideOverlay = mode != 0 && ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Hold to view the selected pattern without controls. Release to restore the controls.");
}

void DrawCalibrationChecks(std::uint32_t& mode) noexcept
{
    ImGui::Spacing();
    ImGui::TextUnformatted("4. Check near-black detail");
    ImGui::Separator();
    int selectedMode = static_cast<int>(mode);
    ImGui::RadioButton("Near-Black Ramp", &selectedMode, 12);
    ImGui::SetItemTooltip("Shows near-black steps. Check that they remain distinct without lifting black.");

    ImGui::Spacing();
    ImGui::TextUnformatted("5. Check color and output containment");
    ImGui::Separator();
    ImGui::RadioButton("Saturated Colors", &selectedMode, 13);
    ImGui::SetItemTooltip("Shows saturated colors for checking the display's color response.");
    ImGui::RadioButton("Output Peak Containment", &selectedMode, 14);
    ImGui::SetItemTooltip("Checks that output highlights stay within Display Peak Brightness.");
    mode = static_cast<std::uint32_t>(selectedMode);
}

[[nodiscard]] DisplayCalibrationActions DrawDisplayCalibrationPanel(
    HdrSettings& settings,
    std::uint32_t& mode,
    const DisplayCalibrationView& view) noexcept
{
    DisplayCalibrationActions actions{};

    ImGui::TextWrapped("Verify display brightness with test patterns. "
        "Reported values are starting points; check clipping, comfortable white, "
        "near-black detail and color before saving a display profile.");
    DrawCalibrationPatternControls(mode, actions);
    DrawCalibrationDisplay(view);
    DrawCalibrationProfileActions(view, actions);
    DrawCalibrationProfileStatus(view);
    DrawCalibrationPeak(settings, mode, view, actions);
    DrawCalibrationWhite(settings, mode, view, actions);
    DrawCalibrationChecks(mode);

    return actions;
}

}  // namespace fo4hdr
