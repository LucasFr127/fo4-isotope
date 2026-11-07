// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/ui/SettingsMenu.hpp"

#include <DearModdingUI/Client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fo4hdr {

// What the page bindings read and edit while the host draws a page.
struct SettingsFrame
{
    dmui::Client* client{};
    HdrSettings* draft{};
    // The values when the menu opened; Revert restores them.
    HdrSettings* session{};
    // [General] HdrEnabled saves like the other settings and applies at the next game start.
    bool* hdrEnabled{};
    bool sessionHdrEnabled{};
    const SettingsMenuView* view{};
    const SettingsMenuRenderer* renderer{};
    bool* commitRequested{};
    // Problems that affect every page; each page's Status group shows them.
    std::array<StatusMessage, 2> problems{};
};

// Valid only while the menu draws one of its pages.
[[nodiscard]] SettingsFrame& CurrentSettingsFrame() noexcept;
// Requests a commit at the end of Present.
void RequestSettingsCommit() noexcept;

template <class Value>
using SettingField = std::function<Value&(HdrSettings&)>;

// Up to four page-specific messages; null entries draw nothing.
using StatusList = std::array<StatusMessage, 4>;

enum class Publish : std::uint8_t
{
    // LUT and peak edits wait for the slider's release.
    onRelease,
    // Constant-only edits publish while the slider moves.
    whileDragging
};

struct FieldText
{
    const char* id;
    const char* label;
    const char* description{};
};

struct SliderRange
{
    double minimum, maximum;
    const char* format;
    double displayScale{1.0};
    Publish publish{Publish::onRelease};
};

template <class Value>
struct ChoiceEntry
{
    Value value;
    const char* key;
    const char* label;
};

// One page as its builder made it: the part the host draws, how Revert restores
// it, and an editable view the page draws itself after the host's rows.
struct BuiltSettingsPage
{
    dmui::SettingsPage page;
    std::vector<std::function<void()>> reverts;
    std::function<void()> drawAfter;
};

// Builds one profile's declarative page. The host then draws its search,
// Modified only filter, Reset all and Revert, and each row's Reset.
class SettingsPageBuilder
{
public:
    SettingsPageBuilder(BuiltSettingsPage& built, const HdrSettings& defaults, bool sdr);

    [[nodiscard]] bool Sdr() const noexcept { return sdr_; }
    [[nodiscard]] const HdrSettings& Defaults() const noexcept { return defaults_; }
    // Diagnostics pages hold no settings to reset or revert.
    void WithoutPageActions() noexcept;
    // Reset all also runs this, after every row is back at its default.
    void AlsoOnReset(std::function<void()> reset);
    // True while the header's search or Modified only filter is in use.
    [[nodiscard]] std::function<bool()> Filtering() const;
    // Draws an editable view after the host's rows; the page decides when.
    void DrawAfterRows(std::function<void()> draw);

    // Starts a group; the rows added next belong to it.
    void Group(const char* id, const char* label, const char* icon, bool expanded = true);
    // A Status group with the page's messages and the shared problems.
    void Status(std::function<StatusList()> messages = {});
    // A returned row stays valid until the next row is added.
    dmui::SettingDescriptor& Number(const FieldText& text, const SliderRange& range,
        SettingField<double> field);
    dmui::SettingDescriptor& Toggle(const FieldText& text, SettingField<bool> field);
    template <class Option>
    dmui::SettingDescriptor& Choice(const FieldText& text, SettingField<Option> field,
        std::span<const ChoiceEntry<Option>> entries);
    // A row the page binds itself, with how Revert restores it.
    dmui::SettingDescriptor& Custom(dmui::SettingDescriptor setting, std::function<void()> revert);
    dmui::SettingDescriptor& Value(const FieldText& text, std::function<void()> draw);
    // Muted text across the row, for a short explanation inside a group.
    void Note(const char* id, const char* text);
    void Messages(std::function<StatusList()> messages);
    void Action(const FieldText& text, const char* button, std::function<void()> activate,
        std::function<bool()> visible = {});

private:
    dmui::SettingDescriptor& Add(dmui::SettingDescriptor setting);

    BuiltSettingsPage& built_;
    dmui::SettingsPage& page_;
    std::vector<std::function<void()>>& reverts_;
    HdrSettings defaults_;
    bool sdr_;
};

// Commits once an edit completes, the slider's release or a click.
void PublishOnRelease(const dmui::SettingEditEvent& edit);
// Commits each change while a slider moves.
void PublishWhileDragging(const dmui::SettingEditEvent& edit);

template <class Option>
[[nodiscard]] std::string ChoiceKey(std::span<const ChoiceEntry<Option>> entries, Option value)
{
    const auto found = std::ranges::find(entries, value, &ChoiceEntry<Option>::value);
    return found == entries.end() ? std::string{} : std::string{found->key};
}

template <class Option>
void SelectChoice(std::span<const ChoiceEntry<Option>> entries, std::string_view key, Option& value)
{
    const auto found = std::ranges::find(entries, key,
        [](const ChoiceEntry<Option>& entry) { return std::string_view{entry.key}; });
    if (found != entries.end()) value = found->value;
}

template <class Option>
dmui::SettingDescriptor& SettingsPageBuilder::Choice(const FieldText& text,
    SettingField<Option> field, std::span<const ChoiceEntry<Option>> entries)
{
    dmui::ChoiceSettingControl control;
    for (const auto& entry : entries) control.options.push_back({entry.key, entry.label});
    const auto defaultValue = field(defaults_);
    dmui::SettingDescriptor setting{.id = text.id, .label = text.label,
        .description = text.description ? text.description : "",
        .control = std::move(control), .defaultValue = ChoiceKey(entries, defaultValue)};
    setting.binding = dmui::BindSetting(
        [field, entries] { return ChoiceKey(entries, field(*CurrentSettingsFrame().draft)); },
        [field, entries](std::string selected) {
            auto& value = field(*CurrentSettingsFrame().draft);
            SelectChoice(entries, selected, value);
            return ChoiceKey(entries, value);
        });
    setting.applyTiming = dmui::SettingApplyTiming::kImmediate;
    setting.isModified = [field, defaultValue] {
        return field(*CurrentSettingsFrame().draft) != defaultValue;
    };
    setting.isDirty = [field] {
        const auto& frame = CurrentSettingsFrame();
        return field(*frame.draft) != field(*frame.session);
    };
    setting.onEdit = PublishOnRelease;
    reverts_.push_back([field] {
        const auto& frame = CurrentSettingsFrame();
        field(*frame.draft) = field(*frame.session);
    });
    return Add(std::move(setting));
}

void BuildOutputPage(SettingsPageBuilder& page);
void BuildToneMappingPage(SettingsPageBuilder& page);
void BuildAdjustmentsPage(SettingsPageBuilder& page);
void BuildColorGradingPage(SettingsPageBuilder& page);
void BuildSelectiveColorPage(SettingsPageBuilder& page);
void BuildWeatherResponsePage(SettingsPageBuilder& page);
void BuildRendererPage(SettingsPageBuilder& page);
void DrawExposureOverlay(const SettingsMenuView& view) noexcept;

}  // namespace fo4hdr
