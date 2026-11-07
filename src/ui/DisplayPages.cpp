// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SettingsPages.hpp"

#include "SettingsStatus.hpp"
#include "ToneMapperRecommendations.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace fo4hdr {
namespace {

using enum ProductionToneMapperAlgorithm;
using Choice = dmui::ChoiceOption<ProductionToneMapperAlgorithm>;
using Text = std::array<char, 256>;

constexpr double kMinimumPeakNits{400.0}, kMaximumPeakNits{2000.0};
constexpr double kMinimumWhiteNits{100.0}, kMaximumWhiteNits{300.0};
constexpr auto kDisplayDescription = "The display the game runs on, as Windows reports it.";

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
void DrawOutputNotes(SettingsPage& page) noexcept
{
    const auto& view = page.view;
    std::array<StatusMessage, 2> notes{};
    if (page.sdr)
    {
        notes[0] = {SdrModeReasonText(view), StatusSeverity::warning};
        notes[1] = view.enbDetected ? StatusMessage{"ENB is installed, so the color settings have no "
                "effect and ENB's own effects stay intact.", StatusSeverity::warning} :
            view.sdrControlsUnavailable ? StatusMessage{"The SDR color settings are unavailable this "
                "session because their shaders could not be built. Restart the game to retry.",
                StatusSeverity::error} :
            StatusMessage{"SDR output is active. The color settings apply to the game's SDR image."};
    }
    DrawPageMessages(page, notes);
}

void DrawHdrRequest(SettingsPage& page) noexcept
{
    DrawSection(page, "hdr", "HDR", [&] {
        (void)DrawCheckboxField(page, {"hdr-enabled", "Enable HDR",
            "Uses HDR10 output on an HDR display with Windows HDR on. Applies after a restart."},
            {page.hdrEnabled, true,
                {dmui::FieldFeedbackSeverity::kWarning, HdrRequestStatus(page.hdrEnabled, page.view)}});
    });
}

void DrawBrightness(SettingsPage& page) noexcept
{
    auto& settings = page.settings;
    DrawSection(page, "brightness", "Brightness", [&] {
        page.commitRequested |= DrawSliderField(page, {"display-peak", "Peak Brightness",
            "The brightest level highlights reach. Match your display's HDR peak."},
            {kMinimumPeakNits, kMaximumPeakNits, "%.0f nits"},
            {settings.displayPeakNits, page.defaults.displayPeakNits});
        page.commitRequested |= DrawSliderField(page, {"ui-white", "Menu & HUD White",
            "Brightness of white menus and HUD."},
            {kMinimumWhiteNits, kMaximumWhiteNits, "%.0f nits", 1.0, true},
            {settings.uiPaperWhiteNits, page.defaults.uiPaperWhiteNits});
        page.commitRequested |= DrawSliderField(page, {"video-white", "Video White",
            "Brightness of white in videos."},
            {kMinimumWhiteNits, kMaximumWhiteNits, "%.0f nits", 1.0, true},
            {settings.videoWhiteNits, page.defaults.videoWhiteNits});
    });
}

// A reported value with a button that copies it into its brightness setting.
[[nodiscard]] bool DrawReportedValue(SettingsPage& page, const FieldText& text, float nits) noexcept
{
    dmui::FieldScope field{page.client, text.id, text.label, text.description};
    if (!field.Visible()) return false;
    const auto value = Nits(nits);
    dmui::ui::TextUnformatted(value.data());
    if (!Reported(nits)) return false;
    dmui::ui::SameLine();
    dmui::ui::PushID(text.id);
    const auto pressed = dmui::ui::Button("Use");
    dmui::ui::PopID();
    return pressed;
}

void DrawDisplayRows(SettingsPage& page) noexcept
{
    const auto& display = page.view.display;
    std::array<char, 256> name{};
    const auto* wide = display.friendlyName[0] != L'\0' ?
        display.friendlyName.data() : display.deviceName.data();
    (void)WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(),
        static_cast<int>(name.size()), nullptr, nullptr);
    DrawValueRow(page, {"display-name", "Display", kDisplayDescription},
        name[0] != '\0' ? name.data() : "Unknown");
    if (DrawReportedValue(page, {"reported-peak", "Reported Peak",
            "Use copies it to Peak Brightness, within the supported range."}, display.peakNits))
    {
        page.settings.displayPeakNits = std::clamp(
            std::round(static_cast<double>(display.peakNits)), kMinimumPeakNits, kMaximumPeakNits);
        page.commitRequested = true;
    }
    DrawValueRow(page, {"reported-full-frame", "Reported Full Frame",
        "The brightness the display reports for a fully bright screen. For reference only."},
        Nits(display.fullFrameNits).data());
    if (DrawReportedValue(page, {"windows-sdr-white", "Windows SDR White",
            "Use copies it to Menu & HUD White, within the supported range."}, display.sdrWhiteNits))
    {
        page.settings.uiPaperWhiteNits = std::clamp(
            static_cast<double>(display.sdrWhiteNits), kMinimumWhiteNits, kMaximumWhiteNits);
        page.commitRequested = true;
    }
}

