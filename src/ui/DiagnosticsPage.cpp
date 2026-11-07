// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SettingsPages.hpp"

#include "SettingsStatus.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>

namespace fo4hdr {
namespace {

namespace ui = dmui::ui;
using Text = std::array<char, 160>;
using Count = unsigned long long;

[[nodiscard]] const SettingsMenuView& View() noexcept { return *CurrentSettingsFrame().view; }
[[nodiscard]] const SettingsMenuRenderer& Renderer() noexcept { return *CurrentSettingsFrame().renderer; }

[[nodiscard]] const char* PersistenceStateName(SettingsPersistenceState state) noexcept
{
    constexpr std::array<const char*, 5> names{"unavailable", "idle", "saving", "saved", "failed"};
    const auto index = static_cast<std::size_t>(state);
    return index < names.size() ? names[index] : "unknown";
}

// A read-only row whose text the page formats each frame.
template <class Format>
void AddFormattedRow(SettingsPageBuilder& page, const FieldText& text, Format format)
{
    (void)page.Value(text, [format] {
        Text value{};
        format(value);
        ui::TextUnformatted(value.data());
    });
}

void AddTextRow(SettingsPageBuilder& page, const FieldText& text, const char* (*value)())
{
    (void)page.Value(text, [value] { ui::TextUnformatted(value()); });
}

void AddStatusRows(SettingsPageBuilder& page)
{
    page.Group("renderer-status", "Renderer", "cpu");
    AddTextRow(page, {"output", "Output"}, [] {
        const auto& view = View();
        return view.sdrMode ? "SDR" : view.hdrActive ? "HDR10 active" : "HDR10 inactive";
    });
    AddFormattedRow(page, {"tone-mapper", "Tone Mapper"}, [](Text& text) {
        const auto& view = View();
        std::snprintf(text.data(), text.size(), "%s at %.0f nits%s",
            SettingsOperatorDisplayName(view.activeSettings.productionAlgorithm),
            view.activeSettings.displayPeakNits, view.lutPending ? " (replacement pending)" : "");
    });
    AddFormattedRow(page, {"settings-generation", "Settings"}, [](Text& text) {
        const auto& view = View();
        std::snprintf(text.data(), text.size(), "active %llu, desired %llu, request %llu",
            Count{view.settingsGeneration}, Count{view.desiredSettingsGeneration},
            Count{view.lutRequestGeneration});
    });
    AddFormattedRow(page, {"resources", "Resources"}, [](Text& text) {
        std::snprintf(text.data(), text.size(), "generation %llu, frame %llu",
            Count{View().resourceGeneration}, Count{View().frame});
    });
    AddTextRow(page, {"render-thread", "Render Thread"}, [] {
        return View().threadCompatible ? "Compatible" : "Disabled: draw and Present threads differ";
    });
    AddTextRow(page, {"renderer-failure", "Renderer Failure"}, [] { return View().rendererFailure; });
    AddTextRow(page, {"live-settings", "Live Settings Error"}, [] {
        return View().liveSettingsError ? "Yes" : "No";
    });
    AddTextRow(page, {"saving", "Saving"}, [] {
        return PersistenceStateName(ReadSettingsPersistenceSnapshot().state);
    });
    AddTextRow(page, {"armor-hud", "Armor HUD"}, [] { return View().armorEncodedStatus; });
}

// A session-only switch that asks the renderer instead of editing a setting.
[[nodiscard]] dmui::SettingDescriptor SessionSwitch(const FieldText& text, bool (*get)(),
    void (*set)(bool))
{
    dmui::SettingDescriptor setting{.id = text.id, .label = text.label,
        .description = text.description, .control = dmui::CheckboxSettingControl{},
        .defaultValue = false};
    setting.binding = dmui::BindSetting(get, [set](bool value) {
        set(value);
        return value;
    });
    setting.applyTiming = dmui::SettingApplyTiming::kImmediate;
    setting.showReset = false;
    return setting;
}

void AddExposure(SettingsPageBuilder& page)
{
    page.Group("exposure", "Exposure", "aperture");
    auto& lock = page.Custom(SessionSwitch({"exposure-lock", "Lock Vanilla Exposure",
        "Holds the qualified GPU exposure so gameplay can be compared. Session only; "
        "focus loss, loading or HDR retirement releases it."},
        [] { return View().exposureRequested; },
        [](bool requested) { Renderer().requestExposure(requested); }), {});
    lock.resolveFeedback = []() -> std::optional<dmui::FieldFeedback> {
        const auto& view = View();
        const char* note = view.exposureFailed ?
            "Unavailable: resources or vanilla source qualification failed." :
            view.exposureRequested && !view.exposureActive ?
                "Waiting for a qualified scene capture." : nullptr;
        if (note == nullptr) return std::nullopt;
        return dmui::FieldFeedback{dmui::FieldFeedbackSeverity::kWarning, note};
    };
}

void CopyPipboyReport() noexcept
{
    const auto& view = View();
    const auto& pipboy = view.pipboy.state;
    const auto& rejections = view.pipboy.rejections;
    std::array<char, 4096> report{};
    int written = std::snprintf(report.data(), report.size(),
        "Pip-Boy lineage observation only; no content/write/complete-coverage proof.\n"
        "status=%s active=%u resourceGeneration=%llu contextGeneration=%llu frame=%llu\n"
        "producer=%llu consumer=%llu matched=%llu unmatched=%llu pending=%llu rejected=%llu\n"
        "lastProducer=%llu lastConsumer=%llu lastMatch=%llu matchedProducerAge=%llu\n"
        "producerResourceChanges=%llu\n"
        "reject missingSRV=%llu SRVContract=%llu missingRTV=%llu RTVContract=%llu alias=%llu predicated=%llu\n"
        "expected: texture=876x700 format=28 mips=1 array=1 samples=1 quality=0; "
        "SRV dimension=4 format=28 mip=0 count=1(or UINT_MAX); RTV dimension=4 format=28 mip=0\n",
        view.pipboy.status, unsigned(pipboy.requested), Count{pipboy.generation},
        Count{pipboy.contextGeneration}, Count{view.frame}, Count{pipboy.producerDraws},
        Count{pipboy.consumerDraws}, Count{pipboy.matchedConsumers}, Count{pipboy.unmatchedConsumers},
        Count{pipboy.pendingConsumers}, Count{pipboy.rejectedDraws}, Count{pipboy.lastProducerFrame},
        Count{pipboy.lastConsumerFrame}, Count{pipboy.lastMatchedFrame}, Count{pipboy.lastProducerAge},
        Count{view.pipboy.producerResourceChanges}, Count{rejections[0]}, Count{rejections[1]},
        Count{rejections[2]}, Count{rejections[3]}, Count{rejections[4]}, Count{rejections[5]});
    const std::array views{&view.pipboy.producerInput, &view.pipboy.producerOutput,
        &view.pipboy.consumerInput};
    constexpr std::array names{"producer t0", "producer RTV0", "consumer t4"};
    for (std::size_t i = 0; i < views.size() && written > 0 &&
        static_cast<std::size_t>(written) < report.size(); ++i)
    {
        const auto& v = *views[i];
        written += std::snprintf(report.data() + written, report.size() - written,
            "%s actual frame=%llu: texture=%ux%u format=%u mips=%u array=%u samples=%u quality=%u; "
            "view dimension=%u format=%u mip=%u count=%u\n", names[i], Count{v.frame},
            v.texture.Width, v.texture.Height, unsigned(v.texture.Format), v.texture.MipLevels,
            v.texture.ArraySize, v.texture.SampleDesc.Count, v.texture.SampleDesc.Quality,
            v.dimension, unsigned(v.format), v.mip, v.mipCount);
    }
    ui::SetClipboardText(report.data());
}

void AddPipboy(SettingsPageBuilder& page)
{
    page.Group("pipboy", "Pip-Boy", "device-mobile");
    (void)page.Custom(SessionSwitch({"pipboy-observation", "Observe Pip-Boy Screen",
        "Records Pip-Boy shader and resource lineage without changing color. Session only."},
        [] { return View().pipboy.state.requested; },
        [](bool observe) { Renderer().requestPipboyObservation(observe); }), {});
    AddTextRow(page, {"pipboy-status", "Status"}, [] { return View().pipboy.status; });
    AddFormattedRow(page, {"pipboy-draws", "Draws"}, [](Text& text) {
        const auto& pipboy = View().pipboy.state;
        std::snprintf(text.data(), text.size(),
            "producer %llu, consumer %llu, matched %llu, unmatched %llu, pending %llu, rejected %llu",
            Count{pipboy.producerDraws}, Count{pipboy.consumerDraws}, Count{pipboy.matchedConsumers},
            Count{pipboy.unmatchedConsumers}, Count{pipboy.pendingConsumers}, Count{pipboy.rejectedDraws});
    });
    AddFormattedRow(page, {"pipboy-frames", "Last Frames"}, [](Text& text) {
        const auto& pipboy = View().pipboy.state;
        std::snprintf(text.data(), text.size(), "producer %llu, consumer %llu, match %llu, age %llu",
            Count{pipboy.lastProducerFrame}, Count{pipboy.lastConsumerFrame},
            Count{pipboy.lastMatchedFrame}, Count{pipboy.lastProducerAge});
    });
    page.Action({"pipboy-report", "Report", "Copies the current observation to the clipboard."},
        "Copy", CopyPipboyReport);
}

}  // namespace

void BuildRendererPage(SettingsPageBuilder& page)
{
    page.WithoutPageActions();
    page.Status();
    AddStatusRows(page);
    if (!page.Sdr()) AddExposure(page);
    AddPipboy(page);
}

void DrawExposureOverlay(const SettingsMenuView& view) noexcept
{
    ui::TextUnformatted(view.exposureActive ?
        "Exposure snapshot held (GPU-valid samples only)" :
        "Exposure lock: waiting for a qualified scene");
}

}  // namespace fo4hdr
