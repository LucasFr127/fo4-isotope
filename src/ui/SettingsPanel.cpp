// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "SettingsPanel.hpp"
#include "Fallout4HDR/ui/SettingsWindow.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"
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
        ImageControl{"Exposure", &HdrSettings::exposureEv,
            {kMinimumSceneExposureEv, kMaximumSceneExposureEv, "%+.2f EV"}},
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
    ImGui::SeparatorText("Scene effects");
    const char* methods[]{"Radial", "Native reference"};
    auto method = static_cast<int>(settings.colorGradingMethod);
    ImGui::SetNextItemWidth(-1.0f);
    bool changed = ImGui::Combo("Grading method", &method, methods, static_cast<int>(std::size(methods)));
    if (changed) settings.colorGradingMethod = static_cast<ColorGradingMethod>(method);
    changed |= DrawNumber("Grading strength", settings.vanillaColorGradingStrengthPercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%"});
    changed |= DrawNumber("Native grade luminance", settings.vanillaGradeLuminancePercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%"});
    changed |= DrawNumber("Native grade color", settings.vanillaGradeColorPercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%"});
    changed |= DrawNumber("Bloom strength", settings.bloomStrengthPercent,
        {kMinimumBloomStrengthPercent, kMaximumBloomStrengthPercent, "%.0f%%"});
    return changed;
}

bool DrawSelectiveRegion(SelectiveColorRegionSettings& region) noexcept
{
    bool changed{};
    changed |= DrawNumber("Hue", region.hueDegrees,
        {kMinimumSelectiveColorHueDegrees, kMaximumSelectiveColorHueDegrees, "%+.1f deg"});
    changed |= DrawNumber("Saturation", region.saturationPercent,
        {kMinimumSelectiveColorSaturationPercent, kMaximumSelectiveColorSaturationPercent, "%.0f%%"});
    changed |= DrawNumber("Brightness", region.brightness,
        {kMinimumSelectiveColorBrightness, kMaximumSelectiveColorBrightness, "%+.0f"});
    return changed;
}

bool DrawSelective(SelectiveColorSettings& settings) noexcept
{
    if (!ImGui::CollapsingHeader("Selective color")) return false;
    ImGui::PushID("selective-color");
    bool changed = ImGui::Checkbox("Enabled", &settings.enabled);
    ImGui::BeginDisabled(!settings.enabled);
    changed |= DrawNumber("Amount", settings.amountPercent,
        {kMinimumSelectiveColorAmountPercent, kMaximumSelectiveColorAmountPercent, "%.0f%%"});
    constexpr std::array names{"Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Magenta"};
    for (std::size_t index = 0; index < settings.regions.size(); ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        ImGui::SeparatorText(names[index]);
        changed |= DrawSelectiveRegion(settings.regions[index]);
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Reset selective color"))
    {
        settings = {};
        changed = true;
    }
    ImGui::PopID();
    return changed;
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

bool DrawOperator(HdrSettings& editable) noexcept
{
    ImGui::SeparatorText("Tone mapper");
    const char* choices[]{"ACES 2 Contrast Look", "AgX", "GT7"};
    auto selected = static_cast<int>(editable.productionAlgorithm);
    if (selected >= static_cast<int>(std::size(choices))) selected = 0;
    ImGui::SetNextItemWidth(-1.0f);
    bool changed = ImGui::Combo("##operator", &selected, choices, static_cast<int>(std::size(choices)));
    if (changed) editable.productionAlgorithm = static_cast<ProductionToneMapperAlgorithm>(selected);
    switch (editable.productionAlgorithm)
    {
    case ProductionToneMapperAlgorithm::agX:
        changed |= DrawAgx(editable.toneMapperParameters);
        break;
    case ProductionToneMapperAlgorithm::gt7:
        changed |= DrawGt7(editable.toneMapperParameters);
        break;
    case ProductionToneMapperAlgorithm::aces2Lmt:
        changed |= DrawNumber("Look strength", editable.toneMapperParameters.acesLookStrength,
            {kMinimumAcesLookStrength, kMaximumAcesLookStrength, "%.2f"});
        ImGui::TextWrapped("Zero bypasses the look; the fixed shadow and gamut adaptations remain.");
        break;
    }
    return changed;
}

const char* OperatorName(ProductionToneMapperAlgorithm algorithm) noexcept
{
    const auto name = ToString(algorithm);
    return name.empty() ? "Unknown operator" : name.data();
}

const char* GradingMethodName(ColorGradingMethod method) noexcept
{
    const auto name = ToString(method);
    return name.empty() ? "Unknown grading method" : name.data();
}

void DrawConfigurationSummary(const char* label, const HdrSettings& value) noexcept
{
    ImGui::TextWrapped("%s: %s, peak %.0f nits, UI %.0f nits, "
        "video %.0f nits, %s grading %.0f%%",
        label, OperatorName(value.productionAlgorithm), value.displayPeakNits,
        value.uiPaperWhiteNits, value.videoWhiteNits,
        GradingMethodName(value.colorGradingMethod),
        value.vanillaColorGradingStrengthPercent);
}

void DrawStatus(const SettingsWindowView& snapshot) noexcept
{
    ImGui::SeparatorText("Status");
    DrawConfigurationSummary("Active", snapshot.activeSettings);
    if (snapshot.lutPending)
        ImGui::Text("Pending: %s at %.0f nits", OperatorName(snapshot.pendingAlgorithm), snapshot.pendingPeakNits);
    if (snapshot.liveSettingsError)
        ImGui::TextWrapped("Settings could not be applied. Active settings retained.");
    const auto persistence = ReadSettingsPersistenceSnapshot();
    if (persistence.state == SettingsPersistenceState::failed)
        ImGui::TextWrapped("Save failed; try again.");
    else if (persistence.state == SettingsPersistenceState::saving)
        ImGui::TextUnformatted("Saving requested settings");
    else if (persistence.state == SettingsPersistenceState::saved)
        ImGui::TextUnformatted("Requested settings saved");
}

bool CanSave(const SettingsWindowView& snapshot) noexcept
{
    return snapshot.threadCompatible &&
        ReadSettingsPersistenceSnapshot().state != SettingsPersistenceState::unavailable;
}

}  // namespace

SettingsPanelActions DrawSettingsPanel(HdrSettings& editable,
    const SettingsWindowView& snapshot) noexcept
{
    ImGui::PushID("hdr-settings");
    SettingsPanelActions actions;
    ImGui::BeginDisabled(!snapshot.threadCompatible);
    actions.request |= DrawOutput(editable);
    actions.request |= DrawOperator(editable);
    actions.request |= DrawImage(editable);
    actions.request |= DrawGrade(editable);
    actions.request |= DrawSelective(editable.selectiveColor);
    if (ImGui::Button("Reset settings"))
    {
        editable = HdrSettings{};
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
