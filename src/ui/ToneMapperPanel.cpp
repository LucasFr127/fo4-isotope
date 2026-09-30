// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "ToneMapperPanel.hpp"
#include "LutProfilesPanel.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"

#include "FO4Isotope/config/HdrSettings.hpp"

#include "SettingsPanelSlider.hpp"

#include <imgui.h>
#include <array>
#include <cmath>

namespace fo4hdr {

namespace detail {

[[nodiscard]] bool DrawRestoreDefaultButton(
    const char* id, bool modified) noexcept
{
    ImGui::SameLine();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!modified);
    const auto clicked = ImGui::Button("Reset");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Restore this setting's default.");
    ImGui::PopID();
    return clicked;
}

void DrawSettingsControlLabel(std::string_view label) noexcept
{
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    const auto resetWidth = ImGui::CalcTextSize("Reset").x +
        ImGui::GetStyle().FramePadding.x * 2.0F;
    ImGui::SetNextItemWidth(std::max(1.0F,
        ImGui::GetContentRegionAvail().x - resetWidth -
            ImGui::GetStyle().ItemSpacing.x));
}

[[nodiscard]] bool DrawSettingsSlider(const char* label, float* value,
    const SettingsSliderRange& range) noexcept
{
    const auto& [minimum, maximum, format, help] = range;
    ImGui::PushID(label);
    DrawSettingsControlLabel(label);
    const auto changed = ImGui::SliderFloat("##value", value, minimum,
        maximum, format, ImGuiSliderFlags_AlwaysClamp);
    DrawSettingsSliderTooltip(help);
    ImGui::PopID();
    return changed;
}

}  // namespace detail

const char* SettingsOperatorDisplayName(ProductionToneMapperAlgorithm algorithm) noexcept
{
    return algorithm == ProductionToneMapperAlgorithm::aces2 ?
        "ACES 2" : ToString(algorithm).data();
}

namespace {

using detail::DrawRestoreDefaultButton;
using detail::DrawSettingsControlLabel;
using detail::DrawSettingsSlider;
using detail::SettingsSliderRange;

struct ColorSettingsTabs
{
    HdrSettings& settings;
    bool& commitRequested;
    const SettingsWindowView& view;
    float footerHeight;

    struct ImageSettingControl
    {
        const char* label;
        const char* resetId;
        double HdrSettings::* value;
        SettingsSliderRange range;
    };

    bool DrawImageSetting(const ImageSettingControl& control) noexcept
    {
        auto& value = settings.*control.value;
        const auto baseline = HdrSettings{}.*control.value;
        float editable = static_cast<float>(value);
        bool changed = DrawSettingsSlider(control.label, &editable, control.range);
        if (changed) value = editable;
        if (DrawRestoreDefaultButton(control.resetId, value != baseline))
        {
            value = baseline;
            changed = true;
        }
        return changed;
    }

    LutProfilesView CurrentLutProfilesView() noexcept { return view.lutProfiles; }

    void DrawNativeGradeProfiles() noexcept
    {
        if (DrawLutProfilesPanel(settings, CurrentLutProfilesView()))
            commitRequested = true;
    }

    int ToneMapperChoiceIndex(const std::array<ProductionToneMapperAlgorithm, 3>& choices,
        ProductionToneMapperAlgorithm algorithm) noexcept
    {
        const auto found = std::find(choices.begin(), choices.end(), algorithm);
        return found == choices.end() ? 0 : static_cast<int>(found - choices.begin());
    }

    void DrawOutputToneMapper() noexcept
    {
        const HdrSettings defaults{};
        constexpr std::array operatorAlgorithms{
            ProductionToneMapperAlgorithm::aces2,
            ProductionToneMapperAlgorithm::agX,
            ProductionToneMapperAlgorithm::gt7};
        const char* operators[]{
            "ACES 2", "AgX", "GT7"};
        int algorithm = ToneMapperChoiceIndex(operatorAlgorithms, settings.productionAlgorithm);
        DrawSettingsControlLabel("Tone Mapper");
        bool algorithmChanged = ImGui::Combo(
            "##tone-mapper", &algorithm, operators,
            static_cast<int>(std::size(operators)));
        ImGui::SetItemTooltip("Selects how scene colors and highlights fit the HDR display.");
        if (DrawRestoreDefaultButton("tone-mapper",
                settings.productionAlgorithm !=
                    defaults.productionAlgorithm))
        {
            algorithm = ToneMapperChoiceIndex(operatorAlgorithms, defaults.productionAlgorithm);
            algorithmChanged = true;
        }
        if (algorithmChanged)
        {
            settings.productionAlgorithm = operatorAlgorithms[
                static_cast<std::size_t>(algorithm)];
            commitRequested = true;
        }
        if (DrawToneMapperPanel(settings))
            commitRequested = true;
    }

