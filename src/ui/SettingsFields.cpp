// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SettingsPages.hpp"

#include <algorithm>

namespace fo4hdr {

bool DrawSliderField(SettingsPage& page, const FieldText& text,
    const SliderRange& range, FieldValue<double> bound) noexcept
{
    auto& [value, defaultValue, note] = bound;
    dmui::FieldScope field{page.client, text.id, text.label, text.description};
    if (!field.Visible()) return false;
    auto displayed = value * range.displayScale;
    const auto minimum = range.minimum * range.displayScale;
    const auto maximum = range.maximum * range.displayScale;
    dmui::ui::PushID(text.id);
    const auto changed = dmui::ui::SliderScalar("##value", &displayed, &minimum, &maximum,
        range.format, dmui::ui::SliderFlags::kAlwaysClamp);
    const auto released = dmui::ui::IsItemDeactivatedAfterEdit();
    dmui::ui::PopID();
    if (changed)
        value = std::clamp(displayed / range.displayScale, range.minimum, range.maximum);
    if (note.text) (void)field.SetFeedback(note.severity, note.text);
    const auto reset = field.End(true, value != defaultValue).value_or(false);
    if (reset) value = defaultValue;
    return reset || released || (changed && range.publishWhileDragging);
}

bool DrawCheckboxField(SettingsPage& page, const FieldText& text,
    FieldValue<bool> bound) noexcept
{
    auto& [value, defaultValue, note] = bound;
    dmui::FieldScope field{page.client, text.id, text.label, text.description};
    if (!field.Visible()) return false;
    dmui::ui::PushID(text.id);
    const auto changed = dmui::ui::Checkbox("##value", &value);
    dmui::ui::PopID();
    if (note.text) (void)field.SetFeedback(note.severity, note.text);
    const auto reset = field.End(true, value != defaultValue).value_or(false);
    if (reset) value = defaultValue;
    return changed || reset;
}

void DrawValueRow(SettingsPage& page, const FieldText& text, std::string_view value) noexcept
{
    dmui::FieldScope field{page.client, text.id, text.label, text.description};
    if (field.Visible()) dmui::ui::TextUnformatted(value.data(), value.data() + value.size());
}

void DrawPageText(SettingsPage& page, const char* text, dmui::TextTone tone) noexcept
{
    if (text != nullptr)
        (void)dmui::DrawStyledText(page.client, text, {.tone = tone, .wrapped = true});
}

bool DrawRestoreDefaultsRow(SettingsPage& page, bool modified) noexcept
{
    (void)page.client.DrawSectionHeader("Defaults");
    dmui::SettingsTableScope table{page.client, "page-defaults"};
    if (!table.Visible()) return false;
    dmui::FieldScope field{page.client, "restore-defaults", "Restore Defaults",
        "Resets every setting on this page to its default."};
    if (!field.Visible()) return false;
    bool pressed{};
    {
        const dmui::DisabledScope unchanged{!modified};
        pressed = dmui::ui::Button("Restore");
    }
    (void)field.End();
    return pressed;
}

}  // namespace fo4hdr
