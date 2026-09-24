// SPDX-License-Identifier: GPL-3.0-or-later

#include "../UnitTestSuite.hpp"

#include "DisplayCalibrationPanel.hpp"
#include "TestHarness.hpp"

#include <imgui.h>

namespace fo4hdr::tests {
namespace {

void RenderCalibrationPanel(ImVec2 windowSize, float scale, std::uint32_t initialMode)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920.0F, 1400.0F);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    FO4HDR_CHECK(pixels != nullptr);
    FO4HDR_CHECK(width > 0 && height > 0);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;

    HdrSettings settings{};
    settings.displayPeakNits = 900.0;
    settings.uiPaperWhiteNits = 150.0;
    settings.videoWhiteNits = 175.0;
    const auto before = settings;
    auto mode = initialMode;
    const DisplayCalibrationView view{
        .displayNameUtf8 = "Living Room HDR Display",
        .reportedPeakNits = 1047.0,
        .reportedFullFrameNits = 612.0,
        .osSdrWhiteNits = 203.0,
        .identityAvailable = true,
        .profileBusy = false,
        .saveReady = true,
        .status = "Profile scan 95% complete; literal %s remains text."
    };

    ImGui::NewFrame();
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    if (ImGui::Begin("Display calibration smoke", nullptr,
            ImGuiWindowFlags_NoSavedSettings))
    {
        const auto verticesBefore =
            ImGui::GetWindowDrawList()->VtxBuffer.Size;
        const auto actions = DrawDisplayCalibrationPanel(
            settings, mode, view);
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size >
            verticesBefore);
        FO4HDR_CHECK(!actions.changed);
        FO4HDR_CHECK(!actions.load);
        FO4HDR_CHECK(!actions.save);
        FO4HDR_CHECK(!actions.hideOverlay);
    }
    ImGui::End();
    ImGui::Render();

    const auto* drawData = ImGui::GetDrawData();
    FO4HDR_CHECK(drawData != nullptr);
    FO4HDR_CHECK(drawData->CmdListsCount > 0);
    FO4HDR_CHECK(drawData->TotalVtxCount > 0);
    FO4HDR_CHECK(drawData->TotalIdxCount > 0);
    FO4HDR_CHECK(settings == before);
    FO4HDR_CHECK(mode == initialMode);
    ImGui::DestroyContext();
}

}  // namespace

void RunDisplayCalibrationTests()
{
    for (const auto mode : {0U, 10U, 11U, 12U, 13U, 14U})
    {
        RenderCalibrationPanel(ImVec2(360.0F, 1100.0F), 1.0F, mode);
        RenderCalibrationPanel(ImVec2(900.0F, 1300.0F), 2.0F, mode);
    }
}

}  // namespace fo4hdr::tests
