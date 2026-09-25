// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "SettingsPanel.hpp"
#include "DisplayCalibrationPanel.hpp"
#include "DisplayDiscovery.hpp"
#include "Fallout4HDR/ui/SettingsWindow.hpp"
#include "ShaderTestSuite.hpp"
#include "TestHarness.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>

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

void CheckDrawData()
{
    const auto* data = ImGui::GetDrawData();
    FO4HDR_CHECK(data != nullptr);
    if (!data) return;
    FO4HDR_CHECK(data->CmdListsCount > 0);
    FO4HDR_CHECK(data->TotalVtxCount > 0);
    FO4HDR_CHECK(data->TotalIdxCount > 0);
}

void RenderSettingsFrame(ProductionToneMapperAlgorithm algorithm)
{
    HdrSettings editable;
    editable.productionAlgorithm = algorithm;
    const auto before = editable;
    SettingsWindowView snapshot;
    snapshot.activeSettings = editable;
    snapshot.lutPending = algorithm != ProductionToneMapperAlgorithm::aces2Lmt;
    snapshot.liveSettingsError = algorithm == ProductionToneMapperAlgorithm::gt7;
    snapshot.pendingAlgorithm = algorithm;
    snapshot.pendingPeakNits = 1000;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({400, 900}, ImGuiCond_Always);
    if (ImGui::Begin("HDR settings", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        const auto vertices = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        const auto actions = DrawSettingsPanel(editable, snapshot);
        FO4HDR_CHECK(!actions.request);
        FO4HDR_CHECK(!actions.save);
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > vertices);
    }
    ImGui::End();
    ImGui::Render();
    CheckDrawData();
    FO4HDR_CHECK(editable == before);
}

void RenderCalibrationFrame()
{
    HdrSettings editable;
    editable.displayPeakNits = 900.0;
    editable.uiPaperWhiteNits = 150.0;
    editable.videoWhiteNits = 175.0;
    const auto before = editable;
    std::uint32_t mode = 11;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({400, 900}, ImGuiCond_Always);
    if (ImGui::Begin("Display calibration", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        const auto vertices = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        const DisplayCalibrationView display{"Authored display", 1200, 400, 203,
            true, false, true, "No profile for this display."};
        const auto actions = DrawDisplayCalibrationPanel(editable, mode, display);
        FO4HDR_CHECK(!actions.changed);
        FO4HDR_CHECK(!actions.hideOverlay);
        FO4HDR_CHECK(!actions.save && !actions.load);
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > vertices);
    }
    ImGui::End();
    ImGui::Render();
    CheckDrawData();
    FO4HDR_CHECK(editable == before);
    FO4HDR_CHECK(mode == 11);
}

}  // namespace

void RunSettingsPanelTests()
{
    const SettingsFrameContext frame;
    for (const auto algorithm : {ProductionToneMapperAlgorithm::aces2Lmt,
             ProductionToneMapperAlgorithm::agX, ProductionToneMapperAlgorithm::gt7})
        RenderSettingsFrame(algorithm);
    RenderCalibrationFrame();
}

}  // namespace fo4hdr::tests
