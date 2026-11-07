// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SettingsPages.hpp"

#include "SettingsStatus.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"

#include <array>
#include <cstddef>
#include <cstdio>

namespace fo4hdr {
namespace {

namespace ui = dmui::ui;
using Text = std::array<char, 160>;
using Count = unsigned long long;

[[nodiscard]] const char* PersistenceStateName(SettingsPersistenceState state) noexcept
{
    constexpr std::array<const char*, 5> names{"unavailable", "idle", "saving", "saved", "failed"};
    const auto index = static_cast<std::size_t>(state);
    return index < names.size() ? names[index] : "unknown";
}

void DrawStatusRows(SettingsPage& page) noexcept
{
    const auto& view = page.view;
    Text text{};
    DrawValueRow(page, {"output", "Output"},
        view.sdrMode ? "SDR" : view.hdrActive ? "HDR10 active" : "HDR10 inactive");
    std::snprintf(text.data(), text.size(), "%s at %.0f nits%s",
        SettingsOperatorDisplayName(view.activeSettings.productionAlgorithm),
        view.activeSettings.displayPeakNits, view.lutPending ? " (replacement pending)" : "");
    DrawValueRow(page, {"tone-mapper", "Tone Mapper"}, text.data());
    std::snprintf(text.data(), text.size(), "active %llu, desired %llu, request %llu",
        Count{view.settingsGeneration}, Count{view.desiredSettingsGeneration},
        Count{view.lutRequestGeneration});
    DrawValueRow(page, {"settings-generation", "Settings"}, text.data());
    std::snprintf(text.data(), text.size(), "generation %llu, frame %llu",
        Count{view.resourceGeneration}, Count{view.frame});
    DrawValueRow(page, {"resources", "Resources"}, text.data());
    DrawValueRow(page, {"render-thread", "Render Thread"},
        view.threadCompatible ? "Compatible" : "Disabled: draw and Present threads differ");
    DrawValueRow(page, {"renderer-failure", "Renderer Failure"}, view.rendererFailure);
    DrawValueRow(page, {"live-settings", "Live Settings Error"}, view.liveSettingsError ? "Yes" : "No");
    DrawValueRow(page, {"saving", "Saving"}, PersistenceStateName(ReadSettingsPersistenceSnapshot().state));
    DrawValueRow(page, {"armor-hud", "Armor HUD"}, view.armorEncodedStatus);
}

void DrawExposureLock(SettingsPage& page, const SettingsMenuRenderer& renderer) noexcept
{
    const auto& view = page.view;
    auto lock = view.exposureRequested;
    const char* note = view.exposureFailed ?
        "Unavailable: resources or vanilla source qualification failed." :
        view.exposureRequested && !view.exposureActive ? "Waiting for a qualified scene capture." : nullptr;
    if (DrawCheckboxField(page, {"exposure-lock", "Lock Vanilla Exposure",
            "Holds the qualified GPU exposure so gameplay can be compared. Session only; "
            "focus loss, loading or HDR retirement releases it."},
            {lock, false, {dmui::FieldFeedbackSeverity::kWarning, note}}))
        renderer.requestExposure(lock);
}

void CopyPipboyReport(const SettingsMenuView& view) noexcept
{
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

void DrawPipboyObservation(SettingsPage& page, const SettingsMenuRenderer& renderer) noexcept
{
    const auto& view = page.view;
    const auto& pipboy = view.pipboy.state;
    auto observe = pipboy.requested;
    if (DrawCheckboxField(page, {"pipboy-observation", "Observe Pip-Boy Screen",
            "Records Pip-Boy shader and resource lineage without changing color. Session only."},
            {observe, false}))
        renderer.requestPipboyObservation(observe);
    DrawValueRow(page, {"pipboy-status", "Status"}, view.pipboy.status);
    Text text{};
    std::snprintf(text.data(), text.size(),
        "producer %llu, consumer %llu, matched %llu, unmatched %llu, pending %llu, rejected %llu",
        Count{pipboy.producerDraws}, Count{pipboy.consumerDraws}, Count{pipboy.matchedConsumers},
        Count{pipboy.unmatchedConsumers}, Count{pipboy.pendingConsumers}, Count{pipboy.rejectedDraws});
    DrawValueRow(page, {"pipboy-draws", "Draws"}, text.data());
    std::snprintf(text.data(), text.size(), "producer %llu, consumer %llu, match %llu, age %llu",
        Count{pipboy.lastProducerFrame}, Count{pipboy.lastConsumerFrame},
        Count{pipboy.lastMatchedFrame}, Count{pipboy.lastProducerAge});
    DrawValueRow(page, {"pipboy-frames", "Last Frames"}, text.data());
    dmui::FieldScope field{page.client, "pipboy-report", "Report",
        "Copies the current observation to the clipboard."};
    if (field.Visible() && ui::Button("Copy")) CopyPipboyReport(view);
}

}  // namespace

void DrawRendererPage(SettingsPage& page, const SettingsMenuRenderer& renderer) noexcept
{
    DrawSection(page, "renderer-status", "Status", [&] { DrawStatusRows(page); });
    if (!page.sdr)
        DrawSection(page, "exposure", "Exposure", [&] { DrawExposureLock(page, renderer); });
    DrawSection(page, "pipboy", "Pip-Boy", [&] { DrawPipboyObservation(page, renderer); });
}

void DrawExposureOverlay(const SettingsMenuView& view) noexcept
{
    ui::TextUnformatted(view.exposureActive ?
        "Exposure snapshot held (GPU-valid samples only)" :
        "Exposure lock: waiting for a qualified scene");
}

}  // namespace fo4hdr
