// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/ui/SettingsWindow.hpp"
#include "ToneMapperPanel.hpp"
#include "SettingsPanelSlider.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/ui/HdrSettingsInput.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <utility>
namespace fo4hdr {
namespace {
using detail::DrawRestoreDefaultButton;
using detail::DrawSettingsControlLabel;
using detail::DrawSettingsSlider;
using detail::SettingsSliderRange;
constexpr const char* kPositionSaveError = "Window position could not be saved. Move the window to retry.";

struct SettingsWindowState {
    HdrSettings sessionSettings{};
    HdrSettings uiDraft{};
    HdrInterfaceSettings sessionInterfaceSettings{};
    HdrInterfaceSettings uiInterfaceDraft{};
    ImGuiStyle baseImGuiStyle{};
    bool uiCommitRequested{};
    bool settingsInteractionError{};
    bool settingsWindowDesiredOpen{};
    bool persistenceRequestError{};
    bool interfaceStylePending{};
    bool interfaceWindowSizePending{true};
    bool interfaceWindowCenterPending{};
    bool interfaceWindowPositionPending{true};
    bool windowPositionSavePending{};
    std::uint64_t windowPositionChangedAt{};
    SettingsAutosave autosave;
};

void DrawExposureLockDiagnostic(SettingsWindowView& view, const SettingsWindowActions& actions) noexcept
{
    bool lockExposure = view.exposureRequested;
    if (ImGui::Checkbox("Lock Native Exposure (Session Only)", &lockExposure))
    {
        actions.requestExposure(lockExposure);
        actions.refreshView(view);
    }
    ImGui::SetItemTooltip("Captures the qualified GPU exposure and scene-filter reference.\n"
        "Takes effect next frame; close settings to compare gameplay.\n"
        "Focus loss, loading, or HDR retirement releases it.\nInvalid GPU samples "
        "use live exposure; no numeric readback is performed.");
    if (view.exposureFailed)
        ImGui::TextWrapped("Exposure lock unavailable: resources or native source qualification failed.");
    else if (view.exposureRequested && !view.exposureActive)
        ImGui::TextUnformatted("Exposure lock: waiting for qualified scene capture");
}

void DrawRendererGenerations(const SettingsWindowView& view) noexcept
{
    ImGui::Text("Resource generation: %llu  Frame: %llu",
        static_cast<unsigned long long>(
            view.resourceGeneration),
        static_cast<unsigned long long>(view.frame));
    ImGui::Text("Settings: active=%llu desired=%llu request=%llu",
        static_cast<unsigned long long>(view.settingsGeneration),
        static_cast<unsigned long long>(
            view.desiredSettingsGeneration),
        static_cast<unsigned long long>(view.lutRequestGeneration));
    ImGui::Text("Tone mapper: %s at %.0f nits%s",
        SettingsOperatorDisplayName(
            view.activeSettings.productionAlgorithm),
        view.activeSettings.displayPeakNits,
        view.lutPending ? " (replacement pending)" : "");
}

void DrawRendererDiagnosticStatus(const SettingsWindowView& view) noexcept
{
    DrawRendererGenerations(view);
    ImGui::Text("Input: %s | dropped: %llu",
        HdrSettingsInputInstalled() ? "owned" : "unavailable",
        static_cast<unsigned long long>(
            HdrSettingsInputOverflowCount()));
    ImGui::Text("Live-settings render thread: %s",
        view.threadCompatible ?
            "compatible" : "disabled (draw/Present mismatch)");
    ImGui::Text("HDR state: %s",
        view.hdrActive ? "active" :
            "inactive");
    ImGui::Separator();
}

void CopyPipboyObservationReport(const SettingsWindowView& view, const SettingsPipboyObservation& pipboy) noexcept
{
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
        view.pipboy.status, unsigned(pipboy.requested),
        static_cast<unsigned long long>(pipboy.generation),
        static_cast<unsigned long long>(pipboy.contextGeneration),
        static_cast<unsigned long long>(view.frame),
        static_cast<unsigned long long>(pipboy.producerDraws),
        static_cast<unsigned long long>(pipboy.consumerDraws),
        static_cast<unsigned long long>(pipboy.matchedConsumers),
        static_cast<unsigned long long>(pipboy.unmatchedConsumers),
        static_cast<unsigned long long>(pipboy.pendingConsumers),
        static_cast<unsigned long long>(pipboy.rejectedDraws),
        static_cast<unsigned long long>(pipboy.lastProducerFrame),
        static_cast<unsigned long long>(pipboy.lastConsumerFrame),
        static_cast<unsigned long long>(pipboy.lastMatchedFrame),
        static_cast<unsigned long long>(pipboy.lastProducerAge),
        static_cast<unsigned long long>(view.pipboy.producerResourceChanges),
        static_cast<unsigned long long>(view.pipboy.rejections[0]),
        static_cast<unsigned long long>(view.pipboy.rejections[1]),
        static_cast<unsigned long long>(view.pipboy.rejections[2]),
        static_cast<unsigned long long>(view.pipboy.rejections[3]),
        static_cast<unsigned long long>(view.pipboy.rejections[4]),
        static_cast<unsigned long long>(view.pipboy.rejections[5]));
    const std::array<const SettingsPipboyViewObservation*, 3> views{
        &view.pipboy.producerInput, &view.pipboy.producerOutput,
        &view.pipboy.consumerInput};
    const std::array<const char*, 3> names{"producer t0", "producer RTV0", "consumer t4"};
    for (std::size_t i = 0; i < views.size() && written > 0 &&
        static_cast<std::size_t>(written) < report.size(); ++i)
    {
        const auto& v = *views[i];
        written += std::snprintf(report.data() + written, report.size() - written,
            "%s actual frame=%llu: texture=%ux%u format=%u mips=%u array=%u samples=%u quality=%u; "
            "view dimension=%u format=%u mip=%u count=%u\n", names[i],
            static_cast<unsigned long long>(v.frame),
            v.texture.Width, v.texture.Height, unsigned(v.texture.Format),
            v.texture.MipLevels, v.texture.ArraySize, v.texture.SampleDesc.Count,
            v.texture.SampleDesc.Quality, v.dimension, unsigned(v.format), v.mip, v.mipCount);
    }
    ImGui::SetClipboardText(report.data());
}

void DrawPipboyObservationDiagnostic(SettingsWindowView& view, const SettingsWindowActions& actions) noexcept
{
    bool observePipboy = view.pipboy.state.requested;
    if (ImGui::Checkbox("Observe Pip-Boy Screen (Session Only)", &observePipboy))
    {
        actions.requestPipboyObservation(observePipboy);
        actions.refreshView(view);
    }
    ImGui::SetItemTooltip("Lineage observer only; no color changes. Captured shader identities\n"
        "and resource matches do not prove image contents or complete draw coverage.\n"
        "Loading, focus loss, or resource retirement ends the session.");
    const auto& pipboy = view.pipboy.state;
    ImGui::TextWrapped("%s", view.pipboy.status);
    ImGui::Text("Producer=%llu consumer=%llu matched=%llu unmatched=%llu pending=%llu rejected=%llu",
        static_cast<unsigned long long>(pipboy.producerDraws),
        static_cast<unsigned long long>(pipboy.consumerDraws),
        static_cast<unsigned long long>(pipboy.matchedConsumers),
        static_cast<unsigned long long>(pipboy.unmatchedConsumers),
        static_cast<unsigned long long>(pipboy.pendingConsumers),
        static_cast<unsigned long long>(pipboy.rejectedDraws));
    ImGui::Text("Last producer=%llu consumer=%llu match=%llu matched age=%llu frames",
        static_cast<unsigned long long>(pipboy.lastProducerFrame),
        static_cast<unsigned long long>(pipboy.lastConsumerFrame),
        static_cast<unsigned long long>(pipboy.lastMatchedFrame),
        static_cast<unsigned long long>(pipboy.lastProducerAge));
    if (ImGui::Button("Copy Pip-Boy Observation Report"))
    {
        CopyPipboyObservationReport(view, pipboy);
    }
    ImGui::SetItemTooltip("Copy the current Pip-Boy observations to the clipboard.");
}

struct SettingsTabs {
    SettingsWindowState& state;
    SettingsWindowView& view;
    const SettingsWindowActions& actions;
    float footerHeight;
    const SettingsPersistenceSnapshot& persistence;
    HdrSettingsInteractionState interactionState;

