// SPDX-License-Identifier: GPL-3.0-or-later

#include "SettingsPanel.hpp"
#include "LiveSettings.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cmath>
#include <span>

namespace fo4hdr {
namespace {

struct SliderRange
{
    double minimum;
    double maximum;
    const char* format;
};

struct OperatorControl
{
    const char* label;
    double ToneMapperParameters::* value;
    SliderRange range;
};

struct ImageControl
{
    const char* label;
    double HdrSettings::* value;
    SliderRange range;
};

bool DrawNumber(const char* label, double& value, SliderRange range) noexcept
{
    ImGui::TextUnformatted(label);
    if (!std::isfinite(value))
    {
        ImGui::TextUnformatted("Invalid value; use Reset settings.");
        return false;
    }
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderScalar("##value", ImGuiDataType_Double, &value,
        &range.minimum, &range.maximum, range.format, ImGuiSliderFlags_AlwaysClamp);
    const auto requested = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Ctrl+click to type an exact value.");
    ImGui::PopID();
    return requested;
}

bool DrawOutput(HdrSettings& settings) noexcept
{
    ImGui::SeparatorText("Output brightness");
    bool changed{};
    changed |= DrawNumber("Display peak", settings.displayPeakNits, {kMinimumDisplayPeakNits, kMaximumDisplayPeakNits, "%.0f nits"});
    changed |= DrawNumber("Menu and HUD white", settings.uiPaperWhiteNits, {kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits, "%.0f nits"});
    changed |= DrawNumber("Video white", settings.videoWhiteNits, {kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits, "%.0f nits"});
    return changed;
}

bool DrawImage(HdrSettings& settings) noexcept
{
    ImGui::SeparatorText("Image adjustments");
    ImGui::PushID("image-controls");
    const std::array controls{
        ImageControl{"Contrast", &HdrSettings::contrast,
            {kMinimumImageAdjustment, kMaximumImageAdjustment, "%.0f"}},
        ImageControl{"Blacks", &HdrSettings::blacks,
            {kMinimumImageAdjustment, kMaximumImageAdjustment, "%.0f"}},
        ImageControl{"Highlights", &HdrSettings::highlights,
            {kMinimumImageAdjustment, kMaximumImageAdjustment, "%.0f"}},
        ImageControl{"Shadows", &HdrSettings::shadows,
            {kMinimumImageAdjustment, kMaximumImageAdjustment, "%.0f"}},
        ImageControl{"Saturation", &HdrSettings::saturationPercent,
            {kMinimumSaturationPercent, kMaximumSaturationPercent, "%.0f%%"}},
        ImageControl{"Temperature", &HdrSettings::temperature,
            {kMinimumImageAdjustment, kMaximumImageAdjustment, "%.0f"}},
        ImageControl{"Tint", &HdrSettings::tint,
            {kMinimumImageAdjustment, kMaximumImageAdjustment, "%.0f"}}};
    bool changed{};
    for (const auto& control : controls)
        changed |= DrawNumber(control.label, settings.*control.value, control.range);
    ImGui::PopID();
    return changed;
}

bool DrawGrade(HdrSettings& settings) noexcept
{
    ImGui::SeparatorText("Vanilla color grading");
    return DrawNumber("Grading strength", settings.vanillaColorGradingStrengthPercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%"});
}

bool DrawControls(ToneMapperParameters& parameters, std::span<const OperatorControl> controls) noexcept
{
    bool changed{};
    for (const auto& control : controls)
        changed |= DrawNumber(control.label, parameters.*control.value, control.range);
    return changed;
}

bool DrawAgx(ToneMapperParameters& parameters) noexcept
{
    const std::array controls{
        OperatorControl{"Middle gray", &ToneMapperParameters::agxMiddleGrayEv,
            {kMinimumAgxMiddleGrayEv, kMaximumAgxMiddleGrayEv, "%.2f EV"}},
        OperatorControl{"Contrast", &ToneMapperParameters::agxContrast,
            {kMinimumAgxContrast, kMaximumAgxContrast, "%.2f"}},
        OperatorControl{"HDR color response", &ToneMapperParameters::agxHdrPurity,
            {kMinimumAgxHdrPurity, kMaximumAgxHdrPurity, "%.2f"}},
        OperatorControl{"Highlight shoulder", &ToneMapperParameters::agxHdrShoulderPower,
            {kMinimumAgxHdrShoulderPower, kMaximumAgxHdrShoulderPower, "%.2f"}}};
    return DrawControls(parameters, controls);
}

bool DrawGt7(ToneMapperParameters& parameters) noexcept
{
    const std::array controls{
        OperatorControl{"Shadow toe", &ToneMapperParameters::gt7ToeStrength,
            {kMinimumGt7ToeStrength, kMaximumGt7ToeStrength, "%.2f"}},
        OperatorControl{"Shoulder start", &ToneMapperParameters::gt7ShoulderStart,
            {kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart, "%.3f"}},
        OperatorControl{"Color blend", &ToneMapperParameters::gt7ColorBlend,
            {kMinimumGt7ColorBlend, kMaximumGt7ColorBlend, "%.2f"}}};
    return DrawControls(parameters, controls);
}

bool DrawOperator(PluginConfiguration& editable) noexcept
{
    ImGui::SeparatorText("Tone mapper");
    const char* choices[]{"ACES 2", "AgX", "GT7"};
    auto selected = static_cast<int>(editable.algorithm);
    if (selected >= static_cast<int>(std::size(choices))) selected = 0;
    ImGui::SetNextItemWidth(-1.0f);
    bool changed = ImGui::Combo("##operator", &selected, choices, static_cast<int>(std::size(choices)));
    if (changed) editable.algorithm = static_cast<OperatorLutAlgorithm>(selected);
    switch (editable.algorithm)
    {
    case OperatorLutAlgorithm::agX:
        changed |= DrawAgx(editable.display.toneMapperParameters);
        break;
    case OperatorLutAlgorithm::gt7:
        changed |= DrawGt7(editable.display.toneMapperParameters);
        break;
    case OperatorLutAlgorithm::aces2:
        ImGui::TextUnformatted("ACES 2 has no adjustable look controls.");
        break;
    }
    return changed;
}

const char* RenderErrorText(LiveSettingsError error) noexcept
{
    constexpr std::array messages{
        "", "Requested values are invalid. Active settings retained.",
        "Builder busy. Active settings retained.", "Table preparation failed. Active settings retained.",
        "Settings could not be applied. The request remains pending.",
        "Settings generation limit reached.", "Live settings are unavailable."};
    const auto index = static_cast<std::size_t>(error);
    return index < messages.size() ? messages[index] : "Live settings are unavailable.";
}

const char* SaveStateText(SettingsPersistenceState state) noexcept
{
    constexpr std::array messages{
        "Saving unavailable", "Not saved", "Saving requested settings",
        "Requested settings saved", "Save failed; try again", "Save status busy"};
    const auto index = static_cast<std::size_t>(state);
    return index < messages.size() ? messages[index] : "Saving unavailable";
}

const char* OperatorName(OperatorLutAlgorithm algorithm) noexcept
{
    const auto name = ToString(algorithm);
    return name.empty() ? "Unknown operator" : name.data();
}

void DrawConfigurationSummary(const char* label, const PluginConfiguration& value) noexcept
{
    ImGui::TextWrapped("%s: %s, peak %.0f nits, UI %.0f nits, "
        "video %.0f nits, grading %.0f%%",
        label, OperatorName(value.algorithm), value.display.displayPeakNits,
        value.display.uiPaperWhiteNits, value.display.videoWhiteNits,
        value.display.vanillaColorGradingStrengthPercent);
}

void DrawStatus(const LiveSettingsSnapshot& snapshot) noexcept
{
    ImGui::SeparatorText("Status");
    if (!snapshot.available)
    {
        ImGui::TextWrapped("Live settings are unavailable. Controls are disabled.");
        return;
    }
    DrawConfigurationSummary("Active", snapshot.active);
    if (snapshot.pending) DrawConfigurationSummary("Pending", snapshot.requested);
    ImGui::TextWrapped("%s", RenderErrorText(snapshot.error));
    ImGui::TextUnformatted(SaveStateText(snapshot.persistence.state));
    if (snapshot.saveRejected) ImGui::TextWrapped("Save request was not accepted; try again.");
}

bool CanSave(const LiveSettingsSnapshot& snapshot) noexcept
{
    if (!snapshot.available) return false;
    if (snapshot.persistence.state == SettingsPersistenceState::unavailable) return false;
    return snapshot.persistence.state != SettingsPersistenceState::busy;
}

}  // namespace

SettingsPanelActions DrawSettingsPanel(PluginConfiguration& editable,
    const LiveSettingsSnapshot& snapshot) noexcept
{
    ImGui::PushID("hdr-settings");
    SettingsPanelActions actions;
    ImGui::BeginDisabled(!snapshot.available);
    actions.request |= DrawOutput(editable.display);
    actions.request |= DrawOperator(editable);
    actions.request |= DrawImage(editable.display);
    actions.request |= DrawGrade(editable.display);
    if (ImGui::Button("Reset settings"))
    {
        editable = PluginConfiguration{};
        actions.request = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanSave(snapshot));
    actions.save = ImGui::Button("Save requested settings");
    ImGui::EndDisabled();
    DrawStatus(snapshot);
    ImGui::PopID();
    return actions;
}

}  // namespace fo4hdr