    void DrawOutputPeak() noexcept
    {
        const HdrSettings defaults{};
        float peak = static_cast<float>(settings.displayPeakNits);
        if (DrawSettingsSlider("Display Peak Brightness", &peak, detail::kDisplayPeakSlider))
            settings.displayPeakNits = std::round(peak);
        if (ImGui::IsItemDeactivatedAfterEdit())
            commitRequested = true;
        if (DrawRestoreDefaultButton("display-peak",
                settings.displayPeakNits != defaults.displayPeakNits))
        {
            settings.displayPeakNits = defaults.displayPeakNits;
            commitRequested = true;
        }
    }

    void DrawOutputWhite() noexcept
    {
        if (DrawImageSetting({"Menu & HUD White", "ui-paper-white", &HdrSettings::uiPaperWhiteNits,
                detail::kUiWhiteSlider}))
            commitRequested = true;
        if (DrawImageSetting({"Video White", "video-white", &HdrSettings::videoWhiteNits,
                detail::kVideoWhiteSlider}))
            commitRequested = true;
    }

    void DrawSelectiveRegion(std::size_t index, SelectiveColorRegionSettings& region,
        const SelectiveColorRegionSettings& baseline, bool& changed) noexcept
    {
        constexpr const char* regionNames[]{"Red", "Orange",
            "Yellow", "Green", "Cyan", "Blue", "Magenta"};
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(regionNames[index]);
        const auto edit = [&changed](const char* id, double& value, SettingsSliderRange range) {
            const auto& [minimum, maximum, format, help] = range;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0F);
            float editable = static_cast<float>(value);
            if (ImGui::DragFloat(id, &editable, 0.1F, minimum,
                    maximum, format, ImGuiSliderFlags_AlwaysClamp))
            {
                value = editable;
                changed = true;
            }
        };
        edit("##hue", region.hueDegrees, {-10.0F, 10.0F, "%+.2f deg"});
        detail::DrawSettingsSliderTooltip("Shifts this color's hue in degrees. 0 keeps its hue unchanged.");
        edit("##saturation", region.saturationPercent, {50.0F, 150.0F, "%.0f%%"});
        detail::DrawSettingsSliderTooltip("Adjusts this color's saturation. 100% keeps it unchanged.");
        edit("##brightness", region.brightness, {-100.0F, 100.0F, "%+.0f"});
        detail::DrawSettingsSliderTooltip(
            "Adjusts this color's brightness by up to one stop. Gray stays unchanged.");
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(region == baseline);
        if (ImGui::Button("Reset"))
        {
            region = baseline;
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Restore all three defaults for this color.");
        ImGui::PopID();
    }

    void DrawSelectiveRegions(SelectiveColorSettings& selective,
        const SelectiveColorSettings& defaults, bool& changed) noexcept
    {
        const auto tableWidth = std::max(ImGui::GetContentRegionAvail().x,
            ImGui::GetFontSize() * 38.0F);
        if (ImGui::BeginTable("selective-regions", 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX,
                ImVec2(0.0F, ImGui::GetFrameHeightWithSpacing() * 9.0F), tableWidth))
        {
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed,
                ImGui::GetFontSize() * 5.0F);
            ImGui::TableSetupColumn("Hue", ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn("Saturation", ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn("Brightness", ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed,
                std::max(ImGui::CalcTextSize("Default").x,
                    ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0F));
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < selective.regions.size(); ++index)
            {
                DrawSelectiveRegion(index, selective.regions[index], defaults.regions[index], changed);
            }
            ImGui::EndTable();
        }
    }