void DrawDisplay(SettingsPage& page) noexcept
{
    DrawSection(page, "display", "Display", [&] {
        if (page.view.display.known)
            DrawDisplayRows(page);
        else
            DrawValueRow(page, {"display-name", "Display", kDisplayDescription},
                "Not reported. Set brightness manually.");
    });
}

[[nodiscard]] bool OutputModified(const SettingsPage& page) noexcept
{
    const auto& settings = page.settings;
    const auto& defaults = page.defaults;
    return !page.hdrEnabled || (!page.sdr &&
        (settings.displayPeakNits != defaults.displayPeakNits ||
            settings.uiPaperWhiteNits != defaults.uiPaperWhiteNits ||
            settings.videoWhiteNits != defaults.videoWhiteNits));
}

void RestoreOutput(SettingsPage& page) noexcept
{
    page.hdrEnabled = true;
    if (page.sdr) return;
    page.settings.displayPeakNits = page.defaults.displayPeakNits;
    page.settings.uiPaperWhiteNits = page.defaults.uiPaperWhiteNits;
    page.settings.videoWhiteNits = page.defaults.videoWhiteNits;
    page.commitRequested = true;
}

struct OperatorControl
{
    FieldText text;
    double ToneMapperParameters::* parameter;
    SliderRange range;
};

// HDR descriptions name the measured range at the nearest tested peak.
void DrawOperatorControl(SettingsPage& page, const OperatorControl& control) noexcept
{
    auto& value = page.settings.toneMapperParameters.*control.parameter;
    const auto defaultValue = page.defaults.toneMapperParameters.*control.parameter;
    auto text = control.text;
    Text description{};
    const auto range = page.sdr ? ToneMapperRecommendedRange{} : GetToneMapperRecommendedRange(
        page.settings.productionAlgorithm, page.settings.displayPeakNits, control.parameter);
    if (range.available)
    {
        std::array<char, 32> minimum{}, maximum{};
        const auto& slider = control.range;
        std::snprintf(minimum.data(), minimum.size(), slider.format, range.minimum * slider.displayScale);
        std::snprintf(maximum.data(), maximum.size(), slider.format, range.maximum * slider.displayScale);
        std::snprintf(description.data(), description.size(), "%s Recommended at %.0f nits: %s to %s.",
            text.description, range.testedPeakNits, minimum.data(), maximum.data());
        text.description = description.data();
    }
    page.commitRequested |= DrawSliderField(page, text, control.range, {value, defaultValue,
        {dmui::FieldFeedbackSeverity::kWarning, ToneMapperRecommendationStatus(value, range)}});
}

void DrawAces2(SettingsPage& page) noexcept
{
    DrawOperatorControl(page, {{"aces-look", "Contrast Look",
        "Strength of the ACES 2 contrast look. 0% removes it."},
        &ToneMapperParameters::acesLookStrength,
        {kMinimumAcesLookStrength, kMaximumAcesLookStrength, "%.0f%%", 100.0}});
}

// SDR keeps Middle Gray at or below neutral and has no HDR shoulder.
void DrawAgx(SettingsPage& page) noexcept
{
    DrawOperatorControl(page, {{"agx-middle-gray", "Middle Gray", page.sdr ?
        "Middle-gray brightness; SDR white stays fixed." :
        "Middle-gray brightness; the display peak stays fixed."},
        &ToneMapperParameters::agxMiddleGrayEv,
        {kMinimumAgxMiddleGrayEv, page.sdr ? kMaximumSdrAgxMiddleGrayEv : kMaximumAgxMiddleGrayEv,
            "%+.2f EV"}});
    DrawOperatorControl(page, {{"agx-contrast", "Curve Contrast",
        "Slope of the curve around middle gray."},
        &ToneMapperParameters::agxContrast, {kMinimumAgxContrast, kMaximumAgxContrast, "%.2f"}});
    if (!page.sdr)
        DrawOperatorControl(page, {{"agx-shoulder", "Highlight Shoulder",
            "Highlight compression near the display peak."},
            &ToneMapperParameters::agxHdrShoulderPower,
            {kMinimumAgxHdrShoulderPower, kMaximumAgxHdrShoulderPower, "%.2fx"}});
    DrawOperatorControl(page, {{"agx-color-response", "Color Response",
        "Hue and saturation balance through the curve. Higher values can also shift brightness."},
        &ToneMapperParameters::agxHdrPurity, {kMinimumAgxHdrPurity, kMaximumAgxHdrPurity, "%.2f"}});
}

