// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SettingsPages.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <utility>

namespace fo4hdr {
namespace {

SettingsFrame g_frame;

[[nodiscard]] bool HasText(const StatusMessage& message) noexcept
{
    return message.text != nullptr && message.text[0] != '\0';
}

[[nodiscard]] std::size_t SeverityIndex(StatusSeverity severity) noexcept
{
    return std::min<std::size_t>(static_cast<std::size_t>(severity), 2U);
}

// Like host field feedback, warnings and errors name their severity in words,
// so the meaning does not rest on color.
void DrawStatusMessage(const StatusMessage& message) noexcept
{
    constexpr std::array prefixes{"", "Warning: ", "Error: "};
    constexpr std::array tones{dmui::TextTone::kMuted, dmui::TextTone::kWarning,
        dmui::TextTone::kError};
    const auto severity = SeverityIndex(message.severity);
    std::array<char, 1024> text{};
    std::snprintf(text.data(), text.size(), "%s%s", prefixes[severity], message.text);
    (void)dmui::DrawStyledText(*g_frame.client, text.data(), {.tone = tones[severity], .wrapped = true});
}

// Info, then warnings, then errors; each severity lists the page's messages
// before the problems every page shares.
template <class Visit>
void ForEachMessage(const StatusList& messages, Visit&& visit)
{
    for (std::size_t severity = 0; severity < 3; ++severity)
    {
        for (const auto& list : {std::span<const StatusMessage>{messages},
                 std::span<const StatusMessage>{g_frame.problems}})
            for (const auto& message : list)
                if (HasText(message) && SeverityIndex(message.severity) == severity) visit(message);
    }
}

[[nodiscard]] dmui::SettingDescriptor Describe(const FieldText& text)
{
    dmui::SettingDescriptor setting;
    setting.id = text.id;
    setting.label = text.label;
    if (text.description != nullptr) setting.description = text.description;
    return setting;
}

}  // namespace

SettingsFrame& CurrentSettingsFrame() noexcept { return g_frame; }

void RequestSettingsCommit() noexcept
{
    if (g_frame.commitRequested != nullptr) *g_frame.commitRequested = true;
}

void PublishOnRelease(const dmui::SettingEditEvent& edit)
{
    if (edit.completed) RequestSettingsCommit();
}

void PublishWhileDragging(const dmui::SettingEditEvent& edit)
{
    if (edit.changed || edit.completed) RequestSettingsCommit();
}

SettingsPageBuilder::SettingsPageBuilder(BuiltSettingsPage& built,
    const HdrSettings& defaults, bool sdr) :
    built_(built), page_(built.page), reverts_(built.reverts), defaults_(defaults), sdr_(sdr)
{
    // Edits apply live and save automatically, so the header needs no Apply.
    page_.actions.showReset = true;
    page_.actions.revert = [&reverts = reverts_] {
        for (const auto& revert : reverts) revert();
        RequestSettingsCommit();
    };
    page_.actionTooltips.reset = "Restores every setting on this page to its default.";
    page_.actionTooltips.revert = "Restores this page's values from when the menu opened.";
}

void SettingsPageBuilder::WithoutPageActions() noexcept
{
    page_.actions = {};
    page_.actionTooltips = {};
}

void SettingsPageBuilder::AlsoOnReset(std::function<void()> reset)
{
    page_.actions.reset = [&page = page_, reset = std::move(reset)] {
        page.ResetToDefaults();
        reset();
        RequestSettingsCommit();
    };
}

std::function<bool()> SettingsPageBuilder::Filtering() const
{
    return [&filter = page_.filter] { return !filter.search.empty() || filter.modifiedOnly; };
}

void SettingsPageBuilder::DrawAfterRows(std::function<void()> draw)
{
    built_.drawAfter = std::move(draw);
}

void SettingsPageBuilder::Group(const char* id, const char* label, const char* icon, bool expanded)
{
    dmui::SettingGroup group{.id = id, .label = label, .expanded = expanded};
    // An explicit name keeps the host from guessing a heading icon.
    if (g_frame.client != nullptr)
        group.glyph = g_frame.client->ResolveIconGlyph(label, icon).value_or(0);
    page_.groups.push_back(std::move(group));
}

void SettingsPageBuilder::Status(std::function<StatusList()> messages)
{
    Group("status", "Status", "info");
    Messages(std::move(messages));
}

dmui::SettingDescriptor& SettingsPageBuilder::Add(dmui::SettingDescriptor setting)
{
    if (page_.groups.empty()) Group("settings", "Settings", "sliders");
    auto& settings = page_.groups.back().settings;
    settings.push_back(std::move(setting));
    return settings.back();
}

dmui::SettingDescriptor& SettingsPageBuilder::Number(const FieldText& text,
    const SliderRange& range, SettingField<double> field)
{
    const auto scale = range.displayScale;
    const auto defaultValue = field(defaults_);
    auto setting = Describe(text);
    setting.control = dmui::DoubleSettingControl{
        .range = dmui::NumericSettingRange<double>{range.minimum * scale, range.maximum * scale},
        .format = range.format};
    setting.defaultValue = defaultValue * scale;
    setting.binding = dmui::BindSetting(
        [field, scale] { return field(*g_frame.draft) * scale; },
        [field, scale, defaultValue](double shown) {
            auto& value = field(*g_frame.draft);
            // A scaled Reset lands on the stored default exactly, not a rounding of it.
            value = shown == defaultValue * scale ? defaultValue : shown / scale;
            return value * scale;
        });
    setting.applyTiming = dmui::SettingApplyTiming::kImmediate;
    setting.isModified = [field, defaultValue] { return field(*g_frame.draft) != defaultValue; };
    setting.isDirty = [field] { return field(*g_frame.draft) != field(*g_frame.session); };
    setting.onEdit = range.publish == Publish::whileDragging ? PublishWhileDragging : PublishOnRelease;
    reverts_.push_back([field] { field(*g_frame.draft) = field(*g_frame.session); });
    return Add(std::move(setting));
}

dmui::SettingDescriptor& SettingsPageBuilder::Toggle(const FieldText& text, SettingField<bool> field)
{
    const auto defaultValue = field(defaults_);
    auto setting = Describe(text);
    setting.control = dmui::CheckboxSettingControl{};
    setting.defaultValue = defaultValue;
    setting.binding = dmui::BindSetting(
        [field] { return field(*g_frame.draft); },
        [field](bool value) { return field(*g_frame.draft) = value; });
    setting.applyTiming = dmui::SettingApplyTiming::kImmediate;
    setting.isModified = [field, defaultValue] { return field(*g_frame.draft) != defaultValue; };
    setting.isDirty = [field] { return field(*g_frame.draft) != field(*g_frame.session); };
    setting.onEdit = PublishOnRelease;
    reverts_.push_back([field] { field(*g_frame.draft) = field(*g_frame.session); });
    return Add(std::move(setting));
}

dmui::SettingDescriptor& SettingsPageBuilder::Custom(dmui::SettingDescriptor setting,
    std::function<void()> revert)
{
    if (revert) reverts_.push_back(std::move(revert));
    return Add(std::move(setting));
}

dmui::SettingDescriptor& SettingsPageBuilder::Value(const FieldText& text, std::function<void()> draw)
{
    auto setting = Describe(text);
    setting.control = dmui::ReadOnlySettingControl{std::move(draw)};
    setting.showReset = false;
    return Add(std::move(setting));
}

void SettingsPageBuilder::Note(const char* id, const char* text)
{
    auto& note = Value({id, ""}, [text] {
        (void)dmui::DrawStyledText(*g_frame.client, text, {.tone = dmui::TextTone::kMuted, .wrapped = true});
    });
    note.presentation = {dmui::RowPresentation::LabelMode::kHidden,
        dmui::RowPresentation::Layout::kFullSpan};
}

void SettingsPageBuilder::Messages(std::function<StatusList()> messages)
{
    const auto list = [messages = std::move(messages)] {
        return messages ? messages() : StatusList{};
    };
    auto& row = Value({"status-messages", ""}, [list] {
        ForEachMessage(list(), DrawStatusMessage);
    });
    row.presentation = {dmui::RowPresentation::LabelMode::kHidden,
        dmui::RowPresentation::Layout::kFullSpan};
    row.isVisible = [list] {
        auto any = false;
        ForEachMessage(list(), [&any](const StatusMessage&) { any = true; });
        return any;
    };
}

void SettingsPageBuilder::Action(const FieldText& text, const char* button,
    std::function<void()> activate, std::function<bool()> visible)
{
    if (page_.groups.empty()) Group("settings", "Settings", "sliders");
    page_.groups.back().actionRows.push_back({.id = text.id, .label = text.label,
        .buttonLabel = button, .description = text.description ? text.description : "",
        .activate = std::move(activate), .isVisible = std::move(visible)});
}

}  // namespace fo4hdr