    void DrawColorGradingMethod() noexcept
    {
        const HdrSettings defaults{};
        const char* gradingMethods[]{"Radial", "Native Reference"};
        int gradingMethod = settings.colorGradingMethod ==
            ColorGradingMethod::nativeReference ? 1 : 0;
        DrawSettingsControlLabel("Color Grading Method");
        bool methodChanged = ImGui::Combo("##color-grading-method", &gradingMethod,
            gradingMethods, static_cast<int>(std::size(gradingMethods)));
        ImGui::SetItemTooltip("Radial scales colors into the game's LUTs, then restores HDR intensity.\n"
            "Native Reference extends grading using sampled color, neutral and black responses.");
        if (DrawRestoreDefaultButton("color-grading-method",
                settings.colorGradingMethod != defaults.colorGradingMethod))
        {
            gradingMethod = 0;
            methodChanged = true;
        }
        if (methodChanged)
        {
            settings.colorGradingMethod = gradingMethod == 1 ?
                ColorGradingMethod::nativeReference : ColorGradingMethod::radial;
            commitRequested = true;
        }
        if (view.activeSettings.colorGradingMethod == ColorGradingMethod::nativeReference &&
            view.nativeReferenceStatus)
            ImGui::TextWrapped("%s", view.nativeReferenceStatus);
    }