    void DrawInterfaceShortcut() noexcept
    {
        const HdrInterfaceSettings defaults{};
        const auto shortcut = FormatHdrSettingsHotkey(state.uiInterfaceDraft.toggleKey);
        ImGui::Text("Settings Shortcut: %s", shortcut.data());
        const bool capturing = HdrSettingsHotkeyCaptureActive();
        if (capturing)
        {
            if (ImGui::Button("Cancel Capture"))
                CancelHdrSettingsHotkeyCapture();
        }
        else if (ImGui::Button("Change Shortcut"))
            BeginHdrSettingsHotkeyCapture();
        ImGui::SetItemTooltip("%s", capturing ?
            "Cancel shortcut capture and keep the current shortcut." :
            "Capture a key, optionally with Ctrl, Alt or Shift. Escape cancels.\n"
            "Windows and system-reserved shortcuts are not accepted.");
        if (DrawRestoreDefaultButton("toggle-key",
                state.uiInterfaceDraft.toggleKey != defaults.toggleKey))
        {
            CancelHdrSettingsHotkeyCapture();
            state.uiInterfaceDraft.toggleKey = defaults.toggleKey;
            SetHdrSettingsHotkey(defaults.toggleKey);
        }
        if (HdrSettingsHotkeyCaptureActive())
            ImGui::TextWrapped("Waiting for a shortcut... Press Escape to cancel.");
    }

