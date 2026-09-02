// SPDX-License-Identifier: GPL-3.0-or-later

#include "DisplayCalibrationPanel.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <imgui.h>

#include <cmath>

namespace fo4hdr {
namespace {

struct BrightnessRange
{
    double minimum;
    double maximum;
    double defaultValue;
};

bool DrawBrightness(const char* label, double& value, BrightnessRange range) noexcept
{
    ImGui::TextUnformatted(label);
    if (!std::isfinite(value))
    {
        ImGui::TextUnformatted("Invalid value; use Reset settings.");
        return false;
    }
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::SliderScalar("##value", ImGuiDataType_Double, &value,
        &range.minimum, &range.maximum, "%.0f nits", ImGuiSliderFlags_AlwaysClamp);
    bool changed = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Ctrl+click to type an exact value.");
    if (ImGui::Button("Restore default"))
    {
        changed = value != range.defaultValue || changed;
        value = range.defaultValue;
    }
    ImGui::PopID();
    return changed;
}

void DrawPattern(const char* label, const char* description,
    std::uint32_t choice, std::uint32_t& mode) noexcept
{
    if (ImGui::RadioButton(label, mode == choice)) mode = choice;
    ImGui::SetItemTooltip("%s", description);
}

bool ValidCalibrationMode(std::uint32_t mode) noexcept
{
    if (mode == 0) return true;
    if (mode < 10) return false;
    return mode <= 14;
}

void DrawPatternControls(std::uint32_t& mode, DisplayCalibrationActions& actions) noexcept
{
    DrawPattern("Patterns off", "Return to the current scene.", 0, mode);
    ImGui::BeginDisabled(mode == 0);
    (void)ImGui::Button("Hold to hide controls");
    actions.hideOverlay = mode != 0 && ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Hold to view the pattern alone; release to restore controls.");
}

bool DrawPeak(HdrSettings& settings, std::uint32_t& mode) noexcept
{
    ImGui::SeparatorText("1. Set peak brightness");
    DrawPattern("Peak-white clipping ramp",
        "32 steps from zero to 112.5 percent of the active peak; above-peak steps are contained.",
        10, mode);
    return DrawBrightness("Calibration display peak", settings.displayPeakNits,
        {kMinimumDisplayPeakNits, kMaximumDisplayPeakNits, HdrSettings{}.displayPeakNits});
}

bool DrawWhites(HdrSettings& settings, std::uint32_t& mode) noexcept
{
    ImGui::SeparatorText("2. Set comfortable white");
    DrawPattern("Menu and HUD white patch",
        "A centered patch at the active menu and HUD white, with black outside.", 11, mode);
    bool changed = DrawBrightness("Calibration menu and HUD white", settings.uiPaperWhiteNits,
        {kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits, HdrSettings{}.uiPaperWhiteNits});
    changed |= DrawBrightness("Calibration video white", settings.videoWhiteNits,
        {kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits, HdrSettings{}.videoWhiteNits});
    ImGui::TextWrapped("Video white affects decoded video independently of menu and HUD white.");
    return changed;
}

void DrawChecks(std::uint32_t& mode) noexcept
{
    ImGui::SeparatorText("3. Inspect black, color and highlights");
    DrawPattern("Near-black ramp", "Ten absolute steps from zero to one nit.", 12, mode);
    DrawPattern("Saturated colors",
        "BT.2020 patches above, converted BT.709 patches below, at active menu and HUD white.",
        13, mode);
    DrawPattern("Output peak containment",
        "Selected-scene channels near 90 percent of active peak are yellow; at peak they are red.",
        14, mode);
}

}  // namespace

DisplayCalibrationActions DrawDisplayCalibrationPanel(
    HdrSettings& settings, std::uint32_t& mode) noexcept
{
    if (!ValidCalibrationMode(mode)) mode = 0;
    DisplayCalibrationActions actions;
    ImGui::PushID("display-calibration");
    ImGui::TextWrapped("Use the patterns on a qualified HDR output. They do not measure display capability. "
        "Pending brightness changes use the active values until applied.");
    DrawPatternControls(mode, actions);
    actions.changed |= DrawPeak(settings, mode);
    actions.changed |= DrawWhites(settings, mode);
    DrawChecks(mode);
    ImGui::PopID();
    return actions;
}

}  // namespace fo4hdr
