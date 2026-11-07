// SPDX-License-Identifier: GPL-3.0-only
// Compiles the GPL-3.0-only DearModdingUI API headers; the CommonLib exceptions do not apply.
#include "FO4Isotope/ui/SettingsMenu.hpp"

#include "SettingsPages.hpp"
#include "SettingsStatus.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace fo4hdr {
namespace {

// The renderer reads and commits the draft on the thread that presents; DMUI
// draws pages and runs frame observers on that thread too.
struct SettingsMenuState
{
    // The draft edits the renderer's active profile; the other profile is kept for saving.
    HdrSettings draft{};
    HdrSettings inactiveSettings{DefaultSdrSettings()};
    bool sdrProfile{};
    HdrInterfaceSettings interfaceDraft{};
    // Each page's Revert restores the values from when the menu opened.
    HdrSettings sessionSettings{};
    HdrInterfaceSettings sessionInterfaceSettings{};
    bool commitRequested{};
    bool developerMode{};
    // FO4Isotope's pages are shown.
    bool menuActive{};
    bool overlayDemanded{};
    // What the host's status line and Health page last received.
    SettingsMenuHealth health{};
    SettingsAutosave autosave;
};

SettingsMenuState g_state;
SettingsMenuRenderer g_renderer{};
std::optional<DMUI_PageHandle> g_exposureOverlay;

// Callbacks cannot be unregistered, so the client lives for the process.
dmui::Client& Client() noexcept
{
    static dmui::Client client{"fo4isotope", "Isotope", {1, 3}, "atom"};
    return client;
}

[[nodiscard]] HdrSettingsProfiles PersistedProfiles() noexcept
{
    return g_state.sdrProfile ? HdrSettingsProfiles{g_state.inactiveSettings, g_state.draft} :
        HdrSettingsProfiles{g_state.draft, g_state.inactiveSettings};
}

[[nodiscard]] const char* SaveProblemText() noexcept
{
    const auto state = ReadSettingsPersistenceSnapshot().state;
    if (state == SettingsPersistenceState::unavailable)
        return "Saving is unavailable, so changes last until the game closes.";
    if (state == SettingsPersistenceState::failed || g_state.autosave.RequestFailed())
        return "Saving failed. Retrying; the current settings stay active.";
    return nullptr;
}

// Problems that affect every page, after each page's own info and warnings.
[[nodiscard]] std::array<StatusMessage, 2> PageProblems(const SettingsMenuView& view) noexcept
{
    constexpr auto error = StatusSeverity::error;
    return {StatusMessage{LiveSettingsErrorText(view), error}, StatusMessage{SaveProblemText(), error}};
}

// Pages edit only when this callback runs on the thread FO4Isotope presents on.
[[nodiscard]] const char* UnavailableText(const SettingsMenuView& view) noexcept
{
    if (g_renderer.refreshView == nullptr || !view.presenting)
        return "The FO4Isotope renderer is not running this session. See FO4Isotope.log.";
    if (!view.drawOnPresentThread)
        return "These settings are unavailable because this menu draws on a different thread "
            "than the game presents.";
    return nullptr;
}

// A page is built for the profile the renderer draws and rebuilt when that changes.
struct ProfilePage
{
    BuiltSettingsPage built;
    std::optional<bool> sdr;
};

template <void (*Build)(SettingsPageBuilder&)>
void DrawPage()
{
    static ProfilePage profilePage;
    auto& built = profilePage.built;
    auto& client = Client();
    SettingsMenuView view;
    if (g_renderer.refreshView != nullptr) g_renderer.refreshView(view);
    ApplyHdrRequest(view, g_state.interfaceDraft.hdrEnabled);
    if (const auto* unavailable = UnavailableText(view))
    {
        std::array<char, 256> text{};
        std::snprintf(text.data(), text.size(), "Error: %s", unavailable);
        (void)dmui::DrawStyledText(client, text.data(), {.tone = dmui::TextTone::kError, .wrapped = true});
        return;
    }
    auto& frame = CurrentSettingsFrame();
    frame = {&client, &g_state.draft, &g_state.sessionSettings, &g_state.interfaceDraft.hdrEnabled,
        g_state.sessionInterfaceSettings.hdrEnabled, &view, &g_renderer, &g_state.commitRequested,
        PageProblems(view)};
    if (profilePage.sdr != g_state.sdrProfile)
    {
        built = {};
        const auto defaults = g_state.sdrProfile ? DefaultSdrSettings() : HdrSettings{};
        SettingsPageBuilder builder{built, defaults, g_state.sdrProfile};
        Build(builder);
        profilePage.sdr = g_state.sdrProfile;
    }
    built.page.Draw(client);
    if (built.drawAfter) built.drawAfter();
    frame.view = nullptr;
}

void ObservePageActivity(const dmui::PageActivity& activity) noexcept
{
    const auto opening = activity.kind == dmui::PageActivityKind::kActivated;
    if (opening)
    {
        g_state.sessionSettings = g_state.draft;
        g_state.sessionInterfaceSettings = g_state.interfaceDraft;
    }
    g_state.menuActive = activity.kind != dmui::PageActivityKind::kDeactivated;
}

// Keeps the developer overlay on screen only while the exposure lock is held.
void UpdateExposureOverlay(const SettingsMenuView& view) noexcept
{
    if (!g_exposureOverlay) return;
    const auto wanted = view.exposureRequested || view.exposureActive;
    if (wanted == g_state.overlayDemanded) return;
    auto& client = Client();
    if (wanted ? client.RequestFrame(*g_exposureOverlay) : client.ReleaseFrame(*g_exposureOverlay))
        g_state.overlayDemanded = wanted;
}

[[nodiscard]] DMUI_StatusSeverity HostSeverity(HealthSeverity severity) noexcept
{
    constexpr std::array<DMUI_StatusSeverity, 4> severities{DMUI_STATUS_SEVERITY_INFO,
        DMUI_STATUS_SEVERITY_SUCCESS, DMUI_STATUS_SEVERITY_WARNING, DMUI_STATUS_SEVERITY_ERROR};
    return severities[std::min<std::size_t>(static_cast<std::size_t>(severity), 3U)];
}

// The host's status line and Health page hear about each change once.
void UpdateHostStatus(const SettingsMenuView& view) noexcept
{
    const auto health = DescribeSettingsMenuHealth(view, SaveProblemText());
    if (health == g_state.health) return;
    auto& client = Client();
    const auto severity = HostSeverity(health.severity);
    (void)client.SetStatus(severity, health.status);
    if (health.summary != nullptr)
        (void)client.ReportDiagnostic({severity, nullptr, health.summary, health.detail});
    g_state.health = health;
}

// Runs after every Present: saves once the renderer applied the draft, at once
// after the menu closes and 500 ms after the last edit while it is open.
void ObserveFrame() noexcept
{
    SettingsMenuView view;
    if (g_renderer.refreshView != nullptr) g_renderer.refreshView(view);
    ApplyHdrRequest(view, g_state.interfaceDraft.hdrEnabled);
    const auto applied = g_renderer.draftApplied == nullptr || g_renderer.draftApplied(g_state.draft);
    g_state.autosave.Update(PersistedProfiles(), g_state.interfaceDraft,
        {GetTickCount64(), applied, g_state.menuActive});
    UpdateHostStatus(view);
    if (g_state.developerMode) UpdateExposureOverlay(view);
}

struct PageEntry
{
    dmui::PageDescriptor descriptor;
    void (*draw)();
};

// Two sidebar categories; within each, pages sort by key.
const std::array kCategories{
    dmui::CategoryDescriptor{"display", "Display", 0, "monitor"},
    dmui::CategoryDescriptor{"color", "Color", 100, "palette"}};
const std::array kPages{
    PageEntry{{.id = "output", .displayName = "Output", .categoryId = "display",
                  .summary = "HDR output and display brightness.", .sortKey = 0, .iconName = "sun"},
        &DrawPage<BuildOutputPage>},
    PageEntry{{.id = "tone-mapping", .displayName = "Tone Mapping", .categoryId = "display",
                  .summary = "Tone mapper and its look.", .sortKey = 100, .iconName = "chart-line"},
        &DrawPage<BuildToneMappingPage>},
    PageEntry{{.id = "adjustments", .displayName = "Adjustments", .categoryId = "color",
                  .summary = "Exposure, contrast, white balance and bloom.", .sortKey = 0,
                  .iconName = "sliders"},
        &DrawPage<BuildAdjustmentsPage>},
    PageEntry{{.id = "color-grading", .displayName = "Color Grading", .categoryId = "color",
                  .summary = "How the game's color grading applies.", .sortKey = 100,
                  .iconName = "palette"},
        &DrawPage<BuildColorGradingPage>},
    PageEntry{{.id = "selective-color", .displayName = "Selective Color", .categoryId = "color",
                  .summary = "Hue, saturation and brightness per color.", .sortKey = 200,
                  .iconName = "swatches"},
        &DrawPage<BuildSelectiveColorPage>},
    PageEntry{{.id = "weather-response", .displayName = "Weather Response", .categoryId = "color",
                  .summary = "Color grading response per weather.", .sortKey = 300,
                  .iconName = "cloud-sun"},
        &DrawPage<BuildWeatherResponsePage>}};

[[nodiscard]] bool RegisterPages() noexcept
{
    auto& client = Client();
    const auto addCategory = [&client](const auto& category) {
        return client.AddCategory(category);
    };
    const auto addPage = [&client](const PageEntry& page) {
        return client.AddPage(page.descriptor, page.draw).has_value();
    };
    return std::ranges::all_of(kCategories, addCategory) && std::ranges::all_of(kPages, addPage);
}

// Developer pages: renderer status, temporary diagnostics and the exposure overlay.
[[nodiscard]] bool RegisterDeveloperPages() noexcept
{
    auto& client = Client();
    if (!client.AddCategory({"diagnostics", "Diagnostics", 900, "pulse"})) return false;
    if (!client.AddPage({"renderer", "Renderer", "diagnostics",
            "Renderer state and temporary developer tools.", 0, DMUI_PAGE_KIND_SETTINGS, "cpu"},
            &DrawPage<BuildRendererPage>)) return false;
    g_exposureOverlay = client.AddPage({"exposure-overlay", "Exposure Lock", nullptr, nullptr, 0,
        DMUI_PAGE_KIND_OVERLAY, "lock"}, [] {
            SettingsMenuView view;
            if (g_renderer.refreshView != nullptr) g_renderer.refreshView(view);
            DrawExposureOverlay(view);
        });
    if (!g_exposureOverlay) return false;
    DMUI_ManagedOverlayOptions options{};
    options.anchor = DMUI_OVERLAY_ANCHOR_TOP_LEFT;
    options.offset = {12.0F, 12.0F};
    options.opacity = 0.85F;
    options.contentScale = 1.0F;
    options.backgroundVisible = 1U;
    options.allowArrangement = 1U;
    return client.ConfigureOverlay(*g_exposureOverlay, options);
}

}  // namespace

