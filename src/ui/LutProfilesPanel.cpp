// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "LutProfilesPanel.hpp"

#include "SettingsPanelSlider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fo4hdr {
namespace {

template <std::size_t Size>
[[nodiscard]] std::string_view BoundedText(
    const std::array<char, Size>& text) noexcept
{
    const auto end = std::find(text.begin(), text.end(), '\0');
    if (end == text.end()) return {};
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

void DrawResponseStatus(const char* status) noexcept
{
    if (status == nullptr || status[0] == '\0') return;
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(status);
    ImGui::PopTextWrapPos();
}

[[nodiscard]] bool DrawBasicChannel(
    const char* name, WeatherGradeChannel& channel,
    const WeatherGradeChannel& defaults) noexcept
{
    ImGui::PushID(name);
    ImGui::SeparatorText(name);
    bool changed = detail::DrawSettingsPanelSlider("Baseline", channel.base, {
        defaults.base, 0.0, 100.0, 1.0, "%.1f%%",
        "Retention when response amount is zero. 100% keeps the full native LUT response."});
    changed |= detail::DrawSettingsPanelSlider("Response Amount", channel.gain, {
        defaults.gain, 0.0, kMaximumWeatherGradeGain, 1.0, "%.2fx",
        "Scales the formula's change from full retention. Zero uses the baseline; one uses the calibrated response."});
    changed |= detail::DrawSettingsPanelSlider("Minimum Retention", channel.minimum, {
        defaults.minimum, 0.0, channel.maximum, 1.0, "%.1f%%",
        "Lower bound applied after the formula, baseline and response amount."});
    ImGui::PopID();
    return changed;
}

[[nodiscard]] bool DrawAdvancedChannel(
    const char* name, WeatherGradeChannel& channel,
    const WeatherGradeChannel& defaults) noexcept
{
    ImGui::PushID(name);
    ImGui::SeparatorText(name);
    bool changed = detail::DrawSettingsPanelSlider("Maximum Retention", channel.maximum, {
        defaults.maximum, channel.minimum, 100.0, 1.0, "%.1f%%",
        "Upper bound applied after the formula, baseline and response amount."});
    changed |= detail::DrawSettingsPanelSlider("Intercept", channel.intercept, {
        defaults.intercept, -kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient,
        1.0, "%.3f", "Formula value before adding the four weighted weather inputs."});
    constexpr std::array labels{
        "Sunlight Sensitivity", "Glare Sensitivity",
        "Ambient Sensitivity", "Sunlight Color Change Sensitivity"};
    for (std::size_t index = 0; index < labels.size(); ++index)
        changed |= detail::DrawSettingsPanelSlider(labels[index], channel.coefficients[index], {
            defaults.coefficients[index], -kMaximumWeatherGradeCoefficient,
            kMaximumWeatherGradeCoefficient, 1.0, "%.3f",
            "Signed weight for this averaged weather input. Negative weights reduce retention as the input increases."});
    ImGui::PopID();
    return changed;
}

void DrawQualification(const WeatherLutResponseView& view, bool enabled) noexcept
{
    if (view.gradingBypassed)
    {
        ImGui::TextWrapped("Production grading bypassed: LUT response adjustments are not applied.");
        return;
    }
    if (!enabled)
        ImGui::TextWrapped("Disabled: global grade controls apply. Automatic response settings remain editable.");
    else if (view.validatedSlots == 0)
        ImGui::TextWrapped("Waiting for a qualified native LUT draw. Global grade controls apply.");
    else if (view.automaticSlots == 0)
        ImGui::TextWrapped("Global fallback: no complete qualified weather response for this draw.");
    else if ((view.automaticSlots & view.validatedSlots) != view.validatedSlots)
        ImGui::TextWrapped("Partial response: some contributors use global fallback.");
    else
        ImGui::TextUnformatted("Automatic response active.");
    DrawResponseStatus(view.status);
    DrawResponseStatus(view.imageSpaceStatus);
}
void DrawContributorIdentity(const ImageSpaceIdentity& identity) noexcept
{
    const auto key = BoundedText(identity.key);
    const auto editorId = BoundedText(identity.editorId);
    const auto name = editorId.empty() ? key : editorId;
    if (name.empty())
    {
        ImGui::TextDisabled("ImageSpace unavailable");
        return;
    }
    ImGui::TextUnformatted(name.data(), name.data() + name.size());
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0F);
        if (!key.empty()) ImGui::TextUnformatted(key.data(), key.data() + key.size());
        ImGui::Text("Form ID: %08X", identity.formId);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void DrawResolvedResponse(const ResolvedLutGrade& response) noexcept
{
    if (std::isfinite(response.luminancePercent) &&
        std::isfinite(response.colorPercent))
        ImGui::Text("L %.1f%% / C %.1f%%",
            response.luminancePercent, response.colorPercent);
    else
        ImGui::TextDisabled("Response unavailable");
}

[[nodiscard]] bool IsValidNativeWeight(float weight) noexcept
{
    return std::isfinite(weight) && weight >= 0.0F && weight <= 1.0F;
}
void DrawContributorResponse(const WeatherLutResponseView& view, std::size_t index) noexcept
{
    if (view.gradingBypassed)
    {
        ImGui::TextDisabled("Production grading bypassed");
        DrawResponseStatus(view.fallbackReasons[index]);
        return;
    }
    DrawResolvedResponse(view.resolved[index]);
    if ((view.automaticSlots & (1U << index)) != 0)
        ImGui::TextUnformatted("Automatic");
    else
    {
        ImGui::TextUnformatted("Global");
        DrawResponseStatus(view.fallbackReasons[index]);
    }
}
void DrawContributorRow(
    const WeatherLutResponseView& view, std::size_t index, bool wide) noexcept
{
    ImGui::PushID(static_cast<int>(index));
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawContributorIdentity(view.currentImageSpaces[index]);
    ImGui::TextDisabled("%u referencing weather slots", view.referencingContexts[index]);
    if (wide) ImGui::TableSetColumnIndex(1);
    const auto weight = view.currentLuts[index].weight;
    if (IsValidNativeWeight(weight))
        ImGui::Text("Weight %.1f%%", static_cast<double>(weight) * 100.0);
    else
        ImGui::TextDisabled("Invalid native weight");
    ImGui::TableSetColumnIndex(wide ? 2 : 1);
    DrawContributorResponse(view, index);
    ImGui::PopID();
}

void DrawContributors(const WeatherLutResponseView& view) noexcept
{
    const bool wide = ImGui::GetContentRegionAvail().x >=
        ImGui::CalcTextSize("ImageSpace native weight resolved response").x +
            ImGui::GetStyle().CellPadding.x * 6.0F;
    if (ImGui::BeginTable("##weather-response-contributors", wide ? 3 : 2,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("ImageSpace");
        if (wide) ImGui::TableSetupColumn("Native Weight");
        ImGui::TableSetupColumn("Resolved Response");
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < view.currentLuts.size(); ++index)
            DrawContributorRow(view, index, wide);
        ImGui::EndTable();
    }
}

}  // namespace

bool DrawWeatherLutResponsePanel(
    HdrSettings& settings, const WeatherLutResponseView& view) noexcept
{
    auto& response = settings.weatherGradeResponse;
    const WeatherGradeResponseSettings defaults{};
    bool changed = ImGui::Checkbox("Enable Automatic Weather Response", &response.enabled);
    ImGui::SetItemTooltip("Uses one fixed response per ImageSpace, averaged from all referencing weather slots.\n"
        "Unqualified and non-weather sources use the global grade controls.");
    if (detail::DrawRestoreDefaultButton("weather-response-enabled", response.enabled != defaults.enabled))
    {
        response.enabled = defaults.enabled;
        changed = true;
    }
    ImGui::TextWrapped("Each ImageSpace has one fixed response from all referencing weather slots. "
        "Native LUT weights blend these responses during weather transitions.");
    DrawQualification(view, response.enabled);
    changed |= DrawBasicChannel("Luminance", response.luminance, defaults.luminance);
    changed |= DrawBasicChannel("Color", response.color, defaults.color);
    if (ImGui::CollapsingHeader("Advanced Formula"))
    {
        ImGui::TextWrapped("Retention = baseline + response amount x (formula - 100), "
            "bounded by minimum and maximum retention.");
        changed |= DrawAdvancedChannel("Luminance", response.luminance, defaults.luminance);
        changed |= DrawAdvancedChannel("Color", response.color, defaults.color);
    }
    if (ImGui::CollapsingHeader("Current Contributors"))
    {
        DrawContributors(view);
    }
    return changed;
}

}  // namespace fo4hdr
