// SPDX-License-Identifier: GPL-3.0-only
// Compiles the GPL-3.0-only DearModdingUI API headers; the CommonLib exceptions do not apply.
#include "SettingsPages.hpp"

#include "SettingsStatus.hpp"
#include "ToneMapperRecommendations.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace fo4hdr {
namespace {

using enum ProductionToneMapperAlgorithm;
using Text = std::array<char, 256>;

constexpr double kMinimumPeakNits{400.0}, kMaximumPeakNits{2000.0};
constexpr double kMinimumWhiteNits{100.0}, kMaximumWhiteNits{300.0};
constexpr auto kDisplayDescription = "The display the game runs on, as Windows reports it.";

[[nodiscard]] HdrSettings& Draft() noexcept { return *CurrentSettingsFrame().draft; }
[[nodiscard]] const SettingsMenuView& View() noexcept { return *CurrentSettingsFrame().view; }

[[nodiscard]] bool Reported(float nits) noexcept
{
    return std::isfinite(nits) && nits > 0.0F;
}

[[nodiscard]] Text Nits(float nits) noexcept
{
    Text text{"Not reported"};
    if (Reported(nits)) std::snprintf(text.data(), text.size(), "%.0f nits", nits);
    return text;
}

// The SDR menu shows why HDR is off and whether the color controls act; HDR has no notes.
[[nodiscard]] StatusList OutputMessages() noexcept
{
    const auto& view = View();
    StatusList messages{};
    if (!view.sdrMode) return messages;
    messages[0] = {SdrModeReasonText(view), StatusSeverity::warning};
    messages[1] = view.enbDetected ? StatusMessage{"ENB is installed, so the color settings have no "
            "effect and ENB's own effects stay intact.", StatusSeverity::warning} :
        view.sdrControlsUnavailable ? StatusMessage{"The SDR color settings are unavailable this "
            "session because their shaders could not be built. Restart the game to retry.",
            StatusSeverity::error} :
        StatusMessage{"SDR output is active. The color settings apply to the game's SDR image."};
    return messages;
}

// HdrEnabled is the interface draft's, not the color profile's, and applies at the next start.
void AddHdrRequest(SettingsPageBuilder& page)
{
    page.Group("hdr", "HDR", "sun");
    dmui::SettingDescriptor setting{.id = "hdr-enabled", .label = "Enable HDR",
        .description = "Uses HDR10 output on an HDR display with Windows HDR on. "
            "Applies after a restart.",
        .control = dmui::CheckboxSettingControl{}, .defaultValue = true};
    setting.binding = dmui::BindSetting(
        [] { return *CurrentSettingsFrame().hdrEnabled; },
        [](bool enabled) { return *CurrentSettingsFrame().hdrEnabled = enabled; });
    setting.isModified = [] { return !*CurrentSettingsFrame().hdrEnabled; };
    setting.isDirty = [] {
        const auto& frame = CurrentSettingsFrame();
        return *frame.hdrEnabled != frame.sessionHdrEnabled;
    };
    setting.resolveFeedback = []() -> std::optional<dmui::FieldFeedback> {
        const auto* status = HdrRequestStatus(*CurrentSettingsFrame().hdrEnabled, View());
        if (status == nullptr) return std::nullopt;
        return dmui::FieldFeedback{dmui::FieldFeedbackSeverity::kWarning, status};
    };
    (void)page.Custom(std::move(setting), [] {
        auto& frame = CurrentSettingsFrame();
        *frame.hdrEnabled = frame.sessionHdrEnabled;
    });
}

void AddBrightness(SettingsPageBuilder& page)
{
    page.Group("brightness", "Brightness", "sun-dim");
    (void)page.Number({"display-peak", "Peak Brightness",
        "The brightest level highlights reach. Match your display's HDR peak."},
        {kMinimumPeakNits, kMaximumPeakNits, "%.0f nits"},
        [](HdrSettings& settings) -> double& { return settings.displayPeakNits; });
    (void)page.Number({"ui-white", "Menu & HUD White", "Brightness of white menus and HUD."},
        {kMinimumWhiteNits, kMaximumWhiteNits, "%.0f nits", 1.0, Publish::whileDragging},
        [](HdrSettings& settings) -> double& { return settings.uiPaperWhiteNits; });
    (void)page.Number({"video-white", "Video White", "Brightness of white in videos."},
        {kMinimumWhiteNits, kMaximumWhiteNits, "%.0f nits", 1.0, Publish::whileDragging},
        [](HdrSettings& settings) -> double& { return settings.videoWhiteNits; });
}

void DrawDisplayName() noexcept
{
    const auto& display = View().display;
    if (!display.known)
    {
        dmui::ui::TextUnformatted("Not reported. Set brightness manually.");
        return;
    }
    std::array<char, 256> name{};
    const auto* wide = display.friendlyName[0] != L'\0' ?
        display.friendlyName.data() : display.deviceName.data();
    (void)WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(),
        static_cast<int>(name.size()), nullptr, nullptr);
    dmui::ui::TextUnformatted(name[0] != '\0' ? name.data() : "Unknown");
}