    void DrawInterfaceScale() noexcept
    {
        const HdrInterfaceSettings defaults{};
        constexpr const char* scaleLabels[]{
            "100%", "125%", "150%", "175%", "200%"};
        constexpr double scaleValues[]{
            1.0, 1.25, 1.5, 1.75, 2.0};
        int scaleIndex{};
        for (int index = 0;
             index < static_cast<int>(std::size(scaleValues));
             ++index)
        {
            if (state.uiInterfaceDraft.scale == scaleValues[index])
            {
                scaleIndex = index;
                break;
            }
        }
        DrawSettingsControlLabel("Interface Scale");
        if (ImGui::Combo("##interface-scale", &scaleIndex,
                scaleLabels,
                static_cast<int>(std::size(scaleLabels))))
        {
            state.uiInterfaceDraft.scale = scaleValues[scaleIndex];
            state.interfaceStylePending = true;
            state.interfaceWindowSizePending = true;
        }
        ImGui::SetItemTooltip("Scale settings text, controls and window dimensions together.");
        if (DrawRestoreDefaultButton("interface-scale",
                state.uiInterfaceDraft.scale != defaults.scale))
        {
            state.uiInterfaceDraft.scale = defaults.scale;
            state.interfaceStylePending = true;
            state.interfaceWindowSizePending = true;
        }
    }

    struct InterfaceDimensionControl
    {
        const char* label;
        const char* resetId;
        double HdrInterfaceSettings::* member;
        SettingsSliderRange range;
    };

    void DrawInterfaceDimension(const InterfaceDimensionControl& control) noexcept
    {
        const auto& [label, resetId, member, range] = control;
        auto& value = state.uiInterfaceDraft.*member;
        const auto baseline = HdrInterfaceSettings{}.*member;
        float editable = static_cast<float>(value);
        if (DrawSettingsSlider(label, &editable, range))
        {
            value = std::round(editable);
            state.interfaceWindowSizePending = true;
        }
        if (DrawRestoreDefaultButton(resetId, value != baseline))
        {
            value = baseline;
            state.interfaceWindowSizePending = true;
        }
    }

    void DrawInterfaceWindowSize() noexcept
    {
        DrawInterfaceDimension({"Window Width", "window-width", &HdrInterfaceSettings::windowWidth,
            {static_cast<float>(kMinimumSettingsWindowWidth), static_cast<float>(kMaximumSettingsWindowWidth),
                "%.0f px", "Sets the settings window width before Interface Scale is applied."}});
        DrawInterfaceDimension({"Window Height", "window-height", &HdrInterfaceSettings::windowHeight,
            {static_cast<float>(kMinimumSettingsWindowHeight), static_cast<float>(kMaximumSettingsWindowHeight),
                "%.0f px", "Sets the settings window height before Interface Scale is applied."}});
        if (ImGui::Button("Reset Size"))
        {
            state.uiInterfaceDraft.windowWidth =
                kDefaultSettingsWindowWidth;
            state.uiInterfaceDraft.windowHeight =
                kDefaultSettingsWindowHeight;
            state.interfaceWindowSizePending = true;
        }
        ImGui::SetItemTooltip("Restore the default window width and height.");
        ImGui::SameLine();
        if (ImGui::Button("Center Window"))
            state.interfaceWindowCenterPending = true;
        ImGui::SetItemTooltip("Center the settings window on the current display.");
    }