const char* SettingsMenuRegistrationName(SettingsMenuRegistration result) noexcept
{
    constexpr std::array<const char*, 4> names{
        "host-absent", "host-rejected", "registration-failed", "registered"};
    const auto index = static_cast<std::size_t>(result);
    return index < names.size() ? names[index] : "unknown";
}

void InitializeSettingsMenu(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings, bool developerMode) noexcept
{
    g_state.draft = g_state.sessionSettings = settings.hdr;
    g_state.inactiveSettings = settings.sdr;
    g_state.sdrProfile = false;
    g_state.interfaceDraft = g_state.sessionInterfaceSettings = interfaceSettings;
    g_state.developerMode = developerMode;
    g_state.autosave.Initialize(settings, interfaceSettings);
}

void SelectSettingsMenuProfile(bool sdr) noexcept
{
    if (sdr == g_state.sdrProfile) return;
    std::swap(g_state.draft, g_state.inactiveSettings);
    g_state.sessionSettings = g_state.draft;
    g_state.sdrProfile = sdr;
}

void ConnectSettingsMenuRenderer(const SettingsMenuRenderer& renderer) noexcept
{
    g_renderer = renderer;
}

SettingsMenuRegistration RegisterSettingsMenu() noexcept
{
    auto& client = Client();
    if (!client.Connect())
        return client.HostPresent() ? SettingsMenuRegistration::hostRejected :
            SettingsMenuRegistration::hostAbsent;
    constexpr auto failed = SettingsMenuRegistration::registrationFailed;
    if (!RegisterPages()) return failed;
    if (g_state.developerMode && !RegisterDeveloperPages()) return failed;
    if (!client.AddPageActivityObserver(&ObservePageActivity)) return failed;
    if (!client.AddFrameObserver(&ObserveFrame)) return failed;
    return SettingsMenuRegistration::registered;
}

const char* SettingsMenuHostUnavailableReason() noexcept
{
    switch (Client().UnavailableReason())
    {
    case DMUI_UNAVAILABLE_HOST_DISABLED: return "host-disabled";
    case DMUI_UNAVAILABLE_BACKEND_FAILED: return "backend-failed";
    default: return nullptr;
    }
}

const HdrSettings& ReadSettingsDraft() noexcept { return g_state.draft; }
bool SettingsCommitRequested() noexcept { return g_state.commitRequested; }
bool ConsumeSettingsCommitRequest() noexcept { return std::exchange(g_state.commitRequested, false); }

}  // namespace fo4hdr
