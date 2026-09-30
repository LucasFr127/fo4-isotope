// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "ToneMapperPanel.hpp"
#include "TestHarness.hpp"
#include <imgui.h>

namespace fo4hdr::tests {

void TestToneMapperPanelSmoke()
{
    const auto render = [](ProductionToneMapperAlgorithm algorithm,
                            ImVec2 windowSize,
                            float scale)
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
        settings.productionAlgorithm = algorithm;
        settings.toneMapperParameters = {
            .acesLookStrength = 0.4,
            .agxMiddleGrayEv = 0.25,
            .agxContrast = 3.0,
            .agxHdrPurity = 0.55,
            .agxHdrShoulderPower = 2.1,
            .gt7ToeStrength = 1.5,
            .gt7ShoulderStart = 0.5,
            .gt7ColorBlend = 0.2};
        const auto before = settings.toneMapperParameters;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
        if (ImGui::Begin("Tone mapper parameters smoke", nullptr,
                ImGuiWindowFlags_NoSavedSettings))
        {
            const auto verticesBefore =
                ImGui::GetWindowDrawList()->VtxBuffer.Size;
            FO4HDR_CHECK(!DrawToneMapperPanel(settings));
            FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size >
                verticesBefore);
        }
        ImGui::End();
        ImGui::Render();

        const auto* drawData = ImGui::GetDrawData();
        FO4HDR_CHECK(drawData != nullptr);
        FO4HDR_CHECK(drawData->CmdListsCount > 0);
        FO4HDR_CHECK(drawData->TotalVtxCount > 0);
        FO4HDR_CHECK(drawData->TotalIdxCount > 0);
        FO4HDR_CHECK(settings.toneMapperParameters == before);
        ImGui::DestroyContext();
    };

    for (const auto algorithm : {
             ProductionToneMapperAlgorithm::aces2,
             ProductionToneMapperAlgorithm::agX,
             ProductionToneMapperAlgorithm::gt7})
    {
        render(algorithm, ImVec2(360.0F, 1000.0F), 1.0F);
        render(algorithm, ImVec2(900.0F, 1300.0F), 2.0F);
    }
}

void TestVanillaGradePanelSmoke()
{
    const auto render = [](ImVec2 windowSize, float scale)
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
        settings.vanillaGradeLuminancePercent = 65.0;
        settings.vanillaGradeColorPercent = 35.0;
        const auto luminanceBefore = settings.vanillaGradeLuminancePercent;
        const auto colorBefore = settings.vanillaGradeColorPercent;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
        if (ImGui::Begin("Vanilla grade controls smoke", nullptr,
                ImGuiWindowFlags_NoSavedSettings))
        {
            const auto verticesBefore =
                ImGui::GetWindowDrawList()->VtxBuffer.Size;
            FO4HDR_CHECK(!DrawVanillaGradePanel(settings));
            FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size >
                verticesBefore);
        }
        ImGui::End();
        ImGui::Render();

        const auto* drawData = ImGui::GetDrawData();
        FO4HDR_CHECK(drawData != nullptr);
        FO4HDR_CHECK(drawData->CmdListsCount > 0);
        FO4HDR_CHECK(drawData->TotalVtxCount > 0);
        FO4HDR_CHECK(drawData->TotalIdxCount > 0);
        FO4HDR_CHECK(
            settings.vanillaGradeLuminancePercent == luminanceBefore);
        FO4HDR_CHECK(settings.vanillaGradeColorPercent == colorBefore);
        ImGui::DestroyContext();
    };

    render(ImVec2(360.0F, 1000.0F), 1.0F);
    render(ImVec2(900.0F, 1300.0F), 2.0F);
}

}  // namespace fo4hdr::tests