    void Draw() noexcept
    {
        if (!ImGui::BeginTabBar("FO4IsotopeTabs",
                ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton)) return;
        DrawColorSettingsTabs(state.uiDraft, state.uiCommitRequested, view, footerHeight);
        DrawInterfaceSettingsTab();
        DrawDiagnosticsSettingsTab();
        ImGui::EndTabBar();
    }

    void DrawInterfaceSettingsTab() noexcept
    {
        if (ImGui::BeginTabItem("Interface"))
        {
            ImGui::BeginChild("Interface-content", ImVec2(0.0F, -footerHeight));
            ImGui::TextWrapped(
                "Adjust the settings window and its shortcut. Drag an edge to resize. "
                "Changes save automatically.");
            ImGui::SeparatorText("Shortcut");
            DrawInterfaceShortcut();
            ImGui::SeparatorText("Window");
            DrawInterfaceScale();
            DrawInterfaceWindowSize();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }

    void DrawDiagnosticsSettingsTab() noexcept
    {
        if (view.developerMode &&
            ImGui::BeginTabItem("Diagnostics"))
        {
            ImGui::BeginChild("Diagnostics-content", ImVec2(0.0F, -footerHeight));
            ImGui::TextWrapped("Inspect renderer health and run temporary diagnostics. "
                "Exposure lock and Pip-Boy observation end on focus loss, loading or resource retirement.");
            ImGui::SeparatorText("Renderer Status");
            DrawRendererDiagnosticStatus(view);
            ImGui::Text("Renderer failure: %s",
                view.rendererFailure);
            ImGui::Text("Live settings error: %s | input control: %u%s | persistence: %u%s",
                view.liveSettingsError ? "yes" : "no",
                static_cast<unsigned>(interactionState),
                state.settingsInteractionError ? " (failed)" : "",
                static_cast<unsigned>(persistence.state),
                state.persistenceRequestError ? " (request rejected)" : "");
            ImGui::Text("Armor HUD composition: %s", view.armorEncodedStatus);
            ImGui::SeparatorText("Exposure");
            DrawExposureLockDiagnostic(view, actions);
            ImGui::SeparatorText("Pip-Boy");
            DrawPipboyObservationDiagnostic(view, actions);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }

};
SettingsWindowState g_state;
bool g_open{};
bool g_backendReady{};

std::atomic<HWND> g_inputWindow{};
std::atomic<bool> g_inputReady{};
SettingsWindowView* g_view{};
const SettingsWindowActions* g_actions{};

void QueueWindowPositionSave(bool force) noexcept
{
    if (!g_state.windowPositionSavePending) return;
    if (!force && GetTickCount64() - g_state.windowPositionChangedAt < 500U) return;
    if (RequestHdrWindowPositionSave(
            g_state.uiInterfaceDraft.windowX, g_state.uiInterfaceDraft.windowY))
        g_state.windowPositionSavePending = false;
}

void ApplySettingsWindowOpen(bool open) noexcept
{
    if (open)
    {
        g_state.sessionSettings = g_state.uiDraft;
        g_state.sessionInterfaceSettings = g_state.uiInterfaceDraft;
    }
    else
    {
        CancelHdrSettingsHotkeyCapture();
        QueueWindowPositionSave(true);
    }
    g_open = open;
}

[[nodiscard]] bool ChangeSettingsWindowOpen(bool open) noexcept
{
    g_state.settingsWindowDesiredOpen = open;
    auto interaction = ReadHdrSettingsInteractionState();
    const auto [targetState, pendingState] = open ?
        std::pair{HdrSettingsInteractionState::active, HdrSettingsInteractionState::opening} :
        std::pair{HdrSettingsInteractionState::inactive, HdrSettingsInteractionState::closing};
    auto applied = interaction == targetState;
    const auto pending = interaction == pendingState;
    if (!applied && !pending)
    {
        if (!RequestHdrSettingsInteraction(open))
        {
            g_state.settingsInteractionError = true;
            g_state.settingsWindowDesiredOpen = g_open;
            return false;
        }
        interaction = ReadHdrSettingsInteractionState();
        applied = interaction == targetState;
    }
    g_state.settingsInteractionError = false;
    if (!applied) return true;
    if (g_open == open) return true;
    ApplySettingsWindowOpen(open);
    return true;
}

HdrSettingsInteractionState UpdateSettingsWindowInteraction() noexcept
{
    auto interactionState = ReadHdrSettingsInteractionState();
    if (g_state.settingsWindowDesiredOpen != g_open)
    {
        if (interactionState == HdrSettingsInteractionState::failed ||
            interactionState == HdrSettingsInteractionState::unavailable)
        {
            g_state.settingsInteractionError = true;
            g_state.settingsWindowDesiredOpen = g_open;
        }
        else
            (void)ChangeSettingsWindowOpen(g_state.settingsWindowDesiredOpen);
    }
    if (ConsumeHdrSettingsFocusLoss())
    {
        g_actions->focusLost();
        g_actions->refreshView(*g_view);

        (void)ChangeSettingsWindowOpen(false);
    }
    if (ConsumeHdrSettingsToggle())
    {
        (void)ChangeSettingsWindowOpen(
            !g_state.settingsWindowDesiredOpen);
    }
    interactionState = ReadHdrSettingsInteractionState();
    return interactionState;
}

void BeginSettingsFrame(ImGuiIO& io) noexcept
{
    io.MouseDrawCursor = true;
    DrainHdrSettingsInput();
    if (g_state.interfaceStylePending)
    {
        ImGui::GetStyle() = g_state.baseImGuiStyle;
        ImGui::GetStyle().ScaleAllSizes(
            static_cast<float>(g_state.uiInterfaceDraft.scale));
        ImGui::GetStyle().FontScaleMain =
            static_cast<float>(g_state.uiInterfaceDraft.scale);
        g_state.interfaceStylePending = false;
    }
    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    HdrSettingsHotkey capturedKey{};
    if (ConsumeHdrSettingsHotkeyCapture(capturedKey))
    {
        g_state.uiInterfaceDraft.toggleKey = capturedKey;
        SetHdrSettingsHotkey(capturedKey);
    }

}

void PrepareSettingsWindowLayout(const ImGuiViewport* viewport, float interfaceScale) noexcept
{
    const auto maximumWindowSize = ImVec2(
        std::max(320.0F, viewport->WorkSize.x - 32.0F),
        std::max(280.0F, viewport->WorkSize.y - 32.0F));
    const auto minimumWindowSize = ImVec2(
        std::min(static_cast<float>(kMinimumSettingsWindowWidth) *
                     interfaceScale,
            maximumWindowSize.x),
        std::min(static_cast<float>(kMinimumSettingsWindowHeight) *
                     interfaceScale,
            maximumWindowSize.y));
    ImGui::SetNextWindowSizeConstraints(
        minimumWindowSize, maximumWindowSize);
    if (g_state.interfaceWindowSizePending)
    {
        ImGui::SetNextWindowSize(ImVec2(
            std::min(static_cast<float>(g_state.uiInterfaceDraft.windowWidth) *
                         interfaceScale,
                maximumWindowSize.x),
            std::min(static_cast<float>(g_state.uiInterfaceDraft.windowHeight) *
                         interfaceScale,
                maximumWindowSize.y)));
        g_state.interfaceWindowSizePending = false;
    }
    const bool positionNeedsCentering = g_state.interfaceWindowPositionPending && g_state.uiInterfaceDraft.windowX < 0.0;
    if (g_state.interfaceWindowCenterPending || positionNeedsCentering)
    {
        ImGui::SetNextWindowPos(ImVec2(
            viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
            viewport->WorkPos.y + viewport->WorkSize.y * 0.5F),
            ImGuiCond_Always, ImVec2(0.5F, 0.5F));
        g_state.interfaceWindowCenterPending = false;
        g_state.interfaceWindowPositionPending = false;
    }
    else if (g_state.interfaceWindowPositionPending)
    {
        ImGui::SetNextWindowPos(ImVec2(
            viewport->WorkPos.x + static_cast<float>(g_state.uiInterfaceDraft.windowX),
            viewport->WorkPos.y + static_cast<float>(g_state.uiInterfaceDraft.windowY)));
        g_state.interfaceWindowPositionPending = false;
    }

}

bool ExposureLockVisible() noexcept
{
    return g_view->exposureRequested || g_view->exposureActive;
}

bool DrawClosedSettingsOverlay() noexcept
{
    QueueWindowPositionSave(true);
    SetHdrSettingsInputCapture(false, false, false);
    if (g_view->developerMode && ExposureLockVisible())
    {
        // Visible during gameplay without taking cursor or input ownership.
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = false;
        ImGui_ImplWin32_NewFrame();
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        constexpr auto lockFlags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("HDR color diagnostics", nullptr, lockFlags))
        {
            if (ExposureLockVisible())
                ImGui::TextUnformatted(g_view->exposureActive ?
                    "Exposure snapshot held (GPU-valid samples only)" :
                    "Exposure lock: waiting for qualified scene");
        }
        ImGui::End();
        ImGui::Render();
        return true;
    }
    return false;
}

void UpdateSettingsWindowPosition(const ImGuiViewport* viewport) noexcept
{
    const auto windowSize = ImGui::GetWindowSize();
    // Clamp in current client space on every size/display transition,
    // keeping the complete window reachable without storing monitor IDs.
    const auto position = ImGui::GetWindowPos();
    const auto x = std::clamp(position.x - viewport->WorkPos.x, 0.0F,
        std::max(0.0F, viewport->WorkSize.x - windowSize.x));
    const auto y = std::clamp(position.y - viewport->WorkPos.y, 0.0F,
        std::max(0.0F, viewport->WorkSize.y - windowSize.y));
    if (position.x != viewport->WorkPos.x + x ||
        position.y != viewport->WorkPos.y + y)
        ImGui::SetWindowPos(ImVec2(viewport->WorkPos.x + x,
            viewport->WorkPos.y + y));
    if (g_state.uiInterfaceDraft.windowX != std::round(x) ||
        g_state.uiInterfaceDraft.windowY != std::round(y))
    {
        g_state.uiInterfaceDraft.windowX = std::round(x);
        g_state.uiInterfaceDraft.windowY = std::round(y);
        g_state.windowPositionChangedAt = GetTickCount64();
        g_state.windowPositionSavePending = true;
    }
}

void UpdateSettingsWindowSize(float interfaceScale) noexcept
{
    const auto windowSize = ImGui::GetWindowSize();
    if (!g_state.interfaceWindowSizePending)
    {
        g_state.uiInterfaceDraft.windowWidth = std::clamp(
            static_cast<double>(
                std::round(windowSize.x / interfaceScale)),
            kMinimumSettingsWindowWidth,
            kMaximumSettingsWindowWidth);
        g_state.uiInterfaceDraft.windowHeight = std::clamp(
            static_cast<double>(
                std::round(windowSize.y / interfaceScale)),
            kMinimumSettingsWindowHeight,
            kMaximumSettingsWindowHeight);
    }

}

void RevertSettingsDraft() noexcept
{
    g_state.uiDraft = g_state.sessionSettings;
    const auto positionX = g_state.uiInterfaceDraft.windowX;
    const auto positionY = g_state.uiInterfaceDraft.windowY;
    g_state.uiInterfaceDraft = g_state.sessionInterfaceSettings;
    g_state.uiInterfaceDraft.windowX = positionX;
    g_state.uiInterfaceDraft.windowY = positionY;
    CancelHdrSettingsHotkeyCapture();
    SetHdrSettingsHotkey(g_state.uiInterfaceDraft.toggleKey);
    g_state.interfaceStylePending = true;
    g_state.interfaceWindowSizePending = true;
    g_state.uiCommitRequested = true;
}

void DrawSettingsRestoreActions(bool narrowFooter) noexcept
{
    if (ImGui::Button("HDR Defaults"))
    {
        g_state.uiDraft = HdrSettings{};
        g_state.uiCommitRequested = true;
    }
    ImGui::SetItemTooltip("Restore HDR defaults; interface settings stay unchanged.");
    if (!narrowFooter) ImGui::SameLine();
    if (ImGui::Button("Revert"))
    {
        RevertSettingsDraft();
    }
    ImGui::SetItemTooltip("Restore HDR and interface settings from when this window opened. Changes save automatically.");

}

void UpdateSettingsAutosave() noexcept
{
    g_state.autosave.Update(g_state.uiDraft, g_state.uiInterfaceDraft,
        {GetTickCount64(), !g_view->lutPending && g_view->activeSettings == g_state.uiDraft, g_open});
    g_state.persistenceRequestError = g_state.autosave.RequestFailed();
}

struct SettingsWindowContents
{
    float interfaceScale;
    const SettingsPersistenceSnapshot& persistence;
    HdrSettingsInteractionState interactionState;
    bool dirty{};