// Reported values are references; the actions copy them into the brightness settings.
void AddDisplay(SettingsPageBuilder& page)
{
    page.Group("display", "Display", "monitor");
    (void)page.Value({"display-name", "Display", kDisplayDescription}, DrawDisplayName);
    const auto reported = [](float SettingsDisplayView::* nits) {
        return [nits] { dmui::ui::TextUnformatted(Nits(View().display.*nits).data()); };
    };
    const auto known = [](float SettingsDisplayView::* nits) {
        return [nits] { return View().display.known && Reported(View().display.*nits); };
    };
    page.Value({"reported-peak", "Reported Peak",
        "The peak brightness the display reports."}, reported(&SettingsDisplayView::peakNits))
        .isVisible = [] { return View().display.known; };
    page.Value({"reported-full-frame", "Reported Full Frame",
        "The brightness the display reports for a fully bright screen. For reference only."},
        reported(&SettingsDisplayView::fullFrameNits)).isVisible = [] { return View().display.known; };
    page.Value({"windows-sdr-white", "Windows SDR White",
        "The SDR content brightness set in Windows display settings."},
        reported(&SettingsDisplayView::sdrWhiteNits)).isVisible = [] { return View().display.known; };
    page.Action({"use-reported-peak", "Use Reported Peak",
        "Sets Peak Brightness to the reported peak, within the supported range."}, "Use", [] {
            Draft().displayPeakNits = std::clamp(std::round(
                static_cast<double>(View().display.peakNits)), kMinimumPeakNits, kMaximumPeakNits);
            RequestSettingsCommit();
        }, known(&SettingsDisplayView::peakNits));
    page.Action({"use-windows-sdr-white", "Use Windows SDR White",
        "Sets Menu & HUD White to the Windows SDR white, within the supported range."}, "Use", [] {
            Draft().uiPaperWhiteNits = std::clamp(
                static_cast<double>(View().display.sdrWhiteNits), kMinimumWhiteNits, kMaximumWhiteNits);
            RequestSettingsCommit();
        }, known(&SettingsDisplayView::sdrWhiteNits));
}

struct OperatorControl
{
    FieldText text;
    double ToneMapperParameters::* parameter;
    SliderRange range;
};

// HDR descriptions name the measured range at the nearest tested peak, and
// values outside it warn.
void Recommend(dmui::SettingDescriptor& setting, const OperatorControl& control)
{
    const auto range = [parameter = control.parameter] {
        const auto& settings = Draft();
        return GetToneMapperRecommendedRange(
            settings.productionAlgorithm, settings.displayPeakNits, parameter);
    };
    setting.resolveDescription = [range, description = std::string{control.text.description},
                                     slider = control.range] {
        const auto recommended = range();
        if (!recommended.available) return description;
        std::array<char, 32> minimum{}, maximum{};
        std::snprintf(minimum.data(), minimum.size(), slider.format,
            recommended.minimum * slider.displayScale);
        std::snprintf(maximum.data(), maximum.size(), slider.format,
            recommended.maximum * slider.displayScale);
        Text text{};
        std::snprintf(text.data(), text.size(), "%s Recommended at %.0f nits: %s to %s.",
            description.c_str(), recommended.testedPeakNits, minimum.data(), maximum.data());
        return std::string{text.data()};
    };
    setting.resolveFeedback = [range, parameter = control.parameter]()
        -> std::optional<dmui::FieldFeedback> {
        const auto* status = ToneMapperRecommendationStatus(
            Draft().toneMapperParameters.*parameter, range());
        if (status == nullptr) return std::nullopt;
        return dmui::FieldFeedback{dmui::FieldFeedbackSeverity::kWarning, status};
    };
}

// Only the selected operator's controls show; SDR has no recommendations.
void AddOperatorControl(SettingsPageBuilder& page, ProductionToneMapperAlgorithm algorithm,
    const OperatorControl& control)
{
    auto& setting = page.Number(control.text, control.range,
        [parameter = control.parameter](HdrSettings& settings) -> double& {
            return settings.toneMapperParameters.*parameter;
        });
    setting.isVisible = [algorithm] { return Draft().productionAlgorithm == algorithm; };
    if (!page.Sdr()) Recommend(setting, control);
}