// SDR lets the shoulder start lower, where the toe ends.
void DrawGt7(SettingsPage& page) noexcept
{
    DrawOperatorControl(page, {{"gt7-toe", "Shadow Toe",
        "Shadow compression. Higher values deepen shadows."},
        &ToneMapperParameters::gt7ToeStrength, {kMinimumGt7ToeStrength, kMaximumGt7ToeStrength, "%.2f"}});
    DrawOperatorControl(page, {{"gt7-shoulder", "Shoulder Start", page.sdr ?
        "Where highlight compression starts, as a share of SDR white." :
        "Where highlight compression starts, as a share of the display peak."},
        &ToneMapperParameters::gt7ShoulderStart,
        {page.sdr ? kMinimumSdrGt7ShoulderStart : kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart,
            "%.1f%%", 100.0}});
    DrawOperatorControl(page, {{"gt7-color-blend", "Color Blend",
        "Highlight hue and saturation handling. 0 uses per-channel RGB curves."},
        &ToneMapperParameters::gt7ColorBlend, {kMinimumGt7ColorBlend, kMaximumGt7ColorBlend, "%.2f"}});
}

void DrawLook(SettingsPage& page) noexcept
{
    const auto algorithm = page.settings.productionAlgorithm;
    if (algorithm == native) return;
    DrawSection(page, "look", "Look", [&] {
        switch (algorithm)
        {
        case aces2: DrawAces2(page); break;
        case agX: DrawAgx(page); break;
        case gt7: DrawGt7(page); break;
        case native: break;
        }
    });
}

[[nodiscard]] std::span<const Choice> ToneMapperChoices(bool sdr)
{
    // SDR lists the game's curve and each qualified operator; HDR has no native curve.
    static const std::array<Choice, 4> choices{{{native, "Vanilla"},
        {aces2, "ACES 2"}, {agX, "AgX"}, {gt7, "GT7"}}};
    static_assert(IsSelectableSdrToneMapper(aces2) && IsSelectableSdrToneMapper(agX) &&
        IsSelectableSdrToneMapper(gt7));
    return sdr ? std::span<const Choice>{choices} : std::span<const Choice>{choices}.subspan(1);
}

[[nodiscard]] Text PendingOperatorText(const SettingsPage& page) noexcept
{
    Text text{};
    const auto& view = page.view;
    if (!view.lutPending) return text;
    const auto* name = SettingsOperatorDisplayName(view.pendingAlgorithm);
    if (page.sdr)
        std::snprintf(text.data(), text.size(), "Applying %s...", name);
    else
        std::snprintf(text.data(), text.size(), "Applying %s at %.0f nits...", name, view.pendingPeakNits);
    return text;
}

void DrawToneMapper(SettingsPage& page) noexcept
{
    auto& settings = page.settings;
    const auto pending = PendingOperatorText(page);
    DrawSection(page, "tone-mapper", "Tone Mapper", [&] {
        page.commitRequested |= DrawChoiceField(page, {"tone-mapper", "Tone Mapper", page.sdr ?
            "Vanilla keeps the game's curve; ACES 2, AgX and GT7 replace it." :
            "How scene colors and highlights fit the display."},
            {settings.productionAlgorithm, page.defaults.productionAlgorithm,
                {dmui::FieldFeedbackSeverity::kInfo, pending[0] != '\0' ? pending.data() : nullptr}},
            ToneMapperChoices(page.sdr));
        // Vanilla follows the Weather Response page instead.
        if (page.sdr && settings.productionAlgorithm != native)
            page.commitRequested |= DrawCheckboxField(page, {"sdr-weather-response", "Weather Response",
                "Turns on Weather Response while this tone mapper replaces the game's curve."},
                {settings.sdrOperatorWeatherResponse, page.defaults.sdrOperatorWeatherResponse});
    });
}

[[nodiscard]] bool ToneMappingModified(const SettingsPage& page) noexcept
{
    const auto& settings = page.settings;
    const auto& defaults = page.defaults;
    return settings.productionAlgorithm != defaults.productionAlgorithm ||
        settings.toneMapperParameters != defaults.toneMapperParameters ||
        settings.sdrOperatorWeatherResponse != defaults.sdrOperatorWeatherResponse;
}

}  // namespace

void DrawOutputPage(SettingsPage& page) noexcept
{
    DrawOutputNotes(page);
    DrawHdrRequest(page);
    if (!page.sdr)
    {
        DrawBrightness(page);
        DrawDisplay(page);
    }
    if (DrawRestoreDefaultsRow(page, OutputModified(page))) RestoreOutput(page);
}

void DrawToneMappingPage(SettingsPage& page) noexcept
{
    DrawToneMapper(page);
    DrawLook(page);
    if (!DrawRestoreDefaultsRow(page, ToneMappingModified(page))) return;
    page.settings.productionAlgorithm = page.defaults.productionAlgorithm;
    page.settings.toneMapperParameters = page.defaults.toneMapperParameters;
    page.settings.sdrOperatorWeatherResponse = page.defaults.sdrOperatorWeatherResponse;
    page.commitRequested = true;
}

}  // namespace fo4hdr