    void DrawOutputSettingsTab() noexcept
    {
        if (ImGui::BeginTabItem("Output"))
        {
            ImGui::BeginChild("Output-content", ImVec2(0.0F, -footerHeight));
            ImGui::TextWrapped("Set display brightness, then choose the tone mapper and its look. "
                "Use Calibration to verify brightness with test patterns.");
            ImGui::SeparatorText("Display Brightness");
            DrawOutputPeak();
            DrawOutputWhite();
            ImGui::SeparatorText("Tone Mapping");
            DrawOutputToneMapper();
            ImGui::Separator();
            ImGui::Text("HDR10 output: %s",
                view.hdrActive ? "active" :
                    "inactive");
            if (view.lutPending)
                ImGui::Text("Requested: %s settings at %.0f nits",
                    SettingsOperatorDisplayName(
                        view.pendingAlgorithm),
                    view.pendingPeakNits);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }

    void DrawImageSettingsTab() noexcept
    {
        if (ImGui::BeginTabItem("Image"))
        {
            ImGui::BeginChild("Image-content", ImVec2(0.0F, -footerHeight));
            ImGui::TextWrapped("Adjust scene brightness, tonal detail and color. "
                "Menu, HUD and video brightness are set in Output.");
            ImGui::SeparatorText("Brightness & Detail");
            bool changed{};
            changed |= DrawImageSetting({"Exposure", "exposure", &HdrSettings::exposureEv,
                {-4.0F, 4.0F, "%.2f EV", "Adjusts scene brightness before color grading. "
                    "+1 EV doubles brightness; -1 EV halves it."}});
            changed |= DrawImageSetting({"Contrast", "contrast", &HdrSettings::contrast,
                {-100.0F, 100.0F, "%.0f",
                    "Adjusts tonal separation around middle gray. 0 keeps it unchanged."}});
            changed |= DrawImageSetting({"Highlights", "highlights", &HdrSettings::highlights,
                {-100.0F, 100.0F, "%.0f", "Adjusts bright scene detail. 0 keeps it unchanged."}});
            changed |= DrawImageSetting({"Shadows", "shadows", &HdrSettings::shadows,
                {-100.0F, 100.0F, "%.0f", "Adjusts dark scene detail across a wider range than Blacks. "
                    "0 keeps it unchanged."}});
            changed |= DrawImageSetting({"Blacks", "blacks", &HdrSettings::blacks,
                {-100.0F, 100.0F, "%.0f", "Adjusts the darkest scene detail while preserving true black. "
                    "0 keeps it unchanged."}});
            ImGui::SeparatorText("Color");
            changed |= DrawImageSetting({"Overall Saturation", "saturation", &HdrSettings::saturationPercent,
                {0.0F, 200.0F, "%.0f%%", "Adjusts scene color intensity while preserving luminance. "
                    "100% keeps it unchanged."}});
            changed |= DrawImageSetting({"Temperature", "temperature", &HdrSettings::temperature,
                {-100.0F, 100.0F, "%.0f",
                    "Adjusts scene white balance between cool and warm. 0 keeps it unchanged."}});
            changed |= DrawImageSetting({"Tint", "tint", &HdrSettings::tint,
                {-100.0F, 100.0F, "%.0f",
                    "Adjusts scene white balance between green and magenta. 0 keeps it unchanged."}});
            if (changed) commitRequested = true;

            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }

    void DrawSelectiveColorSettingsTab() noexcept
    {
        if (ImGui::BeginTabItem("Selective Color"))
        {
            ImGui::BeginChild("Selective Color-content", ImVec2(0.0F, -footerHeight));
            ImGui::TextWrapped("Adjust hue, saturation and brightness for individual colors. "
                "Drag a value or Ctrl+click to type an exact value.");
            ImGui::SeparatorText("Selective Color");
            const SelectiveColorSettings defaults{};
            auto& selective = settings.selectiveColor;
            bool changed = ImGui::Checkbox("Enable Selective Color",
                &selective.enabled);
            ImGui::SetItemTooltip("Applies the individual color adjustments at the selected Amount.");
            if (DrawRestoreDefaultButton("selective-enabled",
                    selective.enabled != defaults.enabled))
            {
                selective.enabled = defaults.enabled;
                changed = true;
            }
            ImGui::BeginDisabled(!selective.enabled);
            float amount = static_cast<float>(selective.amountPercent);
            if (DrawSettingsSlider("Amount", &amount, { 0.0F, 100.0F, "%.0f%%",
                    "Blends all individual color adjustments. 0% removes them; 100% applies them fully."}))
            {
                selective.amountPercent = amount;
                changed = true;
            }
            if (DrawRestoreDefaultButton("selective-amount",
                    selective.amountPercent != defaults.amountPercent))
            {
                selective.amountPercent = defaults.amountPercent;
                changed = true;
            }
            DrawSelectiveRegions(selective, defaults, changed);
            ImGui::EndDisabled();
            if (ImGui::Button("Reset Selective Color"))
            {
                selective = defaults;
                changed = true;
            }
            ImGui::SetItemTooltip("Restore all Selective Color defaults, including Enable and Amount.");
            if (changed) commitRequested = true;
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }

    void DrawEffectsSettingsTab() noexcept
    {
        if (ImGui::BeginTabItem("Effects"))
        {
            ImGui::BeginChild("Effects-content", ImVec2(0.0F, -footerHeight));
            ImGui::TextWrapped("Adjust bloom and the game's color grading. "
                "Grade profiles override luminance and color for individual LUTs or ImageSpaces; "
                "the overall grading strength stays global.");
            ImGui::SeparatorText("Bloom");
            if (DrawImageSetting({"Bloom Strength", "bloom-strength", &HdrSettings::bloomStrengthPercent,
                    {0.0F, 200.0F, "%.0f%%", "Adjusts the bloom contribution. "
                        "0% removes bloom; 100% keeps its original strength."}}))
                commitRequested = true;
            ImGui::SeparatorText("Color Grading");
            if (DrawImageSetting({"Vanilla Color Grading Strength", "vanilla-color-grading",
                    &HdrSettings::vanillaColorGradingStrengthPercent, {0.0F, 100.0F, "%.0f%%",
                    "Blends the game's color grading. 0% removes it; "
                    "100% applies it fully, including grade response adjustments."}}))
                commitRequested = true;
            DrawColorGradingMethod();
            if (DrawVanillaGradePanel(settings))
                commitRequested = true;
            if (ImGui::CollapsingHeader("Grade Profiles"))
                DrawNativeGradeProfiles();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }
};

}  // namespace

void DrawColorSettingsTabs(HdrSettings& settings, bool& commitRequested,
    const SettingsWindowView& view, float footerHeight) noexcept
{
    ColorSettingsTabs panel{settings, commitRequested, view, footerHeight};
    panel.DrawOutputSettingsTab();
    panel.DrawImageSettingsTab();
    panel.DrawEffectsSettingsTab();
    panel.DrawSelectiveColorSettingsTab();
}


[[nodiscard]] bool DrawToneMapperPanel(HdrSettings& settings) noexcept
{
    const ToneMapperParameters defaults{};
    auto& parameters = settings.toneMapperParameters;
    switch (settings.productionAlgorithm)
    {
    case ProductionToneMapperAlgorithm::aces2:
        return detail::DrawSettingsPanelSlider(
            "Contrast Look Strength", parameters.acesLookStrength, {
            defaults.acesLookStrength,
            kMinimumAcesLookStrength, kMaximumAcesLookStrength,
            100.0, "%.0f%%",
            "Strength of the added contrast look before the ACES output "
            "transform. 0% removes the look; 100% keeps the original look."});
    case ProductionToneMapperAlgorithm::agX:
    {
        bool changed{};
        changed |= detail::DrawSettingsPanelSlider(
            "Middle Gray", parameters.agxMiddleGrayEv, {
            defaults.agxMiddleGrayEv,
            kMinimumAgxMiddleGrayEv, kMaximumAgxMiddleGrayEv,
            1.0, "%.2f EV",
            "Adjusts AgX middle-gray brightness while keeping display peak fixed."});
        changed |= detail::DrawSettingsPanelSlider(
            "Contrast", parameters.agxContrast, { defaults.agxContrast,
            kMinimumAgxContrast, kMaximumAgxContrast,
            1.0, "%.2f", "Controls AgX sigmoid contrast."});
        changed |= detail::DrawSettingsPanelSlider(
            "Highlight Shoulder", parameters.agxHdrShoulderPower, {
            defaults.agxHdrShoulderPower,
            kMinimumAgxHdrShoulderPower, kMaximumAgxHdrShoulderPower,
            1.0, "%.2fx", "Adjusts AgX highlight compression near the display peak."});
        changed |= detail::DrawSettingsPanelSlider(
            "HDR Color Response", parameters.agxHdrPurity, {
            defaults.agxHdrPurity,
            kMinimumAgxHdrPurity, kMaximumAgxHdrPurity,
            1.0, "%.2f",
            "Balances hue and saturation before and after AgX's HDR remapping. "
            "Higher values favor the remapped response and can also shift "
            "scene brightness."});
        return changed;
    }
    case ProductionToneMapperAlgorithm::gt7:
    {
        bool changed{};
        changed |= detail::DrawSettingsPanelSlider(
            "Shadow Toe", parameters.gt7ToeStrength, {
            defaults.gt7ToeStrength,
            kMinimumGt7ToeStrength, kMaximumGt7ToeStrength,
            1.0, "%.2f", "Adjusts GT7 shadow compression. Higher values deepen shadows."});
        changed |= detail::DrawSettingsPanelSlider(
            "Shoulder Start", parameters.gt7ShoulderStart, {
            defaults.gt7ShoulderStart,
            kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart,
            100.0, "%.1f%%",
            "Sets where the GT7 highlight shoulder begins as a fraction of display peak."});
        changed |= detail::DrawSettingsPanelSlider(
            "Color Blend", parameters.gt7ColorBlend, {
            defaults.gt7ColorBlend,
            kMinimumGt7ColorBlend, kMaximumGt7ColorBlend,
            1.0, "%.2f",
            "Controls hue and saturation handling in highlights. 0 uses "
            "per-channel RGB curves; higher values favor the ICtCp color treatment."});
        return changed;
    }
    }
    return false;
}

[[nodiscard]] bool DrawVanillaGradePanel(HdrSettings& settings) noexcept
{
    const HdrSettings defaults{};
    bool changed{};
    changed |= detail::DrawSettingsPanelSlider(
        "Vanilla Grade Luminance",
        settings.vanillaGradeLuminancePercent, {
        defaults.vanillaGradeLuminancePercent,
        kMinimumVanillaColorGradingPercent,
        kMaximumVanillaColorGradingPercent,
        1.0, "%.0f%%",
        "Controls the vanilla grade's luminance response before Image "
        "controls. 0% removes that response; 100% keeps it."});
    changed |= detail::DrawSettingsPanelSlider(
        "Vanilla Grade Color",
        settings.vanillaGradeColorPercent, {
        defaults.vanillaGradeColorPercent,
        kMinimumVanillaColorGradingPercent,
        kMaximumVanillaColorGradingPercent,
        1.0, "%.0f%%",
        "Controls the vanilla grade's hue and saturation response before "
        "Image controls. 0% retains the original scene colors rather than "
        "making them gray; 100% keeps the full response."});
    return changed;
}

}  // namespace fo4hdr