// SDR keeps Middle Gray at or below neutral, has no HDR shoulder and lets
// GT7's shoulder start lower, where its toe ends.
void AddLook(SettingsPageBuilder& page)
{
    const auto sdr = page.Sdr();
    page.Group("look", "Look", "sliders");
    AddOperatorControl(page, aces2, {{"aces-look", "Contrast Look",
        "Strength of the ACES 2 contrast look. 0% removes it."},
        &ToneMapperParameters::acesLookStrength,
        {kMinimumAcesLookStrength, kMaximumAcesLookStrength, "%.0f%%", 100.0}});
    AddOperatorControl(page, agX, {{"agx-middle-gray", "Middle Gray", sdr ?
        "Middle-gray brightness; SDR white stays fixed." :
        "Middle-gray brightness; the display peak stays fixed."},
        &ToneMapperParameters::agxMiddleGrayEv,
        {kMinimumAgxMiddleGrayEv, sdr ? kMaximumSdrAgxMiddleGrayEv : kMaximumAgxMiddleGrayEv,
            "%+.2f EV"}});
    AddOperatorControl(page, agX, {{"agx-contrast", "Curve Contrast",
        "Slope of the curve around middle gray."},
        &ToneMapperParameters::agxContrast, {kMinimumAgxContrast, kMaximumAgxContrast, "%.2f"}});
    if (!sdr)
        AddOperatorControl(page, agX, {{"agx-shoulder", "Highlight Shoulder",
            "Highlight compression near the display peak."},
            &ToneMapperParameters::agxHdrShoulderPower,
            {kMinimumAgxHdrShoulderPower, kMaximumAgxHdrShoulderPower, "%.2fx"}});
    AddOperatorControl(page, agX, {{"agx-color-response", "Color Response",
        "Hue and saturation balance through the curve. Higher values can also shift brightness."},
        &ToneMapperParameters::agxHdrPurity, {kMinimumAgxHdrPurity, kMaximumAgxHdrPurity, "%.2f"}});
    AddOperatorControl(page, gt7, {{"gt7-toe", "Shadow Toe",
        "Shadow compression. Higher values deepen shadows."},
        &ToneMapperParameters::gt7ToeStrength, {kMinimumGt7ToeStrength, kMaximumGt7ToeStrength, "%.2f"}});
    AddOperatorControl(page, gt7, {{"gt7-shoulder", "Shoulder Start", sdr ?
        "Where highlight compression starts, as a share of SDR white." :
        "Where highlight compression starts, as a share of the display peak."},
        &ToneMapperParameters::gt7ShoulderStart,
        {sdr ? kMinimumSdrGt7ShoulderStart : kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart,
            "%.1f%%", 100.0}});
    AddOperatorControl(page, gt7, {{"gt7-color-blend", "Color Blend",
        "Highlight hue and saturation handling. 0 uses per-channel RGB curves."},
        &ToneMapperParameters::gt7ColorBlend, {kMinimumGt7ColorBlend, kMaximumGt7ColorBlend, "%.2f"}});
}

// SDR lists the game's curve and each qualified operator; HDR has no native curve.
constexpr std::array<ChoiceEntry<ProductionToneMapperAlgorithm>, 4> kToneMappers{{
    {native, "vanilla", "Vanilla"}, {aces2, "aces2", "ACES 2"},
    {agX, "agx", "AgX"}, {gt7, "gt7", "GT7"}}};
static_assert(IsSelectableSdrToneMapper(aces2) && IsSelectableSdrToneMapper(agX) &&
    IsSelectableSdrToneMapper(gt7));

[[nodiscard]] std::optional<dmui::FieldFeedback> PendingOperatorFeedback()
{
    const auto& view = View();
    if (!view.lutPending) return std::nullopt;
    const auto* name = SettingsOperatorDisplayName(view.pendingAlgorithm);
    Text text{};
    if (view.sdrMode)
        std::snprintf(text.data(), text.size(), "Applying %s...", name);
    else
        std::snprintf(text.data(), text.size(), "Applying %s at %.0f nits...", name, view.pendingPeakNits);
    return dmui::FieldFeedback{dmui::FieldFeedbackSeverity::kInfo, text.data()};
}

void AddToneMapper(SettingsPageBuilder& page)
{
    const auto sdr = page.Sdr();
    page.Group("tone-mapper", "Tone Mapper", "chart-line");
    const std::span<const ChoiceEntry<ProductionToneMapperAlgorithm>> entries{kToneMappers};
    page.Choice<ProductionToneMapperAlgorithm>({"tone-mapper", "Tone Mapper", sdr ?
        "Vanilla keeps the game's curve; ACES 2, AgX and GT7 replace it." :
        "How scene colors and highlights fit the display."},
        [](HdrSettings& settings) -> ProductionToneMapperAlgorithm& {
            return settings.productionAlgorithm;
        }, sdr ? entries : entries.subspan(1)).resolveFeedback = PendingOperatorFeedback;
    if (!sdr) return;
    // Vanilla follows the Weather Response page instead.
    page.Toggle({"sdr-weather-response", "Weather Response",
        "Turns on Weather Response while this tone mapper replaces the game's curve."},
        [](HdrSettings& settings) -> bool& { return settings.sdrOperatorWeatherResponse; })
        .isVisible = [] { return Draft().productionAlgorithm != native; };
}

}  // namespace

void BuildOutputPage(SettingsPageBuilder& page)
{
    page.Status(OutputMessages);
    AddHdrRequest(page);
    if (page.Sdr()) return;
    AddBrightness(page);
    AddDisplay(page);
}

void BuildToneMappingPage(SettingsPageBuilder& page)
{
    page.Status();
    AddToneMapper(page);
    AddLook(page);
}

}  // namespace fo4hdr
