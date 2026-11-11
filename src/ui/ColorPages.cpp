// SPDX-License-Identifier: GPL-3.0-only
// Compiles the GPL-3.0-only DearModdingUI API headers; the CommonLib exceptions do not apply.
#include "SettingsPages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace fo4hdr {
namespace {

[[nodiscard]] HdrSettings& Draft() noexcept { return *CurrentSettingsFrame().draft; }

struct ImageControl
{
    FieldText text;
    double HdrSettings::* value;
    SliderRange range;
};

// Image adjustments only change shader constants, so they publish while dragging.
constexpr SliderRange kSignedAdjustment{
    kMinimumImageAdjustment, kMaximumImageAdjustment, "%+.0f", 1.0, Publish::whileDragging};
constexpr SliderRange kSaturationRange{
    kMinimumSaturationPercent, kMaximumSaturationPercent, "%.0f%%", 1.0, Publish::whileDragging};

constexpr std::array kBrightnessControls{
    ImageControl{{"exposure", "Exposure", "Scene brightness before grading. +1 EV doubles it."},
        &HdrSettings::exposureEv,
        {kMinimumSceneExposureEv, kMaximumSceneExposureEv, "%+.2f EV", 1.0, Publish::whileDragging}},
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
        &HdrSettings::saturationPercent, kSaturationRange},
    ImageControl{{"highlight-saturation", "Highlight Saturation",
        "Color intensity of bright scene detail. Luminance stays the same."},
        &HdrSettings::highlightSaturationPercent, kSaturationRange},
    ImageControl{{"temperature", "Temperature", "White balance from cool to warm."},
        &HdrSettings::temperature, kSignedAdjustment},
    ImageControl{{"tint", "Tint", "White balance from green to magenta."},
        &HdrSettings::tint, kSignedAdjustment}};

constexpr std::array kBloomControls{
    ImageControl{{"bloom", "Bloom Strength", "0% removes bloom; 100% keeps the game's bloom."},
        &HdrSettings::bloomStrengthPercent,
        {kMinimumBloomStrengthPercent, kMaximumBloomStrengthPercent, "%.0f%%", 1.0,
            Publish::whileDragging}}};

constexpr std::array kGradingControls{
    ImageControl{{"grading-strength", "Grading Strength",
        "How much of the game's color grading applies. 0% removes it."},
        &HdrSettings::vanillaColorGradingStrengthPercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%", 1.0,
            Publish::whileDragging}},
    ImageControl{{"fallback-grade-luminance", "Fallback Grade Luminance",
        "The grade's luminance response where Weather Response is off or does not apply."},
        &HdrSettings::fallbackGradeLuminancePercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%"}},
    ImageControl{{"fallback-grade-color", "Fallback Grade Color",
        "The grade's hue and saturation response there. 0% keeps the original scene colors."},
        &HdrSettings::fallbackGradeColorPercent,
        {kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent, "%.0f%%"}}};

template <std::size_t Count>
void AddImageControls(SettingsPageBuilder& page, const std::array<ImageControl, Count>& controls)
{
    for (const auto& control : controls)
        (void)page.Number(control.text, control.range,
            [value = control.value](HdrSettings& settings) -> double& { return settings.*value; });
}

constexpr std::array<ChoiceEntry<ColorGradingMethod>, 2> kGradingMethods{{
    {ColorGradingMethod::rgbRatio, "rgb-ratio", "RGB Ratio"},
    {ColorGradingMethod::affine, "affine", "Affine"}}};
constexpr std::array<ChoiceEntry<ColorGradingCurve>, 2> kGradingCurves{{
    {ColorGradingCurve::gamma22, "gamma22", "Gamma 2.2"},
    {ColorGradingCurve::srgb, "srgb", "sRGB"}}};

// Hosted HDR grades with RGB Ratio only, so the method row stays visible but disabled.
[[nodiscard]] bool OwnsGradingMethod() noexcept { return !CurrentSettingsFrame().view->hostedHdr; }

