// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SettingsPages.hpp"

#include <array>
#include <cstddef>

namespace fo4hdr {
namespace {

struct ImageControl
{
    FieldText text;
    double HdrSettings::* value;
    SliderRange range;
};

// Image adjustments only change shader constants, so they publish while dragging.
constexpr SliderRange kSignedAdjustment{
    kMinimumImageAdjustment, kMaximumImageAdjustment, "%+.0f", 1.0, true};

constexpr std::array kBrightnessControls{
    ImageControl{{"exposure", "Exposure", "Scene brightness before grading. +1 EV doubles it."},
        &HdrSettings::exposureEv,
        {kMinimumSceneExposureEv, kMaximumSceneExposureEv, "%+.2f EV", 1.0, true}},
    ImageControl{{"contrast", "Contrast", "Tonal separation around middle gray."},
        &HdrSettings::contrast, kSignedAdjustment},
    ImageControl{{"highlights", "Highlights", "Bright scene detail."},
        &HdrSettings::highlights, kSignedAdjustment},
    ImageControl{{"shadows", "Shadows", "Dark scene detail, over a wider range than Blacks."},
        &HdrSettings::shadows, kSignedAdjustment},
    ImageControl{{"blacks", "Blacks", "The darkest detail. True black stays black."},
        &HdrSettings::blacks, kSignedAdjustment}};

constexpr std::array kColorControls{
    ImageControl{{"saturation", "Saturation", "Color intensity. Luminance stays the same."},
        &HdrSettings::saturationPercent,
        {kMinimumSaturationPercent, kMaximumSaturationPercent, "%.0f%%", 1.0, true}},
    ImageControl{{"highlight-saturation", "Highlight Saturation",
        "Color intensity of bright highlights. Lower values fade them toward white sooner."},
        &HdrSettings::highlightSaturationPercent,
        {kMinimumSaturationPercent, kMaximumSaturationPercent, "%.0f%%", 1.0, true}},
    ImageControl{{"temperature", "Temperature", "White balance from cool to warm."},
        &HdrSettings::temperature, kSignedAdjustment},
    ImageControl{{"tint", "Tint", "White balance from green to magenta."},
        &HdrSettings::tint, kSignedAdjustment}};

constexpr ImageControl kBloomControl{{"bloom", "Bloom Strength",
    "0% removes bloom; 100% keeps the game's bloom."}, &HdrSettings::bloomStrengthPercent,
    {kMinimumBloomStrengthPercent, kMaximumBloomStrengthPercent, "%.0f%%", 1.0, true}};

constexpr std::array kGradingControls{
    ImageControl{{"grading-strength", "Grading Strength",
        "How much of the game's color grading applies. 0% removes it."},
        &HdrSettings::vanillaColorGradingStrengthPercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%", 1.0, true}},
    ImageControl{{"global-luminance", "Global Grade Luminance",
        "The grade's luminance response where Weather Response is off or does not apply."},
        &HdrSettings::vanillaGradeLuminancePercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%"}},
    ImageControl{{"global-color", "Global Grade Color",
        "The grade's hue and saturation response there. 0% keeps the original scene colors."},
        &HdrSettings::vanillaGradeColorPercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%"}}};

void DrawImageControl(SettingsPage& page, const ImageControl& control) noexcept
{
    page.commitRequested |= DrawSliderField(page, control.text, control.range,
        {page.settings.*control.value, page.defaults.*control.value});
}

template <std::size_t Count>
[[nodiscard]] bool Modified(const SettingsPage& page, const std::array<ImageControl, Count>& controls) noexcept
{
    for (const auto& control : controls)
        if (page.settings.*control.value != page.defaults.*control.value) return true;
    return false;
}

template <std::size_t Count>
void Restore(SettingsPage& page, const std::array<ImageControl, Count>& controls) noexcept
{
    for (const auto& control : controls)
        page.settings.*control.value = page.defaults.*control.value;
}

// Both lift the game's SDR LUTs into HDR, so SDR has neither.
void DrawHdrConversion(SettingsPage& page) noexcept
{
    using MethodChoice = dmui::ChoiceOption<ColorGradingMethod>;
    using CurveChoice = dmui::ChoiceOption<ColorGradingCurve>;
    static const std::array<MethodChoice, 2> methods{{
        {ColorGradingMethod::rgbRatio, "RGB Ratio"}, {ColorGradingMethod::affine, "Affine"}}};
    static const std::array<CurveChoice, 2> curves{{
        {ColorGradingCurve::gamma22, "Gamma 2.2"}, {ColorGradingCurve::srgb, "sRGB"}}};
    auto& settings = page.settings;
    const auto& view = page.view;
    const auto* affineStatus =
        view.activeSettings.colorGradingMethod == ColorGradingMethod::affine ? view.affineStatus : nullptr;
    DrawSection(page, "hdr-conversion", "HDR Conversion", [&] {
        page.commitRequested |= DrawChoiceField(page, {"grading-method", "Grading Method",
            "RGB Ratio keeps each color's channel ratios. Affine keeps lifted blacks constant."},
            {settings.colorGradingMethod, page.defaults.colorGradingMethod,
                {dmui::FieldFeedbackSeverity::kInfo, affineStatus}},
            std::span<const MethodChoice>{methods});
        page.commitRequested |= DrawChoiceField(page, {"grading-curve", "Grading Curve",
            "How the game's LUT colors are read. Gamma 2.2 keeps deeper shadows; sRGB lifts them."},
            {settings.colorGradingCurve, page.defaults.colorGradingCurve},
            std::span<const CurveChoice>{curves});
    });
}

constexpr std::array<const char*, kSelectiveColorRegionCount> kRegionNames{
    "Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Magenta"};

// One color's three adjustments and its Reset share a table row.
[[nodiscard]] bool DrawSelectiveRegion(std::size_t index, SelectiveColorRegionSettings& region,
    const SelectiveColorRegionSettings& defaults) noexcept
{
    namespace ui = dmui::ui;
    bool changed{};
    const auto edit = [&changed](const char* id, double& value, double minimum, double maximum,
                          const char* format) {
        (void)ui::TableNextColumn();
        ui::SetNextItemWidth(-1.0F);
        changed |= ui::DragScalar(id, &value, 0.1F, &minimum, &maximum, format,
            ui::SliderFlags::kAlwaysClamp);
    };
    ui::PushID(static_cast<std::int32_t>(index));
    ui::TableNextRow();
    (void)ui::TableNextColumn();
    ui::TextUnformatted(kRegionNames[index]);
    edit("##hue", region.hueDegrees, -10.0, 10.0, "%+.2f deg");
    edit("##saturation", region.saturationPercent, 50.0, 150.0, "%.0f%%");
    edit("##brightness", region.brightness, -100.0, 100.0, "%+.0f");
    (void)ui::TableNextColumn();
    {
        const dmui::DisabledScope unchanged{region == defaults};
        if (ui::Button("Reset"))
        {
            region = defaults;
            changed = true;
        }
    }
    ui::PopID();
    return changed;
}

[[nodiscard]] bool DrawSelectiveRegions(SelectiveColorSettings& selective,
    const SelectiveColorSettings& defaults) noexcept
{
    namespace ui = dmui::ui;
    using ui::TableColumnFlags;
    constexpr auto flags = ui::TableFlags::kRowBg | ui::TableFlags::kBordersInnerH |
        ui::TableFlags::kSizingStretchProp;
    if (!ui::BeginTable("selective-regions", 5, flags)) return false;
    ui::TableSetupColumn("Color", TableColumnFlags::kWidthFixed, ui::GetFontSize() * 5.0F);
    ui::TableSetupColumn("Hue", TableColumnFlags::kWidthStretch, 1.0F);
    ui::TableSetupColumn("Saturation", TableColumnFlags::kWidthStretch, 1.0F);
    ui::TableSetupColumn("Brightness", TableColumnFlags::kWidthStretch, 1.0F);
    ui::TableSetupColumn("##reset", TableColumnFlags::kWidthFixed, ui::GetFontSize() * 4.0F);
    ui::TableHeadersRow();
    bool changed{};
    for (std::size_t index = 0; index < selective.regions.size(); ++index)
        changed |= DrawSelectiveRegion(index, selective.regions[index], defaults.regions[index]);
    ui::EndTable();
    return changed;
}

}  // namespace

void DrawAdjustmentsPage(SettingsPage& page) noexcept
{
    DrawSection(page, "brightness-detail", "Brightness & Detail", [&] {
        for (const auto& control : kBrightnessControls) DrawImageControl(page, control);
    });
    DrawSection(page, "color", "Color", [&] {
        for (const auto& control : kColorControls) DrawImageControl(page, control);
    });
    DrawSection(page, "bloom", "Bloom", [&] { DrawImageControl(page, kBloomControl); });
    const std::array bloom{kBloomControl};
    const auto modified = Modified(page, kBrightnessControls) ||
        Modified(page, kColorControls) || Modified(page, bloom);
    if (!DrawRestoreDefaultsRow(page, modified)) return;
    Restore(page, kBrightnessControls);
    Restore(page, kColorControls);
    Restore(page, bloom);
    page.commitRequested = true;
}

void DrawColorGradingPage(SettingsPage& page) noexcept
{
    DrawSection(page, "grading-strength", "Strength", [&] {
        for (const auto& control : kGradingControls) DrawImageControl(page, control);
    });
    if (!page.sdr) DrawHdrConversion(page);
    auto& settings = page.settings;
    const auto& defaults = page.defaults;
    const auto modified = Modified(page, kGradingControls) ||
        settings.colorGradingMethod != defaults.colorGradingMethod ||
        settings.colorGradingCurve != defaults.colorGradingCurve;
    if (!DrawRestoreDefaultsRow(page, modified)) return;
    Restore(page, kGradingControls);
    settings.colorGradingMethod = defaults.colorGradingMethod;
    settings.colorGradingCurve = defaults.colorGradingCurve;
    page.commitRequested = true;
}

void DrawSelectiveColorPage(SettingsPage& page) noexcept
{
    auto& selective = page.settings.selectiveColor;
    const auto& defaults = page.defaults.selectiveColor;
    DrawSection(page, "selective-color", "Selective Color", [&] {
        page.commitRequested |= DrawCheckboxField(page, {"selective-enabled", "Enabled",
            "Applies the per-color adjustments below."}, {selective.enabled, defaults.enabled});
        const dmui::DisabledScope off{!selective.enabled};
        page.commitRequested |= DrawSliderField(page, {"selective-amount", "Amount",
            "Blends all color adjustments. 0% removes them."},
            {0.0, 100.0, "%.0f%%", 1.0, true}, {selective.amountPercent, defaults.amountPercent});
    });
    (void)page.client.DrawSectionHeader("Colors");
    DrawPageText(page, "Hue shifts by degrees. Saturation 100% and Brightness 0 leave a color unchanged.");
    {
        const dmui::DisabledScope off{!selective.enabled};
        page.commitRequested |= DrawSelectiveRegions(selective, defaults);
    }
    if (!DrawRestoreDefaultsRow(page, selective != defaults)) return;
    selective = defaults;
    page.commitRequested = true;
}

}  // namespace fo4hdr
