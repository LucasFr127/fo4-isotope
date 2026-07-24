// SPDX-License-Identifier: GPL-3.0-or-later

#include "SettingsPanel.hpp"
#include "LiveSettings.hpp"
#include "TestHarness.hpp"

#include <imgui.h>

namespace fo4hdr::tests {
namespace {

class SettingsFrameContext
{
public:
    SettingsFrameContext()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {640, 1000};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* pixels{};
        int width{};
        int height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        FO4HDR_CHECK(pixels != nullptr);
        FO4HDR_CHECK(width > 0);
        FO4HDR_CHECK(height > 0);
    }

    ~SettingsFrameContext() { ImGui::DestroyContext(); }
};

void DispatchPanelActions(SettingsPanelActions actions, LiveSettings& live,
    const PluginConfiguration& editable)
{
    if (actions.request)
        FO4HDR_CHECK(live.Request(editable) == LiveSettingsRequestStatus::unavailable);
    if (actions.save) FO4HDR_CHECK(!live.RequestSave());
}

void CheckDrawData()
{
    const auto* data = ImGui::GetDrawData();
    FO4HDR_CHECK(data != nullptr);
    if (!data) return;
    FO4HDR_CHECK(data->CmdListsCount > 0);
    FO4HDR_CHECK(data->TotalVtxCount > 0);
    FO4HDR_CHECK(data->TotalIdxCount > 0);
}

void RenderSettingsFrame(OperatorLutAlgorithm algorithm, LiveSettings& live)
{
    PluginConfiguration editable;
    editable.algorithm = algorithm;
    const auto before = editable;
    auto snapshot = live.Snapshot();
    snapshot.requested = editable;
    snapshot.active = editable;
    // Status is host-fed; the dispatcher below still has no active renderer.
    if (algorithm != OperatorLutAlgorithm::aces2)
    {
        snapshot.available = true;
        snapshot.pending = true;
        snapshot.persistence.state = SettingsPersistenceState::busy;
        snapshot.error = LiveSettingsError::workerBusy;
    }
    if (algorithm == OperatorLutAlgorithm::gt7)
    {
        snapshot.active.algorithm = static_cast<OperatorLutAlgorithm>(255);
        snapshot.error = LiveSettingsError::applyRejected;
        snapshot.persistence.state = SettingsPersistenceState::failed;
        snapshot.saveRejected = true;
    }
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({400, 900}, ImGuiCond_Always);
    if (ImGui::Begin("HDR settings", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        const auto vertices = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        const auto actions = DrawSettingsPanel(editable, snapshot);
        FO4HDR_CHECK(!actions.request);
        FO4HDR_CHECK(!actions.save);
        DispatchPanelActions(actions, live, editable);
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > vertices);
    }
    ImGui::End();
    ImGui::Render();
    CheckDrawData();
    FO4HDR_CHECK(editable == before);
}

}  // namespace

void RunSettingsPanelTests()
{
    const SettingsFrameContext context;
    LiveSettings live;
    for (const auto algorithm : {OperatorLutAlgorithm::aces2, OperatorLutAlgorithm::agX,
             OperatorLutAlgorithm::gt7})
        RenderSettingsFrame(algorithm, live);
    // The caller dispatches panel intent through the real unavailable owner.
    DispatchPanelActions({true, true}, live, PluginConfiguration{});
}

}  // namespace fo4hdr::tests