[[nodiscard]] std::optional<dmui::FieldFeedback> GradingMethodFeedback()
{
    const auto& view = *CurrentSettingsFrame().view;
    if (view.hostedHdr)
        return dmui::FieldFeedback{dmui::FieldFeedbackSeverity::kInfo,
            "Hosted Presentation uses RGB Ratio."};
    if (view.activeSettings.colorGradingMethod != ColorGradingMethod::affine ||
        view.affineStatus == nullptr)
        return std::nullopt;
    return dmui::FieldFeedback{dmui::FieldFeedbackSeverity::kInfo, view.affineStatus};
}

// Both lift the game's SDR LUTs into HDR, so SDR has neither.
void AddHdrConversion(SettingsPageBuilder& page)
{
    page.Group("hdr-conversion", "HDR Conversion", "arrows-out");
    auto& method = page.Choice<ColorGradingMethod>({"grading-method", "Grading Method",
        "RGB Ratio reads the LUT at each color's own hue and saturation.\n"
        "Affine reads it as the game does for closer vanilla tints, at a higher GPU cost."},
        [](HdrSettings& settings) -> ColorGradingMethod& { return settings.colorGradingMethod; },
        std::span<const ChoiceEntry<ColorGradingMethod>>{kGradingMethods});
    method.resolveFeedback = GradingMethodFeedback;
    method.isEnabled = OwnsGradingMethod;
    (void)page.Choice<ColorGradingCurve>({"grading-curve", "Grading Curve",
        "How the game's LUT colors are read. Gamma 2.2 keeps deeper shadows; sRGB lifts them."},
        [](HdrSettings& settings) -> ColorGradingCurve& { return settings.colorGradingCurve; },
        std::span<const ChoiceEntry<ColorGradingCurve>>{kGradingCurves});
}

constexpr std::array<const char*, kSelectiveColorRegionCount> kRegionNames{
    "Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Magenta"};
constexpr std::array<const char*, kSelectiveColorRegionCount> kRegionIds{
    "red", "orange", "yellow", "green", "cyan", "blue", "magenta"};

struct RegionControl
{
    const char* suffix;
    const char* label;
    const char* description;
    double SelectiveColorRegionSettings::* value;
    SliderRange range;
};

constexpr std::array kRegionControls{
    RegionControl{"hue", "Hue", "Shifts this color's hue in degrees. 0 leaves it unchanged.",
        &SelectiveColorRegionSettings::hueDegrees,
        {-10.0, 10.0, "%+.2f deg", 1.0, Publish::whileDragging}},
    RegionControl{"saturation", "Saturation", "This color's saturation. 100% leaves it unchanged.",
        &SelectiveColorRegionSettings::saturationPercent,
        {50.0, 150.0, "%.0f%%", 1.0, Publish::whileDragging}},
    RegionControl{"brightness", "Brightness",
        "This color's brightness, by up to one stop. Gray stays unchanged.",
        &SelectiveColorRegionSettings::brightness,
        {-100.0, 100.0, "%+.0f", 1.0, Publish::whileDragging}}};

[[nodiscard]] bool SelectiveColorEnabled() noexcept { return Draft().selectiveColor.enabled; }

// The table edits the colors; these hidden rows let the header's Reset all and
// Revert cover them like every other setting.
void AddRegionSettings(SettingsPageBuilder& page)
{
    page.Group("color-settings", "Colors", "drop");
    for (std::size_t region = 0; region < kSelectiveColorRegionCount; ++region)
        for (const auto& control : kRegionControls)
        {
            std::array<char, 32> id{};
            std::snprintf(id.data(), id.size(), "%s-%s", kRegionIds[region], control.suffix);
            page.Number({id.data(), control.label, control.description}, control.range,
                [region, value = control.value](HdrSettings& settings) -> double& {
                    return settings.selectiveColor.regions[region].*value;
                }).isVisible = [] { return false; };
        }
}

bool g_colorsExpanded{true};