    const char* SettingsSummaryStatus() noexcept
    {
        return g_view->liveSettingsError ||
                g_state.settingsInteractionError ||
                SaveFailed() ?
            "Error" : g_view->lutPending ? "Preparing" : dirty ? "Save pending" :
            "Applied";
    }

    void DrawSettingsSummary() noexcept
    {
        const auto* settingsStatus = SettingsSummaryStatus();
        ImGui::TextWrapped("%s | HDR10 %s | %.0f-nit peak | %s",
            SettingsOperatorDisplayName(g_view->activeSettings.productionAlgorithm),
            g_view->hdrActive ? "active" : "inactive",
            g_view->activeSettings.displayPeakNits,
            settingsStatus);
        if (g_view->developerMode && g_view->exposureActive)
            ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
                "Developer exposure snapshot held (not saved)");
    
    }

    bool SaveFailed() const noexcept
    {
        return persistence.state == SettingsPersistenceState::failed ||
            persistence.state == SettingsPersistenceState::unavailable ||
            g_state.persistenceRequestError;
    }

    const char* SaveStatusText() const noexcept
    {
        if (persistence.state == SettingsPersistenceState::saving)
            return "Saving settings...";
        if (persistence.state == SettingsPersistenceState::unavailable)
            return "Automatic saving is unavailable. Current changes will not survive a restart.";
        if (SaveFailed()) return "Automatic save failed. Retrying; current settings remain active.";
        return "Changes save automatically";
    }

