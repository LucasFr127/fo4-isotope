// SPDX-License-Identifier: GPL-3.0-only
// Compiles the GPL-3.0-only DearModdingUI API headers; the CommonLib exceptions do not apply.
#include "SettingsPages.hpp"

#include "SettingsStatus.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>

namespace fo4hdr {
namespace {

namespace ui = dmui::ui;

[[nodiscard]] HdrSettings& Draft() noexcept { return *CurrentSettingsFrame().draft; }
[[nodiscard]] const WeatherLutResponseView& Response() noexcept
{
    return CurrentSettingsFrame().view->weatherLutResponse;
}

using ChannelField = WeatherGradeChannel& (*)(HdrSettings&);

// One channel's names and row ids, so both channels share one layout.
struct ChannelText
{
    const char* group;
    const char* name;
    const char* icon;
    const char* advancedGroup;
    const char* advancedLabel;
    const char* base;
    const char* gain;
    const char* minimum;
    const char* maximum;
    const char* intercept;
    std::array<const char*, 4> coefficients;
    ChannelField channel;
};

constexpr ChannelText kLuminance{"luminance", "Luminance", "sun", "advanced-luminance", "Advanced Luminance",
    "luminance-base", "luminance-gain", "luminance-minimum", "luminance-maximum",
    "luminance-intercept",
    {"luminance-sunlight", "luminance-glare", "luminance-ambient", "luminance-color-change"},
    [](HdrSettings& settings) -> WeatherGradeChannel& {
        return settings.weatherGradeResponse.luminance;
    }};
constexpr ChannelText kColor{"color", "Color", "palette", "advanced-color", "Advanced Color",
    "color-base", "color-gain", "color-minimum", "color-maximum", "color-intercept",
    {"color-sunlight", "color-glare", "color-ambient", "color-color-change"},
    [](HdrSettings& settings) -> WeatherGradeChannel& {
        return settings.weatherGradeResponse.color;
    }};
constexpr std::array<const char*, 4> kCoefficientLabels{
    "Sunlight", "Glare", "Ambient", "Sunlight Color Change"};
constexpr SliderRange kCoefficientRange{
    -kMaximumWeatherGradeCoefficient, kMaximumWeatherGradeCoefficient, "%+.3f"};

[[nodiscard]] SettingField<double> ChannelValue(ChannelField channel, double WeatherGradeChannel::* value)
{
    return [channel, value](HdrSettings& settings) -> double& { return channel(settings).*value; };
}

// Minimum Retention never passes Maximum Retention, and the reverse.
void Bound(dmui::SettingDescriptor& setting, ChannelField channel, bool minimum)
{
    setting.binding = dmui::BindSetting(
        [channel, minimum] {
            const auto& value = channel(Draft());
            return minimum ? value.minimum : value.maximum;
        },
        [channel, minimum](double requested) {
            auto& value = channel(Draft());
            return minimum ? value.minimum = std::min(requested, value.maximum) :
                value.maximum = std::max(requested, value.minimum);
        });
}

void AddChannel(SettingsPageBuilder& page, const ChannelText& text)
{
    page.Group(text.group, text.name, text.icon);
    (void)page.Number({text.base, "Baseline",
        "Response at 0x Response Amount. 100% keeps the full vanilla response."},
        {0.0, 100.0, "%.1f%%"}, ChannelValue(text.channel, &WeatherGradeChannel::base));
    (void)page.Number({text.gain, "Response Amount",
        "Scales the calibrated change. 0x uses Baseline; 1x uses the calibration."},
        {0.0, kMaximumWeatherGradeGain, "%.2fx"}, ChannelValue(text.channel, &WeatherGradeChannel::gain));
    Bound(page.Number({text.minimum, "Minimum Retention", "The lowest response allowed."},
        {0.0, 100.0, "%.1f%%"}, ChannelValue(text.channel, &WeatherGradeChannel::minimum)),
        text.channel, true);
}

void AddAdvancedChannel(SettingsPageBuilder& page, const ChannelText& text, bool explain)
{
    page.Group(text.advancedGroup, text.advancedLabel, "wrench", false);
    if (explain)
        page.Note("formula", "Retention = Baseline + Response Amount x (formula - 100), "
            "kept between Minimum and Maximum Retention.");
    Bound(page.Number({text.maximum, "Maximum Retention", "The highest response allowed."},
        {0.0, 100.0, "%.1f%%"}, ChannelValue(text.channel, &WeatherGradeChannel::maximum)),
        text.channel, false);
    (void)page.Number({text.intercept, "Intercept",
        "The formula value before the weather inputs are added."}, kCoefficientRange,
        ChannelValue(text.channel, &WeatherGradeChannel::intercept));
    for (std::size_t index = 0; index < kCoefficientLabels.size(); ++index)
        (void)page.Number({text.coefficients[index], kCoefficientLabels[index],
            "Weight of this averaged weather input. Negative values lower the response as it rises."},
            kCoefficientRange, [channel = text.channel, index](HdrSettings& settings) -> double& {
                return channel(settings).coefficients[index];
            });
}

// The host's muted tone, rather than the disabled-widget style, marks secondary text.
void DrawMuted(std::string_view text) noexcept
{
    (void)dmui::DrawStyledText(*CurrentSettingsFrame().client, text, {.tone = dmui::TextTone::kMuted});
}

void DrawUnformatted(std::string_view text) noexcept
{
    ui::TextUnformatted(text.data(), text.data() + text.size());
}

template <std::size_t Size>
[[nodiscard]] std::string_view BoundedText(const std::array<char, Size>& text) noexcept
{
    const auto end = std::find(text.begin(), text.end(), '\0');
    if (end == text.end()) return {};
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

void DrawContributorIdentity(const ImageSpaceIdentity& identity, std::uint32_t contexts) noexcept
{
    const auto key = BoundedText(identity.key);
    const auto editorId = BoundedText(identity.editorId);
    const auto name = editorId.empty() ? key : editorId;
    if (name.empty())
    {
        DrawMuted("ImageSpace unavailable");
        return;
    }
    DrawUnformatted(name);
    std::array<char, 64> detail{};
    std::snprintf(detail.data(), detail.size(), "%08X, %u weather slots", identity.formId, contexts);
    DrawMuted(detail.data());
}

[[nodiscard]] bool IsValidNativeWeight(float weight) noexcept
{
    return std::isfinite(weight) && weight >= 0.0F && weight <= 1.0F;
}

void DrawContributorResponse(const WeatherLutResponseView& view, std::size_t index) noexcept
{
    const auto* reason = view.fallbackReasons[index];
    if (view.gradingBypassed)
        DrawMuted("Bypassed");
    else
    {
        const auto& response = view.resolved[index];
        if (std::isfinite(response.luminancePercent) && std::isfinite(response.colorPercent))
            ui::Text("L %.1f%% / C %.1f%%", response.luminancePercent, response.colorPercent);
        else
            DrawMuted("Response unavailable");
        if ((view.automaticSlots & (1U << index)) != 0)
        {
            DrawMuted("Automatic");
            return;
        }
        DrawMuted("Fallback Grade");
    }
    if (reason != nullptr && reason[0] != '\0') DrawUnformatted(reason);
}

void DrawContributors() noexcept
{
    const auto& view = Response();
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
            DrawMuted("Invalid");
        (void)ui::TableNextColumn();
        DrawContributorResponse(view, index);
        ui::PopID();
    }
    ui::EndTable();
}

[[nodiscard]] bool ResponseActive() noexcept
{
    return EffectiveWeatherGradeResponse(Draft()).enabled;
}

[[nodiscard]] dmui::FieldFeedbackSeverity FeedbackSeverity(StatusSeverity severity) noexcept
{
    return severity == StatusSeverity::error ? dmui::FieldFeedbackSeverity::kError :
        severity == StatusSeverity::warning ? dmui::FieldFeedbackSeverity::kWarning :
        dmui::FieldFeedbackSeverity::kInfo;
}

// The current scene's state, with its explanation as the row's feedback.
void AddStatus(SettingsPageBuilder& page)
{
    page.Group("status", "Status", "info");
    page.Value({"current-scene", "Current Scene",
        "Whether the weather response reaches the current scene's color grading."}, [] {
            const auto scene = WeatherResponseStatus(Response(), ResponseActive());
            const auto tone = scene.detail.severity != StatusSeverity::info ? dmui::TextTone::kWarning :
                scene.detail.text == nullptr ? dmui::TextTone::kSuccess : dmui::TextTone::kInherit;
            (void)dmui::DrawStyledText(*CurrentSettingsFrame().client, scene.value, {.tone = tone});
        }).resolveFeedback = []() -> std::optional<dmui::FieldFeedback> {
            const auto detail = WeatherResponseStatus(Response(), ResponseActive()).detail;
            if (detail.text == nullptr) return std::nullopt;
            return dmui::FieldFeedback{FeedbackSeverity(detail.severity), detail.text};
        };
    page.Messages([] {
        const auto& view = Response();
        return StatusList{view.status, view.imageSpaceStatus};
    });
}

// While an SDR operator holds the response on, the switch shows on and cannot change.
void AddEnabled(SettingsPageBuilder& page)
{
    page.Group("weather-response", "Response", "cloud-sun");
    auto& enabled = page.Toggle({"weather-enabled", "Enabled",
        "Sets the grade's luminance and color response per weather ImageSpace. "
        "Other scenes use the Fallback Grade settings."},
        [](HdrSettings& settings) -> bool& { return settings.weatherGradeResponse.enabled; });
    enabled.binding.get = [] { return dmui::SettingValue{ResponseActive()}; };
    enabled.isEnabled = [] { return !OperatorForcesWeatherResponse(Draft()); };
    enabled.resolveFeedback = []() -> std::optional<dmui::FieldFeedback> {
        if (!OperatorForcesWeatherResponse(Draft())) return std::nullopt;
        return dmui::FieldFeedback{dmui::FieldFeedbackSeverity::kInfo,
            "On while Weather Response is on for the selected tone mapper."};
    };
}

}  // namespace

void BuildWeatherResponsePage(SettingsPageBuilder& page)
{
    AddStatus(page);
    AddEnabled(page);
    AddChannel(page, kLuminance);
    AddChannel(page, kColor);
    AddAdvancedChannel(page, kLuminance, true);
    AddAdvancedChannel(page, kColor, false);
    page.Group("contributors", "Current Contributors", "list", false);
    page.Value({"contributors-table", ""}, DrawContributors).presentation = {
        dmui::RowPresentation::LabelMode::kHidden, dmui::RowPresentation::Layout::kFullSpan};
}

}  // namespace fo4hdr