[[nodiscard]] bool DrawRegionRow(std::size_t index, SelectiveColorRegionSettings& region,
    const SelectiveColorRegionSettings& defaults) noexcept
{
    namespace ui = dmui::ui;
    bool changed{};
    ui::PushID(static_cast<std::int32_t>(index));
    ui::TableNextRow();
    (void)ui::TableNextColumn();
    ui::TextUnformatted(kRegionNames[index]);
    for (const auto& control : kRegionControls)
    {
        (void)ui::TableNextColumn();
        ui::SetNextItemWidth(-1.0F);
        const auto& range = control.range;
        ui::PushID(control.suffix);
        changed |= ui::DragScalar("##value", &(region.*control.value), 0.1F, &range.minimum,
            &range.maximum, range.format, ui::SliderFlags::kAlwaysClamp);
        ui::PopID();
    }
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

// The seven colors in one table, so their adjustments compare at a glance.
void DrawRegionTable(const SelectiveColorSettings& defaults, char32_t glyph) noexcept
{
    namespace ui = dmui::ui;
    using ui::TableColumnFlags;
    auto& client = *CurrentSettingsFrame().client;
    auto& selective = Draft().selectiveColor;
    if (!client.DrawCollapsingSectionHeader("colors", "Colors", glyph, g_colorsExpanded,
            kSelectiveColorRegionCount) || !g_colorsExpanded)
        return;
    (void)dmui::DrawStyledText(client, "Hue shifts by degrees. Saturation 100% and Brightness 0 "
        "leave a color unchanged.", {.tone = dmui::TextTone::kMuted, .wrapped = true});
    const dmui::DisabledScope off{!selective.enabled};
    constexpr auto flags = ui::TableFlags::kRowBg | ui::TableFlags::kBordersInnerH |
        ui::TableFlags::kSizingStretchProp;
    if (!ui::BeginTable("selective-regions", 5, flags)) return;
    ui::TableSetupColumn("Color", TableColumnFlags::kWidthFixed, ui::GetFontSize() * 5.0F);
    for (const auto& control : kRegionControls)
        ui::TableSetupColumn(control.label, TableColumnFlags::kWidthStretch, 1.0F);
    ui::TableSetupColumn("##reset", TableColumnFlags::kWidthFixed, ui::GetFontSize() * 4.0F);
    ui::TableHeadersRow();
    bool changed{};
    for (std::size_t index = 0; index < selective.regions.size(); ++index)
        changed |= DrawRegionRow(index, selective.regions[index], defaults.regions[index]);
    ui::EndTable();
    if (changed) RequestSettingsCommit();
}

}  // namespace

void BuildAdjustmentsPage(SettingsPageBuilder& page)
{
    page.Status();
    page.Group("brightness-detail", "Brightness & Detail", "sun");
    AddImageControls(page, kBrightnessControls);
    page.Group("color", "Color", "palette");
    AddImageControls(page, kColorControls);
    page.Group("bloom", "Bloom", "sparkle");
    AddImageControls(page, kBloomControls);
}

void BuildColorGradingPage(SettingsPageBuilder& page)
{
    page.Status();
    page.Group("grading-strength", "Strength", "gauge");
    AddImageControls(page, kGradingControls);
    if (!page.Sdr()) AddHdrConversion(page);
}

void BuildSelectiveColorPage(SettingsPageBuilder& page)
{
    page.Status();
    page.Group("selective-color", "Selective Color", "swatches");
    (void)page.Toggle({"selective-enabled", "Enabled", "Applies the per-color adjustments below."},
        [](HdrSettings& settings) -> bool& { return settings.selectiveColor.enabled; });
    page.Number({"selective-amount", "Amount", "Blends all color adjustments. 0% removes them."},
        {0.0, 100.0, "%.0f%%", 1.0, Publish::whileDragging},
        [](HdrSettings& settings) -> double& { return settings.selectiveColor.amountPercent; })
        .isEnabled = SelectiveColorEnabled;
    AddRegionSettings(page);
    // Rows disabled while Selective Color is off skip the host's Reset all, so reset them here.
    page.AlsoOnReset([defaults = page.Defaults().selectiveColor] {
        Draft().selectiveColor = defaults;
    });
    const auto glyph = CurrentSettingsFrame().client->ResolveIconGlyph("Colors", "drop").value_or(0);
    page.DrawAfterRows([defaults = page.Defaults().selectiveColor, glyph] {
        DrawRegionTable(defaults, glyph);
    });
}

}  // namespace fo4hdr