    void DrawSettingsSaveStatus() noexcept
    {
        ImGui::PushTextWrapPos();
        if (SaveFailed())
            ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.35F, 1.0F), "%s", SaveStatusText());
        else
            ImGui::TextDisabled("%s", SaveStatusText());
        if (persistence.positionState == SettingsPersistenceState::failed)
            ImGui::TextWrapped("%s", kPositionSaveError);
        ImGui::PopTextWrapPos();
    }

    void Draw() noexcept
    {
        UpdateSettingsWindowSize(interfaceScale);
        dirty = g_state.autosave.Dirty(g_state.uiDraft, g_state.uiInterfaceDraft);
        DrawSettingsSummary();
        const auto width = ImGui::GetContentRegionAvail().x;
        const auto narrowFooter = width < ImGui::GetFontSize() * 27.0F;
        auto footerHeight = ImGui::GetFrameHeightWithSpacing() * (narrowFooter ? 2.0F : 1.0F) +
            ImGui::CalcTextSize(SaveStatusText(), nullptr, false, width).y +
            ImGui::GetStyle().ItemSpacing.y * 2.0F;
        if (persistence.positionState == SettingsPersistenceState::failed)
            footerHeight += ImGui::CalcTextSize(kPositionSaveError, nullptr, false, width).y +
                ImGui::GetStyle().ItemSpacing.y;
        SettingsTabs{g_state, *g_view, *g_actions, footerHeight, persistence, interactionState}.Draw();
        DrawSettingsRestoreActions(narrowFooter);
        DrawSettingsSaveStatus();
    }
};

