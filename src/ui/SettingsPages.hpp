// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/ui/SettingsMenu.hpp"

#include <DearModdingUI/Client.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace fo4hdr {

// One page draw: the draft it edits and this frame's renderer view.
struct SettingsPage
{
    dmui::Client& client;
    HdrSettings& settings;
    // The active profile's defaults: HDR, or vanilla-neutral SDR.
    const HdrSettings& defaults;
    // [General] HdrEnabled saves like the other settings and applies at the next game start.
    bool& hdrEnabled;
    const SettingsMenuView& view;
    bool sdr;
    bool& commitRequested;
};

struct FieldText
{
    const char* id;
    const char* label;
    const char* description{};
};

struct FieldNote
{
    dmui::FieldFeedbackSeverity severity{dmui::FieldFeedbackSeverity::kInfo};
    const char* text{};
};

struct SliderRange
{
    double minimum, maximum;
    const char* format;
    double displayScale{1.0};
    // Constant-only controls publish while dragging; LUT and peak edits wait for release.
    bool publishWhileDragging{};
};

// A setting, the active profile's default for it, and optional row feedback.
template <class Value>
struct FieldValue
{
    Value& value;
    Value defaultValue;
    FieldNote note{};
};

// Field helpers draw one settings-table row with its Reset and return true when
// the edit should publish.
[[nodiscard]] bool DrawSliderField(SettingsPage& page, const FieldText& text,
    const SliderRange& range, FieldValue<double> field) noexcept;
[[nodiscard]] bool DrawCheckboxField(SettingsPage& page, const FieldText& text,
    FieldValue<bool> field) noexcept;
void DrawValueRow(SettingsPage& page, const FieldText& text, std::string_view value) noexcept;
// Muted text for a page-wide note, or the error tone for a problem; null draws nothing.
void DrawPageText(SettingsPage& page, const char* text,
    dmui::TextTone tone = dmui::TextTone::kMuted) noexcept;
// The last row of every page; true when pressed.
[[nodiscard]] bool DrawRestoreDefaultsRow(SettingsPage& page, bool modified) noexcept;

template <class Value>
[[nodiscard]] bool DrawChoiceField(SettingsPage& page, const FieldText& text,
    FieldValue<Value> bound, std::span<const dmui::ChoiceOption<Value>> options) noexcept
{
    auto& [value, defaultValue, note] = bound;
    dmui::FieldScope field{page.client, text.id, text.label, text.description};
    if (!field.Visible()) return false;
    const auto choice = dmui::DrawChoice<Value>(text.id, value, options);
    if (choice.selected) value = *choice.selected;
    if (note.text) (void)field.SetFeedback(note.severity, note.text);
    const auto reset = field.End(true, value != defaultValue).value_or(false);
    if (reset) value = defaultValue;
    return choice.changed || reset;
}

// A titled settings table; the body draws its rows while it is visible.
template <class Body>
void DrawSection(SettingsPage& page, const char* id, const char* title, Body&& body) noexcept
{
    (void)page.client.DrawSectionHeader(title);
    dmui::SettingsTableScope table{page.client, id};
    if (table.Visible()) body();
}

struct CollapsibleSection
{
    const char* id;
    const char* title;
    bool& expanded;
    std::size_t count;
};

// A collapsed-by-default heading; the body draws its own tables.
template <class Body>
void DrawCollapsibleSection(SettingsPage& page, const CollapsibleSection& section, Body&& body) noexcept
{
    if (page.client.DrawCollapsingSectionHeader(
            section.id, section.title, 0, section.expanded, section.count) && section.expanded)
        body();
}

void DrawOutputPage(SettingsPage& page) noexcept;
void DrawToneMappingPage(SettingsPage& page) noexcept;
void DrawAdjustmentsPage(SettingsPage& page) noexcept;
void DrawColorGradingPage(SettingsPage& page) noexcept;
void DrawSelectiveColorPage(SettingsPage& page) noexcept;
void DrawWeatherResponsePage(SettingsPage& page) noexcept;
void DrawRendererPage(SettingsPage& page, const SettingsMenuRenderer& renderer) noexcept;
void DrawExposureOverlay(const SettingsMenuView& view) noexcept;

}  // namespace fo4hdr
