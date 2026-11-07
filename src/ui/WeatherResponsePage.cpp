// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SettingsPages.hpp"

#include "SettingsStatus.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace fo4hdr {
namespace {

namespace ui = dmui::ui;

// One channel's name and element ids, so both channels share one table layout.
struct ChannelText
{
    const char* name;
    const char* advancedTable;
    const char* base;
    const char* gain;
    const char* minimum;
    const char* maximum;
    const char* intercept;
    std::array<const char*, 4> coefficients;
};

constexpr ChannelText kLuminanceIds{"Luminance", "advanced-luminance",
    "luminance-base", "luminance-gain", "luminance-minimum", "luminance-maximum",
    "luminance-intercept",
    {"luminance-sunlight", "luminance-glare", "luminance-ambient", "luminance-color-change"}};
constexpr ChannelText kColorIds{"Color", "advanced-color",
    "color-base", "color-gain", "color-minimum", "color-maximum", "color-intercept",
    {"color-sunlight", "color-glare", "color-ambient", "color-color-change"}};
constexpr std::array<const char*, 4> kCoefficientLabels{
    "Sunlight", "Glare", "Ambient", "Sunlight Color Change"};

void DrawBasicChannel(SettingsPage& page, const ChannelText& ids,
    WeatherGradeChannel& channel, const WeatherGradeChannel& defaults) noexcept
{
    page.commitRequested |= DrawSliderField(page, {ids.base, "Baseline",
        "Response at 0x Response Amount. 100% keeps the full vanilla response."},
        {0.0, 100.0, "%.1f%%"}, {channel.base, defaults.base});
    page.commitRequested |= DrawSliderField(page, {ids.gain, "Response Amount",
        "Scales the calibrated change. 0x uses Baseline; 1x uses the calibration."},
        {0.0, kMaximumWeatherGradeGain, "%.2fx"}, {channel.gain, defaults.gain});
    page.commitRequested |= DrawSliderField(page, {ids.minimum, "Minimum Retention",
        "The lowest response allowed."},
        {0.0, channel.maximum, "%.1f%%"}, {channel.minimum, defaults.minimum});
}

void DrawAdvancedChannel(SettingsPage& page, const ChannelText& ids,
    WeatherGradeChannel& channel, const WeatherGradeChannel& defaults) noexcept
{
    page.commitRequested |= DrawSliderField(page, {ids.maximum, "Maximum Retention",
        "The highest response allowed."},
        {channel.minimum, 100.0, "%.1f%%"}, {channel.maximum, defaults.maximum});
    page.commitRequested |= DrawSliderField(page, {ids.intercept, "Intercept",
        "The formula value before the weather inputs are added."},
        {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient, "%+.3f"},
        {channel.intercept, defaults.intercept});
    for (std::size_t index = 0; index < kCoefficientLabels.size(); ++index)
        page.commitRequested |= DrawSliderField(page, {ids.coefficients[index], kCoefficientLabels[index],
            "Weight of this averaged weather input. Negative values lower the response as it rises."},
            {-kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient, "%+.3f"},
            {channel.coefficients[index], defaults.coefficients[index]});
}

void DrawAdvancedTable(SettingsPage& page, const ChannelText& ids,
    WeatherGradeChannel& channel, const WeatherGradeChannel& defaults) noexcept
{
    DrawPageText(page, ids.name, dmui::TextTone::kAccentMuted);
    dmui::SettingsTableScope table{page.client, ids.advancedTable};
    if (table.Visible()) DrawAdvancedChannel(page, ids, channel, defaults);
}

template <std::size_t Size>
[[nodiscard]] std::string_view BoundedText(const std::array<char, Size>& text) noexcept
{
    const auto end = std::find(text.begin(), text.end(), '\0');
    if (end == text.end()) return {};
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

void DrawUnformatted(std::string_view text) noexcept
{
    ui::TextUnformatted(text.data(), text.data() + text.size());
}

void DrawContributorIdentity(const ImageSpaceIdentity& identity, std::uint32_t contexts) noexcept
{
    const auto key = BoundedText(identity.key);
    const auto editorId = BoundedText(identity.editorId);
    const auto name = editorId.empty() ? key : editorId;
    if (name.empty())
    {
        ui::TextDisabled("ImageSpace unavailable");
        return;
    }
    DrawUnformatted(name);
    ui::TextDisabled("%08X, %u weather slots", identity.formId, contexts);
}

[[nodiscard]] bool IsValidNativeWeight(float weight) noexcept
{
    return std::isfinite(weight) && weight >= 0.0F && weight <= 1.0F;
}

void DrawContributorResponse(const WeatherLutResponseView& view, std::size_t index) noexcept
{
    const auto* reason = view.fallbackReasons[index];
    if (view.gradingBypassed)
        ui::TextDisabled("Bypassed");
    else
    {
        const auto& response = view.resolved[index];
        if (std::isfinite(response.luminancePercent) && std::isfinite(response.colorPercent))
            ui::Text("L %.1f%% / C %.1f%%", response.luminancePercent, response.colorPercent);
        else
            ui::TextDisabled("Response unavailable");
        if ((view.automaticSlots & (1U << index)) != 0)
        {
            ui::TextDisabled("Automatic");
            return;
        }
        ui::TextDisabled("Global Grade");
    }
    if (reason != nullptr && reason[0] != '\0') DrawUnformatted(reason);
}

void DrawContributors(const WeatherLutResponseView& view) noexcept
{
    constexpr auto flags = ui::TableFlags::kRowBg | ui::TableFlags::kBordersInnerH |
        ui::TableFlags::kSizingStretchProp;
    if (!ui::BeginTable("weather-contributors", 3, flags)) return;
    ui::TableSetupColumn("ImageSpace");
    ui::TableSetupColumn("Weight");
    ui::TableSetupColumn("Response");
    ui::TableHeadersRow();
    for (std::size_t index = 0; index < view.currentLuts.size(); ++index)
    {
        ui::PushID(static_cast<std::int32_t>(index));
        ui::TableNextRow();
        (void)ui::TableNextColumn();
        DrawContributorIdentity(view.currentImageSpaces[index], view.referencingContexts[index]);
        (void)ui::TableNextColumn();
        const auto weight = view.currentLuts[index].weight;
        if (IsValidNativeWeight(weight))
            ui::Text("%.1f%%", static_cast<double>(weight) * 100.0);
        else
            ui::TextDisabled("Invalid");
        (void)ui::TableNextColumn();
        DrawContributorResponse(view, index);
        ui::PopID();
    }
    ui::EndTable();
}

// Like other DMUI status pages: a tinted value, then its messages by severity.
void DrawStatus(SettingsPage& page, bool active) noexcept
{
    const auto& view = page.view.weatherLutResponse;
    const auto scene = WeatherResponseStatus(view, active);
    const auto tone = scene.detail.severity != StatusSeverity::info ? dmui::TextTone::kWarning :
        scene.detail.text == nullptr ? dmui::TextTone::kSuccess : dmui::TextTone::kInherit;
    DrawSection(page, "status", "Status", [&] {
        dmui::FieldScope field{page.client, "current-scene", "Current Scene",
            "Whether the weather response reaches the current scene's color grading."};
        if (field.Visible()) (void)dmui::DrawStyledText(page.client, scene.value, {.tone = tone});
    });
    const std::array messages{scene.detail, view.status, view.imageSpaceStatus};
    DrawPageMessages(page, messages);
}

bool g_advancedExpanded{};
bool g_contributorsExpanded{};

}  // namespace

void DrawWeatherResponsePage(SettingsPage& page) noexcept
{
    auto& response = page.settings.weatherGradeResponse;
    const auto& defaults = page.defaults.weatherGradeResponse;
    const auto active = EffectiveWeatherGradeResponse(page.settings).enabled;
    DrawStatus(page, active);
    DrawSection(page, "weather-response", "Response", [&] {
        // While the SDR operator holds the response on, the switch shows on and cannot change.
        const auto forced = OperatorForcesWeatherResponse(page.settings);
        const dmui::DisabledScope held{forced};
        auto forcedOn = true;
        page.commitRequested |= DrawCheckboxField(page, {"weather-enabled", "Enabled",
            "Sets the grade's luminance and color response per weather ImageSpace. "
            "Other scenes use the Global Grade settings."},
            {forced ? forcedOn : response.enabled, forced || defaults.enabled,
                {dmui::FieldFeedbackSeverity::kInfo, forced ?
                    "On while Weather Response is on for the selected tone mapper." : nullptr}});
    });
    DrawSection(page, "luminance", "Luminance", [&] {
        DrawBasicChannel(page, kLuminanceIds, response.luminance, defaults.luminance);
    });
    DrawSection(page, "color", "Color", [&] {
        DrawBasicChannel(page, kColorIds, response.color, defaults.color);
    });
    DrawCollapsibleSection(page, {"advanced", "Advanced", g_advancedExpanded, 12}, [&] {
        DrawPageText(page, "Retention = Baseline + Response Amount x (formula - 100), "
            "kept between Minimum and Maximum Retention.");
        DrawAdvancedTable(page, kLuminanceIds, response.luminance, defaults.luminance);
        DrawAdvancedTable(page, kColorIds, response.color, defaults.color);
    });
    DrawCollapsibleSection(page, {"contributors", "Current Contributors", g_contributorsExpanded,
        page.view.weatherLutResponse.currentLuts.size()}, [&] {
            DrawContributors(page.view.weatherLutResponse);
        });
    if (!DrawRestoreDefaultsRow(page, response != defaults)) return;
    response = defaults;
    page.commitRequested = true;
}

}  // namespace fo4hdr