[[nodiscard]] bool DrawWindow() noexcept
{
    if (!g_backendReady) return false;
    const auto interactionState = UpdateSettingsWindowInteraction();
    UpdateSettingsAutosave();
    const auto persistence = ReadSettingsPersistenceSnapshot();
    if (!g_open) return DrawClosedSettingsOverlay();

    ImGuiIO& io = ImGui::GetIO();
    BeginSettingsFrame(io);
    auto open = g_open;
    const auto interfaceScale =
        static_cast<float>(g_state.uiInterfaceDraft.scale);
    const auto* viewport = ImGui::GetMainViewport();
    PrepareSettingsWindowLayout(viewport, interfaceScale);
    constexpr auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
    const auto showContents = ImGui::Begin("FO4Isotope", &open, flags);
    UpdateSettingsWindowPosition(viewport);
    if (showContents)
        SettingsWindowContents{interfaceScale, persistence, interactionState}.Draw();
    ImGui::End();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) QueueWindowPositionSave(false);
    if (!open) (void)ChangeSettingsWindowOpen(false);
    UpdateSettingsAutosave();
    open = g_open;
    io.MouseDrawCursor = open;
    ImGui::Render();
    SetHdrSettingsInputCapture(open, io.WantCaptureKeyboard, io.WantCaptureMouse);

    return true;
}

} // namespace

