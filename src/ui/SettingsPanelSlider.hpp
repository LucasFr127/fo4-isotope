// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <imgui.h>

#include <algorithm>
#include <string_view>

namespace fo4hdr::detail {

struct SettingsSliderRange
{
    float minimum;
    float maximum;
    const char* format;
    const char* help{};
};

inline constexpr SettingsSliderRange kDisplayPeakSlider{
    400.0F, 2000.0F, "%.0f nits", "Match the display's HDR peak brightness."};
inline constexpr SettingsSliderRange kUiWhiteSlider{
    100.0F, 300.0F, "%.0f nits", "Sets the brightness of white menu and HUD elements."};
inline constexpr SettingsSliderRange kVideoWhiteSlider{
    100.0F, 300.0F, "%.0f nits", "Sets the brightness of white in video playback."};

inline void DrawSettingsSliderTooltip(const char* help) noexcept
{
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0F);
        if (help) ImGui::TextUnformatted(help);
        ImGui::TextUnformatted("Ctrl+click to type an exact value.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

[[nodiscard]] bool DrawRestoreDefaultButton(const char* id, bool modified) noexcept;
void DrawSettingsControlLabel(std::string_view label) noexcept;
[[nodiscard]] bool DrawSettingsSlider(const char* label, float* value,
    const SettingsSliderRange& range) noexcept;

struct SettingsSliderPresentation
{
    double defaultValue;
    double minimum;
    double maximum;
    double displayScale;
    const char* format;
    const char* help;
    const char* resetHelp{"Restore this setting's default."};
};

[[nodiscard]] inline bool DrawSettingsPanelSlider(const char* label,
    double& value, const SettingsSliderPresentation& presentation) noexcept
{
    const auto& [defaultValue, minimum, maximum, displayScale, format, help, resetHelp] = presentation;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    const auto resetWidth = ImGui::CalcTextSize("Reset").x +
        ImGui::GetStyle().FramePadding.x * 2.0F;
    ImGui::SetNextItemWidth(std::max(1.0F,
        ImGui::GetContentRegionAvail().x - resetWidth -
            ImGui::GetStyle().ItemSpacing.x));
    float editable = static_cast<float>(value * displayScale);
    if (ImGui::SliderFloat("##value", &editable,
            static_cast<float>(minimum * displayScale),
            static_cast<float>(maximum * displayScale), format,
            ImGuiSliderFlags_AlwaysClamp))
        value = std::clamp(
            static_cast<double>(editable) / displayScale, minimum, maximum);
    auto changed = ImGui::IsItemDeactivatedAfterEdit();
    DrawSettingsSliderTooltip(help);
    ImGui::SameLine();
    ImGui::BeginDisabled(value == defaultValue);
    if (ImGui::Button("Reset"))
    {
        value = defaultValue;
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::BeginItemTooltip())
    {
        ImGui::TextUnformatted(resetHelp);
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return changed;
}

}  // namespace fo4hdr::detail