void InitializeSettingsWindow(const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept
{
    g_state.sessionSettings = settings;
    g_state.uiDraft = settings;
    g_state.sessionInterfaceSettings = interfaceSettings;
    g_state.uiInterfaceDraft = interfaceSettings;
    SetHdrSettingsHotkey(interfaceSettings.toggleKey);
    g_state.persistenceRequestError = false;
    g_state.interfaceStylePending = false;
    g_state.interfaceWindowSizePending = true;
    g_state.interfaceWindowCenterPending = false;
    g_state.interfaceWindowPositionPending = true;
    g_state.windowPositionSavePending = false;
    g_state.autosave.Initialize(settings, interfaceSettings);
}

void EnsureSettingsWindowBackend(bool requested, HWND window,
    ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (!requested || g_backendReady) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g_state.interfaceWindowPositionPending = true;
    g_state.interfaceWindowSizePending = true;
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.FontDefault = io.Fonts->AddFontDefaultVector();
    g_state.baseImGuiStyle = ImGui::GetStyle();
    ImGui::GetStyle().ScaleAllSizes(static_cast<float>(g_state.uiInterfaceDraft.scale));
    ImGui::GetStyle().FontScaleMain = static_cast<float>(g_state.uiInterfaceDraft.scale);
    const auto platformReady = window != nullptr &&
        io.FontDefault != nullptr && ImGui_ImplWin32_Init(window);
    if (!platformReady) {
        ImGui::DestroyContext();
        return;
    }
    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return;
    }
    g_backendReady = true;
    g_inputWindow.store(window, std::memory_order_release);
    g_inputReady.store(true, std::memory_order_release);
}

void ShutdownSettingsWindowBackend() noexcept
{
    g_inputReady.store(false, std::memory_order_release);
    g_inputWindow.store(nullptr, std::memory_order_release);
    ShutdownHdrSettingsInput();
    if (!g_backendReady) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_backendReady = false;
    g_open = false;
    g_state.settingsWindowDesiredOpen = false;
    g_state.settingsInteractionError = false;
}

bool SettingsWindowBackendReady() noexcept { return g_backendReady; }

SettingsInputActivation TryActivateSettingsWindowInput() noexcept
{
    SettingsInputActivation result{.currentThreadId = GetCurrentThreadId()};
    if (!g_inputReady.load(std::memory_order_acquire)) return result;
    const auto window = g_inputWindow.load(std::memory_order_acquire);
    if (window == nullptr || !IsWindow(window)) return result;
    result.ready = true;
    DWORD process{};
    result.windowThreadId = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) return result;
    result.active = InitializeHdrSettingsInput(window);
    return result;
}

bool DrawSettingsWindow(SettingsWindowView& view,
    const SettingsWindowActions& actions) noexcept
{
    g_view = &view;
    g_actions = &actions;
    const auto ready = DrawWindow();
    g_view = nullptr;
    g_actions = nullptr;
    return ready;
}

bool SetSettingsWindowOpen(bool open) noexcept { return ChangeSettingsWindowOpen(open); }
bool SettingsWindowOpen() noexcept { return g_open; }
const HdrSettings& ReadSettingsDraft() noexcept { return g_state.uiDraft; }
bool SettingsCommitRequested() noexcept { return g_state.uiCommitRequested; }
bool ConsumeSettingsCommitRequest() noexcept { return std::exchange(g_state.uiCommitRequested, false); }

} // namespace fo4hdr
